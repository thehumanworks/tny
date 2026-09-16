/* Private immutable payload view; ownership is transferred as a handle. */
#ifndef TNY_OWNED_EVENT_H
#define TNY_OWNED_EVENT_H
#include "core/events.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct tny_owned_event {
    tny_backend_event ev;
    uint64_t sequence;
    int64_t timestamp_ms;
    const char *provider;
    const char *session_id;
    const char *turn_id;
    size_t owned_bytes;
    bool hooks_done;
    bool suppressed;
    struct tny_owned_event *next;
} tny_owned_event;
/* Copy all payloads before return. NULL means OOM; no partial record escapes.
 * Payload addresses remain valid until free, including across queue transfers.
 * Only runtime bookkeeping and the unpublished reserve turn-id slot are mutable. */
tny_owned_event *tny_owned_event_copy(const tny_backend_event *event, const char *provider,
                                      const char *session_id, const char *turn_id,
                                      size_t turn_capacity);
/* Runtime-only preparation of an unpublished reserve; never allocates. */
void tny_owned_event_set_turn(tny_owned_event *event, const char *session_id, uint64_t sequence,
                              int continuation);
void tny_owned_event_free(tny_owned_event *event);
#ifdef __cplusplus
}
#endif
#endif
