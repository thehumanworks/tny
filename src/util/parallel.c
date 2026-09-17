/* parallel.c -- bounded fork/join over independent work items (ADR 0127). */
#include "util/parallel.h"
#include "util/alloc.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>

typedef struct {
    atomic_size_t next;
    atomic_bool failed;
    size_t count;
    tny_parallel_fn fn;
    void *ud;
} parallel_job;

/* Dynamic claiming: a worker that draws a large file does not hold up the
 * indices behind it, so uneven trees still balance. */
static void claim_items(parallel_job *j) {
    for (;;) {
        size_t i = atomic_fetch_add_explicit(&j->next, 1, memory_order_relaxed);
        if (i >= j->count) return;
        j->fn(i, j->ud);
    }
}

static void *worker_main(void *arg) {
    parallel_job *j = arg;
    /* A fresh thread starts with a clean thread-local allocation scope: no
     * injection armed, nothing failed. Fold a real exhaustion back to the
     * caller; the join below orders it before the caller reads it. */
    claim_items(j);
    if (tny_alloc_scope_failed()) atomic_store_explicit(&j->failed, true, memory_order_relaxed);
    return NULL;
}

size_t tny_parallel_workers(void) {
    long cap = TNY_PARALLEL_MAX_WORKERS;
    const char *env = getenv("TNY_THREADS");
    if (env && *env) {
        char *end = NULL;
        long v = strtol(env, &end, 10);
        if (end && !*end && v >= 1 && v < cap) cap = v;
    }
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus < 1) cpus = 1;
    return (size_t)(cpus < cap ? cpus : cap);
}

size_t tny_parallel_for(size_t count, tny_parallel_fn fn, void *ud) {
    parallel_job j = {.count = count, .fn = fn, .ud = ud};
    atomic_init(&j.next, 0);
    atomic_init(&j.failed, false);
    size_t want = 1;
    if (count >= 2 && !tny_alloc_fault_injection_active()) want = tny_parallel_workers();
    if (want > count) want = count;
    pthread_t threads[TNY_PARALLEL_MAX_WORKERS];
    size_t started = 0;
    /* The caller is worker 0; a failed create (wasm, thread limits) just
     * leaves more items for the threads that exist. */
    for (size_t t = 1; t < want; t++) {
        if (pthread_create(&threads[started], NULL, worker_main, &j) != 0) break;
        started++;
    }
    claim_items(&j);
    for (size_t t = 0; t < started; t++) pthread_join(threads[t], NULL);
    if (atomic_load_explicit(&j.failed, memory_order_relaxed)) tny_alloc_scope_note_failure();
    return started + 1;
}
