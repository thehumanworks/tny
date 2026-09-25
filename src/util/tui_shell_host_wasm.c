#include "util/tui_shell_host.h"
#include <errno.h>
int tui_shell_host_start(const char *command, pid_t *pid) {
    (void)command;
    (void)pid;
    errno = ENOTSUP;
    return -1;
}
int tui_shell_host_poll(pid_t pid) {
    (void)pid;
    return -1;
}
int tui_shell_host_finish(pid_t pid) {
    (void)pid;
    return -1;
}
void tui_shell_host_stop(pid_t pid) { (void)pid; }
