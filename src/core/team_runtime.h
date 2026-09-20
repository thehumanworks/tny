/* Native adapters for durable collaboration. Public requests never name a sender. */
#ifndef TNY_TEAM_RUNTIME_H
#define TNY_TEAM_RUNTIME_H
#include "core/tools.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct tny_swarm_message_plan tny_swarm_message_plan;

/* Same JSON grammar behind CLI, tools and terminal interception. */
char *tny_team_mailbox_parse_argv(int argc, char **argv, char *err, size_t errlen);
const char *tny_team_mailbox_permission(yyjson_val *args);
char *tny_team_mailbox_detail(yyjson_val *args);
int tny_team_mailbox_run(tools_env *env, yyjson_val *args, bool local_operator, buf_t *out,
                         char *err, size_t errlen);
/* Purposeful-swarm named send. This remains a permission-checked adapter over
 * the durable mailbox service; it does not add delivery or acknowledgement
 * semantics. */
char *tny_swarm_message_detail(tools_env *env, yyjson_val *args, char *err, size_t errlen);
int tny_swarm_message_run(tools_env *env, yyjson_val *args, buf_t *out, char *err, size_t errlen);
char *tny_swarm_message_prepare(tools_env *env, yyjson_val *args, tny_swarm_message_plan **plan,
                                char *err, size_t errlen);
int tny_swarm_message_run_prepared(tools_env *env, yyjson_val *args, tny_swarm_message_plan *plan,
                                   buf_t *out, char *err, size_t errlen);
void tny_swarm_message_plan_free(tny_swarm_message_plan *plan);
bool tny_swarm_message_request_equal(yyjson_val *prepared, yyjson_val *current);
bool tny_swarm_message_id(const char *run, int sender_task, uint32_t job_attempt,
                          uint32_t sender_attempt, int recipient_task, uint32_t recipient_attempt,
                          const char *payload, size_t payload_len, char id[65]);
/* Check a service-owned current job record. -2 denied, -1 operator/recorded
 * parent, otherwise the capability-authenticated current member index. The
 * record and caller inputs are trusted C service inputs, never request identity.
 * Mutating callers must hold the job owner/state fence through the operation. */
int tny_team_record_authority(const tools_env *env, yyjson_val *record, bool local_operator);
bool tny_team_capability_matches(yyjson_val *record, int task, int attempt, const char *token);
/* Private, advisory diagnostics only. No raw error strings, events or job state.
 * First category wins per task/attempt. Writes authenticate the inherited member
 * capability against a confined current record, without taking state.lock.
 * Read only after child cleanup, using the supervisor's current identity.
 * NULL means absent, invalid or unreadable; never infer success from absence. */
void tny_team_startup_diagnostic(tny_ctx *ctx, const char *category);
/* Same-thread scope around engine_start only. Delivery captures a local code
 * in memory; end publishes only a failed initial boundary or refused start.
 * Existing engine terminal errors are untouched; later boundaries cannot
 * produce a startup sidecar. */
void tny_team_startup_begin(tny_ctx *ctx);
void tny_team_startup_end(tny_ctx *ctx, bool failed);
const char *tny_team_startup_diagnostic_read(tny_ctx *ctx, const char *run, int task, int attempt);
/* Persist subscribed job IDs in the caller session; metadata is not membership. */
int tny_team_register_run(tools_env *env, const char *run_id);
/* Called only at a quiescent native model-call boundary. No provider reentry. */
int tny_team_deliver(tools_env *env, char *err, size_t errlen);
#ifdef __cplusplus
}
#endif
#endif
