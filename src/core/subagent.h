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

/* One child launch: argv holds only non-secret selectors (argv[0] is this
 * executable, owned); envp is the inherited environment plus the private
 * carriers and ceilings. Exposed for unit tests. */
typedef struct {
    char *argv[32];
    char **envp;
    char *owned[8]; /* "NAME=value" entries added for this child; wiped on free */
    int n_owned;
} tny_subagent_plan;

/* 0 ok; -1 when this executable's path or memory is unavailable (errno
 * ENOTSUP when this build cannot start processes at all). */
int tny_subagent_plan_build(const tools_env *env, const char *resume_id, tny_subagent_plan *plan);
void tny_subagent_plan_free(tny_subagent_plan *plan);

/* Run one child to completion (prompt on stdin, bounded stdout, parent
 * cancellation owns the process tree) and classify its outcome for
 * `action` ("create" or "message"; resume_id for message). Exposed for unit
 * tests, which drive it with real non-tny children. */
char *tny_subagent_run(tools_env *env, const char *action, const char *resume_id,
                       char *const argv[], char *const envp[], const char *prompt);

#endif
