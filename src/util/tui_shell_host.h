#ifndef TNY_TUI_SHELL_HOST_H
#define TNY_TUI_SHELL_HOST_H
#include <sys/types.h>
/* Native host-process seam; browser builds return a clean unsupported error. */
int tui_shell_host_start(const char *command, pid_t *pid);
int tui_shell_host_finish(pid_t pid);
int tui_shell_host_poll(pid_t pid); /* -2 while still running */
void tui_shell_host_stop(pid_t pid);
#endif
