/* agent_presets.h — global, explicit inference selections for named agents. */
#ifndef TNY_AGENT_PRESETS_H
#define TNY_AGENT_PRESETS_H

#include "core/config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TNY_AGENT_PRESET_COUNT_MAX      32u
#define TNY_AGENT_PRESET_TEXT_MAX       256u
#define TNY_AGENT_PRESET_RESULT_MIN     1024u
#define TNY_AGENT_PRESET_RESULT_MAX     65536u
#define TNY_AGENT_PRESET_RESULT_DEFAULT 8192u
#define TNY_AGENT_PRESET_TASK_DEFAULT   "context-retrieval"

/* Validate only settings.json's agent_presets map, without provider resolution,
 * credentials, network, task discovery or execution. Repo/workspace settings
 * never supply presets. Returns 0 or -1 with a value-free diagnostic. */
int tny_agent_presets_validate(const tny_ctx *ctx);

/* Resolve one explicit name to a malloc-owned canonical JSON snapshot, or NULL
 * with a value-free diagnostic. Provider and model are mandatory, never inherited
 * from the parent. Absent effort is omitted; task, description and result cap
 * are materialized. The snapshot has no borrowed settings or credential data. */
char *tny_agent_preset_resolve(const tny_ctx *ctx, const char *name);

/* Validate a persisted canonical snapshot without consulting settings. Requires
 * name/provider/model/task/description/max_result_bytes; effort is optional. */
bool tny_agent_preset_snapshot_valid(yyjson_val *snapshot);

/* Append deterministic discovery containing only sorted names/descriptions.
 * At most COUNT_MAX entries with TEXT_MAX-byte descriptions; no execution. */
void tny_agent_presets_collect(const tny_ctx *ctx, buf_t *out);

#ifdef __cplusplus
}
#endif

#endif
