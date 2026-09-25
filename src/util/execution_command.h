/* Fresh-exec shell guardian, used only by the execution server. */
#ifndef TNY_EXECUTION_COMMAND_H
#define TNY_EXECUTION_COMMAND_H
#include <stdbool.h>
#include <sys/types.h>
/* Spawn a guardian with a private lifeline. Exact argv and context cross fd3,
 * never process arguments. Caller closes *lifeline to request command cleanup,
 * and retains/reaps *pid. Output includes both stdout and stderr. */
int tny_exec_command_start(char *const argv[], const char *cwd, int timeout_ms,
                           const char *session_sock, const char *session_id,
                           const char *permission_mode, bool self_improve, int output_fd,
                           pid_t *pid, int *lifeline);
int tny_exec_command_main(void);
#endif
