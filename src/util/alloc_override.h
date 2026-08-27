/* Force-included only for libtny PIC objects.  Include the system declarations
 * before defining the call-site aliases so libc headers remain untouched. */
#ifndef TNY_ALLOC_OVERRIDE_H
#define TNY_ALLOC_OVERRIDE_H

#include <stdlib.h>
#include <string.h>
#include "util/alloc.h"

#ifndef TNY_ALLOC_IMPLEMENTATION
#define malloc(size) tny_alloc_malloc(size)
#define calloc(count, size) tny_alloc_calloc((count), (size))
#define realloc(ptr, size) tny_alloc_realloc((ptr), (size))
#define strdup(value) tny_alloc_strdup(value)
#endif

#endif
