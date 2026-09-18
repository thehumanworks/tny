/* subagent.h — the native `subagent` tool: durable child `tny ask`
 * sessions (docs/features/mcp-and-skills.md#subagents, docs/adr/0087).
 * Every failure is one `error: SUBAGENT_<CODE>: <guidance>` line. */
#ifndef TNY_SUBAGENT_H
#define TNY_SUBAGENT_H

#include "core/tools.h"

/* Environment variables the child reads through --api-key-env and
 * --base-url-env. They exist only in the child's private environment. */
#define TNY_SUBAGENT_KEY_ENV "TNY_SUBAGENT_API_KEY"
#define TNY_SUBAGENT_URL_ENV "TNY_SUBAGENT_BASE_URL"

/* Runtime-context, argument and ephemeral-action checks that run before the
 * permission gate and any extension event: NULL when the call may proceed,
 * else a malloc'd SUBAGENT_UNSUPPORTED_CONTEXT / INVALID_ARGUMENT /
 * UNSUPPORTED_ACTION diagnostic that never echoes a supplied value. */
char *tny_subagent_prepare_error(const tools_env *env, yyjson_val *args);

/* Execute create|message|inspect|lifecycle. malloc'd tool result. */
char *tny_subagent_execute(tools_env *env, yyjson_val *args);

/* Private C facade. Zero-initialize; do not copy an owning plan. argv/envp
 * are borrowed views into one C++ owner, valid until free or successful
 * replacement. All strings are snapshots, including inherited environment
 * and selectors. No caller/context/environment storage is retained.
 * Credentials stay in envp, never argv; all snapshot bytes are wiped on free. */
typedef struct {
    char **argv;
    char **envp;
    struct tny_subagent_plan_owner *owner;
} tny_subagent_plan;

#ifdef __cplusplus
extern "C" {
#endif
/* 0 ok; -1 when this executable's path or memory is unavailable (errno
 * ENOTSUP when this build cannot start processes at all). Failure leaves
 * the previous plan unchanged; success replaces it. No process is started.
 * Build requires stable inputs/environment for the duration of this call. */
int tny_subagent_plan_build(const tools_env *env, const char *resume_id, tny_subagent_plan *plan);
/* args is NULL or a validated tool-argument object with optional nonempty
 * provider/model/effort strings. Omitted selectors preserve parent inheritance.
 * An exact same-provider selector retains resolved parent configuration; a
 * different selector delegates configuration/auth to the normal child CLI,
 * without parent resolved credentials, URL, wire, model or effort. Explicit
 * model/effort independently override either path (including effort "default").
 * Ambient user environment remains available; private carriers are replaced.
 * Ownership, failure atomicity and process availability match build above. */
int tny_subagent_plan_build_selected(const tools_env *env, const char *resume_id, yyjson_val *args,
                                     tny_subagent_plan *plan);
/* Idempotent, allocation-free; clears the handle and both borrowed views. */
void tny_subagent_plan_free(tny_subagent_plan *plan);
#ifdef __cplusplus
}
#endif

/* Run one child to completion (prompt on stdin, bounded stdout, parent
 * cancellation owns the process tree) and classify its outcome for
 * `action` ("create" or "message"; resume_id for message). Exposed for unit
 * tests, which drive it with real non-tny children. */
char *tny_subagent_run(tools_env *env, const char *action, const char *resume_id,
                       char *const argv[], char *const envp[], const char *prompt);

#endif
