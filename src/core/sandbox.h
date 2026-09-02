/* sandbox.h — OS command sandbox construction for the terminal child. */
#ifndef TNY_SANDBOX_H
#define TNY_SANDBOX_H

#include "core/config.h"

#include <stdbool.h>
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

/* Working wrapper for this host, or NONE. Presence on disk is not enough:
 * the wrapper is probed once per process around `sh -c 'exit 0'` (nested
 * Seatbelt and namespace-restricted bubblewrap both fail that probe), so
 * doctor never claims a wrapper that cannot launch and `auto` falls back
 * to none on such hosts instead of failing every terminal call. */
tny_sandbox_kind tny_sandbox_available(void);

/* Run wrapper around a trivial command and report whether it exited 0
 * within timeout_ms (3 s for the plain form). Exposed for tests and doctor. */
bool tny_sandbox_probe(tny_sandbox_kind kind, const char *wrapper);
bool tny_sandbox_probe_ms(tny_sandbox_kind kind, const char *wrapper, int timeout_ms);

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
