/* alloc.h -- libtny allocation boundary and deterministic fault injection.
 *
 * Normal CLI builds use the C allocator directly.  The shared-library build
 * force-includes alloc_override.h, which routes allocations through these
 * wrappers so a public API call can observe allocator exhaustion without
 * installing a process-global handler or using longjmp. */
#ifndef TNY_ALLOC_H
#define TNY_ALLOC_H

#include <stdbool.h>
#include <stddef.h>

void tny_alloc_scope_begin(const char *name);
bool tny_alloc_scope_failed(void);
void tny_alloc_scope_clear(void);

void *tny_alloc_malloc(size_t size);
void *tny_alloc_calloc(size_t count, size_t size);
void *tny_alloc_realloc(void *ptr, size_t size);
char *tny_alloc_strdup(const char *value);

#endif
