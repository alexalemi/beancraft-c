#ifndef BC_BIGNUM_H
#define BC_BIGNUM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Pointer-tagged bignum representation
// - If LSB is 1: immediate value (value >> 1)
// - If LSB is 0: pointer to BigLimbs struct
//
// This allows small numbers (up to ~4.6 * 10^18) to be stored inline
// without any heap allocation, making inc/dec operations very fast.

typedef uint64_t Bignum;

// Heap-allocated large number (base-2^64, LSB first)
typedef struct {
    uint32_t len;       // Number of limbs in use
    uint32_t cap;       // Allocated capacity
    uint64_t limbs[];   // Flexible array member
} BigLimbs;

// Tag bit constants
#define BIGNUM_IMMEDIATE     0x1ULL
#define BIGNUM_TAG_MASK      0x1ULL
#define BIGNUM_MAX_IMMEDIATE (INT64_MAX >> 1)  // ~4.6 * 10^18

// Branch prediction hints
#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x)   (x)
#define UNLIKELY(x) (x)
#endif

// ============================================================
// Inline fast-path functions
// ============================================================

// Check if value is stored inline (not on heap)
static inline bool bignum_is_immediate(Bignum x) {
    return (x & BIGNUM_TAG_MASK) == BIGNUM_IMMEDIATE;
}

// Check if bignum is zero
static inline bool bignum_is_zero(Bignum x) {
    // Zero is represented as just the tag bit (value 0 << 1 | 1 = 1)
    return x == BIGNUM_IMMEDIATE;
}

// Create zero
static inline Bignum bignum_zero(void) {
    return BIGNUM_IMMEDIATE;
}

// Extract immediate value (assumes is_immediate is true)
static inline uint64_t bignum_get_immediate(Bignum x) {
    return x >> 1;
}

// Create immediate from value (assumes value <= BIGNUM_MAX_IMMEDIATE)
static inline Bignum bignum_make_immediate(uint64_t val) {
    return (val << 1) | BIGNUM_IMMEDIATE;
}

// Get pointer to heap data (assumes is_immediate is false)
static inline BigLimbs *bignum_get_ptr(Bignum x) {
    return (BigLimbs *)x;
}

// ============================================================
// Creation and destruction
// ============================================================

// Create bignum from uint64 (may allocate if > BIGNUM_MAX_IMMEDIATE)
Bignum bignum_from_u64(uint64_t val);

// Create bignum from signed int64
Bignum bignum_from_i64(int64_t val);

// Free heap-allocated bignum (safe to call on immediate values)
void bignum_free(Bignum *x);

// Clone a bignum (copies heap data if necessary)
Bignum bignum_clone(Bignum x);

// ============================================================
// Core operations (hot path - must be very fast)
// ============================================================

// Increment in place
// Always succeeds (may allocate on overflow to large number)
bool bignum_inc(Bignum *x);

// Decrement in place
// Returns true if value was positive and was decremented
// Returns false if value was zero (does not modify)
bool bignum_dec(Bignum *x);

// ============================================================
// Arithmetic for optimized patterns
// ============================================================

// Add: returns a + b (allocates new bignum)
Bignum bignum_add(Bignum a, Bignum b);

// Multiply: returns a * b (allocates new bignum). Does NOT modify a or b.
Bignum bignum_mul(Bignum a, Bignum b);

// Add in place: dst += src
bool bignum_add_into(Bignum *dst, Bignum src);

// Divide x by k (k must be > 0) in place; x becomes floor(x / k).
// Returns the remainder (always < k, hence fits in u64).
uint64_t bignum_divmod_small(Bignum *x, uint64_t k);

// Set to zero (frees heap if necessary)
void bignum_set_zero(Bignum *x);

// ============================================================
// Comparison
// ============================================================

// Compare two bignums
// Returns: -1 if a < b, 0 if a == b, 1 if a > b
int bignum_cmp(Bignum a, Bignum b);

// Equality check
bool bignum_eq(Bignum a, Bignum b);

// ============================================================
// Conversion
// ============================================================

// Try to convert to u64
// Returns true if successful, false if too large
bool bignum_to_u64(Bignum x, uint64_t *out);

// Convert to decimal string (caller must free with free())
char *bignum_to_string(Bignum x);

// Parse from decimal string
// Returns bignum_zero() on parse error
Bignum bignum_from_string(const char *s);

#endif // BC_BIGNUM_H
