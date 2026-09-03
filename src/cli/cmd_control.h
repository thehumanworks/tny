/* cmd_control.h — standalone session-control CLI verbs (ADR 0058). */
#ifndef TNY_CMD_CONTROL_H
#define TNY_CMD_CONTROL_H

#include <stdbool.h>

/* `json` carries the already-parsed leading global --json flag. Both
 * commands also accept --json after the subcommand. */
int cmd_ask_user(bool json, int argc, char **argv);
int cmd_image(bool json, int argc, char **argv);

#endif
