/* sandbox.h — OS command sandbox construction for the terminal child. */
#ifndef TNY_SANDBOX_H
#define TNY_SANDBOX_H

#include "core/config.h"

#include <stddef.h>

typedef enum {
    TNY_SANDBOX_NONE = 0,
    TNY_SANDBOX_SEATBELT,
    TNY_SANDBOX_BWRAP,
} tny_sandbox_kind;

typedef struct {
    tny_sandbox_kind kind;
    char **argv;
    size_t argc;
} tny_sandbox_command;

/* Installed wrapper for this host, or NONE. Availability is deliberately a
 * PATH/executable check: doctor must not claim a wrapper that cannot be
 * launched, while namespace policy failures remain an execution-time error. */
tny_sandbox_kind tny_sandbox_available(void);

/* Effective terminal-child mode. yolo always returns NONE; auto selects the
 * available wrapper and explicit os returns NONE when unsupported. */
tny_sandbox_kind tny_sandbox_effective(const tny_ctx *ctx);
const char *tny_sandbox_kind_name(tny_sandbox_kind kind);
const char *tny_sandbox_kind_description(tny_sandbox_kind kind);

/* Pure argv/profile constructor. wrapper is an absolute test/exec path for
 * SEATBELT/BWRAP and is ignored for NONE. */
int tny_sandbox_command_build_kind(const tny_ctx *ctx, tny_sandbox_kind kind, const char *wrapper,
                                   const char *shell, const char *command, tny_sandbox_command *out,
                                   char *err, size_t errlen);

/* Resolve the effective mode and installed wrapper, then construct argv.
 * Explicit os on an unsupported host is a clean error; auto falls back. */
int tny_sandbox_command_build(const tny_ctx *ctx, const char *shell, const char *command,
                              tny_sandbox_command *out, char *err, size_t errlen);
void tny_sandbox_command_free(tny_sandbox_command *command);

/* Extract the absolute target from common seatbelt/bwrap write-denial output.
 * malloc'd, or NULL when the output is unrelated. */
char *tny_sandbox_denied_path(const char *output);

#endif
