/* Private execution RPC state; never serializes callbacks or widens ctx policy. */
#ifndef TNY_EXECUTION_STATE_H
#define TNY_EXECUTION_STATE_H
#include "core/tools.h"

/* initial sends version-1 execution metadata and an empty append-only message
 * outbox, never owner transcript/audit/compaction history, plus grants and
 * ephemeral results, but no
 * parent-owned queued images. pending_count reports their reserved capacity.
 * Delta rejects fields outside the projection and cannot replace/remove
 * messages. It sends changed scalar/replaced fields plus append-only array suffixes
 * against baseline_session, retaining concurrent owner appends, and captured
 * image bytes. Encoding never mutates env. Caller clears images after ACK and
 * refreshes its immutable session baseline after each successful sync. */
yyjson_mut_val *tny_execution_state_encode(yyjson_mut_doc *doc, tools_env *env,
                                           yyjson_val *baseline_session, bool initial);
/* Stage/validate all state before committing. Initial requires session==NULL;
 * env ctx/perm already belong to this runtime. Deltas preserve unrelated parent
 * session fields, existing result handles/grants, and queued images. Empty
 * session deltas do not clone the owner document. The wire bound applies to
 * transport state, not the complete merged durable history. Changed
 * session documents are persisted by the owner before success/ACK; a save
 * failure returns false with the merged memory retained (outcome uncertain).
 * Initial restores are execution snapshots; their private execution_save
 * callback must synchronously transfer and publish changes through the owner. */
bool tny_execution_state_apply(tools_env *env, yyjson_val *state, bool initial);
#endif
