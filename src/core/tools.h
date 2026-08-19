/* tools.h — built-in tool registry for the native loop and `tny acp`.
 * Names follow fx (docs/features/mcp-and-skills.md). */
#ifndef TNY_TOOLS_H
#define TNY_TOOLS_H

#include "core/config.h"
#include "core/session.h"
#include "core/perm.h"
#include "core/events.h"
#include "core/backend.h"

struct mcp_client; /* mcp/mcp.h */

typedef struct tools_env {
    tny_ctx     *ctx;
    tny_session *session;
    perm_engine *perm;
    /* Interactive approval hook (TUI). NULL means PROMPT cannot be resolved:
     * the call is denied and the run stops per docs/cli.md. */
    tny_perm_decision (*prompt)(const char *tool, const char *summary, void *ud);
    void *prompt_ud;
    /* Event sink for TOOL_START / TOOL_END / STATUS. */
    tny_event_cb ev_cb;
    void *ev_ud;
    /* Lazily started MCP client (owned by caller; may be NULL). */
    struct mcp_client *mcp;
    /* set true when a PROMPT could not be resolved (ask-mode CLI) */
    bool perm_blocked;
} tools_env;

/* OpenAI "tools" array JSON for every built-in (+ selected MCP tools).
 * malloc'd. */
char *tools_schema_json(tools_env *env);

/* Execute one call. Returns a malloc'd string for the role:"tool" message
 * (bounded; large output is stored as a session result handle). Never NULL. */
char *tools_execute(tools_env *env, const char *name, const char *args_json);

/* Undo the last mutating file tool (session-scoped). Returns malloc'd
 * status line. */
char *tools_undo_last(tools_env *env);

/* Individual tool groups (internal wiring) */
char *tool_fs_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled);
char *tool_shell_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled);
char *tool_web_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled);
char *tool_ext_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled);

/* Shared helpers for tool impls */
char *tool_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Resolve a workspace-relative or absolute path; NULL + err message if the
 * path escapes allowed roots without approval. */
char *tool_resolve_path(tools_env *env, const char *path, char **err_out);
/* Bound a result: if len > ctx->max_tool_result_bytes, store blob and return
 * preview + handle notice; else return a copy. */
char *tool_bound_result(tools_env *env, const char *data, size_t len);

#endif
