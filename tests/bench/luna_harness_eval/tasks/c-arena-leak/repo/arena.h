#ifndef ARENA_H
#define ARENA_H
#include <stddef.h>
struct arena_chunk;
typedef struct arena { struct arena_chunk *head; } arena;
void *arena_alloc(arena *a, size_t n);
void arena_reset(arena *a);
void arena_destroy(arena *a);
size_t arena_live_chunks(void);
#endif
