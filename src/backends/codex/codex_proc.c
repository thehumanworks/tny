/* codex_proc.c — spawning and reaping `codex app-server`, plus the stderr
 * drain. A full stderr pipe stalls the host (docs/architecture.md), so the
 * reader runs on every dispatch tick. */
#include "backends/codex/codex.h"
#include "util/tny_poll.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <unistd.h>

#define CX_STDERR_LINE_MAX 4096

/* Bind an ephemeral loopback port, note it, release it. Racy in theory;
 * the connect retry loop reports a clear error if the port got stolen. */
int cx_pick_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    socklen_t sl = sizeof sa;
    if (getsockname(fd, (struct sockaddr *)&sa, &sl) != 0) {
        close(fd);
        return -1;
    }
    int port = ntohs(sa.sin_port);
    close(fd);
    return port;
}

int cx_spawn(cx_impl *o, int port, char *err, size_t errlen) {
    int pfd[2];
    if (pipe(pfd) != 0) {
        snprintf(err, errlen, "codex: pipe failed: %s", strerror(errno));
        return -1;
    }
    char listen[64];
    snprintf(listen, sizeof listen, "ws://127.0.0.1:%d", port);
    char bin[512];
    snprintf(bin, sizeof bin, "%s",
             o->ctx->codex_bin && *o->ctx->codex_bin ? o->ctx->codex_bin : "codex");

    pid_t pid = fork();
    if (pid < 0) {
        snprintf(err, errlen, "codex: fork failed: %s", strerror(errno));
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0); /* own group: version-manager shims fork the real
                        * binary, and a group kill is the only signal that
                        * reaches it (an orphaned app-server otherwise
                        * outlives tny) */
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        if (pfd[1] != STDERR_FILENO) {
            dup2(pfd[1], STDERR_FILENO);
            close(pfd[1]);
        }
        close(pfd[0]);
        char *argv[] = {bin, (char *)"app-server", (char *)"--listen", listen, NULL};
        execvp(bin, argv);
        _exit(127);
    }
    setpgid(pid, pid); /* both sides set it: closes the fork/exec race */
    close(pfd[1]);
    set_nonblock(pfd[0], true);
    o->child = pid;
    o->child_err = pfd[0];
    o->child_reaped = false;
    return 0;
}

/* Host stderr is startup/diagnostic noise ("listening on: ws://…").
 * Forward it only under TNY_DEBUG; otherwise keep a short tail so a
 * startup failure can still show what the host said. */
static void cx_emit_stderr_line(cx_impl *o) {
    if (!o->child_line.data) {
        buf_clear(&o->child_line);
        return;
    }
    char *s = str_trim(o->child_line.data);
    if (*s) {
        if (!o->ctx->library_mode && tny_debug())
            fprintf(stderr, "codex: %.*s\n", CX_STDERR_LINE_MAX, s);
        if (o->stderr_tail.len > 2048) {
            buf_clear(&o->stderr_tail); /* keep it a tail, not a log */
        }
        buf_appendf(&o->stderr_tail, "codex: %.*s\n", CX_STDERR_LINE_MAX, s);
    }
    buf_clear(&o->child_line);
}

void cx_drain_child_stderr(cx_impl *o) {
    if (o->child_err < 0) return;
    for (;;) {
        char tmp[2048];
        ssize_t n = read(o->child_err, tmp, sizeof tmp);
        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (tmp[i] == '\n') {
                    cx_emit_stderr_line(o);
                } else if (o->child_line.len < CX_STDERR_LINE_MAX) {
                    buf_append(&o->child_line, tmp + i, 1);
                }
            }
            continue;
        }
        if (n == 0) { /* host closed its stderr */
            if (o->child_line.len) cx_emit_stderr_line(o);
            close(o->child_err);
            o->child_err = -1;
            return;
        }
        if (errno == EINTR) continue;
        return; /* EAGAIN */
    }
}

bool cx_child_gone(cx_impl *o) {
    if (o->child <= 0) return false;
    if (o->child_reaped) return true;
    int st = 0;
    pid_t r = waitpid(o->child, &st, WNOHANG);
    if (r == o->child) {
        o->child_reaped = true;
        return true;
    }
    return r < 0 && errno != EINTR;
}

void cx_stop_child(cx_impl *o) {
    if (o->wrote_registry) {
        /* our host is going away; unpublish it unless a newer tny already
         * wrote its own entry (cx_registry_remove checks the pid) */
        if (o->child > 0) cx_registry_remove(o->child);
        o->wrote_registry = false;
    }
    if (o->child_err >= 0) {
        cx_drain_child_stderr(o);
        if (o->child_err >= 0) {
            close(o->child_err);
            o->child_err = -1;
        }
    }
    if (o->child <= 0) return;
    if (!o->child_reaped) {
        if (kill(-o->child, SIGTERM) != 0) kill(o->child, SIGTERM);
        int64_t deadline = now_ms() + 2000;
        for (;;) {
            int st = 0;
            pid_t r = waitpid(o->child, &st, WNOHANG);
            if (r == o->child || (r < 0 && errno != EINTR)) {
                o->child_reaped = true;
                break;
            }
            if (now_ms() > deadline) break;
            tny_poll(NULL, 0, 20);
        }
        if (!o->child_reaped) {
            if (kill(-o->child, SIGKILL) != 0) kill(o->child, SIGKILL);
            int st = 0;
            while (waitpid(o->child, &st, 0) < 0 && errno == EINTR) {}
            o->child_reaped = true;
        } else {
            /* the direct child is gone; sweep any forked descendants */
            kill(-o->child, SIGKILL);
        }
    }
    o->child = 0;
}

int cx_capture(char *const argv[], char *out, size_t outcap, int timeout_ms) {
    int pfd[2];
    if (!out || outcap == 0) return -1;
    out[0] = 0;
    if (pipe(pfd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        if (pfd[1] > STDERR_FILENO) close(pfd[1]);
        close(pfd[0]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pfd[1]);
    set_nonblock(pfd[0], true);
    size_t used = 0;
    int64_t deadline = now_ms() + timeout_ms;
    bool eof = false;
    while (!eof && now_ms() < deadline) {
        struct pollfd pf = {pfd[0], POLLIN, 0};
        if (tny_poll(&pf, 1, 50) <= 0) continue;
        for (;;) {
            char tmp[512];
            ssize_t n = read(pfd[0], tmp, sizeof tmp);
            if (n > 0) {
                size_t room = outcap - 1 - used;
                size_t take = (size_t)n < room ? (size_t)n : room;
                memcpy(out + used, tmp, take);
                used += take;
                out[used] = 0;
                continue;
            }
            if (n == 0) {
                eof = true;
                break;
            }
            if (errno == EINTR) continue;
            break;
        }
    }
    close(pfd[0]);
    int status = 0, rc = -1;
    for (;;) {
        pid_t r = waitpid(pid, &status, eof ? 0 : WNOHANG);
        if (r == pid) {
            rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            break;
        }
        if (r < 0 && errno == EINTR) continue;
        if (now_ms() > deadline + 500) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        tny_poll(NULL, 0, 20);
    }
    return rc;
}
