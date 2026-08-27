/* ssh.h — remote tool runtime (docs/adr/0022).
 *
 * tny itself stays local (config, sessions, provider, TUI). When ctx->ssh_host
 * is set, every workspace tool of the native loop (files, grep, terminal, …)
 * executes on the remote host through one persistent OpenSSH ControlMaster
 * connection. The remote host needs only a POSIX sh + coreutils; no tny. */
#ifndef TNY_SSH_H
#define TNY_SSH_H

#include "util/util.h"
#include <stdbool.h>
#include <stddef.h>

struct tny_ctx;

/* Parse user@host[:port] / [v6]:port into ctx->ssh_host / ctx->ssh_port.
 * 0 ok; -1 with a message in err. Does not connect. */
int ssh_target_set(struct tny_ctx *ctx, const char *spec, char *err, size_t errlen);

/* Open (or reuse) the ControlMaster for ctx->ssh_host. Interactive: inherits
 * the terminal so OpenSSH can ask for passwords / host keys. Resolves
 * ctx->ssh_cwd to an absolute remote path. 0 ok, -1 with err. */
int ssh_connect(struct tny_ctx *ctx, char *err, size_t errlen);

/* Close the ControlMaster and clear the target. Safe when not connected. */
void ssh_disconnect(struct tny_ctx *ctx);

/* Run a POSIX sh script on the remote host (cwd = ctx->ssh_cwd), feeding
 * `in` (may be NULL) to its stdin and collecting stdout+stderr into `out`
 * (capped at out_cap bytes; *truncated set when exceeded). timeout_s <= 0
 * means no timeout. Returns the exit code (124 + *timed_out on timeout,
 * 255 when ssh itself failed). Never prompts: BatchMode. */
int ssh_run(struct tny_ctx *ctx, const char *script, const char *in, size_t inlen, int timeout_s,
            size_t out_cap, buf_t *out, bool *truncated, bool *timed_out);

/* Append s to b as one single-quoted POSIX shell word. */
void ssh_shell_quote(buf_t *b, const char *s);

#endif
