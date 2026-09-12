/* Disposable observer fixture, linked instead of main.o and image_transform.o.
 * No PID reuse is simulated and no uncertain signal authority is forwarded. */
#include "util/image_transform.h"
#include "util/process.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static bool reaped, stopped, exited_at_stop, invalid_authority;
static pid_t observed_pid;
static int64_t started;
static bool cancel_mode;

static pid_t observe_waitpid(pid_t pid, int *status, int flags) {
    pid_t got = waitpid(pid, status, flags);
    if (got == pid) reaped = true;
    return got;
}

static int observe_stop(pid_t pid) {
    siginfo_t info = {0};
    stopped = true;
    observed_pid = pid;
    if (reaped || waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT) < 0) {
        invalid_authority = true;
        return -1;
    }
    exited_at_stop = info.si_pid == pid;
    return tny_process_kill_tree(pid);
}

#undef TNY_IMAGE_TOOL_WALL_MS
#define TNY_IMAGE_TOOL_WALL_MS 500
#define waitpid                observe_waitpid
#define tny_process_kill_tree  observe_stop
#include "../../src/util/image_transform.c"
#undef waitpid
#undef tny_process_kill_tree

static bool cancel_after_exit(void *unused) {
    (void)unused;
    return cancel_mode && monotonic_ms() - started >= 200;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--converter") == 0) {
        pid_t holder = fork();
        if (holder < 0) return 3;
        if (!holder) {
            /* Detached stdout holder self-expires: no orphan PID signaling. */
            if (setsid() < 0) _exit(4);
            sleep(2);
            _exit(0);
        }
        return 0;
    }
    if (argc != 2) return 2;
    cancel_mode = strcmp(argv[1], "cancel") == 0;
    tny_image_tool tool = {0};
    char *self = realpath(argv[0], NULL);
    if (!self || !identity(self, &tool)) return 3;
    snprintf(tool.path, sizeof tool.path, "%s", self);
    free(self);
    char *child_argv[] = {tool.path, (char *)"--converter", NULL};
    char err[256] = "";
    buf_t out;
    buf_init(&out);
    started = monotonic_ms();
    int rc =
        tny_image_tool_run(&tool, child_argv, NULL, cancel_after_exit, NULL, &out, err, sizeof err);
    int64_t elapsed = monotonic_ms() - started;
    buf_free(&out);
    int status;
    bool fully_reaped =
        observed_pid > 0 && waitpid(observed_pid, &status, WNOHANG) == -1 && errno == ECHILD;
    printf("rc=%d stopped=%d exited_at_stop=%d reaped=%d invalid_authority=%d "
           "fully_reaped=%d elapsed_ms=%lld error=%s\n",
           rc, stopped, exited_at_stop, reaped, invalid_authority, fully_reaped, (long long)elapsed,
           err);
    return rc == (cancel_mode ? 130 : 1) && stopped && exited_at_stop && reaped &&
                   !invalid_authority && fully_reaped && elapsed < 1500 &&
                   strstr(err, cancel_mode ? "interrupted" : "time limit")
               ? 0
               : 1;
}
