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
struct tny_tool_registration;
struct tny_tool_call;

typedef struct tools_env {
    tny_ctx *ctx;
    tny_session_state *session;
    perm_engine *perm;
    /* Interactive approval hook (TUI). NULL means PROMPT cannot be resolved:
     * the call is denied and the run stops per docs/cli.md. */
    tny_perm_decision (*prompt)(const char *tool, const char *summary, void *ud);
    void *prompt_ud;
    /* Event sink for TOOL_START / TOOL_END / STATUS. */
    tny_backend_event_cb ev_cb;
    void *ev_ud;
    /* Lazily started MCP client (owned by caller; may be NULL). */
    struct mcp_client *mcp;
    /* set true when a PROMPT could not be resolved (ask-mode CLI) */
    bool perm_blocked;
    /* Paths queued by read_image. Flushed as a user-role image_url
     * message after the role:tool results (docs/adr/0008). */
    char *pending_images[9];
    int n_pending_images;
} tools_env;

/* One parsed tool invocation. Preparing performs canonicalization and the
 * permission lookup once; the parsed args remain valid while a native call is
 * parked for an asynchronous permission response. */
typedef struct {
    char *name;
    char *permission_tool;
    yyjson_doc *doc;
    yyjson_val *args;
    char *detail;
    char *detail2;
    char *summary; /* allocated only for PERM_PROMPT */
    char *error;   /* validation error when prepare returns -1 */
    perm_verdict verdict;
    struct tny_tool_registration *custom;
    struct tny_tool_call *custom_call;
} tools_call;

/* OpenAI "tools" array JSON for every built-in. MCP tools are never
 * promoted here; they ride the system-prompt catalog (docs/adr/0049).
 * malloc'd. */
char *tools_schema_json(tools_env *env);

/* Execute one call. Returns a malloc'd string for the role:"tool" message
 * (bounded; large output is stored as a session result handle). Never NULL. */
char *tools_execute(tools_env *env, const char *name, const char *args_json);

int tools_call_prepare(tools_env *env, const char *name, const char *args_json, tools_call *call);
void tools_call_grant(tools_env *env, const tools_call *call);
char *tools_call_execute(tools_env *env, tools_call *call);
bool tools_call_pending(const tools_call *call);
int tools_call_take_async(tools_call *call, char **result, bool *is_error);
void tools_call_invalidate_async(tools_call *call);
void tools_call_free(tools_call *call);

/* Undo the last mutating file tool (session-scoped). Returns malloc'd
 * status line. */
char *tools_undo_last(tools_env *env);

/* Individual tool groups (internal wiring) */
char *tool_fs_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled);
char *tool_shell_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled);
char *tool_web_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled);
char *tool_ext_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled);
/* Remote variants of the workspace tools when ctx->ssh_host is set
 * (tools_ssh.c, docs/adr/0022); *handled=false when tools stay local. */
char *tool_ssh_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled);

/* Shared helpers for tool impls */
char *tool_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Resolve a workspace-relative or absolute path; NULL + err message if the
 * path escapes allowed roots without approval. */
char *tool_resolve_path(tools_env *env, const char *path, char **err_out);
/* Bound a result: if len > ctx->max_tool_result_bytes, store blob and return
 * preview + handle notice; else return a copy. */
char *tool_bound_result(tools_env *env, const char *data, size_t len);
/* Inject queued read_image files as one user message. 0 ok, -1 on error. */
int tools_flush_images(tools_env *env, char *err, size_t errlen);

#endif
