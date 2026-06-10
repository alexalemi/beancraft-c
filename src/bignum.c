#include "beancraft/bignum.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================
// Heap allocation helpers
// ============================================================

// malloc that aborts on OOM (like the rest of the bignum allocators) instead
// of letting a NULL flow into memcpy/snprintf.
static void *xmalloc(size_t size) {
    void *p = malloc(size);
    if (!p) {
        fprintf(stderr, "beancraft: out of memory\n");
        abort();
    }
    return p;
}

static BigLimbs *limbs_alloc(uint32_t capacity) {
    BigLimbs *limbs = malloc(sizeof(BigLimbs) + capacity * sizeof(uint64_t));
    if (!limbs) {
        fprintf(stderr, "beancraft: out of memory\n");
        abort();
    }
    limbs->len = 0;
    limbs->cap = capacity;
    return limbs;
}

static BigLimbs *limbs_grow(BigLimbs *limbs) {
    uint32_t new_cap = limbs->cap * 2;
    BigLimbs *new_limbs = realloc(limbs, sizeof(BigLimbs) + new_cap * sizeof(uint64_t));
    if (!new_limbs) {
        fprintf(stderr, "beancraft: out of memory\n");
        abort();
    }
    new_limbs->cap = new_cap;
    return new_limbs;
}

// ============================================================
// Creation and destruction
// ============================================================

Bignum bignum_from_u64(uint64_t val) {
    if (val <= BIGNUM_MAX_IMMEDIATE) {
        return bignum_make_immediate(val);
    }

    // Need heap allocation
    BigLimbs *limbs = limbs_alloc(2);
    limbs->limbs[0] = val;
    limbs->len = 1;
    return (Bignum)limbs;
}

Bignum bignum_from_i64(int64_t val) {
    if (val < 0) {
        // Beancraft only uses non-negative integers
        // Treat negative as zero
        return bignum_zero();
    }
    return bignum_from_u64((uint64_t)val);
}

void bignum_free(Bignum *x) {
    if (!bignum_is_immediate(*x)) {
        free(bignum_get_ptr(*x));
    }
    *x = bignum_zero();
}

Bignum bignum_clone(Bignum x) {
    if (bignum_is_immediate(x)) {
        return x;
    }

    BigLimbs *src = bignum_get_ptr(x);
    BigLimbs *dst = limbs_alloc(src->cap);
    dst->len = src->len;
    memcpy(dst->limbs, src->limbs, src->len * sizeof(uint64_t));
    return (Bignum)dst;
}

// ============================================================
// Slow paths for increment/decrement
// ============================================================

// Promote immediate to heap and increment
static bool bignum_inc_promote(Bignum *x, uint64_t val) {
    BigLimbs *limbs = limbs_alloc(2);

    // val + 1 where val = BIGNUM_MAX_IMMEDIATE
    // This means val + 1 = 2^62 which fits in one 64-bit limb
    limbs->limbs[0] = val + 1;
    limbs->len = 1;
    *x = (Bignum)limbs;
    return true;
}

// Increment heap-allocated bignum
static bool bignum_inc_slow(Bignum *x) {
    BigLimbs *limbs = bignum_get_ptr(*x);

    // Add with carry propagation
    for (uint32_t i = 0; i < limbs->len; i++) {
        limbs->limbs[i]++;
        if (limbs->limbs[i] != 0) {
            // No overflow, done
            return true;
        }
        // Overflow - carry to next limb
    }

    // All limbs overflowed - need to extend
    if (limbs->len >= limbs->cap) {
        limbs = limbs_grow(limbs);
        *x = (Bignum)limbs;
    }

    limbs->limbs[limbs->len] = 1;
    limbs->len++;
    return true;
}

// Decrement heap-allocated bignum
static bool bignum_dec_slow(Bignum *x) {
    BigLimbs *limbs = bignum_get_ptr(*x);

    // Check for zero first
    bool all_zero = true;
    for (uint32_t i = 0; i < limbs->len; i++) {
        if (limbs->limbs[i] != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        return false;  // Can't decrement zero
    }

    // Subtract with borrow propagation
    for (uint32_t i = 0; i < limbs->len; i++) {
        if (limbs->limbs[i] != 0) {
            limbs->limbs[i]--;
            break;
        }
        // Borrow: wrap to UINT64_MAX and continue
        limbs->limbs[i] = UINT64_MAX;
    }

    // Normalize: remove leading zero limbs
    while (limbs->len > 1 && limbs->limbs[limbs->len - 1] == 0) {
        limbs->len--;
    }

    // Check if we can demote to immediate
    if (limbs->len == 1 && limbs->limbs[0] <= BIGNUM_MAX_IMMEDIATE) {
        uint64_t val = limbs->limbs[0];
        free(limbs);
        *x = bignum_make_immediate(val);
    }

    return true;
}

// ============================================================
// Core operations
// ============================================================

bool bignum_inc(Bignum *x) {
    if (LIKELY(bignum_is_immediate(*x))) {
        uint64_t val = bignum_get_immediate(*x);
        if (LIKELY(val < BIGNUM_MAX_IMMEDIATE)) {
            *x = bignum_make_immediate(val + 1);
            return true;
        }
        // Overflow to heap
        return bignum_inc_promote(x, val);
    }
    return bignum_inc_slow(x);
}

bool bignum_dec(Bignum *x) {
    if (LIKELY(bignum_is_immediate(*x))) {
        uint64_t val = bignum_get_immediate(*x);
        if (val == 0) {
            return false;  // Can't decrement zero
        }
        *x = bignum_make_immediate(val - 1);
        return true;
    }
    return bignum_dec_slow(x);
}

void bignum_set_zero(Bignum *x) {
    if (!bignum_is_immediate(*x)) {
        free(bignum_get_ptr(*x));
    }
    *x = bignum_zero();
}

// ============================================================
// Arithmetic
// ============================================================

Bignum bignum_add(Bignum a, Bignum b) {
    // Fast path: both immediate and sum fits
    if (LIKELY(bignum_is_immediate(a) && bignum_is_immediate(b))) {
        uint64_t va = bignum_get_immediate(a);
        uint64_t vb = bignum_get_immediate(b);

        // Immediates are <= BIGNUM_MAX_IMMEDIATE = 2^62 - 1, so the sum is at
        // most 2^63 - 2 and can never overflow u64.
        uint64_t sum = va + vb;
        if (sum <= BIGNUM_MAX_IMMEDIATE) {
            return bignum_make_immediate(sum);
        }
        // Sum doesn't fit in an immediate; promote to a single heap limb.
        BigLimbs *limbs = limbs_alloc(2);
        limbs->limbs[0] = sum;
        limbs->len = 1;
        return (Bignum)limbs;
    }

    // General case: at least one is on heap
    // Convert both to limb form and add
    uint64_t a_inline[1], b_inline[1];
    const uint64_t *a_limbs;
    const uint64_t *b_limbs;
    uint32_t a_len, b_len;

    if (bignum_is_immediate(a)) {
        a_inline[0] = bignum_get_immediate(a);
        a_limbs = a_inline;
        a_len = 1;
    } else {
        BigLimbs *al = bignum_get_ptr(a);
        a_limbs = al->limbs;
        a_len = al->len;
    }

    if (bignum_is_immediate(b)) {
        b_inline[0] = bignum_get_immediate(b);
        b_limbs = b_inline;
        b_len = 1;
    } else {
        BigLimbs *bl = bignum_get_ptr(b);
        b_limbs = bl->limbs;
        b_len = bl->len;
    }

    // Result has at most max(a_len, b_len) + 1 limbs
    uint32_t max_len = (a_len > b_len) ? a_len : b_len;
    BigLimbs *result = limbs_alloc(max_len + 1);

    uint64_t carry = 0;
    for (uint32_t i = 0; i < max_len; i++) {
        uint64_t ai = (i < a_len) ? a_limbs[i] : 0;
        uint64_t bi = (i < b_len) ? b_limbs[i] : 0;

        uint64_t sum = ai + bi + carry;
        carry = (sum < ai || (carry && sum == ai)) ? 1 : 0;
        result->limbs[i] = sum;
    }

    result->len = max_len;
    if (carry) {
        result->limbs[result->len] = 1;
        result->len++;
    }

    // Check if we can represent as immediate
    if (result->len == 1 && result->limbs[0] <= BIGNUM_MAX_IMMEDIATE) {
        uint64_t val = result->limbs[0];
        free(result);
        return bignum_make_immediate(val);
    }

    return (Bignum)result;
}

Bignum bignum_mul(Bignum a, Bignum b) {
    // 0 * x = x * 0 = 0
    if (bignum_is_zero(a) || bignum_is_zero(b)) {
        return bignum_zero();
    }

    // Fast path: both immediate.
    if (LIKELY(bignum_is_immediate(a) && bignum_is_immediate(b))) {
        uint64_t va = bignum_get_immediate(a);
        uint64_t vb = bignum_get_immediate(b);

        uint64_t prod;
        if (!__builtin_mul_overflow(va, vb, &prod)) {
            if (prod <= BIGNUM_MAX_IMMEDIATE) {
                return bignum_make_immediate(prod);
            }
            // Fits in 64 bits but not in an immediate.
            BigLimbs *limbs = limbs_alloc(2);
            limbs->limbs[0] = prod;
            limbs->len = 1;
            return (Bignum)limbs;
        }
        // Overflows 64 bits: take the full 128-bit product.
        __uint128_t wide = (__uint128_t)va * (__uint128_t)vb;
        BigLimbs *limbs = limbs_alloc(2);
        limbs->limbs[0] = (uint64_t)wide;
        limbs->limbs[1] = (uint64_t)(wide >> 64);
        limbs->len = (limbs->limbs[1] != 0) ? 2 : 1;
        return (Bignum)limbs;
    }

    // General case: schoolbook limb multiply. View each operand as a base-2^64
    // little-endian limb array (immediates become a single limb).
    uint64_t a_inline[1], b_inline[1];
    const uint64_t *a_limbs;
    const uint64_t *b_limbs;
    uint32_t a_len, b_len;

    if (bignum_is_immediate(a)) {
        a_inline[0] = bignum_get_immediate(a);
        a_limbs = a_inline;
        a_len = 1;
    } else {
        BigLimbs *al = bignum_get_ptr(a);
        a_limbs = al->limbs;
        a_len = al->len;
    }

    if (bignum_is_immediate(b)) {
        b_inline[0] = bignum_get_immediate(b);
        b_limbs = b_inline;
        b_len = 1;
    } else {
        BigLimbs *bl = bignum_get_ptr(b);
        b_limbs = bl->limbs;
        b_len = bl->len;
    }

    // The product has at most a_len + b_len limbs.
    uint32_t r_len = a_len + b_len;
    BigLimbs *result = limbs_alloc(r_len);
    for (uint32_t i = 0; i < r_len; i++) result->limbs[i] = 0;

    for (uint32_t i = 0; i < a_len; i++) {
        uint64_t carry = 0;
        for (uint32_t j = 0; j < b_len; j++) {
            // a[i]*b[j] + result[i+j] + carry  <=  (2^64-1)^2 + 2*(2^64-1) = 2^128-1,
            // so the high half is a valid 64-bit carry.
            __uint128_t cur = (__uint128_t)a_limbs[i] * (__uint128_t)b_limbs[j]
                            + (__uint128_t)result->limbs[i + j]
                            + (__uint128_t)carry;
            result->limbs[i + j] = (uint64_t)cur;
            carry = (uint64_t)(cur >> 64);
        }
        result->limbs[i + b_len] += carry;   // result[i+b_len] was still 0 here
    }

    // Normalize, then demote to an immediate if it now fits.
    result->len = r_len;
    while (result->len > 1 && result->limbs[result->len - 1] == 0) result->len--;
    if (result->len == 1 && result->limbs[0] <= BIGNUM_MAX_IMMEDIATE) {
        uint64_t v = result->limbs[0];
        free(result);
        return bignum_make_immediate(v);
    }
    return (Bignum)result;
}

bool bignum_add_into(Bignum *dst, Bignum src) {
    if (bignum_is_zero(src)) {
        return true;
    }

    Bignum result = bignum_add(*dst, src);
    if (!bignum_is_immediate(*dst)) {
        free(bignum_get_ptr(*dst));
    }
    *dst = result;
    return true;
}

uint64_t bignum_divmod_small(Bignum *x, uint64_t k) {
    // k > 0 is a precondition.
    if (bignum_is_immediate(*x)) {
        uint64_t v = bignum_get_immediate(*x);
        *x = bignum_make_immediate(v / k);   // quotient <= v <= MAX_IMMEDIATE
        return v % k;
    }

    BigLimbs *limbs = bignum_get_ptr(*x);
    uint64_t rem = 0;
    for (int i = (int)limbs->len - 1; i >= 0; i--) {
        // rem < k, so cur = rem*2^64 + limb < k*2^64, hence cur/k < 2^64.
        __uint128_t cur = ((__uint128_t)rem << 64) | limbs->limbs[i];
        limbs->limbs[i] = (uint64_t)(cur / k);
        rem = (uint64_t)(cur % k);
    }

    // Normalize and demote to immediate if it now fits.
    while (limbs->len > 1 && limbs->limbs[limbs->len - 1] == 0) limbs->len--;
    if (limbs->len == 1 && limbs->limbs[0] <= BIGNUM_MAX_IMMEDIATE) {
        uint64_t v = limbs->limbs[0];
        free(limbs);
        *x = bignum_make_immediate(v);
    }
    return rem;
}

// ============================================================
// Comparison
// ============================================================

int bignum_cmp(Bignum a, Bignum b) {
    bool a_imm = bignum_is_immediate(a);
    bool b_imm = bignum_is_immediate(b);

    if (a_imm && b_imm) {
        uint64_t va = bignum_get_immediate(a);
        uint64_t vb = bignum_get_immediate(b);
        if (va < vb) return -1;
        if (va > vb) return 1;
        return 0;
    }

    if (a_imm) return -1;  // a is small, b is large
    if (b_imm) return 1;   // a is large, b is small

    BigLimbs *al = bignum_get_ptr(a);
    BigLimbs *bl = bignum_get_ptr(b);

    if (al->len != bl->len) {
        return (al->len > bl->len) ? 1 : -1;
    }

    // Compare from most significant limb
    for (int i = (int)al->len - 1; i >= 0; i--) {
        if (al->limbs[i] > bl->limbs[i]) return 1;
        if (al->limbs[i] < bl->limbs[i]) return -1;
    }

    return 0;
}

bool bignum_eq(Bignum a, Bignum b) {
    return bignum_cmp(a, b) == 0;
}

// ============================================================
// Conversion
// ============================================================

bool bignum_to_u64(Bignum x, uint64_t *out) {
    if (bignum_is_immediate(x)) {
        *out = bignum_get_immediate(x);
        return true;
    }

    BigLimbs *limbs = bignum_get_ptr(x);
    if (limbs->len == 1) {
        *out = limbs->limbs[0];
        return true;
    }

    // Too large
    return false;
}

char *bignum_to_string(Bignum x) {
    if (bignum_is_zero(x)) {
        char *s = xmalloc(2);
        s[0] = '0';
        s[1] = '\0';
        return s;
    }

    if (bignum_is_immediate(x)) {
        uint64_t val = bignum_get_immediate(x);
        char *s = xmalloc(21);  // Max 20 digits for u64
        snprintf(s, 21, "%" PRIu64, val);
        return s;
    }

    // For large numbers, we need to do repeated division by 10
    // Clone the number since we'll be modifying it
    BigLimbs *limbs = bignum_get_ptr(x);
    uint32_t len = limbs->len;
    uint64_t *tmp = xmalloc(len * sizeof(uint64_t));
    memcpy(tmp, limbs->limbs, len * sizeof(uint64_t));

    // Estimate max digits: log10(2^64^n) = 64*n * log10(2) ≈ 19.3*n
    size_t max_digits = len * 20 + 1;
    char *digits = xmalloc(max_digits);
    size_t pos = 0;

    while (len > 0) {
        // Divide tmp by 10, get remainder
        uint64_t remainder = 0;
        for (int i = (int)len - 1; i >= 0; i--) {
            __uint128_t val = ((__uint128_t)remainder << 64) | tmp[i];
            tmp[i] = (uint64_t)(val / 10);
            remainder = (uint64_t)(val % 10);
        }
        digits[pos++] = '0' + (char)remainder;

        // Trim leading zeros
        while (len > 0 && tmp[len - 1] == 0) {
            len--;
        }
    }

    free(tmp);

    // Reverse the digits
    char *result = xmalloc(pos + 1);
    for (size_t i = 0; i < pos; i++) {
        result[i] = digits[pos - 1 - i];
    }
    result[pos] = '\0';
    free(digits);

    return result;
}

Bignum bignum_from_string(const char *s) {
    if (!s || !*s) return bignum_zero();

    // Skip leading zeros
    while (*s == '0' && *(s + 1)) s++;

    size_t len = strlen(s);
    if (len == 0) return bignum_zero();

    // For short strings that fit in u64, parse directly
    if (len <= 18) {
        uint64_t val = 0;
        for (size_t i = 0; i < len; i++) {
            if (s[i] < '0' || s[i] > '9') return bignum_zero();
            val = val * 10 + (uint64_t)(s[i] - '0');
        }
        return bignum_from_u64(val);
    }

    // For larger numbers, multiply-add digit by digit
    Bignum result = bignum_zero();
    Bignum ten = bignum_from_u64(10);

    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') {
            bignum_free(&result);
            return bignum_zero();
        }

        // result = result * 10 + digit
        Bignum scaled = bignum_mul(result, ten);
        bignum_free(&result);
        Bignum digit = bignum_from_u64((uint64_t)(s[i] - '0'));
        result = bignum_add(scaled, digit);
        bignum_free(&scaled);
        bignum_free(&digit);
    }

    return result;
}
