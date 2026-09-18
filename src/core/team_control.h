/* Thin team control over jobs. No independent execution/recovery authority. */
#ifndef TNY_TEAM_CONTROL_H
#define TNY_TEAM_CONTROL_H
#include "core/jobs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TNY_TEAM_START,
    TNY_TEAM_STATUS,
    TNY_TEAM_COLLECT,
    TNY_TEAM_WAIT_ANY,
    TNY_TEAM_CANCEL,
    TNY_TEAM_VERIFY,
    TNY_TEAM_NONE
} tny_team_op;

/* Only runtime adapters construct this identity, NEVER from tool arguments or
 * ambient TNY_SESSION_ID. Local CLI operators have same-user jobs authority.
 * A session caller is the recorded parent or a current fenced member. Members
 * need all four fields. Parent callers need only session_id. */
typedef struct {
    bool local_operator;
    const char *session_id;
    const char *run_id;
    int task_index;
    int attempt;
} tny_team_caller;

#define TNY_TEAM_WAIT_MAX_MS 30000
#define TNY_TEAM_SESSION_MAX (4u * 1024u * 1024u)

tny_team_op tny_team_op_parse(const char *name);
const char *tny_team_permission_tool(tny_team_op op);
bool tny_team_op_is_sensitive(tny_team_op op);
/* Shared grammar: OP --request FILE|- [--json]. Bounded JSON only. Caller owns
 * *request_out; errors are static safe strings. Parser does not confer authority. */
tny_team_op tny_team_parse_argv(int argc, char **argv, const char *stdin_text, size_t stdin_len,
                                char **request_out, const char **error);
/* Validate and resolve permission detail without executing work. Returned detail
 * and *error are malloc'd. team_verify includes exact command/cwd/run/task/fence;
 * a team_status/read grant cannot authorize it. Verification is fail-closed until
 * a strict check-process cleanup seam is wired (ADR0141). */
char *tny_team_detail(tny_ctx *ctx, const tny_team_caller *caller, tny_team_op op, yyjson_val *args,
                      char **error);
/* Already-permitted request, revalidated with current membership. 0: submitted or
 * observation returned, NOT accepted work. 1: invalid/unauthorized/unsupported;
 * 2: failed collection/run or state error; 124: wait ended without an unseen
 * terminal item, never cancellation; 130: caller interrupted its wait only.
 * Wait-any is a bounded client fallback, not a reason to poll inside model turns.
 * Cancel passes expected_attempt to the jobs transaction; integration requires
 * core jobs to enforce that fence atomically, not merely our preflight check. */
int tny_team_run(tny_ctx *ctx, const tny_team_caller *caller, tny_team_op op, yyjson_val *args,
                 buf_t *out, char *err, size_t errlen, bool (*cancelled)(void *), void *cancel_ud);
#ifdef __cplusplus
}
#endif
#endif
