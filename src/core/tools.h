/* tools.h — built-in tool registry for the native loop and `tny acp`.
 * Names follow fx (docs/features/mcp-and-skills.md). */
#ifndef TNY_TOOLS_H
#define TNY_TOOLS_H

#include "core/config.h"
#include "core/edit.h"
#include "core/session.h"
#include "core/perm.h"
#include "core/events.h"
#include "core/backend.h"

struct mcp_client;    /* mcp/mcp.h */
struct tny_intercept; /* core/intercept.h */
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
    /* Free-text frontend question hook. NULL (or a NULL answer) preserves
     * the non-interactive fallback used by one-shot/embedded callers. */
    char *(*ask_user)(const char *question, void *ud);
    void *ask_user_ud;
    /* A terminal child can block on the runner control socket while the
     * backend is inside this tool call. Pumping this callback may service
     * only socket control traffic; it must never dispatch the backend. */
    int (*control_pump)(void *ud, int timeout_ms);
    void *control_pump_ud;
    const char *session_sock;
    const char *session_id;
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
    /* Set when a `terminal` command was recognised as a first-party tny verb
     * and runs in-process instead (docs/adr/0063). */
    struct tny_intercept *intercept;
} tools_call;

/* Human label of an intercepted call ("tny edit docs/x.md"), or NULL for an
 * ordinary call. Backends put it in the TOOL_START detail so extensions and
 * ACP clients see the verb rather than an opaque shell blob. */
const char *tools_call_label(const tools_call *call);

/* OpenAI "tools" array JSON for every built-in. MCP tools are never
 * promoted here; they ride the system-prompt catalog (docs/adr/0049).
 * malloc'd. */
char *tools_schema_json(tools_env *env);

/* Execute one call. Returns a malloc'd string for the role:"tool" message.
 * `all` bounds large output behind a session handle; shell profiles give
 * terminal a small preview plus a complete spill-file path (ADR 0062).
 * Never NULL. */
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

/* Snapshot `abs` into the session's one-deep undo slot before a mutating
 * file operation overwrites it. */
void tools_undo_record(tools_env *env, const char *abs);

/* The path a permission rule sees for a path-shaped argument: absolute
 * locally, ssh_cwd-relative under --ssh. malloc'd, NULL on failure. */
char *tools_path_detail(tools_env *env, const char *path);

/* Shell command for one subagent turn (`tny ask` child). The parent's
 * resolved provider travels with the child (docs/features/mcp-and-skills.md)
 * and every interpolated value — including the model-supplied id and prompt —
 * is shell-quoted. stderr_path, when non-NULL, becomes a `2>` redirect so
 * startup failures stay diagnosable. malloc'd. Exposed for unit tests. */
char *tools_subagent_command(tools_env *env, const char *id, const char *prompt,
                             const char *stderr_path);

/* Individual tool groups (internal wiring) */
char *tool_fs_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled);
char *tool_shell_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled);
char *tool_web_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled);
/* true when settings name a web_search provider ("web_search_command" or
 * "web_search_url"); the schema omits web_search otherwise (docs/adr/0055). */
bool tool_web_search_configured(tny_ctx *ctx);
/* Expand every {query} / {{query}} in tmpl with the percent-encoded query.
 * malloc'd. */
char *tool_web_search_expand(const char *tmpl, const char *query);
char *tool_ext_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled);
/* Remote variants of the workspace tools when ctx->ssh_host is set
 * (tools_ssh.c, docs/adr/0022); *handled=false when tools stay local. */
char *tool_ssh_execute(tools_env *env, const char *name, yyjson_val *args, bool *handled);
/* Exact-match edit of a file on the --ssh host for the intercepted
 * `tny edit` (docs/adr/0063): cat it, apply tny_edit_file_exact semantics to
 * a local staging copy, then send the result back over stdin and rename it
 * into place. A transport failure returns a malloc'd message in *err_out. */
tny_edit_status tool_ssh_edit_exact(tools_env *env, const char *path, const char *old_text,
                                    const char *new_text, bool replace_all, tny_edit_result *result,
                                    char **err_out);

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
/* Validate a local png/jpeg/gif/webp and queue it through the same
 * next-request path as read_image. allowed_roots_only is true for the socket
 * operation; read_image already passed its permission gate. resolved_out
 * borrows the queued path; mime_out is static. */
int tools_queue_image(tools_env *env, const char *path, bool allowed_roots_only,
                      const char **resolved_out, const char **mime_out, size_t *len_out, char *err,
                      size_t errlen);

#endif
