/* jobs_host.c — see jobs_host.h. The only file where durable jobs touch
 * platform-specific process, lock and filesystem behavior. */
#include "util/jobs_host.h"

#include "util/util.h"
#include "util/tny_poll.h"
#include "util/process.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef __EMSCRIPTEN__
#include <sys/file.h>
#include <sys/wait.h>
#endif

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

bool tny_jobs_host_execution_supported(void) {
    return tny_process_scope_native_jobs() || tny_process_tree_supported();
}

int tny_jobs_host_mkdir_private(const char *path) {
    if (!path || !*path) return EINVAL;
    char *copy = xstrdup(path);
    if (!copy) return ENOMEM;
    int rc = 0;
    for (char *p = copy + 1; *p && !rc; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(copy, 0700) != 0 && errno != EEXIST) rc = errno;
        *p = '/';
    }
    if (!rc && mkdir(copy, 0700) != 0 && errno != EEXIST) rc = errno;
    if (!rc) {
        struct stat st;
        if (stat(copy, &st) != 0) rc = errno;
        else if (!S_ISDIR(st.st_mode)) rc = ENOTDIR;
    }
    free(copy);
    return rc;
}

static int write_private(const char *path, const void *data, size_t len, bool once) {
    if (!path || !*path || (!data && len)) return EINVAL;
    buf_t tmp;
    buf_init(&tmp);
    uint8_t nonce[8] = {0};
    if (!random_bytes(nonce, sizeof nonce)) {
        buf_free(&tmp);
        return EIO;
    }
    buf_appendf(&tmp, "%s.tmp-", path);
    for (size_t i = 0; i < sizeof nonce; i++) buf_appendf(&tmp, "%02x", nonce[i]);
    if (buf_oom(&tmp)) {
        buf_free(&tmp);
        return ENOMEM;
    }
    int fd = open(tmp.data, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        int e = errno;
        buf_free(&tmp);
        return e;
    }
    int rc = 0;
    const char *p = data;
    size_t left = len;
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            rc = n < 0 ? errno : EIO;
            break;
        }
        p += n;
        left -= (size_t)n;
    }
    if (!rc && fsync(fd) != 0) rc = errno;
    if (close(fd) != 0 && !rc) rc = errno;
    if (!rc && (once ? link(tmp.data, path) : rename(tmp.data, path)) != 0) rc = errno;
    if (rc || once) unlink(tmp.data);
    buf_free(&tmp);
    return rc;
}

int tny_jobs_host_write_private(const char *path, const void *data, size_t len) {
    return write_private(path, data, len, false);
}

int tny_jobs_host_write_once(const char *path, const void *data, size_t len) {
    return write_private(path, data, len, true);
}

int tny_jobs_host_snapshot(const char *path, const void *data, size_t len) {
    int rc = tny_jobs_host_write_once(path, data, len);
    if (rc != EEXIST) return rc;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno;
    struct stat st;
    rc =
        fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size != len
            ? EINVAL
            : 0;
    size_t offset = 0;
    while (!rc && offset < len) {
        char chunk[4096];
        size_t want = len - offset < sizeof chunk ? len - offset : sizeof chunk;
        ssize_t n = read(fd, chunk, want);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || memcmp(chunk, (const char *)data + offset, (size_t)n) != 0) rc = EINVAL;
        else offset += (size_t)n;
    }
    close(fd);
    return rc;
}

int tny_jobs_host_lock_open(const char *path) {
    if (!path || !*path) {
        errno = EINVAL;
        return -1;
    }
    return open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
}

tny_jobs_lock_rc tny_jobs_host_lock_try(int fd) {
#ifdef __EMSCRIPTEN__
    (void)fd;
    errno = ENOTSUP;
    return TNY_JOBS_LOCK_ERROR;
#else
    if (fd < 0) return TNY_JOBS_LOCK_ERROR;
    for (;;) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0) return TNY_JOBS_LOCK_ACQUIRED;
        if (errno == EINTR) continue;
        return errno == EWOULDBLOCK || errno == EAGAIN ? TNY_JOBS_LOCK_BUSY : TNY_JOBS_LOCK_ERROR;
    }
#endif
}

void tny_jobs_host_lock_release(int fd) {
#ifndef __EMSCRIPTEN__
    if (fd >= 0) flock(fd, LOCK_UN);
#else
    (void)fd;
#endif
}

void tny_jobs_host_lock_close(int fd) {
    if (fd >= 0) close(fd); /* the advisory lock goes with the description */
}

tny_jobs_owner_state tny_jobs_host_owner_state(const char *lock_path) {
#ifdef __EMSCRIPTEN__
    (void)lock_path;
    return TNY_JOBS_OWNER_UNKNOWN;
#else
    if (!lock_path) return TNY_JOBS_OWNER_UNKNOWN;
    int fd = open(lock_path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return errno == ENOENT ? TNY_JOBS_OWNER_FREE : TNY_JOBS_OWNER_UNKNOWN;
    /* A11: the probe is always LOCK_EX|LOCK_NB and never waits. EWOULDBLOCK
     * means an active or uncertain owner, so reclaim is denied. */
    tny_jobs_lock_rc rc = tny_jobs_host_lock_try(fd);
    if (rc == TNY_JOBS_LOCK_ACQUIRED) tny_jobs_host_lock_release(fd);
    close(fd);
    return rc == TNY_JOBS_LOCK_ACQUIRED ? TNY_JOBS_OWNER_FREE
           : rc == TNY_JOBS_LOCK_BUSY   ? TNY_JOBS_OWNER_HELD
                                        : TNY_JOBS_OWNER_UNKNOWN;
#endif
}

bool tny_jobs_host_fd_is_file(int fd, const char *path) {
    if (fd < 0 || !path) return false;
    struct stat a, b;
    if (fstat(fd, &a) != 0 || stat(path, &b) != 0) return false;
    return S_ISREG(a.st_mode) && a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

int tny_jobs_host_set_cloexec(int fd) {
    if (fd < 0) return EINVAL;
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0) return errno;
    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) return errno;
    return 0;
}

char *tny_jobs_host_canonical_output(const char *path, bool *alias_out, const char **err) {
    if (alias_out) *alias_out = false;
    if (err) *err = NULL;
    if (!path || !*path) {
        if (err) *err = "an output path is required";
        return NULL;
    }
    if (strlen(path) > 3000) {
        if (err) *err = "the output path is too long";
        return NULL;
    }
    /* Absolute, but deliberately unresolved: the final component's own
     * identity is what an alias check has to look at. path_abs() resolves
     * symlinks, which would hide exactly the case being rejected. */
    char *absolute = NULL;
    if (path[0] == '/') absolute = xstrdup(path);
    else {
        char cwd[3100];
        if (!getcwd(cwd, sizeof cwd)) {
            if (err) *err = "the working directory cannot be read";
            return NULL;
        }
        absolute = path_join(cwd, path);
    }
    if (!absolute || absolute[0] != '/') {
        free(absolute);
        if (err) *err = "the output path cannot be resolved";
        return NULL;
    }
    char *slash = strrchr(absolute, '/');
    char *name = slash ? slash + 1 : NULL;
    if (!name || !*name || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        free(absolute);
        if (err) *err = "the output path must name a file";
        return NULL;
    }
    /* Resolve the directory, keep the final component literal: a destination
     * that does not exist yet is the normal case. */
    *slash = 0;
    char *dir = path_abs(absolute[0] ? absolute : "/");
    *slash = '/';
    if (!dir) {
        free(absolute);
        if (err) *err = "the output directory does not exist";
        return NULL;
    }
    char *canonical = path_join(dir, name);
    free(dir);
    free(absolute);
    if (!canonical) {
        if (err) *err = "out of memory";
        return NULL;
    }
    struct stat st;
    if (lstat(canonical, &st) == 0) {
        if (S_ISLNK(st.st_mode)) {
            if (alias_out) *alias_out = true;
            if (err) *err = "the output path is a symbolic link";
            free(canonical);
            return NULL;
        }
        if (S_ISREG(st.st_mode) && st.st_nlink > 1) {
            if (alias_out) *alias_out = true;
            if (err) *err = "the output path has more than one hard link";
            free(canonical);
            return NULL;
        }
        if (!S_ISREG(st.st_mode)) {
            if (err) *err = "the output path is not a regular file";
            free(canonical);
            return NULL;
        }
    }
    return canonical;
}

/* One deadline covers BOTH private transfer and acknowledgement. Neither fd
 * may block even after poll readiness. CLI signals interrupt only this wait;
 * tool callers retain their own signal handlers and cancellation callback. */
static volatile sig_atomic_t handshake_interrupted;
static void handshake_signal(int sig) {
    (void)sig;
    handshake_interrupted = 1;
}

int tny_jobs_host_handshake(int payload_fd, int ack_fd, const char *payload, size_t len,
                            const char *id, int timeout_ms, bool (*cancelled)(void *), void *ud) {
    int64_t deadline = monotonic_ms() + timeout_ms;
    struct sigaction ignore = {0}, stop = {0}, oldpipe = {0}, oldint = {0}, oldterm = {0};
    ignore.sa_handler = SIG_IGN;
    stop.sa_handler = handshake_signal;
    sigemptyset(&ignore.sa_mask);
    sigemptyset(&stop.sa_mask);
    bool pipe_set = sigaction(SIGPIPE, &ignore, &oldpipe) == 0;
    bool int_set = false, term_set = false;
    handshake_interrupted = 0;
    if (!cancelled) {
        int_set = sigaction(SIGINT, &stop, &oldint) == 0;
        term_set = sigaction(SIGTERM, &stop, &oldterm) == 0;
    }
    int rc = EIO;
    int pf = fcntl(payload_fd, F_GETFL), af = fcntl(ack_fd, F_GETFL);
    if (!pipe_set || pf < 0 || af < 0 || fcntl(payload_fd, F_SETFL, pf | O_NONBLOCK) != 0 ||
        fcntl(ack_fd, F_SETFL, af | O_NONBLOCK) != 0)
        goto done;
    size_t off = 0, got = 0;
    char ack[64] = {0};
    bool sent = false;
    for (;;) {
        if (handshake_interrupted || (cancelled && cancelled(ud))) {
            rc = ECANCELED;
            break;
        }
        int64_t left = deadline - monotonic_ms();
        if (left <= 0) {
            rc = ETIMEDOUT;
            break;
        }
        if (!sent && off == len) {
            /* EOF belongs to the request protocol, so finish the write end
             * here rather than waiting for the acknowledgement first. */
            close(payload_fd);
            payload_fd = -1;
            sent = true;
        }
        struct pollfd fd = {sent ? ack_fd : payload_fd, sent ? POLLIN : POLLOUT, 0};
        int ready = tny_poll(&fd, 1, left < 100 ? (int)left : 100);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        if (!ready) continue;
        if (!sent) {
            ssize_t n = write(payload_fd, payload + off, len - off);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if (n <= 0) break;
            off += (size_t)n;
        } else {
            ssize_t n = read(ack_fd, ack + got, sizeof ack - got - 1);
            if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
            if (n <= 0) break;
            got += (size_t)n;
            ack[got] = 0;
            if (strchr(ack, '\n')) {
                char expected[64];
                snprintf(expected, sizeof expected, "ok %s\n", id);
                rc = strcmp(ack, expected) == 0 ? 0 : EIO;
                break;
            }
            if (got + 1 == sizeof ack) break;
        }
    }
done:
    if (payload_fd >= 0) close(payload_fd);
    close(ack_fd);
    if (term_set) sigaction(SIGTERM, &oldterm, NULL);
    if (int_set) sigaction(SIGINT, &oldint, NULL);
    if (pipe_set) sigaction(SIGPIPE, &oldpipe, NULL);
    return rc;
}

int tny_jobs_host_reap(pid_t pid, int *status) {
#ifdef __EMSCRIPTEN__
    (void)pid;
    (void)status;
    return -1;
#else
    if (pid <= 1) return -1;
    for (;;) {
        int raw = 0;
        pid_t got = waitpid(pid, &raw, WNOHANG);
        if (got == pid) {
            if (status) *status = raw;
            return 1;
        }
        if (got == 0) return 0;
        if (errno == EINTR) continue;
        return -1;
    }
#endif
}

int tny_jobs_host_signal_owned(pid_t pid, int sig) {
#ifdef __EMSCRIPTEN__
    (void)pid;
    (void)sig;
    return ENOTSUP;
#else
    if (pid <= 1) return EINVAL;
    return kill(pid, sig) == 0 ? 0 : errno;
#endif
}

void tny_jobs_host_sleep_ms(int ms) {
    if (ms <= 0) return;
    usleep((unsigned)ms * 1000u);
}

void tny_jobs_host_detach_session(void) {
#ifndef __EMSCRIPTEN__
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    if (getpgrp() != getpid()) setsid();
#endif
}
