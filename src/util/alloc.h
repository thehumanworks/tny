/* alloc.h -- libtny allocation boundary and deterministic fault injection.
 *
 * Normal CLI builds use the C allocator directly.  The shared-library build
 * force-includes alloc_override.h, which routes allocations through these
 * wrappers so a public API call can observe allocator exhaustion without
 * installing a process-global handler or using longjmp. */
#ifndef TNY_ALLOC_H
#define TNY_ALLOC_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

void tny_alloc_scope_begin(const char *name);
bool tny_alloc_scope_failed(void);
void tny_alloc_scope_clear(void);
/* Mark provider OOM at the first quiescent parser/transport boundary. Test
 * accounting covers all subsequent attempts through the current public call,
 * including the interval before the runtime enters reserved settlement. */
void tny_alloc_provider_failed(void);

/* Quiescent emergency cancellation: release owned resources, but do not
 * construct protocol requests, transcript entries or callback payloads. */
void tny_alloc_settlement_begin(void);
void tny_alloc_settlement_end(void);
bool tny_alloc_settling(void);

void *tny_alloc_malloc(size_t size);
void *tny_alloc_calloc(size_t count, size_t size);
void *tny_alloc_realloc(void *ptr, size_t size);
char *tny_alloc_strdup(const char *value);
/* C11 wrapper: C++ TUs must not call libc strtol, which glibc 2.38+
 * redirects to __isoc23_strtol@GLIBC_2.38 via libstdc++'s _GNU_SOURCE. */
long tny_c_strtol(const char *nptr, char **endptr, int base);

#ifdef TNY_ALLOC_TESTING
/* Test-only introspection for the process-isolated fault harness. These are
 * intentionally absent from production objects and the public ABI. */
size_t tny_alloc_test_scope_count(void);
bool tny_alloc_test_scope_injected(void);
size_t tny_alloc_test_settlement_count(void);
size_t tny_alloc_test_settlement_allocations(void);
/* Process-wide C++ owner/container allocations; excludes ordinary C buffers. */
void tny_alloc_test_owned_acquire(void);
void tny_alloc_test_owned_release(void);
size_t tny_alloc_test_owned_live(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
