/* Private owned storage, borrowed by the C scheduler. Never copy these records.
 * Mutate buffers through buf_*; use admission/reset for pending resources. */
#ifndef TNY_OPENAI_TURN_OWNER_H
#define TNY_OPENAI_TURN_OWNER_H
#include "core/tools.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    char *id;
    tools_call call;
    tny_perm_decision decision;
    char *original_args, *effective_args, *control_extension, *control_reason;
} oa_pending;
typedef struct {
    buf_t text, rawbody, toolcall_log;
    char *steer;
    oa_pending permission, custom;
} oa_turn_storage;
oa_turn_storage *oa_turn_new(void);
void oa_turn_free(oa_turn_storage **turn);
/* Copies all borrowed metadata before moving call. Failure leaves source and
 * destination unchanged. Caller explicitly invalidates async authority on failure. */
int oa_pending_admit(oa_pending *pending, const char *id, const char *original,
                     const char *effective, const char *extension, const char *reason,
                     tools_call *call);
/* Resource-only release. Async authority MUST already have been consumed,
 * moved or explicitly invalidated by the C transition. */
void oa_pending_reset(oa_pending *pending);
void oa_turn_take_steer(oa_turn_storage *turn, char *owned);
#ifdef __cplusplus
}
#endif
#endif
