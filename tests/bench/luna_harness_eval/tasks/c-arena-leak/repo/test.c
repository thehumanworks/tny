#include "arena.h"
#include <assert.h>
#include <string.h>
int main(void) {
    arena a = {0};
    char *first = arena_alloc(&a, 400);
    assert(first);
    memset(first, 7, 400);
    assert(arena_alloc(&a, 500));
    assert(arena_live_chunks() >= 2);
    arena_reset(&a);
    assert(arena_live_chunks() == 0);
    assert(arena_alloc(&a, 8));
    arena_destroy(&a);
    assert(arena_live_chunks() == 0);
    return 0;
}
