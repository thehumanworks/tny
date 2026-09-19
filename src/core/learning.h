/* Automatic empirical recovery learning: fixed storage, no model calls. */
#ifndef TNY_LEARNING_H
#define TNY_LEARNING_H
#ifdef __cplusplus
extern "C" {
#endif
#include "util/util.h"
#include "util/learning_store.h"

typedef enum tny_learning_event {
    TNY_LEARN_EDIT,
    TNY_LEARN_READ,
    TNY_LEARN_SEARCH,
    TNY_LEARN_TERMINAL,
    TNY_LEARN_OTHER
} tny_learning_event;

typedef struct tny_learning {
    tny_learning_counter rules[TNY_LEARNING_RULES]; /* READ, SEARCH, TERMINAL */
    tny_learning_store store;
    uint64_t pending_scope; /* RAM only; zero cannot identify a target. */
    unsigned intervening;
    int diagnostic; /* -1 until a successful diagnostic */
    bool enabled;
} tny_learning;

/* Call each turn. Caller supplies default-on config and opts out of persistence
 * for ephemeral, library and remote contexts. */
void tny_learning_begin(tny_learning *learning, const char *tny_dir, const char *workspace,
                        const char *session_id, bool enabled, bool persist);
/* Only classified, actually executed first-party results may reach this API. */
void tny_learning_observe(tny_learning *learning, tny_learning_event event, uint64_t scope,
                          bool ok);
/* Append <=1KiB of fixed safe guidance plus counters; no eligible rules => no output. */
void tny_learning_collect(const tny_learning *learning, buf_t *out);
#ifdef __cplusplus
}
#endif
#endif
