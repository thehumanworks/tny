/* instructions.h — AGENTS.md chain loader (docs/features/mcp-and-skills.md). */
#ifndef TNY_INSTRUCTIONS_H
#define TNY_INSTRUCTIONS_H

#include "core/config.h"
#include "util/util.h"

/* Append the project-instruction chain to out: ~/.tny/AGENTS.md, launch
 * ancestors (root→cwd order so narrower paths come later and win), and the
 * primary workspace. CLAUDE.md is the alias when AGENTS.md is absent.
 * No-op when ctx->context_enabled is false. */
void instructions_collect(tny_ctx *ctx, buf_t *out);

#endif
