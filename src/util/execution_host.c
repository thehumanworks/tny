#include "util/execution_host.h"
#include "core/execution_protocol.h"
#include "util/process.h"
#include "util/tny_poll.h"
#include "util/util.h"
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef __EMSCRIPTEN__
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#ifdef __linux__
#include <dirent.h>
#endif
extern char **environ;

static int configure(int fd) {
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
        return -1;
#ifdef SO_NOSIGPIPE
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one)) return -1;
#endif
    return 0;
}

int tny_exec_host_start(tny_exec_host *host) {
    *host = (tny_exec_host){.fd = -1, .pid = -1};
    /* Exact process identity is required for cancellation, not best-effort groups. */
    if (!tny_process_tree_supported()) return ENOTSUP;
    char *exe = tny_process_self_path();
    if (!exe) return ENOTSUP;
    int fds[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds)) {
        free(exe);
        return errno;
    }
    int rc = 0;
    if (configure(fds[0]) || configure(fds[1])) rc = errno;
    if (!rc) {
        char option[] = "--exec-server";
        char *argv[] = {exe, option, NULL};
        tny_fd_mapping map = {.source = fds[1], .target = 3};
        size_t count = 0;
        while (environ[count]) count++;
        char **environment = calloc(count + 1, sizeof *environment);
        if (!environment) rc = ENOMEM;
        else {
            size_t used = 0;
            for (size_t i = 0; i < count; i++)
                if (!tny_process_scope_env_reserved(environ[i])) environment[used++] = environ[i];
            rc = tny_process_spawn_mapped(argv, environment, &map, 1, &host->pid);
            free(environment);
        }
    }
    free(exe);
    close(fds[1]);
    if (rc) close(fds[0]);
    else host->fd = fds[0];
    return rc;
}

int tny_exec_host_accept(void) {
    struct sockaddr_storage address = {0};
    socklen_t len = sizeof address;
    int type = 0;
    socklen_t typelen = sizeof type;
    if (getsockopt(3, SOL_SOCKET, SO_TYPE, &type, &typelen) || type != SOCK_STREAM ||
        getpeername(3, (struct sockaddr *)&address, &len) ||
        len < offsetof(struct sockaddr_storage, ss_family) + sizeof address.ss_family ||
        address.ss_family != AF_UNIX || configure(3))
        return -1;
#ifdef __linux__
    /* musl has no spawn close-from action. This is a fresh, single-threaded
     * private entry point: close every unlisted descriptor before restoring
     * any context or executing code. The guardian uses the same entry seam. */
    DIR *directory = opendir("/proc/self/fd");
    if (!directory) return -1;
    int listing = dirfd(directory);
    struct dirent *entry;
    while ((entry = readdir(directory))) {
        char *end = NULL;
        long descriptor = strtol(entry->d_name, &end, 10);
        if (end && !*end && descriptor >= 4 && descriptor != listing && descriptor <= INT_MAX)
            close((int)descriptor);
    }
    closedir(directory);
#endif
    return 3;
}

static int ready(int fd, short events, int64_t deadline, tny_exec_cancel_fn cancel, void *ud) {
    for (;;) {
        if (cancel && cancel(ud)) {
            errno = ECANCELED;
            return -1;
        }
        int64_t left = deadline - monotonic_ms();
        if (left <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        struct pollfd p = {.fd = fd, .events = events};
        int rc = tny_poll(&p, 1, left > 25 ? 25 : (int)left);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) return -1;
        if (rc > 0) return 0; /* recv/send distinguishes EOF from readiness */
    }
}

static int transfer(int fd, void *data, size_t len, bool writing, int64_t deadline,
                    tny_exec_cancel_fn cancel, void *ud) {
    size_t offset = 0;
    while (offset < len) {
        if (ready(fd, writing ? POLLOUT : POLLIN, deadline, cancel, ud)) return -1;
        ssize_t n;
        if (writing) {
#ifdef MSG_NOSIGNAL
            n = send(fd, (char *)data + offset, len - offset, MSG_NOSIGNAL);
#else
            n = send(fd, (char *)data + offset, len - offset, 0);
#endif
        } else n = recv(fd, (char *)data + offset, len - offset, 0);
        if (n > 0) offset += (size_t)n;
        else if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        else {
            if (!n) errno = EPIPE;
            return -1;
        }
    }
    return 0;
}

int tny_exec_host_send(int fd, const char *json, int64_t deadline, tny_exec_cancel_fn cancel,
                       void *ud) {
    size_t len = json ? strlen(json) : 0;
    if (!len || len > TNY_EXEC_FRAME_MAX) {
        errno = EMSGSIZE;
        return -1;
    }
    unsigned char header[4] = {(unsigned char)(len >> 24), (unsigned char)(len >> 16),
                               (unsigned char)(len >> 8), (unsigned char)len};
    if (transfer(fd, header, sizeof header, true, deadline, cancel, ud)) return -1;
    return transfer(fd, (void *)json, len, true, deadline, cancel, ud);
}

char *tny_exec_host_receive(int fd, int64_t deadline, tny_exec_cancel_fn cancel, void *ud) {
    unsigned char h[4];
    if (transfer(fd, h, sizeof h, false, deadline, cancel, ud)) return NULL;
    size_t len = ((size_t)h[0] << 24) | ((size_t)h[1] << 16) | ((size_t)h[2] << 8) | h[3];
    if (!len || len > TNY_EXEC_FRAME_MAX) {
        errno = EMSGSIZE;
        return NULL;
    }
    char *data = malloc(len + 1);
    if (!data) return NULL;
    if (transfer(fd, data, len, false, deadline, cancel, ud) || memchr(data, 0, len)) {
        free(data);
        return NULL;
    }
    data[len] = 0;
    return data;
}

bool tny_exec_host_disconnected(int fd) {
    char c;
    ssize_t n = recv(fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
    return n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK);
}

int tny_exec_host_expect_eof(int fd, int64_t deadline, tny_exec_cancel_fn cancel, void *ud) {
    for (;;) {
        if (ready(fd, POLLIN, deadline, cancel, ud)) return -1;
        char byte;
        ssize_t n = recv(fd, &byte, 1, 0);
        if (n == 0) return 0;
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        errno = EPROTO;
        return -1;
    }
}

int tny_exec_host_close(tny_exec_host *host, bool completed) {
    if (host->fd >= 0) {
        close(host->fd);
        host->fd = -1;
    }
    if (host->pid <= 1 || host->reaped) return 0;
    int status = 0;
    /* Closing the lifeline first lets cooperative services cancel and remove
     * owned temporary artifacts. Bound that grace; unresponsive code is then
     * stopped through retained direct-child authority. An early failed exit
     * still reports unknown cleanup, never proven descendant absence. */
    {
        int64_t deadline = monotonic_ms() + 1000;
        do {
            pid_t got = waitpid(host->pid, &status, WNOHANG);
            if (got == host->pid) {
                host->reaped = true;
                return completed && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
            }
            if (got < 0 && errno != EINTR) return -1;
            tny_poll(NULL, 0, 5);
        } while (monotonic_ms() < deadline);
    }
    return tny_process_stop_owned_tree(host->pid, &status, &host->reaped);
}
#else
int tny_exec_host_start(tny_exec_host *host) {
    *host = (tny_exec_host){.fd = -1, .pid = -1};
    return ENOTSUP;
}
int tny_exec_host_accept(void) { return -1; }
int tny_exec_host_send(int fd, const char *json, int64_t deadline, tny_exec_cancel_fn c, void *ud) {
    (void)fd;
    (void)json;
    (void)deadline;
    (void)c;
    (void)ud;
    errno = ENOTSUP;
    return -1;
}
char *tny_exec_host_receive(int fd, int64_t deadline, tny_exec_cancel_fn c, void *ud) {
    (void)fd;
    (void)deadline;
    (void)c;
    (void)ud;
    errno = ENOTSUP;
    return NULL;
}
bool tny_exec_host_disconnected(int fd) {
    (void)fd;
    return true;
}
int tny_exec_host_expect_eof(int fd, int64_t deadline, tny_exec_cancel_fn c, void *ud) {
    (void)fd;
    (void)deadline;
    (void)c;
    (void)ud;
    errno = ENOTSUP;
    return -1;
}
int tny_exec_host_close(tny_exec_host *host, bool completed) {
    (void)host;
    (void)completed;
    return -1;
}
#endif
