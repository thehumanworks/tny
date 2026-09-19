/* Private bounded evidence; no tool text or target identifiers on disk. */
#ifndef TNY_LEARNING_STORE_H
#define TNY_LEARNING_STORE_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define TNY_LEARNING_RULES 3
#define TNY_LEARNING_LIMIT 1000

typedef struct {
    uint32_t successes, failures;
    char session_id[17];
} tny_learning_counter;

typedef struct {
    char root[4096];
    char key[17];
    char session_id[17];
    tny_learning_counter delta[TNY_LEARNING_RULES];
    bool active;
} tny_learning_store;

/* init never creates files. Unsupported/corrupt stores remain untouched. */
void tny_learning_store_init(tny_learning_store *store, const char *tny_dir, const char *workspace,
                             const char *session_id, bool persist,
                             tny_learning_counter counters[TNY_LEARNING_RULES]);
void tny_learning_counter_add(tny_learning_counter *counter, bool ok);
void tny_learning_store_flush(tny_learning_store *store,
                              tny_learning_counter counters[TNY_LEARNING_RULES]);
void tny_learning_store_carry(tny_learning_store *store,
                              tny_learning_counter counters[TNY_LEARNING_RULES],
                              const tny_learning_counter pending[TNY_LEARNING_RULES]);
/* Local evidence is retained if a nonblocking merge cannot complete. */
void tny_learning_store_record(tny_learning_store *store,
                               tny_learning_counter counters[TNY_LEARNING_RULES], unsigned rule,
                               bool ok);
#ifdef __cplusplus
}
#endif
#endif
