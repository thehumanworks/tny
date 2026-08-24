/* tools_shell.c — terminal tool. Sandbox v1: none/auto→none, documented in
 * doctor (docs/features/permissions.md allows this with disclosure). */
#include "core/tools.h"
#include "util/tny_poll.h"
#include "util/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <sys/wait.h>
#include <fcntl.h>

#define SHELL_MAX_OUT (512u * 1024u)

static char *run_background(tools_env *env, const char *cmd) {
    char *logdir = env->session ? path_join(env->session->dir, "bg")
                                : xstrdup("/tmp/tny-bg");
    mkdir_p(logdir);
    char *id = gen_id();
    buf_t logpath;
    buf_init(&logpath);
    buf_appendf(&logpath, "%s/%s.log", logdir, id);

    pid_t pid = fork();
    if (pid < 0) {
        free(logdir); free(id); buf_free(&logpath);
        return tool_err("fork failed");
    }
    if (pid == 0) {
        setsid();
        int fd = open(logpath.data, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, 0); close(devnull); }
        if (chdir(env->ctx->cwd) != 0) _exit(127);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    buf_t out;
    buf_init(&out);
    buf_appendf(&out, "started in background: pid %d\ncwd: %s\nlog: %s\n"
                      "Check progress with read_file on the log.",
                (int)pid, env->ctx->cwd, logpath.data);
    free(logdir);
    free(id);
    buf_free(&logpath);
    return buf_detach(&out);
}

char *tool_shell_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled) {
    if (strcmp(name, "terminal") != 0) { *handled = false; return NULL; }
    *handled = true;
    const char *cmd = jget_str(args, "command");
    if (!cmd || !*cmd) return tool_err("missing command");
    int64_t timeout_s = jget_int(args, "timeout_s", 120);
    if (timeout_s <= 0 || timeout_s > 600) timeout_s = 120;
    if (jget_bool(args, "background", false)) return run_background(env, cmd);

    int pipefd[2];
    if (pipe(pipefd) != 0) return tool_err("pipe failed");
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        return tool_err("fork failed");
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], 1);
        dup2(pipefd[1], 2);
        close(pipefd[1]);
        if (chdir(env->ctx->cwd) != 0) _exit(127);
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    buf_t out;
    buf_init(&out);
    int64_t deadline = now_ms() + timeout_s * 1000;
    bool truncated = false, timed_out = false;
    for (;;) {
        struct pollfd pf = {pipefd[0], POLLIN, 0};
        int left = (int)(deadline - now_ms());
        if (left <= 0) { timed_out = true; break; }
        int pr = tny_poll(&pf, 1, left > 500 ? 500 : left);
        if (pr < 0) break;
        if (pr == 0) continue;
        char tmp[8192];
        ssize_t n = read(pipefd[0], tmp, sizeof tmp);
        if (n == 0) break;
        if (n < 0) break;
        if (out.len < SHELL_MAX_OUT) buf_append(&out, tmp, (size_t)n);
        else truncated = true;
    }
    close(pipefd[0]);
    int status = 0;
    if (timed_out) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
    }
    waitpid(pid, &status, 0);
    buf_t res;
    buf_init(&res);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
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
    buf_free(&out);
    char *bounded = tool_bound_result(env, res.data, res.len);
    buf_free(&res);
    return bounded;
}
