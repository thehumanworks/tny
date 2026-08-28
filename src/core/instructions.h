/* instructions.h — AGENTS.md chain loader (docs/features/mcp-and-skills.md). */
#ifndef TNY_INSTRUCTIONS_H
#define TNY_INSTRUCTIONS_H

#include "core/config.h"
#include "util/util.h"

/* Append the project-instruction chain to out: ~/.tny/AGENTS.md, then
 * either the launch-dir ancestor chain (local tools) or AGENTS.md from
 * the remote cwd when ctx->ssh_host is set (docs/adr/0040). CLAUDE.md is
 * the alias when AGENTS.md is absent. No-op when ctx->context_enabled is
 * false. */
void instructions_collect(tny_ctx *ctx, buf_t *out);
/* Refresh the one process snapshot used by both provider requests and
 * extension metadata events. */
int instructions_refresh(tny_ctx *ctx);

#endif
