/* mcp.h — MCP client (stdio JSONL + Streamable HTTP; trusted profile
 * ~/.tny/mcp.json only).
 * Servers warm in the background at native-session start (docs/adr/0049,
 * docs/adr/0051); without threads, or for a server the warm-up missed,
 * connect stays lazy at the first mcp_* tool call. HTTP works on wasm. */
#ifndef TNY_MCP_H
#define TNY_MCP_H

#include "core/tools.h"
#include "util/util.h"

/* Start every ~/.tny/mcp.json server on detached threads: open its transport,
 * negotiate stateless v2 or legacy initialization, and cache tools/list.
 * Non-blocking; call once per process at native session
 * start (never for --help/--version or `tny acp` server mode — mcp_disabled
 * makes it a no-op). Failures stay silent until a call names the server. */
void mcp_warm_start(struct tny_ctx *ctx);

/* Append the cached tool catalog (namespaced server/tool + one-line
 * description, capped) to a system-prompt buffer. Never blocks and never
 * spawns; servers still warming are simply absent this turn. */
void mcp_catalog_collect(struct tny_ctx *ctx, buf_t *out);

/* All return malloc'd strings for the tool message. */
char *mcp_features(tools_env *env);
char *mcp_search_tools(tools_env *env, const char *query);
char *mcp_call_tool(tools_env *env, const char *server, const char *tool, const char *args_json);

/* Kill spawned MCP servers (process teardown). */
void mcp_shutdown_all(void);

#endif
