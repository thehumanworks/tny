/* sandbox.c — Seatbelt/bubblewrap argv construction. tny itself never enters
 * the sandbox; tools_shell execs this wrapper only in the terminal child. */
#include "core/sandbox.h"
#include "util/util.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef TNY_SHELL_PATH
#define TNY_SHELL_PATH "/bin/sh"
#endif

static char *find_on_path(const char *name) {
    const char *path = getenv("PATH");
    if (!path || !*path) return NULL;
    const char *p = path;
    while (true) {
        const char *end = strchr(p, ':');
        size_t n = end ? (size_t)(end - p) : strlen(p);
        char *dir = n ? xstrndup(p, n) : xstrdup(".");
        char *candidate = dir ? path_join(dir, name) : NULL;
        free(dir);
        if (candidate && access(candidate, X_OK) == 0) {
            char *absolute = path_abs(candidate);
            free(candidate);
            return absolute;
        }
        free(candidate);
        if (!end) break;
        p = end + 1;
    }
    return NULL;
}

static char *wrapper_path(tny_sandbox_kind kind) {
    if (kind == TNY_SANDBOX_SEATBELT) {
        const char *path = "/usr/bin/sandbox-exec";
        return access(path, X_OK) == 0 ? xstrdup(path) : NULL;
    }
    if (kind == TNY_SANDBOX_BWRAP) return find_on_path("bwrap");
    return NULL;
}

/* A wrapper binary on disk is not a wrapper that works: sandbox-exec cannot
 * nest inside another Seatbelt sandbox (nix builds), and bubblewrap needs
 * unprivileged user namespaces, which Ubuntu 24.04 and many CI images
 * restrict. Run the wrapper once around `sh -c 'exit 0'` and believe the exit
 * status, not the file system. */
bool tny_sandbox_probe_ms(tny_sandbox_kind kind, const char *wrapper, int timeout_ms) {
    if (!wrapper || !*wrapper || kind == TNY_SANDBOX_NONE) return false;
    const char *argv_seatbelt[] = {
        wrapper, "-p", "(version 1)(allow default)", TNY_SHELL_PATH, "-c", "exit 0", NULL};
    const char *argv_bwrap[] = {wrapper,
                                "--ro-bind",
                                "/",
                                "/",
                                "--dev",
                                "/dev",
                                "--proc",
                                "/proc",
                                "--unshare-pid",
                                "--",
                                TNY_SHELL_PATH,
                                "-c",
                                "exit 0",
                                NULL};
    const char **argv = kind == TNY_SANDBOX_SEATBELT ? argv_seatbelt : argv_bwrap;
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
            close(devnull);
        }
        setpgid(0, 0);
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    int64_t deadline = now_ms() + (timeout_ms > 0 ? timeout_ms : 3000);
    int status = 0;
    for (;;) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) break;
        if (done < 0) return false;
        if (now_ms() >= deadline) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return false;
        }
        usleep(10 * 1000);
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool tny_sandbox_probe(tny_sandbox_kind kind, const char *wrapper) {
    return tny_sandbox_probe_ms(kind, wrapper, 3000);
}

static tny_sandbox_kind host_wrapper_kind(void) {
#if defined(__EMSCRIPTEN__)
    return TNY_SANDBOX_NONE;
#elif defined(__APPLE__)
    return TNY_SANDBOX_SEATBELT;
#elif defined(__linux__)
    return TNY_SANDBOX_BWRAP;
#else
    return TNY_SANDBOX_NONE;
#endif
}

tny_sandbox_kind tny_sandbox_available(void) {
    /* Cached per process: the probe forks the wrapper once, and doctor,
     * status, the TUI and every terminal call share the answer. */
    static int cached = -1;
    if (cached >= 0) return (tny_sandbox_kind)cached;
    tny_sandbox_kind kind = host_wrapper_kind();
    char *path = kind == TNY_SANDBOX_NONE ? NULL : wrapper_path(kind);
    if (!path || !tny_sandbox_probe(kind, path)) kind = TNY_SANDBOX_NONE;
    free(path);
    cached = (int)kind;
    return kind;
}

tny_sandbox_kind tny_sandbox_effective(const tny_ctx *ctx) {
    if (!ctx || ctx->perm_mode == TNY_MODE_YOLO || !ctx->sandbox_mode ||
        strcmp(ctx->sandbox_mode, "none") == 0)
        return TNY_SANDBOX_NONE;
    if (strcmp(ctx->sandbox_mode, "auto") != 0 && strcmp(ctx->sandbox_mode, "os") != 0)
        return TNY_SANDBOX_NONE;
    return tny_sandbox_available();
}

const char *tny_sandbox_kind_name(tny_sandbox_kind kind) {
    return kind == TNY_SANDBOX_NONE ? "none" : "os";
}

const char *tny_sandbox_kind_description(tny_sandbox_kind kind) {
    if (kind == TNY_SANDBOX_SEATBELT) return "macOS Seatbelt";
    if (kind == TNY_SANDBOX_BWRAP) return "Linux bubblewrap";
    return "no OS wrapper";
}

static char *sandbox_temp_dir(void) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp || !dir_exists(tmp)) tmp = "/tmp";
    return path_abs(tmp);
}

static void profile_string(buf_t *profile, const char *value) {
    buf_appends(profile, "\"");
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p == '\\' || *p == '"') {
            char escaped[2] = {'\\', (char)*p};
            buf_append(profile, escaped, sizeof escaped);
        } else if (*p == '\n') buf_appends(profile, "\\n");
        else if (*p == '\r') buf_appends(profile, "\\r");
        else buf_append(profile, p, 1);
    }
    buf_appends(profile, "\"");
}

static void profile_subpath(buf_t *profile, const char *path) {
    buf_appends(profile, " (subpath ");
    profile_string(profile, path);
    buf_appends(profile, ")");
}

static char *seatbelt_profile(const tny_ctx *ctx) {
    buf_t profile;
    buf_init(&profile);
    buf_appends(&profile, "(version 1)\n"
                          "(deny default)\n"
                          "(allow process*)\n"
                          "(allow file-read*)\n"
                          "(allow file-write*");
    profile_subpath(&profile, ctx->cwd);
    for (int i = 0; i < ctx->n_extra_dirs; i++) {
        char *path = path_abs(ctx->extra_dirs[i]);
        if (path) profile_subpath(&profile, path);
        free(path);
    }
    char *tmp = sandbox_temp_dir();
    if (tmp) profile_subpath(&profile, tmp);
    free(tmp);
    buf_appends(&profile, " (literal \"/dev/null\")"
                          " (literal \"/dev/tty\")"
                          " (literal \"/dev/dtracehelper\"))\n"
                          "(allow network-outbound)\n"
                          "(allow network-inbound (local tcp \"localhost:*\")"
                          " (local udp \"localhost:*\"))\n"
                          "(allow sysctl-read)\n"
                          "(allow mach-lookup)\n"
                          "(allow signal (target same-sandbox))\n");
    return buf_detach(&profile);
}

static int argv_alloc(tny_sandbox_command *out, tny_sandbox_kind kind, size_t argc) {
    out->argv = calloc(argc + 1, sizeof *out->argv);
    if (!out->argv) return -1;
    out->kind = kind;
    out->argc = argc;
    return 0;
}

static bool argv_put(tny_sandbox_command *out, size_t *at, const char *value) {
    if (*at >= out->argc) return false;
    out->argv[*at] = xstrdup(value);
    if (!out->argv[*at]) return false;
    (*at)++;
    return true;
}

static bool argv_bind(tny_sandbox_command *out, size_t *at, const char *option, const char *path) {
    return argv_put(out, at, option) && argv_put(out, at, path) && argv_put(out, at, path);
}

int tny_sandbox_command_build_kind(const tny_ctx *ctx, tny_sandbox_kind kind, const char *wrapper,
                                   const char *shell, const char *command, tny_sandbox_command *out,
                                   char *err, size_t errlen) {
    if (!ctx || !shell || !command || !out) {
        if (err && errlen) snprintf(err, errlen, "invalid sandbox command");
        return -1;
    }
    memset(out, 0, sizeof *out);
    if (kind == TNY_SANDBOX_NONE) {
        if (argv_alloc(out, kind, 3) != 0) goto oom;
        size_t at = 0;
        if (!argv_put(out, &at, shell) || !argv_put(out, &at, "-c") || !argv_put(out, &at, command))
            goto oom;
        return 0;
    }
    if (!wrapper || !*wrapper) {
        if (err && errlen) snprintf(err, errlen, "sandbox wrapper is unavailable");
        return -1;
    }
    if (kind == TNY_SANDBOX_SEATBELT) {
        char *profile = seatbelt_profile(ctx);
        if (!profile || argv_alloc(out, kind, 6) != 0) {
            free(profile);
            goto oom;
        }
        size_t at = 0;
        bool ok = argv_put(out, &at, wrapper) && argv_put(out, &at, "-p") &&
                  argv_put(out, &at, profile) && argv_put(out, &at, shell) &&
                  argv_put(out, &at, "-c") && argv_put(out, &at, command);
        free(profile);
        if (!ok) goto oom;
        return 0;
    }
    if (kind == TNY_SANDBOX_BWRAP) {
        size_t argc = 19 + (size_t)ctx->n_extra_dirs * 3;
        if (argv_alloc(out, kind, argc) != 0) goto oom;
        size_t at = 0;
        bool ok = argv_put(out, &at, wrapper) && argv_put(out, &at, "--ro-bind") &&
                  argv_put(out, &at, "/") && argv_put(out, &at, "/") &&
                  argv_bind(out, &at, "--bind", ctx->cwd);
        for (int i = 0; ok && i < ctx->n_extra_dirs; i++) {
            char *path = path_abs(ctx->extra_dirs[i]);
            ok = path && argv_bind(out, &at, "--bind", path);
            free(path);
        }
        char *tmp = sandbox_temp_dir();
        ok = ok && tmp && argv_bind(out, &at, "--bind", tmp);
        free(tmp);
        ok = ok && argv_put(out, &at, "--dev") && argv_put(out, &at, "/dev") &&
             argv_put(out, &at, "--proc") && argv_put(out, &at, "/proc") &&
             argv_put(out, &at, "--unshare-pid") && argv_put(out, &at, "--") &&
             argv_put(out, &at, shell) && argv_put(out, &at, "-c") && argv_put(out, &at, command);
        if (!ok || at != argc) goto oom;
        return 0;
    }
    if (err && errlen) snprintf(err, errlen, "unknown sandbox wrapper");
    return -1;

oom:
    tny_sandbox_command_free(out);
    if (err && errlen) snprintf(err, errlen, "out of memory building sandbox command");
    return -1;
}

int tny_sandbox_command_build(const tny_ctx *ctx, const char *shell, const char *command,
                              tny_sandbox_command *out, char *err, size_t errlen) {
    tny_sandbox_kind kind = tny_sandbox_effective(ctx);
    bool explicit_os = ctx && ctx->perm_mode != TNY_MODE_YOLO && ctx->sandbox_mode &&
                       strcmp(ctx->sandbox_mode, "os") == 0;
    if (kind == TNY_SANDBOX_NONE && explicit_os) {
        if (err && errlen)
            snprintf(err, errlen,
                     "os sandbox requested but unavailable on this host (terminal was not run)");
        return -1;
    }
    char *wrapper = wrapper_path(kind);
    int rc = tny_sandbox_command_build_kind(ctx, kind, wrapper, shell, command, out, err, errlen);
    free(wrapper);
    return rc;
}

void tny_sandbox_command_free(tny_sandbox_command *command) {
    if (!command) return;
    if (command->argv) {
        for (size_t i = 0; i < command->argc; i++) free(command->argv[i]);
        free(command->argv);
    }
    memset(command, 0, sizeof *command);
}

char *tny_sandbox_denied_path(const char *output) {
    if (!output) return NULL;
    const char *markers[] = {": Operation not permitted", ": Permission denied",
                             ": Read-only file system", NULL};
    const char *hit = NULL;
    for (int i = 0; markers[i]; i++) {
        const char *candidate = strstr(output, markers[i]);
        if (candidate && (!hit || candidate < hit)) hit = candidate;
    }
    if (!hit) return NULL;
    const char *line = hit;
    while (line > output && line[-1] != '\n') line--;
    const char *start = memchr(line, '/', (size_t)(hit - line));
    if (!start) return NULL;
    const char *end = hit;
    while (end > start && (end[-1] == ':' || end[-1] == ' ' || end[-1] == '\'' || end[-1] == '"'))
        end--;
    return end > start ? xstrndup(start, (size_t)(end - start)) : NULL;
}
