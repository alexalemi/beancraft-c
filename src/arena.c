#include "beancraft/arena.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define DEFAULT_BLOCK_SIZE (64 * 1024)  // 64 KB
#define MIN_BLOCK_SIZE 256

static ArenaBlock *block_new(size_t size) {
    ArenaBlock *block = malloc(sizeof(ArenaBlock) + size);
    if (!block) {
        fprintf(stderr, "beancraft: out of memory\n");
        abort();
    }
    block->next = NULL;
    block->size = size;
    block->used = 0;
    return block;
}

Arena *arena_new(size_t default_block_size) {
    if (default_block_size < MIN_BLOCK_SIZE) {
        default_block_size = DEFAULT_BLOCK_SIZE;
    }

    Arena *arena = malloc(sizeof(Arena));
    if (!arena) {
        fprintf(stderr, "beancraft: out of memory\n");
        abort();
    }

    arena->default_block_size = default_block_size;
    arena->first = block_new(default_block_size);
    arena->current = arena->first;

    return arena;
}

void arena_free(Arena *arena) {
    if (!arena) return;

    ArenaBlock *block = arena->first;
    while (block) {
        ArenaBlock *next = block->next;
        free(block);
        block = next;
    }
    free(arena);
}

static inline size_t align_up(size_t n, size_t align) {
    return (n + align - 1) & ~(align - 1);
}

void *arena_alloc_aligned(Arena *arena, size_t size, size_t align) {
    ArenaBlock *block = arena->current;

    // Align the current position
    size_t aligned_used = align_up(block->used, align);

    // Check if it fits in current block
    if (aligned_used + size <= block->size) {
        void *ptr = block->data + aligned_used;
        block->used = aligned_used + size;
        return ptr;
    }

    // Need a new block - make it at least big enough for this allocation
    size_t new_size = arena->default_block_size;
    if (size + align > new_size) {
        new_size = size + align;
    }

    ArenaBlock *new_block = block_new(new_size);
    block->next = new_block;
    arena->current = new_block;

    size_t new_aligned = align_up(0, align);
    void *ptr = new_block->data + new_aligned;
    new_block->used = new_aligned + size;
    return ptr;
}

void *arena_alloc(Arena *arena, size_t size) {
    // Default to pointer alignment
    return arena_alloc_aligned(arena, size, sizeof(void *));
}

void *arena_alloc_zero(Arena *arena, size_t size) {
    void *ptr = arena_alloc(arena, size);
    memset(ptr, 0, size);
    return ptr;
}

char *arena_strdup(Arena *arena, const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *copy = arena_alloc(arena, len + 1);
    memcpy(copy, s, len + 1);
    return copy;
}

char *arena_strndup(Arena *arena, const char *s, size_t len) {
    if (!s) return NULL;
    char *copy = arena_alloc(arena, len + 1);
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

char *arena_sprintf(Arena *arena, const char *fmt, ...) {
    va_list args, args_copy;
    va_start(args, fmt);
    va_copy(args_copy, args);

    // First pass: determine required size
    int len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (len < 0) {
        va_end(args_copy);
        return NULL;
    }

    // Allocate and format
    char *buf = arena_alloc(arena, (size_t)len + 1);
    vsnprintf(buf, (size_t)len + 1, fmt, args_copy);
    va_end(args_copy);

    return buf;
}

void arena_reset(Arena *arena) {
    // Free all blocks except the first
    ArenaBlock *block = arena->first->next;
    while (block) {
        ArenaBlock *next = block->next;
        free(block);
        block = next;
    }

    // Reset first block
    arena->first->next = NULL;
    arena->first->used = 0;
    arena->current = arena->first;
}

size_t arena_total_allocated(const Arena *arena) {
    size_t total = 0;
    for (ArenaBlock *block = arena->first; block; block = block->next) {
        total += block->used;
    }
    return total;
}
