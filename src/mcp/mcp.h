/* mcp.h — MCP client (stdio JSONL v1; trusted profile ~/.tny/mcp.json only).
 * Lazy: nothing is spawned until the model uses an mcp_* tool. */
#ifndef TNY_MCP_H
#define TNY_MCP_H

#include "core/tools.h"

/* All return malloc'd strings for the tool message. */
char *mcp_features(tools_env *env);
char *mcp_search_tools(tools_env *env, const char *query);
char *mcp_call_tool(tools_env *env, const char *server, const char *tool, const char *args_json);

/* JSON-RPC frame builders (append one object, without the trailing newline). */
void mcp_fmt_request(buf_t *b, int id, const char *method, const char *params_json);
void mcp_fmt_notify(buf_t *b, const char *method);

/* Override the request timeout for deterministic fixture tests. Non-positive
 * values restore the production default. */
void mcp_set_timeout_ms(int timeout_ms);

/* Kill spawned MCP servers (process teardown). */
void mcp_shutdown_all(void);

#endif
