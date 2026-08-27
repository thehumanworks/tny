#define TNY_ALLOC_IMPLEMENTATION 1
#include "util/alloc_override.h"

#include <errno.h>
#include <stdint.h>

typedef struct {
    size_t allocation_index;
    size_t fail_at;
    bool failed;
} tny_alloc_state;

static _Thread_local tny_alloc_state alloc_state;

void tny_alloc_scope_begin(const char *name) {
    alloc_state.allocation_index = 0;
    alloc_state.fail_at = 0;
    alloc_state.failed = false;
#ifdef TNY_ALLOC_TESTING
    const char *scope = getenv("TNY_TEST_ALLOC_SCOPE");
    const char *index = getenv("TNY_TEST_ALLOC_FAIL_AT");
    if (scope && name && strcmp(scope, name) == 0 && index && *index) {
        char *end = NULL;
        errno = 0;
        unsigned long long value = strtoull(index, &end, 10);
        if (!errno && end && !*end && value > 0 && value <= SIZE_MAX)
            alloc_state.fail_at = (size_t)value;
    }
#else
    (void)name;
#endif
}

bool tny_alloc_scope_failed(void) { return alloc_state.failed; }

void tny_alloc_scope_clear(void) { alloc_state.failed = false; }

static bool should_fail(void) {
    alloc_state.allocation_index++;
    if (alloc_state.fail_at &&
        alloc_state.allocation_index == alloc_state.fail_at) {
        alloc_state.failed = true;
        errno = ENOMEM;
        return true;
    }
    return false;
}

void *tny_alloc_malloc(size_t size) {
    if (should_fail()) return NULL;
    void *ptr = malloc(size);
    if (!ptr && size) alloc_state.failed = true;
    return ptr;
}

void *tny_alloc_calloc(size_t count, size_t size) {
    if (should_fail()) return NULL;
    void *ptr = calloc(count, size);
    if (!ptr && count && size) alloc_state.failed = true;
    return ptr;
}

void *tny_alloc_realloc(void *ptr, size_t size) {
    if (should_fail()) return NULL;
    void *next = realloc(ptr, size);
    if (!next && size) alloc_state.failed = true;
    return next;
}

char *tny_alloc_strdup(const char *value) {
    if (!value) return NULL;
    size_t size = strlen(value) + 1;
    char *copy = tny_alloc_malloc(size);
    if (copy) memcpy(copy, value, size);
    return copy;
}
