#include "util/tui_shell_host.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

int tui_shell_host_start(const char *command, pid_t *pid) {
    int p[2];
    if (pipe(p) != 0) return -1;
    const char *shell = getenv("SHELL");
    if (!shell || shell[0] != '/') shell = "/bin/sh";
    pid_t child = fork();
    if (child < 0) {
        close(p[0]);
        close(p[1]);
        return -1;
    }
    if (!child) {
        if (setpgid(0, 0) != 0) _exit(127);
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0 || dup2(p[1], STDOUT_FILENO) < 0 ||
            dup2(p[1], STDERR_FILENO) < 0)
            _exit(127);
        close(nullfd);
        close(p[0]);
        close(p[1]);
        execl(shell, shell, "-c", command, (char *)NULL);
        _exit(127);
    }
    close(p[1]);
    *pid = child;
    int flags = fcntl(p[0], F_GETFL);
    if (flags < 0 || fcntl(p[0], F_SETFL, flags | O_NONBLOCK) < 0) {
        tui_shell_host_stop(child);
        close(p[0]);
        return -1;
    }
    return p[0];
}

int tui_shell_host_finish(pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

int tui_shell_host_poll(pid_t pid) {
    int status = 0;
    pid_t done = waitpid(pid, &status, WNOHANG);
    if (done == 0 || (done < 0 && errno == EINTR)) return -2;
    if (done < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

void tui_shell_host_stop(pid_t pid) {
    if (pid <= 0) return;
    if (getpgid(pid) == pid) kill(-pid, SIGKILL);
    kill(pid, SIGKILL); /* setpgid may not yet have run */
    (void)tui_shell_host_finish(pid);
}
