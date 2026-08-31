/* bridge.c — spawn and supervise `cursor-sdk-bridge`.
 * The child's stdout is redirected onto the same pipe as its stderr so a
 * chatty host can never corrupt tny's stdout (`ask --json`). Everything that
 * is not the ready line is forwarded to our stderr; the ready line and the
 * bearer token are never printed or logged. */
#include "backends/cursor/cursor.h"
#include "util/tny_poll.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#ifndef __EMSCRIPTEN__
#include <spawn.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef __EMSCRIPTEN__
extern char **environ;

/* glibc exposes the cwd actions only under _GNU_SOURCE.  tny intentionally
 * builds with the narrower _DEFAULT_SOURCE/_BSD_SOURCE feature set, while the
 * symbol itself has been part of glibc since 2.29. */
#if defined(__GLIBC__) && !defined(__USE_GNU)
extern int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t *actions,
                                                const char *path);
#endif

typedef struct {
    char **values;
    char *api_key;
    char *store_token;
} cursor_spawn_env;

static bool env_is(const char *entry, const char *name) {
    size_t n = strlen(name);
    return strncmp(entry, name, n) == 0 && entry[n] == '=';
}

static char *env_assignment(const char *name, const char *value) {
    size_t nl = strlen(name), vl = strlen(value);
    if (nl > SIZE_MAX - vl - 2) return NULL;
    char *entry = malloc(nl + vl + 2);
    if (!entry) return NULL;
    snprintf(entry, nl + vl + 2, "%s=%s", name, value);
    return entry;
}

static void spawn_env_free(cursor_spawn_env *env) {
    if (!env) return;
    secure_free(env->api_key);
    secure_free(env->store_token);
    free(env->values);
    memset(env, 0, sizeof *env);
}

/* Build an immutable child environment without mutating environ.  This is
 * important in libtny: another embedding thread may be reading the process
 * environment while a Cursor session is starting. */
static int spawn_env_init(cursor_spawn_env *out, const char *api_key, const char *store_token,
                          char *err, size_t errlen) {
    static char client_language[] = "CURSOR_SDK_CLIENT_LANGUAGE=c";
    memset(out, 0, sizeof *out);
    bool replace_api_key = api_key && *api_key;
    size_t count = 0;
    while (environ && environ[count]) count++;
    if (count > (SIZE_MAX / sizeof *out->values) - 4) {
        snprintf(err, errlen, "cursor: environment is too large");
        return -1;
    }
    out->values = malloc((count + 4) * sizeof *out->values);
    if (!out->values) goto oom;
    if (replace_api_key) {
        out->api_key = env_assignment("CURSOR_API_KEY", api_key);
        if (!out->api_key) goto oom;
    }
    if (store_token) {
        out->store_token = env_assignment("CURSOR_SDK_STORE_CALLBACK_AUTH_TOKEN", store_token);
        if (!out->store_token) goto oom;
    }

    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        char *entry = environ[i];
        if (env_is(entry, "CURSOR_SDK_CLIENT_LANGUAGE") ||
            env_is(entry, "CURSOR_SDK_STORE_CALLBACK_AUTH_TOKEN") ||
            (replace_api_key && env_is(entry, "CURSOR_API_KEY")))
            continue;
        out->values[used++] = entry;
    }
    if (out->api_key) out->values[used++] = out->api_key;
    out->values[used++] = client_language;
    if (out->store_token) out->values[used++] = out->store_token;
    out->values[used] = NULL;
    return 0;

oom:
    spawn_env_free(out);
    snprintf(err, errlen, "cursor: out of memory preparing bridge environment");
    return -1;
}

static int fd_cloexec_above_stdio(int fd) {
    if (fd <= STDERR_FILENO) {
        int moved = fcntl(fd, F_DUPFD, STDERR_FILENO + 1);
        if (moved < 0) return -1;
        close(fd);
        fd = moved;
    }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static int bridge_pipe(int p[2]) {
    if (pipe(p) != 0) return -1;
    p[0] = fd_cloexec_above_stdio(p[0]);
    if (p[0] < 0) {
        close(p[1]);
        return -1;
    }
    p[1] = fd_cloexec_above_stdio(p[1]);
    if (p[1] < 0) {
        close(p[0]);
        return -1;
    }
    return 0;
}

static int spawn_bridge(pid_t *pid, const char *bin, char *const bridge_argv[], char *const envp[],
                        const char *cwd, const int p[2]) {
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attrs;
    int rc = posix_spawn_file_actions_init(&actions);
    if (rc != 0) return rc;
    bool attrs_ready = false;

#if !defined(__MSYS__) && !defined(__CYGWIN__)
    rc = posix_spawn_file_actions_addchdir_np(&actions, cwd);
    if (rc != 0) goto done;
#endif
    rc = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (rc != 0) goto done;
    rc = posix_spawn_file_actions_adddup2(&actions, p[1], STDOUT_FILENO);
    if (rc != 0) goto done;
    rc = posix_spawn_file_actions_adddup2(&actions, p[1], STDERR_FILENO);
    if (rc != 0) goto done;
    rc = posix_spawn_file_actions_addclose(&actions, p[0]);
    if (rc != 0) goto done;
    rc = posix_spawn_file_actions_addclose(&actions, p[1]);
    if (rc != 0) goto done;

    rc = posix_spawnattr_init(&attrs);
    if (rc != 0) goto done;
    attrs_ready = true;
    rc = posix_spawnattr_setpgroup(&attrs, 0);
    if (rc != 0) goto done;
    rc = posix_spawnattr_setflags(&attrs, POSIX_SPAWN_SETPGROUP);
    if (rc != 0) goto done;

#if defined(__MSYS__) || defined(__CYGWIN__)
    /* MSYS2/Cygwin currently provide posix_spawn and process-group/file
     * actions but no cwd action.  A fixed shell adapter performs only chdir
     * and exec; after exec the bridge still receives bridge_argv exactly, and
     * the shell never receives a credential in argv. */
    (void)bin;
    static char shell_command[] = "cd -P -- \"$1\" && shift && exec \"$@\"";
    static char shell_name[] = "tny-cursor-spawn";
    char *spawn_argv[24];
    size_t argc = 0;
    spawn_argv[argc++] = (char *)TNY_SHELL_PATH;
    spawn_argv[argc++] = (char *)"-c";
    spawn_argv[argc++] = shell_command;
    spawn_argv[argc++] = shell_name;
    spawn_argv[argc++] = (char *)cwd;
    for (size_t i = 0; bridge_argv[i]; i++) {
        if (argc + 1 >= sizeof spawn_argv / sizeof spawn_argv[0]) {
            rc = E2BIG;
            goto done;
        }
        spawn_argv[argc++] = bridge_argv[i];
    }
    spawn_argv[argc] = NULL;
    rc = posix_spawnp(pid, TNY_SHELL_PATH, &actions, &attrs, spawn_argv, envp);
#else
    rc = posix_spawnp(pid, bin, &actions, &attrs, bridge_argv, envp);
#endif

done:
    if (attrs_ready) posix_spawnattr_destroy(&attrs);
    posix_spawn_file_actions_destroy(&actions);
    return rc;
}
#endif /* !__EMSCRIPTEN__ */

void cursor_bridge_init(cursor_bridge *bp) {
    memset(bp, 0, sizeof *bp);
    bp->err_fd = -1;
    buf_init(&bp->acc);
}

#ifndef __EMSCRIPTEN__
static const char *resolve_bin(tny_ctx *ctx) {
    if (ctx && ctx->bridge_bin && *ctx->bridge_bin) return ctx->bridge_bin;
    const char *e = getenv("CURSOR_SDK_BRIDGE_BIN");
    if (e && *e) return e;
    return "cursor-sdk-bridge";
}
#endif

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
    if (!bp->quiet && tny_debug()) fprintf(stderr, "cursor-sdk-bridge: %.*s\n", (int)len, line);
}

/* Split accumulated bytes into lines. -1 if a ready line was malformed. */
static int consume_lines(cursor_bridge *bp, buf_t *capture, char *err, size_t errlen) {
    while (bp->acc.len) {
        char *nl = memchr(bp->acc.data, '\n', bp->acc.len);
        if (!nl) break;
        size_t len = (size_t)(nl - bp->acc.data);
        int rc = 0;
        if (!bp->ready) rc = cursor_ready_parse(bp->acc.data, len, &bp->info, err, errlen);
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
            if (consume_lines(bp, capture, err, errlen) != 0) {
                *bad = true;
                return -1;
            }
            continue;
        }
        if (n == 0) return 0;
        if (errno == EINTR) continue;
        return got;
    }
}

#ifndef __EMSCRIPTEN__
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
        secure_free(raw);
        snprintf(err, errlen, "bridge auth token file is empty or oversized");
        return -1;
    }
    memcpy(bp->token, t, tl + 1);
    secure_free(raw);
    return 0;
}
#endif

int cursor_bridge_spawn(cursor_bridge *bp, tny_ctx *ctx, const char *api_key,
                        const cursor_bridge_launch_options *options, int timeout_ms, char *err,
                        size_t errlen) {
#ifdef __EMSCRIPTEN__
    (void)bp;
    (void)ctx;
    (void)api_key;
    (void)options;
    (void)timeout_ms;
    snprintf(err, errlen, "cursor: sdk.v1 bridge process is unavailable in WebAssembly");
    return -1;
#else
    const char *bin = resolve_bin(ctx);
    bp->quiet = ctx && ctx->library_mode;
    if (options && (!!options->store_callback_url != !!options->store_callback_token)) {
        snprintf(err, errlen, "cursor: store callback URL and token must be supplied together");
        return -1;
    }
    cursor_spawn_env spawn_env;
    const char *store_token = options ? options->store_callback_token : NULL;
    if (spawn_env_init(&spawn_env, api_key, store_token, err, errlen) != 0) return -1;
    int p[2];
    if (bridge_pipe(p) != 0) {
        snprintf(err, errlen, "pipe: %s", strerror(errno));
        spawn_env_free(&spawn_env);
        return -1;
    }
    char *argv[18];
    int argc = 0;
    argv[argc++] = (char *)bin;
    argv[argc++] = (char *)"--workspace";
    argv[argc++] = ctx->cwd;
    argv[argc++] = (char *)"--host";
    argv[argc++] = (char *)"127.0.0.1";
    argv[argc++] = (char *)"--port";
    argv[argc++] = (char *)"0";
    if (options && options->state_root) {
        argv[argc++] = (char *)"--state-root";
        argv[argc++] = (char *)options->state_root;
    }
    if (options && options->local_store_json) {
        argv[argc++] = (char *)"--local-store";
        argv[argc++] = (char *)options->local_store_json;
    }
    if (options && options->store_callback_url) {
        argv[argc++] = (char *)"--store-callback-url";
        argv[argc++] = (char *)options->store_callback_url;
    }
    argv[argc] = NULL;

    pid_t pid = 0;
    int spawn_error = spawn_bridge(&pid, bin, argv, spawn_env.values, ctx->cwd, p);
    spawn_env_free(&spawn_env);
    if (spawn_error != 0) {
        close(p[0]);
        close(p[1]);
        if (spawn_error == ENOENT || spawn_error == EACCES || spawn_error == ENOEXEC)
            snprintf(err, errlen,
                     "cannot run '%s' (not found or not executable); "
                     "set CURSOR_SDK_BRIDGE_BIN or --bridge-bin",
                     bin);
        else snprintf(err, errlen, "cannot start '%s': %s", bin, strerror(spawn_error));
        return -1;
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
            snprintf(err, errlen, "%s did not print a ready line within %d s%s%s", bin,
                     timeout_ms / 1000, capture.len ? ": " : "", capture.len ? capture.data : "");
            bad = true;
            break;
        }
        struct pollfd pf = {bp->err_fd, POLLIN, 0};
        int pr = tny_poll(&pf, 1, left > 200 ? 200 : left);
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
                snprintf(err, errlen,
                         "cannot run '%s' (not found or not executable); "
                         "set CURSOR_SDK_BRIDGE_BIN or --bridge-bin",
                         bin);
            else if (code >= 0)
                snprintf(err, errlen, "%s exited with status %d before the ready line%s%s", bin,
                         code, capture.len ? ": " : "", capture.len ? capture.data : "");
            else
                snprintf(err, errlen, "%s closed its output before the ready line%s%s", bin,
                         capture.len ? ": " : "", capture.len ? capture.data : "");
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
#endif
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
        if (kill(-bp->pid, SIGTERM) != 0) kill(bp->pid, SIGTERM);
        int waited = 0;
        for (;;) {
            int st = 0;
            pid_t w = waitpid(bp->pid, &st, WNOHANG);
            if (w == bp->pid || (w < 0 && errno != EINTR)) {
                kill(-bp->pid, SIGKILL); /* sweep forked descendants */
                break;
            }
            if (waited >= grace_ms) {
                if (kill(-bp->pid, SIGKILL) != 0) kill(bp->pid, SIGKILL);
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
