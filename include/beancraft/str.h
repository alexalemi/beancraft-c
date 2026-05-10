#ifndef BC_STR_H
#define BC_STR_H

#include "arena.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Interned string - unique instance per string value
// Can be compared by pointer equality
typedef struct Str {
    const char *data;   // Null-terminated
    uint32_t len;
    uint32_t hash;
} Str;

// String pool for interning
typedef struct StrPool StrPool;

// Create a new string pool
StrPool *strpool_new(Arena *arena);

// Intern a string - returns existing instance or creates new one
// The returned Str* is valid for the lifetime of the pool's arena
Str *str_intern(StrPool *pool, const char *s, size_t len);

// Intern a null-terminated string
Str *str_intern_cstr(StrPool *pool, const char *s);

// Intern a formatted string
Str *str_internf(StrPool *pool, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

// Compare interned strings (pointer comparison)
static inline bool str_eq(const Str *a, const Str *b) {
    return a == b;
}

// Get the number of interned strings
size_t strpool_count(const StrPool *pool);

// FNV-1a hash function (exposed for other uses)
uint32_t fnv1a_hash(const char *data, size_t len);

#endif // BC_STR_H
