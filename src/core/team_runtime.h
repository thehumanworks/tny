/* Native adapters for durable collaboration. Public requests never name a sender. */
#ifndef TNY_TEAM_RUNTIME_H
#define TNY_TEAM_RUNTIME_H
#include "core/tools.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Same JSON grammar behind CLI, tools and terminal interception. */
char *tny_team_mailbox_parse_argv(int argc, char **argv, char *err, size_t errlen);
const char *tny_team_mailbox_permission(yyjson_val *args);
char *tny_team_mailbox_detail(yyjson_val *args);
int tny_team_mailbox_run(tools_env *env, yyjson_val *args, bool local_operator, buf_t *out,
                         char *err, size_t errlen);
/* Check a service-owned current job record. -2 denied, -1 operator/recorded
 * parent, otherwise the capability-authenticated current member index. The
 * record and caller inputs are trusted C service inputs, never request identity.
 * Mutating callers must hold the job owner/state fence through the operation. */
int tny_team_record_authority(const tools_env *env, yyjson_val *record, bool local_operator);
bool tny_team_capability_matches(yyjson_val *record, int task, int attempt, const char *token);
/* Persist subscribed job IDs in the caller session; metadata is not membership. */
int tny_team_register_run(tools_env *env, const char *run_id);
/* Called only at a quiescent native model-call boundary. No provider reentry. */
int tny_team_deliver(tools_env *env, char *err, size_t errlen);
#ifdef __cplusplus
}
#endif
#endif
