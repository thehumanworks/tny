/* mcp.h — MCP client (stdio JSONL v1; trusted profile ~/.tny/mcp.json only).
 * Lazy: nothing is spawned until the model uses an mcp_* tool. */
#ifndef TNY_MCP_H
#define TNY_MCP_H

#include "core/tools.h"

/* All return malloc'd strings for the tool message. */
char *mcp_features(tools_env *env);
char *mcp_search_tools(tools_env *env, const char *query);
char *mcp_call_tool(tools_env *env, const char *server, const char *tool,
                    const char *args_json);

/* Kill spawned MCP servers (process teardown). */
void mcp_shutdown_all(void);

#endif
