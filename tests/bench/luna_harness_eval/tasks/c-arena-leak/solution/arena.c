#include "arena.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

struct arena_chunk {
    struct arena_chunk *next;
    size_t used;
    size_t capacity;
    max_align_t alignment;
    unsigned char data[];
};
static size_t live_chunks;

size_t arena_live_chunks(void) { return live_chunks; }

void *arena_alloc(arena *a, size_t n) {
    const size_t alignment = _Alignof(max_align_t);
    if (n == 0) n = 1;
    if (n > SIZE_MAX - alignment) return NULL;
    n = (n + alignment - 1) / alignment * alignment;
    if (!a->head || a->head->capacity - a->head->used < n) {
        size_t capacity = n > 256 ? n : 256;
        if (capacity > SIZE_MAX - sizeof(struct arena_chunk)) return NULL;
        struct arena_chunk *chunk = malloc(sizeof(*chunk) + capacity);
        if (!chunk) return NULL;
        chunk->next = a->head;
        chunk->used = 0;
        chunk->capacity = capacity;
        a->head = chunk;
        ++live_chunks;
    }
    void *result = a->head->data + a->head->used;
    a->head->used += n;
    return result;
}

void arena_reset(arena *a) {
    struct arena_chunk *chunk = a->head;
    while (chunk) {
        struct arena_chunk *next = chunk->next;
        free(chunk);
        --live_chunks;
        chunk = next;
    }
    a->head = NULL;
}

void arena_destroy(arena *a) {
    struct arena_chunk *chunk = a->head;
    while (chunk) {
        struct arena_chunk *next = chunk->next;
        free(chunk);
        --live_chunks;
        chunk = next;
    }
    a->head = NULL;
}
