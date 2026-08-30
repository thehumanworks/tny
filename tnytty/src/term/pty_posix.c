/* pty_posix.c — the POSIX pty implementation (macOS, Linux). */
#include "term/pty.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

int tt_pty_spawn_at(tt_pty *p, char *const argv[], int cols, int rows, const char *cwd) {
    p->master = -1;
    p->pid = -1;
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) return -1;
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        close(master);
        return -1;
    }
    const char *slave_name = ptsname(master);
    if (!slave_name) {
        close(master);
        return -1;
    }
    struct winsize ws = {0};
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;

    /* The parent must not return before the child has opened the slave:
     * on BSD ptys (macOS) a master write before that is dropped with
     * EAGAIN, so an immediate create-then-input would lose bytes. */
    int sync_pipe[2];
    if (pipe(sync_pipe) != 0) {
        close(master);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(master);
        close(sync_pipe[0]);
        close(sync_pipe[1]);
        return -1;
    }
    if (pid == 0) {
        close(sync_pipe[0]);
        setsid();
        int slave = open(slave_name, O_RDWR);
        if (slave < 0) _exit(127);
#ifdef TIOCSCTTY
        ioctl(slave, TIOCSCTTY, 0);
#endif
        ioctl(slave, TIOCSWINSZ, &ws);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) close(slave);
        close(master);
        ssize_t wr = write(sync_pipe[1], "r", 1);
        (void)wr;
        close(sync_pipe[1]);
        setenv("TERM", "xterm-256color", 1);
        signal(SIGPIPE, SIG_DFL);
        if (cwd && cwd[0] == '/' && chdir(cwd) != 0) {
            fprintf(stderr, "tnytty: chdir %s: %s\r\n", cwd, strerror(errno));
            _exit(127);
        }
        execvp(argv[0], argv);
        fprintf(stderr, "tnytty: exec %s: %s\r\n", argv[0], strerror(errno));
        _exit(127);
    }
    close(sync_pipe[1]);
    char ready;
    ssize_t rr;
    do { rr = read(sync_pipe[0], &ready, 1); } while (rr < 0 && errno == EINTR);
    close(sync_pipe[0]); /* EOF (child died pre-open) is fine: reap sees it */
    if (set_nonblock(master) != 0) {
        int saved = errno;
        close(master);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        errno = saved;
        return -1;
    }
    p->master = master;
    p->pid = pid;
    return 0;
}

int tt_pty_spawn(tt_pty *p, char *const argv[], int cols, int rows) {
    return tt_pty_spawn_at(p, argv, cols, rows, NULL);
}

int tt_pty_resize(tt_pty *p, int cols, int rows) {
    if (p->master < 0) return -1;
    struct winsize ws = {0};
    ws.ws_col = (unsigned short)cols;
    ws.ws_row = (unsigned short)rows;
    if (ioctl(p->master, TIOCSWINSZ, &ws) != 0) return -1;
    if (p->pid > 0) kill(p->pid, SIGWINCH);
    return 0;
}

void tt_pty_kill(tt_pty *p) {
    if (p->pid > 0) kill(-p->pid, SIGHUP);
}

void tt_pty_force_kill(tt_pty *p) {
    if (p->pid > 0) kill(-p->pid, SIGKILL);
}

int tt_pty_reap(tt_pty *p, int *exit_code, bool block) {
    if (p->pid <= 0) return -1;
    int status = 0;
    pid_t r;
    do { r = waitpid(p->pid, &status, block ? 0 : WNOHANG); } while (r < 0 && errno == EINTR);
    if (r == 0) return 0;
    if (r < 0) return errno == ECHILD ? -1 : 0;
    p->pid = -1;
    if (exit_code) {
        if (WIFEXITED(status)) *exit_code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) *exit_code = 128 + WTERMSIG(status);
        else *exit_code = 1;
    }
    return 1;
}

void tt_pty_close(tt_pty *p) {
    if (p->master >= 0) close(p->master);
    p->master = -1;
}
