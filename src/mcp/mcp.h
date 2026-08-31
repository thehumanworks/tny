/* mcp.h — MCP client (stdio JSONL v1; native + explicitly opted-in imports).
 * Servers warm in the background at native-session start (docs/adr/0049);
 * without threads, or for a server the warm-up missed, spawn stays lazy at
 * the first mcp_* tool call. */
#ifndef TNY_MCP_H
#define TNY_MCP_H

#include "core/tools.h"
#include "util/util.h"

/* Start every active merged-config server on detached threads: spawn, initialize,
 * cache tools/list. Non-blocking; call once per process at native session
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

/* Human or JSON listing of configured servers (source attributed). Does not
 * spawn. Notices from skipped/malformed imports are included. */
char *mcp_list_text(struct tny_ctx *ctx);
char *mcp_list_json(struct tny_ctx *ctx);

/* Kill spawned MCP servers (process teardown). */
void mcp_shutdown_all(void);

#endif
