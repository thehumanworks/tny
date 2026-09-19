/* tools_shell.c — terminal tool. Only the forked command child receives the
 * OS sandbox wrapper; the tny runner and host-provider tools stay outside. */
#include "core/tools.h"
#include "core/sandbox.h"
#include "util/process.h"
#include "util/terminal_task.h"
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
#define SHELL_EXIT_CANCELLED      130 /* the shell's own "interrupted" status */

/* The turn's cooperative cancellation signal (tools.h), read without
 * consuming it and without re-entering the backend that is waiting on this
 * call: a frontend pump would recurse into the tool frame it is inside. */
static bool shell_cancelled(const tools_env *env) {
    return env->cancelled && env->cancelled(env->cancelled_ud);
}

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
    /* Anything this child starts is nested inside a turn: it inherits the
     * effective permission mode and may not widen it (docs/adr/0063). */
    setenv("TNY_NESTED", "1", 1);
    setenv("TNY_NESTED_MODE", tny_perm_mode_name(env->ctx->perm_mode), 1);
}

static void background_setup(void *ud) { shell_control_env(ud); }

char *tool_terminal_task_result(const tny_terminal_task *task, const char *observation,
                                const char *status_source) {
    buf_t out;
    buf_init(&out);
    buf_appends(&out, "{\"task_id\":");
    jescape(&out, task->id);
    buf_appends(&out, ",\"state\":");
    jescape(&out, tny_terminal_state_name(task->state));
    buf_appends(&out, ",\"exit_code\":");
    if (task->exit_code >= 0) buf_appendf(&out, "%d", task->exit_code);
    else buf_appends(&out, "null");
    buf_appends(&out, ",\"signal\":");
    if (task->signal) buf_appendf(&out, "%d", task->signal);
    else buf_appends(&out, "null");
    buf_appendf(&out, ",\"error_code\":%d,\"log\":", task->error);
    char *log = task->dir ? path_join(task->dir, "output.log") : NULL;
    jescape(&out, log ? log : "");
    free(log);
    buf_appends(&out, ",\"observation\":");
    jescape(&out, observation);
    buf_appends(&out, ",\"status_source\":");
    jescape(&out, status_source);
    buf_appends(&out, ",\"collect\":{\"tool\":\"terminal\",\"arguments\":{\"task_id\":");
    jescape(&out, task->id);
    buf_appends(&out, ",\"wait_s\":30}}}");
    return buf_detach(&out);
}

static char *background_call(tools_env *env, const char *cmd, const char *id, yyjson_val *args) {
    /* Owned task completion must not leave a first-party detached writer using
     * its workspace or permit. The ownership restriction survives synchronous
     * descendants even though their member capability is deliberately stripped.
     * This is not containment of arbitrary same-user shell daemonization. */
    if (!id && getenv(TNY_JOB_PARENT_ENV))
        return tool_err("background terminals are unavailable inside an owned job; "
                        "run the command in the foreground or ask the parent for a DAG task");
    if (!tny_terminal_supported())
        return tool_err("background terminal tasks are unsupported on this platform");
    int64_t wait_s = jget_int(args, "wait_s", 0);
    if ((jget(args, "wait_s") && !yyjson_is_int(jget(args, "wait_s"))) || wait_s < 0 ||
        wait_s > 600)
        return tool_err("wait_s must be between 0 and 600");
    if (id && (cmd || jget_bool(args, "background", false)))
        return tool_err("task_id cannot be combined with command or background");
    if (!id && jget(args, "wait_s")) return tool_err("wait_s requires task_id");
    char *root = path_join(env->ctx->tny_dir, "terminal");
    if (!root) return tool_err("out of memory");
    tny_terminal_task task = {0};
    int rc;
    const char *observation = "snapshot";
    if (!id) {
        tny_sandbox_command sandbox = {0};
        char err[192] = {0};
        if (tny_sandbox_command_build(env->ctx, TNY_SHELL_PATH, cmd, &sandbox, err, sizeof err) !=
            0) {
            free(root);
            return tool_err("%s", err);
        }
        rc = tny_terminal_start(root, env->ctx->cwd, sandbox.argv, background_setup, env, &task);
        tny_sandbox_command_free(&sandbox);
        if (!task.dir) {
            free(root);
            return tool_err("background launch failed: %s", strerror(rc));
        }
        observation = rc ? "launch_unconfirmed" : "launched";
        char task_id[sizeof task.id];
        memcpy(task_id, task.id, sizeof task_id);
        tny_terminal_task_free(&task);
        rc = tny_terminal_inspect(root, task_id, &task);
    } else {
        int64_t deadline = monotonic_ms() + wait_s * 1000;
        for (;;) {
            rc = tny_terminal_inspect(root, id, &task);
            if (rc || tny_terminal_finished(&task) || !wait_s) break;
            if (env->control_pump) env->control_pump(env->control_pump_ud, 0);
            if (shell_cancelled(env)) {
                observation = "cancelled";
                break;
            }
            int64_t left = deadline - monotonic_ms();
            if (left <= 0) {
                observation = "timed_out";
                break;
            }
            tny_terminal_task_free(&task);
            (void)tny_poll(NULL, 0, left < 50 ? (int)left : 50);
        }
    }
    char *result = rc ? tool_err("cannot inspect terminal task: %s", strerror(rc))
                      : tool_terminal_task_result(&task, observation, "waitpid");
    tny_terminal_task_free(&task);
    free(root);
    return result;
}

char *tool_shell_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled) {
    if (strcmp(name, "terminal") != 0) {
        *handled = false;
        return NULL;
    }
    *handled = true;
    const char *cmd = jget_str(args, "command");
    const char *id = jget_str(args, "task_id");
    if (jget(args, "task_id") && !id) return tool_err("task_id must be a string");
    if (id) return background_call(env, cmd, id, args);
    if (!cmd || !*cmd) return tool_err("missing command or task_id");
    if (jget_bool(args, "background", false)) return background_call(env, cmd, NULL, args);
    if (jget(args, "wait_s")) return tool_err("wait_s requires task_id");
    int64_t timeout_s = jget_int(args, "timeout_s", 120);
    if (timeout_s <= 0 || timeout_s > 600) timeout_s = 120;

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
    /* Also set from here: a cancellation that arrives before the child's own
     * setpgid(2) must still find the group this process kills. */
    setpgid(pid, pid);
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
    bool truncated = false, timed_out = false, output_limited = false, cancelled = false;
    for (;;) {
        if (env->control_pump) env->control_pump(env->control_pump_ud, 0);
        if (shell_cancelled(env)) {
            cancelled = true;
            break;
        }
        struct pollfd pf = {pipefd[0], POLLIN, 0};
        int left = (int)(deadline - now_ms());
        if (left <= 0) {
            timed_out = true;
            break;
        }
        /* The slice bounds how long a cancelled turn keeps a live child. */
        int slice = env->control_pump || env->cancelled ? 50 : 500;
        int pr = tny_poll(&pf, 1, left > slice ? slice : left);
        if (pr < 0) {
            /* A delivered signal is not a broken pipe: keep the output and
             * let the probe above decide whether this turn is cancelled. */
            if (errno == EINTR) continue;
            break;
        }
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
    bool reaped = false;
    /* A child that closed its stdout can still be running, so the wait after
     * EOF keeps the deadline and the cancellation signal live instead of
     * blocking in waitpid(2) until the command decides to exit. */
    while (!cancelled && !timed_out && !output_limited) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            reaped = true;
            break;
        }
        if (done < 0) {
            if (errno == EINTR) continue;
            break; /* nothing left to reap */
        }
        if (shell_cancelled(env)) cancelled = true;
        else if (now_ms() >= deadline) timed_out = true;
        else tny_poll(NULL, 0, 20);
    }
    /* The unreaped child pins its pid, so the sweep below owns exactly this
     * command and its descendants — never an unrelated process. */
    if (!reaped && (cancelled || timed_out || output_limited)) tny_process_kill_tree(pid);
    while (!reaped && waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (result_fd >= 0) close(result_fd);
    buf_t res;
    buf_init(&res);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    /* The user interrupted the turn: report that, not the signal this
     * process chose to stop the command with. */
    if (cancelled) code = SHELL_EXIT_CANCELLED;
    char *denied_path = sandbox_kind != TNY_SANDBOX_NONE ? tny_sandbox_denied_path(out.data) : NULL;
    if (shell_profile) {
        buf_appendf(&res, "exit: %d\nbytes: %zu\ncwd: %s\n", code, output_bytes, env->ctx->cwd);
        if (cancelled) buf_appends(&res, "cancelled: interrupted and the command was killed\n");
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
    if (cancelled) buf_appends(&res, "(cancelled: interrupted and the command was killed)\n");
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
