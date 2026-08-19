/* bridge.c — spawn and supervise `cursor-sdk-bridge`.
 * The child's stdout is redirected onto the same pipe as its stderr so a
 * chatty host can never corrupt tny's stdout (`ask --json`). Everything that
 * is not the ready line is forwarded to our stderr; the ready line and the
 * bearer token are never printed or logged. */
#include "backends/cursor/cursor.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void cursor_bridge_init(cursor_bridge *bp) {
    memset(bp, 0, sizeof *bp);
    bp->err_fd = -1;
    buf_init(&bp->acc);
}

static const char *resolve_bin(tny_ctx *ctx) {
    if (ctx && ctx->bridge_bin && *ctx->bridge_bin) return ctx->bridge_bin;
    const char *e = getenv("CURSOR_SDK_BRIDGE_BIN");
    if (e && *e) return e;
    return "cursor-sdk-bridge";
}

/* Forward one host line. Drops anything containing the bearer token. */
static void forward_line(cursor_bridge *bp, const char *line, size_t len, buf_t *capture) {
    while (len && (line[len - 1] == '\r' || line[len - 1] == '\n')) len--;
    if (!len) return;
    if (bp->token[0] && len < 64u * 1024u) {
        char *tmp = xstrndup(line, len);
        bool leak = strstr(tmp, bp->token) != NULL;
        free(tmp);
        if (leak) return;
    }
    if (capture && capture->len < 2048) buf_appendf(capture, "%.*s ", (int)len, line);
    fprintf(stderr, "cursor-sdk-bridge: %.*s\n", (int)len, line);
}

/* Split accumulated bytes into lines. -1 if a ready line was malformed. */
static int consume_lines(cursor_bridge *bp, buf_t *capture, char *err, size_t errlen) {
    while (bp->acc.len) {
        char *nl = memchr(bp->acc.data, '\n', bp->acc.len);
        if (!nl) break;
        size_t len = (size_t)(nl - bp->acc.data);
        int rc = 0;
        if (!bp->ready)
            rc = cursor_ready_parse(bp->acc.data, len, &bp->info, err, errlen);
        if (rc < 0) return -1;
        if (rc == 1) bp->ready = true;
        else forward_line(bp, bp->acc.data, len, capture);
        buf_consume(&bp->acc, len + 1);
    }
    if (bp->acc.len > CURSOR_MAX_STDERR) buf_clear(&bp->acc); /* runaway host */
    return 0;
}

/* Read whatever is buffered. 1 read something, 0 EOF, -1 would-block/error. */
static int drain(cursor_bridge *bp, buf_t *capture, char *err, size_t errlen, bool *bad) {
    int got = -1;
    for (;;) {
        char tmp[8192];
        ssize_t n = read(bp->err_fd, tmp, sizeof tmp);
        if (n > 0) {
            buf_append(&bp->acc, tmp, (size_t)n);
            got = 1;
            if (consume_lines(bp, capture, err, errlen) != 0) { *bad = true; return -1; }
            continue;
        }
        if (n == 0) return 0;
        if (errno == EINTR) continue;
        return got;
    }
}

static int load_token(cursor_bridge *bp, char *err, size_t errlen) {
    if (bp->info.auth_token[0]) {
        snprintf(bp->token, sizeof bp->token, "%s", bp->info.auth_token);
        return 0;
    }
    size_t n = 0;
    char *raw = file_slurp(bp->info.auth_token_file, &n);
    if (!raw) {
        snprintf(err, errlen, "cannot read the bridge auth token file (%s)",
                 bp->info.auth_token_file);
        return -1;
    }
    char *t = str_trim(raw);
    size_t tl = strlen(t);
    if (tl == 0 || tl >= sizeof bp->token) {
        free(raw);
        snprintf(err, errlen, "bridge auth token file is empty or oversized");
        return -1;
    }
    memcpy(bp->token, t, tl + 1);
    free(raw);
    return 0;
}

int cursor_bridge_spawn(cursor_bridge *bp, tny_ctx *ctx, const char *api_key,
                        int timeout_ms, char *err, size_t errlen) {
    const char *bin = resolve_bin(ctx);
    int p[2];
    if (pipe(p) != 0) {
        snprintf(err, errlen, "pipe: %s", strerror(errno));
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        snprintf(err, errlen, "fork: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, 0); close(devnull); }
        dup2(p[1], 1); /* host stdout must never reach tny stdout */
        dup2(p[1], 2);
        close(p[0]);
        close(p[1]);
        if (chdir(ctx->cwd) != 0) _exit(127);
        if (api_key && *api_key) setenv("CURSOR_API_KEY", api_key, 1);
        setenv("CURSOR_SDK_CLIENT_LANGUAGE", "c", 1);
        char *argv[] = {(char *)bin, (char *)"--workspace", ctx->cwd,
                        (char *)"--host", (char *)"127.0.0.1",
                        (char *)"--port", (char *)"0", NULL};
        execvp(bin, argv);
        _exit(127);
    }
    close(p[1]);
    bp->pid = pid;
    bp->err_fd = p[0];
    set_nonblock(bp->err_fd, true);

    buf_t capture;
    buf_init(&capture);
    int64_t deadline = now_ms() + timeout_ms;
    bool bad = false, eof = false;
    while (!bp->ready && !bad) {
        int left = (int)(deadline - now_ms());
        if (left <= 0) {
            snprintf(err, errlen, "%s did not print a ready line within %d s%s%s",
                     bin, timeout_ms / 1000, capture.len ? ": " : "",
                     capture.len ? capture.data : "");
            bad = true;
            break;
        }
        struct pollfd pf = {bp->err_fd, POLLIN, 0};
        int pr = poll(&pf, 1, left > 200 ? 200 : left);
        if (pr < 0 && errno != EINTR) {
            snprintf(err, errlen, "poll on bridge stderr: %s", strerror(errno));
            bad = true;
            break;
        }
        if (pr > 0 && drain(bp, &capture, err, errlen, &bad) == 0) eof = true;
        if (bad || bp->ready) break;

        int st = 0;
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid || eof) {
            drain(bp, &capture, err, errlen, &bad);
            if (bp->ready || bad) {
                if (w == pid) bp->pid = 0;
                break;
            }
            /* the pipe can close a hair before the child is reapable */
            for (int i = 0; i < 30 && w != pid; i++) {
                struct timespec ts = {0, 10 * 1000 * 1000};
                nanosleep(&ts, NULL);
                w = waitpid(pid, &st, WNOHANG);
            }
            if (w == pid) bp->pid = 0;
            int code = (w == pid && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;
            if (code == 127)
                snprintf(err, errlen, "cannot run '%s' (not found or not executable); "
                                      "set CURSOR_SDK_BRIDGE_BIN or --bridge-bin", bin);
            else if (code >= 0)
                snprintf(err, errlen, "%s exited with status %d before the ready line%s%s",
                         bin, code, capture.len ? ": " : "",
                         capture.len ? capture.data : "");
            else
                snprintf(err, errlen, "%s closed its output before the ready line%s%s",
                         bin, capture.len ? ": " : "", capture.len ? capture.data : "");
            bad = true;
            break;
        }
    }
    buf_free(&capture);
    if (bad || !bp->ready || load_token(bp, err, errlen) != 0) {
        cursor_bridge_stop(bp, 500);
        return -1;
    }
    return 0;
}

void cursor_bridge_pump(cursor_bridge *bp) {
    if (bp->err_fd < 0) return;
    char err[256];
    bool bad = false;
    if (drain(bp, NULL, err, sizeof err, &bad) == 0) {
        /* host closed its output: stop reading, keep the process check to
         * disconnect() so a finished stream can still be flushed */
        close(bp->err_fd);
        bp->err_fd = -1;
    }
}

void cursor_bridge_stop(cursor_bridge *bp, int grace_ms) {
    if (bp->pid > 0) {
        kill(bp->pid, SIGTERM);
        int waited = 0;
        for (;;) {
            int st = 0;
            pid_t w = waitpid(bp->pid, &st, WNOHANG);
            if (w == bp->pid || (w < 0 && errno != EINTR)) break;
            if (waited >= grace_ms) {
                kill(bp->pid, SIGKILL);
                waitpid(bp->pid, &st, 0);
                break;
            }
            struct timespec ts = {0, 10 * 1000 * 1000};
            nanosleep(&ts, NULL);
            waited += 10;
        }
        bp->pid = 0;
    }
    if (bp->err_fd >= 0) {
        close(bp->err_fd);
        bp->err_fd = -1;
    }
    buf_free(&bp->acc);
    buf_init(&bp->acc);
    memset(bp->token, 0, sizeof bp->token);
    memset(&bp->info, 0, sizeof bp->info);
    bp->ready = false;
}
