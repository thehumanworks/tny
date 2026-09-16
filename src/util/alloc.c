#define TNY_ALLOC_IMPLEMENTATION 1
#include "util/alloc_override.h"

#include <errno.h>
#include <stdint.h>

typedef struct {
    size_t allocation_index;
    size_t fail_at;
    bool failed;
    bool injected;
    bool provider_failed;
} tny_alloc_state;

static _Thread_local tny_alloc_state alloc_state;
static _Thread_local bool settling;
#ifdef TNY_ALLOC_TESTING
static _Thread_local size_t settlement_count, settlement_allocations;
#endif

bool tny_alloc_settling(void) { return settling; }
void tny_alloc_settlement_begin(void) {
    settling = true;
#ifdef TNY_ALLOC_TESTING
    settlement_count++;
#endif
}
void tny_alloc_settlement_end(void) { settling = false; }

#ifdef TNY_ALLOC_TESTING
#include <stdatomic.h>
static atomic_size_t owned_live;
void tny_alloc_test_owned_acquire(void) {
    atomic_fetch_add_explicit(&owned_live, 1, memory_order_relaxed);
}
void tny_alloc_test_owned_release(void) {
    atomic_fetch_sub_explicit(&owned_live, 1, memory_order_relaxed);
}
size_t tny_alloc_test_owned_live(void) {
    return atomic_load_explicit(&owned_live, memory_order_relaxed);
}
#endif

void tny_alloc_scope_begin(const char *name) {
    alloc_state.allocation_index = 0;
    alloc_state.fail_at = 0;
    alloc_state.failed = false;
    alloc_state.injected = false;
    alloc_state.provider_failed = false;
#ifdef TNY_ALLOC_TESTING
    settlement_count = settlement_allocations = 0;
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

void tny_alloc_provider_failed(void) {
    alloc_state.failed = true;
    alloc_state.provider_failed = true;
}

long tny_c_strtol(const char *nptr, char **endptr, int base) {
    return strtol(nptr, endptr, base);
}

bool tny_alloc_scope_failed(void) { return alloc_state.failed; }

void tny_alloc_scope_clear(void) { alloc_state.failed = false; }

#ifdef TNY_ALLOC_TESTING
#if defined(__GNUC__) || defined(__clang__)
#define TNY_ALLOC_TEST_VISIBLE __attribute__((visibility("default")))
#else
#define TNY_ALLOC_TEST_VISIBLE
#endif
TNY_ALLOC_TEST_VISIBLE size_t tny_alloc_test_scope_count(void) {
    return alloc_state.allocation_index;
}

TNY_ALLOC_TEST_VISIBLE bool tny_alloc_test_scope_injected(void) { return alloc_state.injected; }
TNY_ALLOC_TEST_VISIBLE size_t tny_alloc_test_settlement_count(void) { return settlement_count; }
TNY_ALLOC_TEST_VISIBLE size_t tny_alloc_test_settlement_allocations(void) {
    return settlement_allocations;
}
#undef TNY_ALLOC_TEST_VISIBLE
#endif

static bool should_fail(void) {
    alloc_state.allocation_index++;
#ifdef TNY_ALLOC_TESTING
    if (settling || alloc_state.provider_failed) settlement_allocations++;
#endif
    if (alloc_state.fail_at && alloc_state.allocation_index == alloc_state.fail_at) {
        alloc_state.failed = true;
        alloc_state.injected = true;
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
