#ifndef BC_ERROR_H
#define BC_ERROR_H

#include "arena.h"
#include <stddef.h>
#include <stdbool.h>

// Error categories
typedef enum {
    BC_ERR_NONE = 0,
    BC_ERR_IO,          // File not found, read error
    BC_ERR_SYNTAX,      // Parse error
    BC_ERR_SEMANTIC,    // Undefined label, register
    BC_ERR_RUNTIME,     // Instruction pointer out of bounds
    BC_ERR_OVERFLOW,    // Max steps exceeded
} BcErrorKind;

// Error with source location
typedef struct {
    BcErrorKind kind;
    const char *message;    // Arena-allocated
    const char *filename;   // May be NULL
    uint32_t line;          // 1-indexed, 0 if unknown
    uint32_t column;        // 1-indexed, 0 if unknown
} BcError;

// Result type - either success (value) or error
typedef struct {
    bool ok;
    union {
        void *value;
        BcError error;
    };
} BcResult;

// Create a successful result
static inline BcResult bc_ok(void *value) {
    return (BcResult){ .ok = true, .value = value };
}

// Create an error result
BcResult bc_err(Arena *arena, BcErrorKind kind, const char *filename,
                uint32_t line, uint32_t column, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));

// Create an error without location
BcResult bc_errf(Arena *arena, BcErrorKind kind, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

// Format an error for display
char *bc_error_format(Arena *arena, const BcError *err);

// Print error to stderr
void bc_error_print(const BcError *err);

// Convenience macros
#define BC_OK(val) bc_ok((void *)(val))

#define BC_TRY(result) do { \
    BcResult _r = (result); \
    if (!_r.ok) return _r; \
} while(0)

#define BC_UNWRAP(result, var) do { \
    BcResult _r = (result); \
    if (!_r.ok) return _r; \
    var = _r.value; \
} while(0)

#endif // BC_ERROR_H
