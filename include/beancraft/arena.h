#ifndef BC_ARENA_H
#define BC_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Arena allocator - simple bump allocator with block chaining
// All allocations from an arena are freed together when the arena is destroyed

typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t size;
    size_t used;
    uint8_t data[];
} ArenaBlock;

typedef struct Arena {
    ArenaBlock *first;
    ArenaBlock *current;
    size_t default_block_size;
} Arena;

// Create a new arena with the specified default block size
Arena *arena_new(size_t default_block_size);

// Free an arena and all its allocations
void arena_free(Arena *arena);

// Allocate memory from the arena (never returns NULL - aborts on OOM)
void *arena_alloc(Arena *arena, size_t size);

// Allocate zeroed memory
void *arena_alloc_zero(Arena *arena, size_t size);

// Allocate memory with specific alignment
void *arena_alloc_aligned(Arena *arena, size_t size, size_t align);

// Duplicate a string into the arena
char *arena_strdup(Arena *arena, const char *s);

// Duplicate a string with known length into the arena
char *arena_strndup(Arena *arena, const char *s, size_t len);

// Printf-style allocation
char *arena_sprintf(Arena *arena, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

// Reset arena (keeps first block, frees rest)
void arena_reset(Arena *arena);

// Get total bytes allocated
size_t arena_total_allocated(const Arena *arena);

#endif // BC_ARENA_H
