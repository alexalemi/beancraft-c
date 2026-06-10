#include "beancraft/bignum.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define TEST(name) static void test_##name(void)
#define RUN(name) do { \
    printf("  " #name "..."); \
    fflush(stdout); \
    test_##name(); \
    printf(" OK\n"); \
} while(0)

// ============================================================
// Basic tests
// ============================================================

TEST(zero) {
    Bignum x = bignum_zero();
    assert(bignum_is_zero(x));
    assert(bignum_is_immediate(x));

    uint64_t val;
    assert(bignum_to_u64(x, &val));
    assert(val == 0);
}

TEST(from_u64_small) {
    Bignum x = bignum_from_u64(42);
    assert(bignum_is_immediate(x));
    assert(!bignum_is_zero(x));

    uint64_t val;
    assert(bignum_to_u64(x, &val));
    assert(val == 42);
}

TEST(from_u64_large) {
    // A value that doesn't fit in immediate
    uint64_t large = BIGNUM_MAX_IMMEDIATE + 100;
    Bignum x = bignum_from_u64(large);
    assert(!bignum_is_immediate(x));

    uint64_t val;
    assert(bignum_to_u64(x, &val));
    assert(val == large);

    bignum_free(&x);
}

// ============================================================
// Increment tests
// ============================================================

TEST(inc_simple) {
    Bignum x = bignum_zero();
    assert(bignum_inc(&x));
    assert(!bignum_is_zero(x));

    uint64_t val;
    assert(bignum_to_u64(x, &val));
    assert(val == 1);
}

TEST(inc_many) {
    Bignum x = bignum_zero();
    for (int i = 0; i < 1000; i++) {
        assert(bignum_inc(&x));
    }

    uint64_t val;
    assert(bignum_to_u64(x, &val));
    assert(val == 1000);
    assert(bignum_is_immediate(x));
}

TEST(inc_overflow_to_heap) {
    Bignum x = bignum_from_u64(BIGNUM_MAX_IMMEDIATE);
    assert(bignum_is_immediate(x));

    assert(bignum_inc(&x));
    assert(!bignum_is_immediate(x));  // Now on heap

    uint64_t val;
    assert(bignum_to_u64(x, &val));
    assert(val == BIGNUM_MAX_IMMEDIATE + 1);

    bignum_free(&x);
}

// ============================================================
// Decrement tests
// ============================================================

TEST(dec_simple) {
    Bignum x = bignum_from_u64(5);
    assert(bignum_dec(&x));

    uint64_t val;
    assert(bignum_to_u64(x, &val));
    assert(val == 4);
}

TEST(dec_zero_returns_false) {
    Bignum x = bignum_zero();
    assert(!bignum_dec(&x));
    assert(bignum_is_zero(x));
}

TEST(dec_to_zero) {
    Bignum x = bignum_from_u64(1);
    assert(bignum_dec(&x));
    assert(bignum_is_zero(x));
}

TEST(dec_demote_from_heap) {
    // Start just above immediate threshold
    Bignum x = bignum_from_u64(BIGNUM_MAX_IMMEDIATE + 1);
    assert(!bignum_is_immediate(x));

    assert(bignum_dec(&x));
    assert(bignum_is_immediate(x));  // Should demote

    uint64_t val;
    assert(bignum_to_u64(x, &val));
    assert(val == BIGNUM_MAX_IMMEDIATE);
}

// ============================================================
// Roundtrip tests
// ============================================================

TEST(inc_dec_roundtrip) {
    Bignum x = bignum_zero();

    for (int i = 0; i < 10000; i++) {
        assert(bignum_inc(&x));
    }

    for (int i = 0; i < 10000; i++) {
        assert(bignum_dec(&x));
    }

    assert(bignum_is_zero(x));
    assert(!bignum_dec(&x));  // Can't go below zero
}

TEST(inc_dec_roundtrip_through_heap) {
    Bignum x = bignum_from_u64(BIGNUM_MAX_IMMEDIATE - 5);

    // Go up past the immediate threshold
    for (int i = 0; i < 100; i++) {
        assert(bignum_inc(&x));
    }
    assert(!bignum_is_immediate(x));

    // Come back down
    for (int i = 0; i < 100; i++) {
        assert(bignum_dec(&x));
    }
    assert(bignum_is_immediate(x));

    uint64_t val;
    assert(bignum_to_u64(x, &val));
    assert(val == BIGNUM_MAX_IMMEDIATE - 5);
}

// ============================================================
// Addition tests
// ============================================================

TEST(add_simple) {
    Bignum a = bignum_from_u64(10);
    Bignum b = bignum_from_u64(20);
    Bignum c = bignum_add(a, b);

    uint64_t val;
    assert(bignum_to_u64(c, &val));
    assert(val == 30);

    bignum_free(&c);
}

TEST(add_into) {
    Bignum dst = bignum_from_u64(100);
    Bignum src = bignum_from_u64(50);

    bignum_add_into(&dst, src);

    uint64_t val;
    assert(bignum_to_u64(dst, &val));
    assert(val == 150);

    bignum_free(&dst);
    bignum_free(&src);
}

TEST(add_overflow) {
    // Adding two large immediates that overflow to heap
    Bignum a = bignum_from_u64(BIGNUM_MAX_IMMEDIATE);
    Bignum b = bignum_from_u64(BIGNUM_MAX_IMMEDIATE);
    Bignum c = bignum_add(a, b);

    assert(!bignum_is_immediate(c));

    // Result should be 2 * BIGNUM_MAX_IMMEDIATE
    // This fits in u64 but not in immediate
    uint64_t val;
    assert(bignum_to_u64(c, &val));
    assert(val == 2 * BIGNUM_MAX_IMMEDIATE);

    bignum_free(&c);
}

// ============================================================
// Multiplication tests
// ============================================================

TEST(mul_zero) {
    Bignum a = bignum_from_u64(0), b = bignum_from_u64(0);
    Bignum c = bignum_mul(a, b);
    assert(bignum_is_zero(c));
    bignum_free(&c);

    Bignum x = bignum_from_u64(5), y = bignum_from_u64(0);
    Bignum z = bignum_mul(x, y);
    assert(bignum_is_zero(z));
    bignum_free(&z);

    Bignum p = bignum_from_u64(0), q = bignum_from_u64(5);
    Bignum r = bignum_mul(p, q);
    assert(bignum_is_zero(r));
    bignum_free(&r);
}

TEST(mul_small) {
    struct { uint64_t a, b, prod; } cases[] = {
        { 1, 1, 1 }, { 7, 8, 56 }, { 12, 12, 144 }, { 100, 200, 20000 },
        { 13, 17, 221 }, { 1, 123456789, 123456789 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Bignum a = bignum_from_u64(cases[i].a);
        Bignum b = bignum_from_u64(cases[i].b);
        Bignum c = bignum_mul(a, b);
        uint64_t val;
        assert(bignum_to_u64(c, &val));
        assert(val == cases[i].prod);
        // Operands unchanged.
        assert(bignum_eq(a, bignum_from_u64(cases[i].a)));
        assert(bignum_eq(b, bignum_from_u64(cases[i].b)));
        bignum_free(&c);
    }
}

TEST(mul_immediate_overflows_64) {
    // 0xFFFFFFFF * 0xFFFFFFFF = 0xFFFFFFFE00000001, still fits in u64 but not in
    // an immediate (which is only 63 bits).
    Bignum a = bignum_from_u64(0xFFFFFFFFULL);
    Bignum b = bignum_from_u64(0xFFFFFFFFULL);
    Bignum c = bignum_mul(a, b);
    uint64_t val;
    assert(bignum_to_u64(c, &val));
    assert(val == 0xFFFFFFFE00000001ULL);
    bignum_free(&c);

    // A genuinely 128-bit product: (2^63 - 1) * 4 = 2^65 - 4 -> 2 limbs.
    Bignum x = bignum_from_u64(BIGNUM_MAX_IMMEDIATE);  // 2^62 - 1
    Bignum y = bignum_from_u64(8);
    Bignum z = bignum_mul(x, y);                       // 2^65 - 8, > 2^64
    assert(!bignum_is_immediate(z));
    assert(!bignum_to_u64(z, &val));                   // doesn't fit in 64 bits
    // Check via string: (2^62 - 1) * 8 = 2^65 - 8 = 36893488147419103224
    char *s = bignum_to_string(z);
    assert(strcmp(s, "36893488147419103224") == 0);
    free(s);
    bignum_free(&z);
}

TEST(mul_heap_operands) {
    // A ~30-digit number times another, checked against the known product.
    Bignum a = bignum_from_string("123456789012345678901234567890");
    Bignum b = bignum_from_string("987654321098765432109876543210");
    assert(!bignum_is_immediate(a));
    assert(!bignum_is_immediate(b));
    Bignum c = bignum_mul(a, b);
    char *s = bignum_to_string(c);
    // 123456789012345678901234567890 * 987654321098765432109876543210
    assert(strcmp(s, "121932631137021795226185032733622923332237463801111263526900") == 0);
    free(s);
    bignum_free(&a);
    bignum_free(&b);
    bignum_free(&c);

    // heap * immediate
    Bignum big = bignum_from_string("100000000000000000000");  // 10^20
    Bignum two = bignum_from_u64(2);
    Bignum d = bignum_mul(big, two);
    char *ds = bignum_to_string(d);
    assert(strcmp(ds, "200000000000000000000") == 0);
    free(ds);
    bignum_free(&big);
    bignum_free(&d);
}

// ============================================================
// String conversion tests
// ============================================================

TEST(to_string_zero) {
    Bignum x = bignum_zero();
    char *s = bignum_to_string(x);
    assert(strcmp(s, "0") == 0);
    free(s);
}

TEST(to_string_small) {
    Bignum x = bignum_from_u64(12345);
    char *s = bignum_to_string(x);
    assert(strcmp(s, "12345") == 0);
    free(s);
}

TEST(from_string_simple) {
    Bignum x = bignum_from_string("12345");

    uint64_t val;
    assert(bignum_to_u64(x, &val));
    assert(val == 12345);
}

TEST(from_string_zero) {
    Bignum x = bignum_from_string("0");
    assert(bignum_is_zero(x));
}

TEST(string_roundtrip) {
    Bignum x = bignum_from_u64(9876543210ULL);
    char *s = bignum_to_string(x);
    Bignum y = bignum_from_string(s);

    assert(bignum_eq(x, y));

    free(s);
    bignum_free(&y);
}

// ============================================================
// divmod_small tests (backs the interpreter's DIVMOD fold and the
// devices' byte extraction)
// ============================================================

TEST(divmod_small_immediate) {
    Bignum x = bignum_from_u64(1234567);
    uint64_t rem = bignum_divmod_small(&x, 10);
    assert(rem == 7);
    uint64_t q;
    assert(bignum_to_u64(x, &q));
    assert(q == 123456);
}

TEST(divmod_small_by_256) {
    Bignum x = bignum_from_u64(0xCAFEu);
    assert(bignum_divmod_small(&x, 256) == 0xFE);
    uint64_t q;
    assert(bignum_to_u64(x, &q));
    assert(q == 0xCA);
}

TEST(divmod_small_multi_limb) {
    // x = 10 * (2^64 + 3) + 7: dividing by 10 exercises the cross-limb
    // remainder carry and leaves a two-limb quotient.
    Bignum x = bignum_from_u64(UINT64_MAX);  // 2^64 - 1, heap
    assert(!bignum_is_immediate(x));
    for (int i = 0; i < 4; i++) assert(bignum_inc(&x));   // 2^64 + 3
    Bignum ten = bignum_from_u64(10);
    Bignum prod = bignum_mul(x, ten);                     // 10*2^64 + 30
    for (int i = 0; i < 7; i++) assert(bignum_inc(&prod));// 10*2^64 + 37

    uint64_t rem = bignum_divmod_small(&prod, 10);
    assert(rem == 7);
    assert(bignum_eq(prod, x));   // quotient = 2^64 + 3

    bignum_free(&x);
    bignum_free(&prod);
}

TEST(divmod_small_demotes_quotient) {
    // A heap value divided down to immediate range must demote (bignum_cmp
    // and the IS_ZERO codegen rely on heap values never fitting an immediate).
    Bignum x = bignum_from_u64(UINT64_MAX);
    assert(!bignum_is_immediate(x));
    uint64_t rem = bignum_divmod_small(&x, 1u << 16);
    assert(rem == 0xFFFF);
    assert(bignum_is_immediate(x));
    uint64_t q;
    assert(bignum_to_u64(x, &q));
    assert(q == UINT64_MAX >> 16);
}

// ============================================================
// Limb-boundary tests: inc/dec across 2^64
// ============================================================

TEST(inc_crosses_limb_boundary) {
    // 2^64 - 1 is a single (heap) limb; one more inc must grow to two limbs.
    Bignum x = bignum_from_u64(UINT64_MAX);
    assert(bignum_inc(&x));
    uint64_t back;
    assert(!bignum_to_u64(x, &back));   // 2^64 no longer fits a u64

    char *s = bignum_to_string(x);
    assert(strcmp(s, "18446744073709551616") == 0);   // 2^64
    free(s);
    bignum_free(&x);
}

TEST(dec_crosses_limb_boundary) {
    Bignum x = bignum_from_u64(UINT64_MAX);
    assert(bignum_inc(&x));             // 2^64, two limbs
    assert(bignum_dec(&x));             // back to 2^64 - 1
    uint64_t back;
    assert(bignum_to_u64(x, &back));
    assert(back == UINT64_MAX);
    bignum_free(&x);
}

TEST(add_carry_creates_new_top_limb) {
    // (2^128 - 1) + 1 = 2^128: carry must propagate through both limbs and
    // append a third.
    Bignum max = bignum_from_u64(UINT64_MAX);            // 2^64 - 1
    Bignum sq = bignum_mul(max, max);                    // 2^128 - 2^65 + 1
    Bignum t1 = bignum_add(sq, max);                     // + 2^64 - 1
    Bignum all_ones = bignum_add(t1, max);               // 2^128 - 1
    Bignum one = bignum_from_u64(1);
    Bignum sum = bignum_add(all_ones, one);              // 2^128

    char *s = bignum_to_string(sum);
    assert(strcmp(s, "340282366920938463463374607431768211456") == 0);  // 2^128
    free(s);

    bignum_free(&max); bignum_free(&sq); bignum_free(&t1);
    bignum_free(&all_ones); bignum_free(&sum);
}

TEST(clone_heap_value) {
    Bignum x = bignum_from_u64(UINT64_MAX);
    assert(bignum_inc(&x));
    Bignum y = bignum_clone(x);
    assert(bignum_eq(x, y));
    assert(bignum_dec(&y));             // mutating the clone...
    assert(!bignum_eq(x, y));           // ...must not touch the original
    bignum_free(&x);
    bignum_free(&y);
}

// ============================================================
// Comparison tests
// ============================================================

TEST(cmp_equal) {
    Bignum a = bignum_from_u64(42);
    Bignum b = bignum_from_u64(42);
    assert(bignum_cmp(a, b) == 0);
    assert(bignum_eq(a, b));
}

TEST(cmp_less) {
    Bignum a = bignum_from_u64(10);
    Bignum b = bignum_from_u64(20);
    assert(bignum_cmp(a, b) < 0);
    assert(!bignum_eq(a, b));
}

TEST(cmp_greater) {
    Bignum a = bignum_from_u64(100);
    Bignum b = bignum_from_u64(50);
    assert(bignum_cmp(a, b) > 0);
}

// ============================================================
// Transfer semantics (dst += src; src = 0) -- as used by the QBE backend
// ============================================================

TEST(transfer) {
    Bignum src = bignum_from_u64(100);
    Bignum dst = bignum_from_u64(50);

    bignum_add_into(&dst, src);
    bignum_set_zero(&src);

    uint64_t dst_val, src_val;
    assert(bignum_to_u64(dst, &dst_val));
    assert(bignum_to_u64(src, &src_val));

    assert(dst_val == 150);
    assert(src_val == 0);
    assert(bignum_is_zero(src));
}

// ============================================================
// Main
// ============================================================

int main(void) {
    printf("Running bignum tests:\n");

    RUN(zero);
    RUN(from_u64_small);
    RUN(from_u64_large);

    RUN(inc_simple);
    RUN(inc_many);
    RUN(inc_overflow_to_heap);

    RUN(dec_simple);
    RUN(dec_zero_returns_false);
    RUN(dec_to_zero);
    RUN(dec_demote_from_heap);

    RUN(inc_dec_roundtrip);
    RUN(inc_dec_roundtrip_through_heap);

    RUN(add_simple);
    RUN(add_into);
    RUN(add_overflow);

    RUN(mul_zero);
    RUN(mul_small);
    RUN(mul_immediate_overflows_64);
    RUN(mul_heap_operands);

    RUN(to_string_zero);
    RUN(to_string_small);
    RUN(from_string_simple);
    RUN(from_string_zero);
    RUN(string_roundtrip);

    RUN(divmod_small_immediate);
    RUN(divmod_small_by_256);
    RUN(divmod_small_multi_limb);
    RUN(divmod_small_demotes_quotient);

    RUN(inc_crosses_limb_boundary);
    RUN(dec_crosses_limb_boundary);
    RUN(add_carry_creates_new_top_limb);
    RUN(clone_heap_value);

    RUN(cmp_equal);
    RUN(cmp_less);
    RUN(cmp_greater);

    RUN(transfer);

    printf("\nAll bignum tests passed!\n");
    return 0;
}
