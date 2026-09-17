/* parallel.h -- bounded fork/join over independent work items (ADR 0132).
 *
 * A pool exists only for the duration of one call: workers are created,
 * every index in [0, count) is claimed exactly once, and all workers are
 * joined before the call returns, so no thread outlives it and later fork()
 * sites see a single-threaded process again. Items must write only to their
 * own slot; they must not touch tny_ctx, session state, a shared yyjson
 * document or buf_t, and must not fork. */
#ifndef TNY_PARALLEL_H
#define TNY_PARALLEL_H

#include <stdbool.h>
#include <stddef.h>

#define TNY_PARALLEL_MAX_WORKERS 8

typedef void (*tny_parallel_fn)(size_t index, void *userdata);

/* Upper bound on workers for one call: min(online CPUs, TNY_THREADS,
 * TNY_PARALLEL_MAX_WORKERS), at least 1. TNY_THREADS=1 forces serial runs. */
size_t tny_parallel_workers(void);

/* Run fn(i, ud) for every i in [0, count), then join. The calling thread is
 * one of the workers, so an item that runs there stays inside the caller's
 * allocation scope; a failure observed on any other worker is folded back
 * into the caller's scope before returning, so tny_alloc_scope_failed()
 * remains the single oracle. Runs on the calling thread alone when count < 2,
 * when one worker is allowed, while allocation fault injection is armed
 * (injected indices stay enumerable), or when no thread can be created (the
 * wasm build). Returns the number of threads that took part, 1 for serial. */
size_t tny_parallel_for(size_t count, tny_parallel_fn fn, void *ud);

#endif
