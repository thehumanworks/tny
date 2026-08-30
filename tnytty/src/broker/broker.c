#include "broker/broker.h"

#include "api/http.h"
#include "session/session.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef TNYTTY_VERSION
#define TNYTTY_VERSION "0.0.0-dev"
#endif

#define BROKER_SCROLLBACK 2000
#define BROKER_MAX_FDS    128

static volatile sig_atomic_t broker_stop;

static void on_stop(int signo) {
    (void)signo;
    broker_stop = 1;
}

static int socket_probe(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    close(fd);
    return rc;
}

int tt_broker_default_socket(char *out, size_t cap, char *err, size_t errcap) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    int n = runtime && *runtime ? snprintf(out, cap, "%s/tnytty/daemon.sock", runtime) : -1;
    if (n < 0 || (size_t)n >= cap || (size_t)n >= sizeof(((struct sockaddr_un *)0)->sun_path))
        n = snprintf(out, cap, "/tmp/tnytty-%lu/daemon.sock", (unsigned long)geteuid());
    if (n < 0 || (size_t)n >= cap || (size_t)n >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        if (err && errcap) snprintf(err, errcap, "broker socket path is too long");
        errno = ENAMETOOLONG;
        return -1;
    }
    return tt_broker_prepare_socket(out, err, errcap);
}

int tt_broker_prepare_socket(const char *path, char *err, size_t errcap) {
    if (!path || !*path || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        if (err && errcap) snprintf(err, errcap, "broker socket path is empty or too long");
        errno = EINVAL;
        return -1;
    }
    char dir[sizeof(((struct sockaddr_un *)0)->sun_path)];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir) {
        if (err && errcap) snprintf(err, errcap, "broker socket needs a private parent directory");
        errno = EINVAL;
        return -1;
    }
    *slash = '\0';
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        if (err && errcap) snprintf(err, errcap, "mkdir %s: %s", dir, strerror(errno));
        return -1;
    }
    struct stat st;
    if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid()) {
        if (err && errcap)
            snprintf(err, errcap, "broker directory is not a same-user directory: %s", dir);
        errno = EPERM;
        return -1;
    }
    if (chmod(dir, 0700) != 0) {
        if (err && errcap) snprintf(err, errcap, "chmod %s: %s", dir, strerror(errno));
        return -1;
    }
    return 0;
}

int tt_broker_run(const char *socket_path, int ready_fd) {
    broker_stop = 0;
    tt_registry reg;
    tt_registry_init(&reg, BROKER_SCROLLBACK);
    tt_api api = {&reg, NULL, TNYTTY_VERSION};
    char err[256];
    tt_http *http = tt_http_listen_unix(&api, socket_path, err, sizeof err);
    if (!http) {
        fprintf(stderr, "tnytty: broker: %s\n", err);
        if (ready_fd >= 0) {
            ssize_t ignored = write(ready_fd, "0", 1);
            (void)ignored;
            close(ready_fd);
        }
        tt_registry_free(&reg);
        return 1;
    }
    if (ready_fd >= 0) {
        ssize_t ignored = write(ready_fd, "1", 1);
        (void)ignored;
        close(ready_fd);
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    struct pollfd fds[BROKER_MAX_FDS];
    tt_session *sessions[BROKER_MAX_FDS];
    char scratch[16384];
    while (!broker_stop) {
        int n = 0;
        int ndetached = tt_registry_poll_fill(&reg, fds, sessions, BROKER_MAX_FDS / 2, false);
        n += ndetached;
        int nattached =
            tt_registry_poll_fill(&reg, fds + n, sessions + n, BROKER_MAX_FDS / 2, true);
        n += nattached;
        int http_i = n;
        n += tt_http_fill(http, fds + n, BROKER_MAX_FDS - n);
        int rc = poll(fds, (nfds_t)n, 100);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        tt_registry_poll_handle(fds, sessions, ndetached + nattached, scratch, sizeof scratch);
        tt_http_handle(http, fds + http_i, n - http_i);
    }
    tt_http_free(http);
    tt_registry_free(&reg);
    return 0;
}

int tt_broker_start(const char *socket_path, int timeout_ms, char *err, size_t errcap) {
    if (socket_probe(socket_path) == 0) return 0;
    if (tt_broker_prepare_socket(socket_path, err, errcap) != 0) return -1;
    char lock_path[160];
    if (snprintf(lock_path, sizeof lock_path, "%s.lock", socket_path) >= (int)sizeof lock_path) {
        if (err && errcap) snprintf(err, errcap, "broker lock path is too long");
        errno = ENAMETOOLONG;
        return -1;
    }
    int lock_flags = O_CREAT | O_RDWR;
#ifdef O_NOFOLLOW
    lock_flags |= O_NOFOLLOW;
#endif
    int lock_fd = open(lock_path, lock_flags, 0600);
    struct stat lock_st;
    if (lock_fd < 0 || fstat(lock_fd, &lock_st) != 0 || lock_st.st_uid != geteuid() ||
        !S_ISREG(lock_st.st_mode) || flock(lock_fd, LOCK_EX) != 0) {
        int saved = errno ? errno : EPERM;
        if (lock_fd >= 0) close(lock_fd);
        if (err && errcap) snprintf(err, errcap, "cannot lock broker startup: %s", strerror(saved));
        errno = saved;
        return -1;
    }
    chmod(lock_path, 0600);
    if (socket_probe(socket_path) == 0) {
        close(lock_fd);
        return 0;
    }
    int ready[2];
    if (pipe(ready) != 0) {
        close(lock_fd);
        if (err && errcap) snprintf(err, errcap, "broker ready pipe: %s", strerror(errno));
        return -1;
    }
    pid_t child = fork();
    if (child < 0) {
        close(ready[0]);
        close(ready[1]);
        close(lock_fd);
        if (err && errcap) snprintf(err, errcap, "broker fork: %s", strerror(errno));
        return -1;
    }
    if (child == 0) {
        close(lock_fd);
        close(ready[0]);
        if (setsid() < 0) _exit(1);
        pid_t grandchild = fork();
        if (grandchild < 0) _exit(1);
        if (grandchild > 0) _exit(0);
        int nullfd = open("/dev/null", O_RDWR);
        if (nullfd >= 0) {
            dup2(nullfd, STDIN_FILENO);
            dup2(nullfd, STDOUT_FILENO);
            dup2(nullfd, STDERR_FILENO);
            if (nullfd > STDERR_FILENO) close(nullfd);
        }
        _exit(tt_broker_run(socket_path, ready[1]));
    }
    close(ready[1]);
    while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
    struct pollfd pfd = {ready[0], POLLIN, 0};
    int budget = timeout_ms > 0 ? timeout_ms : 3000;
    int pr;
    do { pr = poll(&pfd, 1, budget); } while (pr < 0 && errno == EINTR);
    char state = 0;
    if (pr > 0) {
        ssize_t ignored = read(ready[0], &state, 1);
        (void)ignored;
    }
    close(ready[0]);
    if (state == '1' && socket_probe(socket_path) == 0) {
        close(lock_fd);
        return 0;
    }
    /* A simultaneous starter may have won the bind race. */
    for (int i = 0; i < 20; i++) {
        if (socket_probe(socket_path) == 0) {
            close(lock_fd);
            return 0;
        }
        poll(NULL, 0, 10);
    }
    if (err && errcap) snprintf(err, errcap, "broker did not become ready at %s", socket_path);
    errno = ETIMEDOUT;
    close(lock_fd);
    return -1;
}
