#include "util/git.h"
#include "util/tny_poll.h"
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
int git_run(const char *cwd, const char *const *args, buf_t *out) {
    (void)cwd;
    (void)args;
    buf_clear(out);
    buf_appends(out, "Git worktrees are unavailable in wasm; use a native tny build");
    return -1;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

int git_run(const char *cwd, const char *const *args, buf_t *out) {
    buf_clear(out);
    char *argv[32] = {(char *)"git", (char *)"-C", (char *)cwd};
    size_t n = 0;
    while (args[n] && n + 4 < sizeof argv / sizeof *argv) {
        argv[n + 3] = (char *)args[n];
        n++;
    }
    if (args[n]) return -1;
    size_t count = 0;
    while (environ[count]) count++;
    char **env = calloc(count + 2, sizeof *env);
    if (!env) return -1;
    n = 0;
    for (size_t i = 0; i < count; i++) {
        /* Git commands here are local. Ambient repository/config overrides
         * must not redirect a merge or removal into another checkout. */
        if (!str_starts(environ[i], "GIT_")) env[n++] = environ[i];
    }
    env[n] = (char *)"GIT_TERMINAL_PROMPT=0";
    int pipes[2];
    if (pipe(pipes) < 0) {
        free(env);
        return -1;
    }
    /* Keep the pipe away from stdio even when the caller closed stdin. */
    int e = 0;
    for (size_t i = 0; i < 2; i++) {
        if (pipes[i] < 3) {
            int fd = fcntl(pipes[i], F_DUPFD_CLOEXEC, 3);
            if (fd < 0) e = errno;
            else {
                close(pipes[i]);
                pipes[i] = fd;
            }
        }
        if (fcntl(pipes[i], F_SETFD, FD_CLOEXEC) < 0) e = errno;
    }
    posix_spawn_file_actions_t actions;
    bool actions_ready = posix_spawn_file_actions_init(&actions) == 0;
    if (!actions_ready) e = ENOMEM;
    if (!e) e = posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    if (!e) e = posix_spawn_file_actions_adddup2(&actions, pipes[1], 1);
    if (!e) e = posix_spawn_file_actions_adddup2(&actions, pipes[1], 2);
    posix_spawnattr_t attr;
    bool attr_ready = posix_spawnattr_init(&attr) == 0;
    if (!attr_ready) e = ENOMEM;
    if (!e) e = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    if (!e) e = posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid = -1;
    if (!e) e = posix_spawnp(&pid, "git", &actions, &attr, argv, env);
    if (actions_ready) posix_spawn_file_actions_destroy(&actions);
    if (attr_ready) posix_spawnattr_destroy(&attr);
    free(env);
    close(pipes[1]);
    if (e) {
        close(pipes[0]);
        buf_appendf(out, "cannot start git: %s", strerror(e));
        return -1;
    }
    int status = 0, rc = -1;
    bool eof = false, reaped = false;
    int64_t deadline = monotonic_ms() + 120000;
    while (!eof || !reaped) {
        if (!reaped) {
            pid_t got = waitpid(pid, &status, WNOHANG);
            if (got == pid) reaped = true;
            else if (got < 0 && errno != EINTR) break;
        }
        if (monotonic_ms() >= deadline) break;
        struct pollfd fd = {pipes[0], POLLIN, 0};
        int pr = tny_poll(eof ? NULL : &fd, eof ? 0 : 1, 20);
        if (pr < 0 && errno != EINTR) break;
        if (pr > 0) {
            char chunk[4096];
            ssize_t nr = read(pipes[0], chunk, sizeof chunk);
            if (nr == 0) eof = true;
            else if (nr > 0 && out->len < 16384) {
                size_t take = (size_t)nr;
                if (take > 16384 - out->len) take = 16384 - out->len;
                buf_append(out, chunk, take);
            } else if (nr < 0 && errno != EINTR) break;
        }
    }
    if (eof && reaped && !out->oom) rc = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    else {
        kill(-pid, SIGKILL);
        if (!reaped) {
            while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        }
        buf_appends(out, "\ngit did not complete; worktree kept");
    }
    close(pipes[0]);
    /* Preserve leading whitespace (notably porcelain status and filenames). */
    while (out->len && out->data[out->len - 1] == '\n') out->data[--out->len] = 0;
    return rc;
}
#endif
