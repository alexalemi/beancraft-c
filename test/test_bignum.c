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
// QBE runtime tests
// ============================================================

TEST(qbe_inc_dec) {
    Bignum reg = bignum_zero();

    assert(qbe_bignum_is_zero(reg) == 1);

    qbe_bignum_inc(&reg);
    assert(qbe_bignum_is_zero(reg) == 0);

    assert(qbe_bignum_dec(&reg) == 1);
    assert(qbe_bignum_is_zero(reg) == 1);

    assert(qbe_bignum_dec(&reg) == 0);  // Can't dec zero
}

TEST(qbe_transfer) {
    Bignum src = bignum_from_u64(100);
    Bignum dst = bignum_from_u64(50);

    qbe_bignum_transfer(&dst, &src);

    uint64_t dst_val, src_val;
    assert(bignum_to_u64(dst, &dst_val));
    assert(bignum_to_u64(src, &src_val));

    assert(dst_val == 150);
    assert(src_val == 0);
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

    RUN(to_string_zero);
    RUN(to_string_small);
    RUN(from_string_simple);
    RUN(from_string_zero);
    RUN(string_roundtrip);

    RUN(cmp_equal);
    RUN(cmp_less);
    RUN(cmp_greater);

    RUN(qbe_inc_dec);
    RUN(qbe_transfer);

    printf("\nAll bignum tests passed!\n");
    return 0;
}
