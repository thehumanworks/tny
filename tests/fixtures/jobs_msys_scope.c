#define _GNU_SOURCE
#include "util/process.h"
#include "util/util.h"
#include "util/tny_poll.h"
#include <windows.h>
#include <sys/wait.h>
#include <spawn.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
extern char **environ;
static char *self;
void tny_scope_test_pause(int point) {
    const char *value = getenv("TNY_SCOPE_TEST_BOUNDARY");
    if (value && atoi(value) == point) {
        dprintf(1, "bootstrap %ld %lu\n", (long)getpid(), (unsigned long)GetCurrentProcessId());
        raise(SIGSTOP);
    }
}
static void report(const char *tag) {
    dprintf(3, "%s %ld %lu\n", tag, (long)getpid(), (unsigned long)GetCurrentProcessId());
}
static int pipes(int f[2]) {
    if (pipe(f)) return -1;
    fcntl(f[0], F_SETFD, FD_CLOEXEC);
    fcntl(f[1], F_SETFD, FD_CLOEXEC);
    return 0;
}
static int spawn(char *a, char *b, char *c, int input, int output, pid_t *pid) {
    int in = fcntl(input, F_DUPFD_CLOEXEC, 10), out = fcntl(output, F_DUPFD_CLOEXEC, 10);
    if (in < 0 || out < 0) return 90;
    char *argv[] = {self, a, b, c, NULL};
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t attr;
    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&attr);
    posix_spawn_file_actions_adddup2(&fa, in, 0);
    posix_spawn_file_actions_adddup2(&fa, out, 3);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    sigset_t defaults, empty;
    sigemptyset(&empty);
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGCHLD);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setsigmask(&attr, &empty);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
    int e = posix_spawn(pid, self, &fa, &attr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(in);
    close(out);
    return e;
}
static int absence(pid_t pid) {
    for (int i = 0; i < 500; i++) {
        if (kill(pid, 0) < 0 && errno == ESRCH) return 1;
        usleep(10000);
    }
    return 0;
}
static int stopped(pid_t pid) {
    int st = 0;
    for (int i = 0; i < 500; i++) {
        pid_t p = waitpid(pid, &st, WNOHANG | WUNTRACED);
        if (p == pid) return WIFSTOPPED(st);
        if (p < 0) return 0;
        usleep(10000);
    }
    return 0;
}
int main(int argc, char **argv) {
    self = argv[0];
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGCHLD, SIG_DFL);
    if (argc > 1 && !strcmp(argv[1], "sentinel")) {
        report("sentinel");
        char c;
        return read(0, &c, 1) == 1 && c == 'Q' ? 0 : 40;
    }
    if (argc > 1 && !strcmp(argv[1], "bootstrap-exec")) {
        execl(self, self, "bootstrap", argv[2], argv[3], NULL);
        return 19;
    }
    if (argc > 1 && !strcmp(argv[1], "bootstrap")) {
        int admitted = tny_process_scope_admit();
        if (admitted) return admitted;
        report("WORK_FORBIDDEN");
        return 24;
    }
    if (argc > 1 && !strcmp(argv[1], "supervisor")) {
        if (tny_process_supervisor_init()) return 30;
        report("supervisor");
        int bi[2], bo[2], si[2];
        if (pipes(bi) || pipes(bo) || pipes(si)) return 32;
        setenv("TNY_SCOPE_TEST_BOUNDARY", argv[2], 1);
        char *absolute = tny_process_self_path();
        char *child_argv[] = {absolute, "bootstrap-exec", "unused", argv[2], NULL};
        tny_process_scope *scope = NULL;
        if (tny_process_scope_spawn(child_argv, environ, bi[0], bo[1], &scope)) return 33;
        free(absolute);
        unsetenv("TNY_SCOPE_TEST_BOUNDARY");
        pid_t boot = tny_process_scope_pid(scope), sentinel = -1;
        if (spawn("sentinel", NULL, NULL, si[0], 3, &sentinel)) return 34;
        close(bi[0]);
        close(bo[1]);
        close(si[0]);
        FILE *boundary_report = fdopen(bo[0], "r");
        char boundary_line[128];
        if (!fgets(boundary_line, sizeof boundary_line, boundary_report)) return 39;
        dprintf(3, "%s", boundary_line);
        if (!stopped(boot)) return 35;
        report("stopped_confirmed");
        char command;
        if (read(0, &command, 1) != 1) return 36;
        if (command != 'C') return 37;
        int term = tny_process_scope_terminate(scope) == 0;
        int killed = tny_process_scope_go(scope, bi[1]) == -1;
        int status = 0, reaped = 0, empty = 0;
        for (int i = 0; i < 500; i++) {
            reaped = tny_process_scope_reap(scope, &status);
            empty = tny_process_scope_empty(scope);
            if (reaped == 1 && empty == 1) break;
            tny_poll(NULL, 0, 10);
        }
        int gone = absence(boot), live = kill(sentinel, 0) == 0;
        dprintf(3, "cancel_result %d %d %d %d %d %lu %d\n", term, killed, reaped == 1, gone, live,
                (unsigned long)(empty == 1 ? 0 : 1), empty >= 0);
        int destroyed = tny_process_scope_destroy(scope) == 0;
        fclose(boundary_report);
        close(bi[1]);
        write(si[1], "Q", 1);
        close(si[1]);
        waitpid(sentinel, &status, 0);
        /* outer is deliberately never explicitly closed while supervisor runs. */
        return term && killed && reaped == 1 && gone && live && empty == 1 && destroyed ? 0 : 38;
    }
    if (argc != 3) return 1;
    int individual = !strcmp(argv[1], "individual");
    int extin[2], extout[2];
    if (pipes(extin) || pipes(extout)) return 8;
    pid_t external = -1;
    if (spawn("sentinel", NULL, NULL, extin[0], extout[1], &external)) return 9;
    close(extin[0]);
    close(extout[1]);
    FILE *external_report = fdopen(extout[0], "r");
    char external_line[128];
    if (!fgets(external_line, sizeof external_line, external_report)) return 10;
    printf("external_%s", external_line);
    int input[2], output[2];
    if (pipes(input) || pipes(output)) return 2;
    pid_t supervisor = -1;
    if (spawn("supervisor", argv[2], NULL, input[0], output[1], &supervisor)) return 3;
    close(input[0]);
    close(output[1]);
    FILE *f = fdopen(output[0], "r");
    pid_t pids[3] = {supervisor, -1, -1};
    HANDLE handles[3] = {NULL, NULL, NULL};
    for (int i = 0; i < 4; i++) {
        char line[256], tag[64];
        long pid;
        unsigned long winpid;
        if (!fgets(line, sizeof line, f) || sscanf(line, "%63s %ld %lu", tag, &pid, &winpid) != 3)
            return 4;
        printf("observed %s", line);
        if (!strcmp(tag, "stopped_confirmed")) continue;
        int index = !strcmp(tag, "supervisor") ? 0 : !strcmp(tag, "bootstrap") ? 1 : 2;
        pids[index] = (pid_t)pid;
        handles[index] =
            OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE,
                        (DWORD)winpid);
        if (!handles[index]) return 5;
    }
    int pass = 1, status = 0;
    if (individual) {
        write(input[1], "C", 1);
        char line[256];
        int term, killed, reaped, gone, live, query;
        unsigned long active;
        if (!fgets(line, sizeof line, f) ||
            sscanf(line, "cancel_result %d %d %d %d %d %lu %d", &term, &killed, &reaped, &gone,
                   &live, &active, &query) != 7)
            return 6;
        printf("%s", line);
        pass = term && killed && reaped && gone && live && !active && query;
    } else {
        int rc = kill(supervisor, SIGKILL);
        printf("actual_supervisor_SIGKILL=%d\n", rc);
        if (rc) pass = 0;
    }
    for (int i = 0; i < 3; i++) {
        DWORD w = WaitForSingleObject(handles[i], 5000);
        printf("handle_wait[%d]=%lu\n", i, (unsigned long)w);
        if (w != WAIT_OBJECT_0) {
            pass = 0;
            TerminateProcess(handles[i], 99);
            WaitForSingleObject(handles[i], 5000);
        }
        CloseHandle(handles[i]);
    }
    pid_t reaped = waitpid(supervisor, &status, 0);
    printf("supervisor_reaped=%d status=%d\n", reaped == supervisor, status);
    if (reaped != supervisor) pass = 0;
    for (int i = 0; i < 3; i++) {
        int gone = absence(pids[i]);
        printf("strict_absence[%d]=%d\n", i, gone);
        if (!gone) pass = 0;
    }
    int external_live = kill(external, 0) == 0;
    printf("external_sentinel_live=%d\n", external_live);
    if (!external_live) pass = 0;
    write(extin[1], "Q", 1);
    close(extin[1]);
    waitpid(external, &status, 0);
    fclose(external_report);
    close(input[1]);
    fclose(f);
    printf("RESULT %s mode=%s boundary=%s\n", pass ? "PASS" : "FAIL", argv[1], argv[2]);
    return pass ? 0 : 7;
}
