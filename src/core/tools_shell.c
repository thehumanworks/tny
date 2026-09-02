/* tools_shell.c — terminal tool. Only the forked command child receives the
 * OS sandbox wrapper; the tny runner and host-provider tools stay outside. */
#include "core/tools.h"
#include "core/sandbox.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include <fcntl.h>

#ifndef TNY_SHELL_PATH
#define TNY_SHELL_PATH "/bin/sh"
#endif

#define SHELL_MAX_OUT             (512u * 1024u)
#define SHELL_PROFILE_PREVIEW_MAX (8u * 1024u)
#define SHELL_PROFILE_OUTPUT_MAX  (64u * 1024u * 1024u)

static int write_complete(int fd, const char *data, size_t len) {
    while (len) {
        ssize_t n = write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        data += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static char *result_file_open(tools_env *env, int *fd_out) {
    *fd_out = -1;
    const char *root = env->session && env->session->dir && !env->ctx->no_save ? env->session->dir
                                                                               : env->ctx->tny_dir;
    char *dir = path_join(root, "results");
    if (!dir || mkdir_p(dir) != 0) {
        free(dir);
        return NULL;
    }
    char *path = NULL;
    for (int attempt = 0; attempt < 10; attempt++) {
        char name[96];
        snprintf(name, sizeof name, "terminal-%lld-%d-%d.txt", (long long)now_ms(), (int)getpid(),
                 attempt);
        free(path);
        path = path_join(dir, name);
        if (!path) break;
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) {
            *fd_out = fd;
            break;
        }
        if (errno != EEXIST) break;
    }
    free(dir);
    if (*fd_out < 0) {
        free(path);
        return NULL;
    }
    return path;
}

static void shell_control_env(tools_env *env) {
    if (env->session_sock && *env->session_sock) setenv("TNY_SESSION_SOCK", env->session_sock, 1);
    else unsetenv("TNY_SESSION_SOCK");
    if (env->session_id && *env->session_id) setenv("TNY_SESSION_ID", env->session_id, 1);
    else unsetenv("TNY_SESSION_ID");
}

static char *run_background(tools_env *env, const char *cmd) {
    tny_sandbox_command sandbox = {0};
    char sandbox_err[192] = {0};
    if (tny_sandbox_command_build(env->ctx, TNY_SHELL_PATH, cmd, &sandbox, sandbox_err,
                                  sizeof sandbox_err) != 0)
        return tool_err("%s", sandbox_err);
    char *logdir = env->session ? path_join(env->session->dir, "bg") : xstrdup("/tmp/tny-bg");
    mkdir_p(logdir);
    char *id = gen_id();
    buf_t logpath;
    buf_init(&logpath);
    buf_appendf(&logpath, "%s/%s.log", logdir, id);

    pid_t pid = fork();
    if (pid < 0) {
        tny_sandbox_command_free(&sandbox);
        free(logdir);
        free(id);
        buf_free(&logpath);
        return tool_err("fork failed");
    }
    if (pid == 0) {
        setsid();
        shell_control_env(env);
        int fd = open(logpath.data, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
            close(fd);
        }
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, 0);
            close(devnull);
        }
        if (chdir(env->ctx->cwd) != 0) _exit(127);
        execv(sandbox.argv[0], sandbox.argv);
        _exit(127);
    }
    tny_sandbox_command_free(&sandbox);
    buf_t out;
    buf_init(&out);
    buf_appendf(&out,
                "started in background: pid %d\ncwd: %s\nlog: %s\n"
                "Check progress with read_file on the log.",
                (int)pid, env->ctx->cwd, logpath.data);
    free(logdir);
    free(id);
    buf_free(&logpath);
    return buf_detach(&out);
}

char *tool_shell_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled) {
    if (strcmp(name, "terminal") != 0) {
        *handled = false;
        return NULL;
    }
    *handled = true;
    const char *cmd = jget_str(args, "command");
    if (!cmd || !*cmd) return tool_err("missing command");
    int64_t timeout_s = jget_int(args, "timeout_s", 120);
    if (timeout_s <= 0 || timeout_s > 600) timeout_s = 120;
    if (jget_bool(args, "background", false)) {
        char *started = run_background(env, cmd);
        if (!tny_tool_profile_is_shell(env->ctx) || !started) return started;
        buf_t result;
        buf_init(&result);
        buf_appendf(&result, "exit: 0\nbytes: %zu\ncwd: %s\n%s", strlen(started), env->ctx->cwd,
                    started);
        free(started);
        return buf_detach(&result);
    }

    tny_sandbox_command sandbox = {0};
    char sandbox_err[192] = {0};
    if (tny_sandbox_command_build(env->ctx, TNY_SHELL_PATH, cmd, &sandbox, sandbox_err,
                                  sizeof sandbox_err) != 0)
        return tool_err("%s", sandbox_err);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        tny_sandbox_command_free(&sandbox);
        return tool_err("pipe failed");
    }
    pid_t pid = fork();
    if (pid < 0) {
        tny_sandbox_command_free(&sandbox);
        close(pipefd[0]);
        close(pipefd[1]);
        return tool_err("fork failed");
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], 1);
        dup2(pipefd[1], 2);
        close(pipefd[1]);
        shell_control_env(env);
        if (chdir(env->ctx->cwd) != 0) _exit(127);
        setpgid(0, 0);
        execv(sandbox.argv[0], sandbox.argv);
        _exit(127);
    }
    tny_sandbox_kind sandbox_kind = sandbox.kind;
    tny_sandbox_command_free(&sandbox);
    close(pipefd[1]);
    buf_t out;
    buf_init(&out);
    bool shell_profile = tny_tool_profile_is_shell(env->ctx);
    size_t preview_max = env->ctx->max_tool_result_bytes;
    if (preview_max > SHELL_PROFILE_PREVIEW_MAX) preview_max = SHELL_PROFILE_PREVIEW_MAX;
    int result_fd = -1;
    char *result_path = shell_profile ? result_file_open(env, &result_fd) : NULL;
    size_t output_bytes = 0;
    int64_t deadline = now_ms() + timeout_s * 1000;
    bool truncated = false, timed_out = false, output_limited = false;
    for (;;) {
        if (env->control_pump) env->control_pump(env->control_pump_ud, 0);
        struct pollfd pf = {pipefd[0], POLLIN, 0};
        int left = (int)(deadline - now_ms());
        if (left <= 0) {
            timed_out = true;
            break;
        }
        int slice = env->control_pump ? 50 : 500;
        int pr = tny_poll(&pf, 1, left > slice ? slice : left);
        if (pr < 0) break;
        if (pr == 0) {
            if (env->control_pump) env->control_pump(env->control_pump_ud, 0);
            continue;
        }
        char tmp[8192];
        ssize_t n = read(pipefd[0], tmp, sizeof tmp);
        if (n == 0) break;
        if (n < 0) break;
        size_t got = (size_t)n;
        if (shell_profile) {
            size_t keep = got;
            if (keep > SHELL_PROFILE_OUTPUT_MAX - output_bytes) {
                keep = SHELL_PROFILE_OUTPUT_MAX - output_bytes;
                output_limited = true;
            }
            if (result_fd >= 0 && write_complete(result_fd, tmp, keep) != 0) {
                close(result_fd);
                result_fd = -1;
                unlink(result_path);
                free(result_path);
                result_path = NULL;
            }
            size_t preview = keep;
            if (preview > preview_max - out.len) preview = preview_max - out.len;
            if (preview) buf_append(&out, tmp, preview);
            output_bytes += keep;
            if (output_bytes > out.len) truncated = true;
            if (output_limited) break;
        } else if (out.len < SHELL_MAX_OUT) {
            buf_append(&out, tmp, got);
        } else {
            truncated = true;
        }
    }
    close(pipefd[0]);
    int status = 0;
    if (timed_out || output_limited) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
    }
    waitpid(pid, &status, 0);
    if (result_fd >= 0) close(result_fd);
    buf_t res;
    buf_init(&res);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    char *denied_path = sandbox_kind != TNY_SANDBOX_NONE ? tny_sandbox_denied_path(out.data) : NULL;
    if (shell_profile) {
        buf_appendf(&res, "exit: %d\nbytes: %zu\ncwd: %s\n", code, output_bytes, env->ctx->cwd);
        if (denied_path)
            buf_appendf(&res,
                        "error: os sandbox denied a write to %s; add its parent directory to "
                        "workspace extra dirs with `tny workspace add DIR` or `--add-dir DIR`\n",
                        denied_path);
        if (timed_out)
            buf_appendf(&res, "timed out after %llds and was killed\n", (long long)timeout_s);
        if (output_limited)
            buf_appendf(&res, "output stopped at the %u MiB hard cap\n",
                        SHELL_PROFILE_OUTPUT_MAX / (1024u * 1024u));
        if (out.len) buf_append(&res, out.data, out.len);
        else buf_appends(&res, "(no output)");
        if (truncated && result_path) buf_appendf(&res, "\nfull: %s", result_path);
        else if (result_path) {
            unlink(result_path);
            free(result_path);
            result_path = NULL;
        }
        free(result_path);
        free(denied_path);
        buf_free(&out);
        return buf_detach(&res);
    }
    if (denied_path) {
        buf_appendf(&res,
                    "error: os sandbox denied a write to %s; add its parent directory to "
                    "workspace extra dirs with `tny workspace add DIR` or `--add-dir DIR`\n",
                    denied_path);
    }
    if (timed_out)
        buf_appendf(&res, "(timed out after %llds and was killed)\n", (long long)timeout_s);
    buf_appendf(&res, "exit code: %d\n", code);
    if (out.len) {
        buf_appends(&res, "output:\n");
        buf_append(&res, out.data, out.len);
        if (truncated) buf_appends(&res, "\n…(output truncated)");
    } else {
        buf_appends(&res, "(no output)");
    }
    free(denied_path);
    free(result_path);
    buf_free(&out);
    char *bounded = tool_bound_result(env, res.data, res.len);
    buf_free(&res);
    return bounded;
}
