#include "broker/broker.h"

#include "api/http.h"
#include "session/session.h"

#include "yyjson.h"

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
/* Listener + 32 connections for each of the private and public servers. */
#define BROKER_HTTP_FDS 66

static volatile sig_atomic_t broker_stop;

typedef struct {
    tt_registry reg;
    tt_api local_api;
    tt_api public_api;
    tt_http *local_http;
    tt_http *public_http;
    char host[128];
    int port;
    char *token;
} broker_runtime;

static void on_stop(int signo) {
    (void)signo;
    broker_stop = 1;
}

static void broker_listen_response(tt_buf *out, const broker_runtime *b) {
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
    if (!doc || !root) {
        if (doc) yyjson_mut_doc_free(doc);
        tt_api_error(out, 500, "out of memory");
        return;
    }
    yyjson_mut_doc_set_root(doc, root);
    yyjson_mut_obj_add_bool(doc, root, "listening", b->public_http != NULL);
    yyjson_mut_obj_add_str(doc, root, "host", b->public_http ? b->host : "");
    yyjson_mut_obj_add_int(doc, root, "port", b->public_http ? b->port : 0);
    yyjson_mut_obj_add_bool(doc, root, "auth", b->token && b->token[0]);
    size_t len = 0;
    char *json = yyjson_mut_write(doc, 0, &len);
    yyjson_mut_doc_free(doc);
    if (!json) {
        tt_api_error(out, 500, "out of memory");
        return;
    }
    tt_api_respond(out, 200, "application/json", json, len);
    free(json);
}

static bool broker_local_route(void *user, const char *method, const char *path, const char *body,
                               size_t body_len, tt_buf *out) {
    broker_runtime *b = user;
    if (strcmp(path, "/v1/broker/listen") != 0) return false;
    if (strcmp(method, "GET") == 0) {
        broker_listen_response(out, b);
        return true;
    }
    if (strcmp(method, "POST") != 0) {
        tt_api_error(out, 405, "method not allowed");
        return true;
    }
    yyjson_doc *doc = body_len ? yyjson_read(body, body_len, 0) : NULL;
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL;
    yyjson_val *host_val = root ? yyjson_obj_get(root, "host") : NULL;
    yyjson_val *port_val = root ? yyjson_obj_get(root, "port") : NULL;
    yyjson_val *token_val = root ? yyjson_obj_get(root, "token") : NULL;
    const char *host = yyjson_is_str(host_val) ? yyjson_get_str(host_val) : NULL;
    int64_t port = yyjson_is_int(port_val) ? yyjson_get_sint(port_val) : 0;
    bool token_supplied = yyjson_is_str(token_val);
    const char *token = token_supplied ? yyjson_get_str(token_val) : "";
    if (!host || !*host || strlen(host) >= sizeof b->host || port < 1 || port > 65535 ||
        (token_supplied && strlen(token) >= 240)) {
        if (doc) yyjson_doc_free(doc);
        tt_api_error(out, 400, "invalid broker listen configuration");
        return true;
    }
    if (b->public_http) {
        bool same_address = strcmp(host, b->host) == 0 && port == b->port;
        bool same_token = !token_supplied || strcmp(token, b->token ? b->token : "") == 0;
        yyjson_doc_free(doc);
        if (!same_address || !same_token) {
            tt_api_error(out, 409, "broker already has a different public listener");
            return true;
        }
        broker_listen_response(out, b);
        return true;
    }
    char *owned_token = strdup(token);
    if (!owned_token) {
        yyjson_doc_free(doc);
        tt_api_error(out, 500, "out of memory");
        return true;
    }
    b->public_api.token = owned_token;
    char err[256];
    tt_http *listener = tt_http_listen(&b->public_api, host, (int)port, err, sizeof err);
    if (!listener) {
        b->public_api.token = NULL;
        free(owned_token);
        yyjson_doc_free(doc);
        tt_api_error(out, 409, err);
        return true;
    }
    b->public_http = listener;
    b->token = owned_token;
    b->port = (int)port;
    snprintf(b->host, sizeof b->host, "%s", host);
    yyjson_doc_free(doc);
    broker_listen_response(out, b);
    return true;
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
    broker_runtime b;
    memset(&b, 0, sizeof b);
    tt_registry_init(&b.reg, BROKER_SCROLLBACK);
    b.local_api = (tt_api){&b.reg, NULL, TNYTTY_VERSION};
    b.public_api = (tt_api){&b.reg, NULL, TNYTTY_VERSION};
    char err[256];
    b.local_http = tt_http_listen_unix(&b.local_api, socket_path, err, sizeof err);
    if (!b.local_http) {
        fprintf(stderr, "tnytty: broker: %s\n", err);
        if (ready_fd >= 0) {
            ssize_t ignored = write(ready_fd, "0", 1);
            (void)ignored;
            close(ready_fd);
        }
        tt_registry_free(&b.reg);
        return 1;
    }
    tt_http_set_local_route(b.local_http, broker_local_route, &b);
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

    struct pollfd *fds = NULL;
    tt_session **sessions = NULL;
    int fds_cap = 0;
    char scratch[16384];
    while (!broker_stop) {
        int needed = b.reg.count + BROKER_HTTP_FDS;
        if (needed < 128) needed = 128;
        if (needed > fds_cap) {
            struct pollfd *grown_fds = realloc(fds, (size_t)needed * sizeof *fds);
            if (!grown_fds) break;
            fds = grown_fds;
            tt_session **grown_sessions = realloc(sessions, (size_t)needed * sizeof *sessions);
            if (!grown_sessions) break;
            sessions = grown_sessions;
            fds_cap = needed;
        }
        int n = 0;
        int session_cap = fds_cap - BROKER_HTTP_FDS;
        int ndetached = tt_registry_poll_fill(&b.reg, fds, sessions, session_cap, false);
        n += ndetached;
        int nattached = tt_registry_poll_fill(&b.reg, fds + n, sessions + n, session_cap - n, true);
        n += nattached;
        int local_i = n;
        n += tt_http_fill(b.local_http, fds + n, fds_cap - n);
        int public_i = -1, public_n = 0;
        if (b.public_http) {
            public_i = n;
            public_n = tt_http_fill(b.public_http, fds + n, fds_cap - n);
            n += public_n;
        }
        int rc = poll(fds, (nfds_t)n, 100);
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        tt_registry_poll_handle(fds, sessions, ndetached + nattached, scratch, sizeof scratch);
        tt_http_handle(b.local_http, fds + local_i, (public_i >= 0 ? public_i : n) - local_i);
        if (public_i >= 0) tt_http_handle(b.public_http, fds + public_i, public_n);
    }
    tt_http_free(b.public_http);
    tt_http_free(b.local_http);
    free(b.token);
    free(sessions);
    free(fds);
    tt_registry_free(&b.reg);
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
