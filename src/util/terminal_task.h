/* Detached terminal commands: one waiter owns one child, never a stored PID. */
#ifndef TNY_TERMINAL_TASK_H
#define TNY_TERMINAL_TASK_H

#include <stdbool.h>

/* Values are persisted in status.json; append new states, never reorder. */
// C/C++ boundary: retain the C enum layout. NOLINTNEXTLINE(performance-enum-size)
typedef enum {
    TNY_TERMINAL_STARTING,
    TNY_TERMINAL_RUNNING,
    TNY_TERMINAL_COMPLETED,
    TNY_TERMINAL_FAILED,
    TNY_TERMINAL_SIGNALLED,
    TNY_TERMINAL_LAUNCH_FAILED,
    TNY_TERMINAL_UNKNOWN
} tny_terminal_state;

typedef struct {
    char id[17];
    char *dir;
    tny_terminal_state state;
    int exit_code; /* -1 unless waitpid observed a normal exit */
    int signal;
    int error;
} tny_terminal_task;

#ifdef __cplusplus
extern "C" {
#endif

bool tny_terminal_supported(void);
bool tny_terminal_valid_id(const char *id);
/* Register before launch. The detached waiter survives caller loss. setup runs
 * only in the command child, before exec. Returns errno; a registered task is
 * still inspectable on failure. No PID is exposed as control authority. */
int tny_terminal_start(const char *root, const char *cwd, char *const argv[], void (*setup)(void *),
                       void *ud, tny_terminal_task *task);
/* Only a validated opaque ID under root is accepted. A live owner lock proves
 * running; an abandoned or unreadable nonterminal record becomes unknown. */
int tny_terminal_inspect(const char *root, const char *id, tny_terminal_task *task);
bool tny_terminal_finished(const tny_terminal_task *task);
const char *tny_terminal_state_name(tny_terminal_state state);
void tny_terminal_task_free(tny_terminal_task *task);

#ifdef __cplusplus
}
#endif
#endif
