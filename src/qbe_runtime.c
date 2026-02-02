// QBE Runtime Library for Beancraft
// This file is compiled separately and linked with QBE-generated code
//
// Simplified version using 64-bit integers only.
// For full bignum support, compile with -DUSE_GMP and link with -lgmp

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#ifdef USE_GMP
#include <gmp.h>
#endif

// Bignum representation (pointer-tagged)
// LSB = 1: immediate value, upper 63 bits are the value
// LSB = 0: pointer to heap-allocated value (GMP if enabled)

#define BIGNUM_MAX_IMMEDIATE ((INT64_MAX >> 1))

typedef uint64_t Bignum;

static inline bool is_immediate(Bignum x) {
    return x & 1;
}

static inline int64_t get_immediate(Bignum x) {
    return (int64_t)x >> 1;
}

static inline Bignum make_immediate(int64_t x) {
    return ((uint64_t)x << 1) | 1;
}

#ifdef USE_GMP
static inline mpz_t* get_heap_ptr(Bignum x) {
    return (mpz_t*)x;
}

// Allocate a new heap bignum
static Bignum heap_alloc(void) {
    mpz_t *p = malloc(sizeof(mpz_t));
    mpz_init(*p);
    return (Bignum)p;
}

// Free a heap bignum
static void heap_free(Bignum x) {
    if (!is_immediate(x) && x != 0) {
        mpz_clear(*get_heap_ptr(x));
        free(get_heap_ptr(x));
    }
}
#endif

// Increment a register (in-place)
void bc_inc(Bignum *reg) {
    Bignum x = *reg;

    if (is_immediate(x)) {
        int64_t val = get_immediate(x);
        if (val < BIGNUM_MAX_IMMEDIATE) {
            *reg = make_immediate(val + 1);
            return;
        }
#ifdef USE_GMP
        // Overflow to heap
        mpz_t *p = malloc(sizeof(mpz_t));
        mpz_init_set_si(*p, val);
        mpz_add_ui(*p, *p, 1);
        *reg = (Bignum)p;
#else
        // Without GMP, just saturate (or could abort)
        fprintf(stderr, "Warning: bignum overflow, GMP not enabled\n");
#endif
    } else {
#ifdef USE_GMP
        mpz_add_ui(*get_heap_ptr(x), *get_heap_ptr(x), 1);
#endif
    }
}

// Decrement a register (in-place)
// Returns 1 if the value was positive (decrement happened)
// Returns 0 if the value was zero (no change)
int bc_dec(Bignum *reg) {
    Bignum x = *reg;

    if (is_immediate(x)) {
        int64_t val = get_immediate(x);
        if (val <= 0) {
            return 0;  // Was zero (or negative, shouldn't happen)
        }
        *reg = make_immediate(val - 1);
        return 1;
    } else {
#ifdef USE_GMP
        mpz_t *p = get_heap_ptr(x);
        if (mpz_sgn(*p) <= 0) {
            return 0;  // Was zero
        }
        mpz_sub_ui(*p, *p, 1);

        // Try to demote to immediate if it fits
        if (mpz_fits_slong_p(*p)) {
            long val = mpz_get_si(*p);
            if (val >= 0 && val <= BIGNUM_MAX_IMMEDIATE) {
                mpz_clear(*p);
                free(p);
                *reg = make_immediate(val);
            }
        }
        return 1;
#else
        return 0;  // Shouldn't reach here without GMP
#endif
    }
}

// Convert bignum to uint64 (for printing)
// Returns the value if it fits, or UINT64_MAX if too large
uint64_t bc_bignum_to_u64(Bignum x) {
    if (is_immediate(x)) {
        int64_t val = get_immediate(x);
        return val >= 0 ? (uint64_t)val : 0;
    } else {
#ifdef USE_GMP
        mpz_t *p = get_heap_ptr(x);
        if (mpz_fits_ulong_p(*p)) {
            return mpz_get_ui(*p);
        }
#endif
        return UINT64_MAX;  // Too large or no GMP
    }
}

// Convert bignum to string (caller must free)
char* bc_bignum_to_string(Bignum x) {
    if (is_immediate(x)) {
        char *s = malloc(32);
        snprintf(s, 32, "%ld", get_immediate(x));
        return s;
    } else {
#ifdef USE_GMP
        return mpz_get_str(NULL, 10, *get_heap_ptr(x));
#else
        char *s = malloc(16);
        snprintf(s, 16, "OVERFLOW");
        return s;
#endif
    }
}

// Set a register from uint64
void bc_set_from_u64(Bignum *reg, uint64_t value) {
#ifdef USE_GMP
    // Free existing heap value if any
    if (!is_immediate(*reg) && *reg != 0) {
        heap_free(*reg);
    }

    if (value <= (uint64_t)BIGNUM_MAX_IMMEDIATE) {
        *reg = make_immediate((int64_t)value);
    } else {
        mpz_t *p = malloc(sizeof(mpz_t));
        mpz_init_set_ui(*p, value);
        *reg = (Bignum)p;
    }
#else
    if (value <= (uint64_t)BIGNUM_MAX_IMMEDIATE) {
        *reg = make_immediate((int64_t)value);
    } else {
        fprintf(stderr, "Warning: value too large for 63-bit immediate\n");
        *reg = make_immediate(BIGNUM_MAX_IMMEDIATE);
    }
#endif
}

// Cleanup a register (free heap memory)
void bc_cleanup(Bignum *reg) {
#ifdef USE_GMP
    heap_free(*reg);
#endif
    *reg = make_immediate(0);
}

// =============================================================================
// Optimized operations - O(1) regardless of register values
// =============================================================================

// Zero a register: reg = 0
void bc_zero(Bignum *reg) {
#ifdef USE_GMP
    if (!is_immediate(*reg) && *reg != 0) {
        heap_free(*reg);
    }
#endif
    *reg = make_immediate(0);
}

// Transfer: dst += src; src = 0
// This is O(1) - just adds the values and zeros the source
void bc_transfer(Bignum *src, Bignum *dst) {
    Bignum s = *src;
    Bignum d = *dst;

    // Fast path: both are immediates
    if (is_immediate(s) && is_immediate(d)) {
        int64_t sv = get_immediate(s);
        int64_t dv = get_immediate(d);
        int64_t sum = sv + dv;

        // Check for overflow
        if (sum >= 0 && sum <= BIGNUM_MAX_IMMEDIATE) {
            *dst = make_immediate(sum);
            *src = make_immediate(0);
            return;
        }
        // Fall through to GMP path
    }

#ifdef USE_GMP
    // Need GMP for large values
    mpz_t sum;
    mpz_init(sum);

    // Add source value
    if (is_immediate(s)) {
        mpz_set_si(sum, get_immediate(s));
    } else {
        mpz_set(sum, *get_heap_ptr(s));
    }

    // Add destination value
    if (is_immediate(d)) {
        if (get_immediate(d) >= 0) {
            mpz_add_ui(sum, sum, (unsigned long)get_immediate(d));
        } else {
            mpz_sub_ui(sum, sum, (unsigned long)(-get_immediate(d)));
        }
    } else {
        mpz_add(sum, sum, *get_heap_ptr(d));
    }

    // Free old values if heap
    if (!is_immediate(s) && s != 0) heap_free(s);
    if (!is_immediate(d) && d != 0) heap_free(d);

    // Store result
    if (mpz_fits_slong_p(sum)) {
        long val = mpz_get_si(sum);
        if (val >= 0 && val <= BIGNUM_MAX_IMMEDIATE) {
            *dst = make_immediate(val);
            mpz_clear(sum);
        } else {
            mpz_t *p = malloc(sizeof(mpz_t));
            *p = sum;  // Transfer ownership
            *dst = (Bignum)p;
        }
    } else {
        mpz_t *p = malloc(sizeof(mpz_t));
        *p = sum;
        *dst = (Bignum)p;
    }
    *src = make_immediate(0);
#else
    // Without GMP, try our best with 64-bit
    if (is_immediate(s) && is_immediate(d)) {
        int64_t sv = get_immediate(s);
        int64_t dv = get_immediate(d);
        // Saturating add
        int64_t sum = sv + dv;
        if (sum > BIGNUM_MAX_IMMEDIATE) {
            fprintf(stderr, "Warning: transfer overflow, clamping to max\n");
            sum = BIGNUM_MAX_IMMEDIATE;
        }
        *dst = make_immediate(sum);
        *src = make_immediate(0);
    }
#endif
}
