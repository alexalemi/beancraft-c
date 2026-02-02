#include "beancraft/str.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#define INITIAL_CAPACITY 64
#define LOAD_FACTOR 0.75

typedef struct StrEntry {
    Str *str;
    struct StrEntry *next;  // For chaining
} StrEntry;

struct StrPool {
    Arena *arena;
    StrEntry **buckets;
    size_t capacity;
    size_t count;
};

// FNV-1a hash
uint32_t fnv1a_hash(const char *data, size_t len) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)data[i];
        hash *= 16777619u;
    }
    return hash;
}

StrPool *strpool_new(Arena *arena) {
    StrPool *pool = arena_alloc(arena, sizeof(StrPool));
    pool->arena = arena;
    pool->capacity = INITIAL_CAPACITY;
    pool->count = 0;
    pool->buckets = arena_alloc_zero(arena, sizeof(StrEntry *) * pool->capacity);
    return pool;
}

static void strpool_grow(StrPool *pool) {
    size_t new_capacity = pool->capacity * 2;
    StrEntry **new_buckets = arena_alloc_zero(pool->arena,
                                               sizeof(StrEntry *) * new_capacity);

    // Rehash all entries
    for (size_t i = 0; i < pool->capacity; i++) {
        StrEntry *entry = pool->buckets[i];
        while (entry) {
            StrEntry *next = entry->next;
            size_t new_idx = entry->str->hash & (new_capacity - 1);
            entry->next = new_buckets[new_idx];
            new_buckets[new_idx] = entry;
            entry = next;
        }
    }

    pool->buckets = new_buckets;
    pool->capacity = new_capacity;
}

Str *str_intern(StrPool *pool, const char *s, size_t len) {
    if (!s) return NULL;

    uint32_t hash = fnv1a_hash(s, len);
    size_t idx = hash & (pool->capacity - 1);

    // Search existing entries
    for (StrEntry *entry = pool->buckets[idx]; entry; entry = entry->next) {
        if (entry->str->hash == hash &&
            entry->str->len == len &&
            memcmp(entry->str->data, s, len) == 0) {
            return entry->str;
        }
    }

    // Not found - create new entry
    if ((double)(pool->count + 1) / pool->capacity > LOAD_FACTOR) {
        strpool_grow(pool);
        idx = hash & (pool->capacity - 1);
    }

    // Allocate string data
    char *data = arena_alloc(pool->arena, len + 1);
    memcpy(data, s, len);
    data[len] = '\0';

    // Allocate Str struct
    Str *str = arena_alloc(pool->arena, sizeof(Str));
    str->data = data;
    str->len = (uint32_t)len;
    str->hash = hash;

    // Create and insert entry
    StrEntry *entry = arena_alloc(pool->arena, sizeof(StrEntry));
    entry->str = str;
    entry->next = pool->buckets[idx];
    pool->buckets[idx] = entry;
    pool->count++;

    return str;
}

Str *str_intern_cstr(StrPool *pool, const char *s) {
    if (!s) return NULL;
    return str_intern(pool, s, strlen(s));
}

Str *str_internf(StrPool *pool, const char *fmt, ...) {
    va_list args, args_copy;
    va_start(args, fmt);
    va_copy(args_copy, args);

    // Determine required size
    int len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (len < 0) {
        va_end(args_copy);
        return NULL;
    }

    // Format into temporary buffer
    char *buf = arena_alloc(pool->arena, (size_t)len + 1);
    vsnprintf(buf, (size_t)len + 1, fmt, args_copy);
    va_end(args_copy);

    // Intern the result
    return str_intern(pool, buf, (size_t)len);
}

size_t strpool_count(const StrPool *pool) {
    return pool->count;
}
