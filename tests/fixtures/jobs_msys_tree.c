#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
static void note(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd >= 0) {
        dprintf(fd, "%ld\n", (long)getpid());
        close(fd);
    }
}
static void bounded_wait(const char *gate) {
    time_t end = time(NULL) + 45;
    while (time(NULL) < end && (!gate || access(gate, F_OK) == 0)) usleep(10000);
}
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    if (!strcmp(argv[1], "sentinel")) {
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGHUP, SIG_DFL);
        char path[1024];
        snprintf(path, sizeof path, "%s.pid", argv[2]);
        note(path);
        bounded_wait(argv[2]);
        return 0;
    }
    if (argc > 3 && !strcmp(argv[3], "auto")) signal(SIGCHLD, SIG_IGN);
    note(argv[2]);
    pid_t child = fork();
    if (child < 0) return 3;
    if (!child) {
        if (setsid() < 0) return 4;
        note(argv[2]);
        pid_t leaf = fork();
        if (leaf < 0) return 5;
        if (!leaf) note(argv[2]);
    }
    bounded_wait(NULL);
    return 0;
}
