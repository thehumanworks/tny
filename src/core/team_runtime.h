/* Native adapters for durable collaboration. Public requests never name a sender. */
#ifndef TNY_TEAM_RUNTIME_H
#define TNY_TEAM_RUNTIME_H
#include "core/tools.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Private per-attempt bearer; only its SHA-256 belongs in a job record. */
bool tny_team_capability_new(char token[65], char digest[65]);
/* Same JSON grammar behind CLI, tools and terminal interception. */
char *tny_team_mailbox_parse_argv(int argc, char **argv, char *err, size_t errlen);
const char *tny_team_mailbox_permission(yyjson_val *args);
char *tny_team_mailbox_detail(yyjson_val *args);
int tny_team_mailbox_run(tools_env *env, yyjson_val *args, bool local_operator, buf_t *out,
                         char *err, size_t errlen);
/* Persist subscribed job IDs in the caller session; metadata is not membership. */
int tny_team_register_run(tools_env *env, const char *run_id);
/* Called only at a quiescent native model-call boundary. No provider reentry. */
int tny_team_deliver(tools_env *env, char *err, size_t errlen);
#ifdef __cplusplus
}
#endif
#endif
