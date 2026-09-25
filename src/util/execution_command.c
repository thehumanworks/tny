#include "util/execution_command.h"
#include "util/execution_host.h"
#include "util/process.h"
#include "util/tny_poll.h"
#include "util/util.h"
#include "core/execution_protocol.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifndef __EMSCRIPTEN__
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
extern char **environ;

static int command_socket(int fd) {
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
static bool optional(yyjson_mut_doc *d, yyjson_mut_val *obj, const char *name, const char *text) {
    yyjson_mut_val *v = text ? yyjson_mut_strcpy(d, text) : yyjson_mut_null(d);
    return v && yyjson_mut_obj_add_val(d, obj, name, v);
}
int tny_exec_command_start(char *const argv[], const char *cwd, int timeout_ms,
                           const char *session_sock, const char *session_id,
                           const char *permission_mode, bool self_improve, int output_fd,
                           pid_t *pid, int *lifeline) {
    *pid = -1;
    *lifeline = -1;
    if (!argv || !argv[0] || !cwd || !permission_mode || timeout_ms < 1 || timeout_ms > 600000 ||
        !tny_process_tree_supported())
        return EINVAL;
    yyjson_mut_doc *d = yyjson_mut_doc_new(jallocator());
    yyjson_mut_val *obj = d ? yyjson_mut_obj(d) : NULL, *args = d ? yyjson_mut_arr(d) : NULL;
    if (d) yyjson_mut_doc_set_root(d, obj);
    bool ok = obj && args && yyjson_mut_obj_add_val(d, obj, "argv", args) &&
              yyjson_mut_obj_add_strcpy(d, obj, "cwd", cwd) &&
              yyjson_mut_obj_add_int(d, obj, "timeout_ms", timeout_ms) &&
              optional(d, obj, "session_sock", session_sock) &&
              optional(d, obj, "session_id", session_id) &&
              yyjson_mut_obj_add_strcpy(d, obj, "permission_mode", permission_mode) &&
              yyjson_mut_obj_add_bool(d, obj, "self_improve", self_improve);
    size_t argc = 0;
    for (; ok && argv[argc]; ++argc) {
        if (argc >= 256) {
            ok = false;
            break;
        }
        ok = yyjson_mut_arr_add_strcpy(d, args, argv[argc]);
    }
    char *request = ok ? jwrite(d) : NULL;
    yyjson_mut_doc_free(d);
    if (!request) return ENOMEM;
    if (strlen(request) > TNY_EXEC_FRAME_MAX) {
        secure_free(request);
        return E2BIG;
    }
    char *exe = tny_process_self_path();
    int fds[2];
    if (!exe) {
        secure_free(request);
        return ENOTSUP;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds)) {
        int rc = errno;
        free(exe);
        secure_free(request);
        return rc;
    }
    int rc = command_socket(fds[0]) || command_socket(fds[1]) ? errno : 0;
    if (!rc) {
        char *entry[] = {exe, (char *)"--exec-command", NULL};
        tny_fd_mapping maps[] = {{.source = fds[1], .target = 3},
                                 {.source = output_fd, .target = 1},
                                 {.source = output_fd, .target = 2}};
        rc = tny_process_spawn_mapped(entry, environ, maps, 3, pid);
    }
    free(exe);
    close(fds[1]);
    if (!rc && tny_exec_host_send(fds[0], request, monotonic_ms() + 5000, NULL, NULL)) rc = errno;
    secure_free(request);
    if (rc) {
        close(fds[0]);
        if (*pid > 0) {
            int status = 0;
            bool reaped = false;
            (void)tny_process_stop_owned_tree(*pid, &status, &reaped);
        }
        *pid = -1;
    } else *lifeline = fds[0];
    return rc;
}
/* After the one request frame, only EOF is legal. Unexpected trailing bytes
 * cancel instead of keeping a readable descriptor in a busy loop. */
static bool command_lifeline_lost(int fd) {
    char byte;
    ssize_t n = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    return n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR);
}
static volatile sig_atomic_t command_interrupted;
static void interrupted(int signal_number) {
    (void)signal_number;
    command_interrupted = 1;
}
static bool command_environment(yyjson_val *root) {
    yyjson_val *sock = jget(root, "session_sock"), *id = jget(root, "session_id");
    const char *mode = jget_str(root, "permission_mode");
    if ((!yyjson_is_null(sock) && !yyjson_is_str(sock)) ||
        (!yyjson_is_null(id) && !yyjson_is_str(id)) || !mode ||
        (strcmp(mode, "yolo") != 0 && strcmp(mode, "ask") != 0 && strcmp(mode, "auto") != 0) ||
        !yyjson_is_bool(jget(root, "self_improve")))
        return false;
    if ((yyjson_is_str(sock) ? setenv("TNY_SESSION_SOCK", yyjson_get_str(sock), 1)
                             : unsetenv("TNY_SESSION_SOCK")) ||
        (yyjson_is_str(id) ? setenv("TNY_SESSION_ID", yyjson_get_str(id), 1)
                           : unsetenv("TNY_SESSION_ID")) ||
        setenv("TNY_NESTED", "1", 1) || setenv("TNY_NESTED_MODE", mode, 1) ||
        setenv("TNY_SELF_IMPROVE", jget_bool(root, "self_improve", false) ? "1" : "0", 1))
        return false;
    return true;
}
int tny_exec_command_main(void) {
    int fd = tny_exec_host_accept();
    if (fd < 0 || !tny_process_tree_supported()) return 125;
    char *wire = tny_exec_host_receive(fd, monotonic_ms() + 5000, NULL, NULL);
    yyjson_doc *doc = wire ? jparse(wire, strlen(wire)) : NULL;
    secure_free(wire);
    yyjson_val *root = doc ? yyjson_doc_get_root(doc) : NULL, *args = jget(root, "argv");
    const char *cwd = jget_str(root, "cwd");
    int64_t timeout = jget_int(root, "timeout_ms", 0);
    size_t argc = yyjson_arr_size(args);
    if (!tny_exec_json_valid(root) || yyjson_obj_size(root) != 7 || !yyjson_is_arr(args) || !argc ||
        argc > 256 || !cwd || timeout < 1 || timeout > 600000 || !command_environment(root)) {
        yyjson_doc_free(doc);
        close(fd);
        return 125;
    }
    char *argv[257] = {0};
    size_t i, n;
    yyjson_val *arg;
    yyjson_arr_foreach(args, i, n, arg) {
        if (!yyjson_is_str(arg)) {
            yyjson_doc_free(doc);
            close(fd);
            return 125;
        }
        argv[i] = (char *)yyjson_get_str(arg);
    }
    if (argv[0][0] != '/' || chdir(cwd)) {
        yyjson_doc_free(doc);
        close(fd);
        return 125;
    }
    struct sigaction action = {0};
    action.sa_handler = interrupted;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGTERM, &action, NULL) || sigaction(SIGINT, &action, NULL) ||
        sigaction(SIGHUP, &action, NULL)) {
        yyjson_doc_free(doc);
        close(fd);
        return 125;
    }
    struct sigaction child_action = {0};
    child_action.sa_handler = SIG_DFL;
    sigemptyset(&child_action.sa_mask);
    if (sigaction(SIGCHLD, &child_action, NULL)) {
        yyjson_doc_free(doc);
        close(fd);
        return 125;
    }
    if (command_lifeline_lost(fd)) {
        yyjson_doc_free(doc);
        close(fd);
        return 125;
    }
    pid_t shell = -1;
    tny_fd_mapping output[] = {{.source = STDOUT_FILENO, .target = STDOUT_FILENO},
                               {.source = STDERR_FILENO, .target = STDERR_FILENO}};
    int rc = tny_process_spawn_mapped(argv, environ, output, 2, &shell);
    yyjson_doc_free(doc);
    if (rc) {
        close(fd);
        return 127;
    }
    int64_t deadline = monotonic_ms() + timeout;
    int status = 0;
    bool reaped = false;
    for (;;) {
        /* Check the lifeline before consuming wait authority. On parent loss,
         * a still-live shell remains pinned for the identity-safe tree stop. */
        if (command_interrupted || monotonic_ms() >= deadline || command_lifeline_lost(fd)) break;
        pid_t got = waitpid(shell, &status, WNOHANG);
        if (got == shell) {
            reaped = true;
            break;
        }
        if (got < 0 && errno != EINTR) break;
        struct pollfd p = {.fd = fd, .events = POLLIN};
        (void)tny_poll(&p, 1, 25);
    }
    close(fd);
    if (!reaped) {
        int cleanup = tny_process_stop_owned_tree(shell, &status, &reaped);
        if (cleanup || !reaped) return 125;
        return 130;
    }
    /* Deliberately daemonized children that reparent before cancellation are
     * outside the existing ancestry-based command scope. */
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
#else
int tny_exec_command_start(char *const argv[], const char *cwd, int timeout_ms,
                           const char *session_sock, const char *session_id,
                           const char *permission_mode, bool self_improve, int output_fd,
                           pid_t *pid, int *lifeline) {
    (void)argv;
    (void)cwd;
    (void)timeout_ms;
    (void)session_sock;
    (void)session_id;
    (void)permission_mode;
    (void)self_improve;
    (void)output_fd;
    *pid = -1;
    *lifeline = -1;
    return ENOTSUP;
}
int tny_exec_command_main(void) { return 125; }
#endif
