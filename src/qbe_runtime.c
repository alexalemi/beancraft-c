// Runtime support for QBE-compiled Beancraft programs.
//
// The generated code keeps each register as a Bignum (a pointer-tagged 64-bit
// word; see beancraft/bignum.h) and calls these C functions for the operations
// it can't express directly. They are thin wrappers over the same arbitrary-
// precision implementation the interpreter uses, so a compiled program and the
// interpreter agree on every result.

#include "beancraft/bignum.h"
#include "beancraft/devices.h"
#include <stdint.h>

// --- operations invoked from generated code -------------------------------

// `inc R` where R is a device inc-trigger: fire its side effect (no increment).
void bc_dev_inc(uint64_t reg) { device_on_inc((uint32_t)reg); }
// `deb R` where R is a device poll register: let the device refresh its regs.
void bc_dev_deb(uint64_t reg) { device_on_deb((uint32_t)reg); }


void bc_inc(Bignum *reg) {
    bignum_inc(reg);
}

// Returns 1 if the register was positive (and has been decremented), 0 if zero.
int bc_dec(Bignum *reg) {
    return bignum_dec(reg) ? 1 : 0;
}

void bc_zero(Bignum *reg) {
    bignum_set_zero(reg);
}

// dst += src  (a TRANSFER fans this out over its destinations, then zeroes src)
void bc_add_into(Bignum *dst, Bignum src) {
    bignum_add_into(dst, src);
}

// reg := floor(reg / k); returns reg mod k. (k >= 2; a DIVMOD calls this, then
// fans the quotient out over its destinations, zeroes reg, and branches on the
// remainder.)
uint64_t bc_divmod(Bignum *reg, uint64_t k) {
    return bignum_divmod_small(reg, k);
}

// The folded multiply loop `for C { [T := 0;] TRANSFER S->{D_1..D_m, T}; TRANSFER T->{S} }`.
// `regs` is the register file; c/s/t are register indices; `dests` holds the m
// accumulator indices (may be NULL when m == 0). After C >= 1 iterations:
//   D_i += C*S + (C-1)*T   (using the initial S, T);  S := S + T;  T := 0;  C := 0.
// When `preclear` is set the body re-zeroed T each round, so T is provably 0 and
// the (C-1)*T term -- and the `S += T` -- drop out. C == 0 is a no-op. None of
// {C, S, T, D_i} alias (the optimizer guarantees it).
void bc_muladd(Bignum *regs, uint64_t c, uint64_t s, uint64_t t,
               const uint64_t *dests, uint64_t m, uint64_t preclear) {
    if (bignum_is_zero(regs[c])) return;
    Bignum addend;
    if (preclear) {
        bignum_set_zero(&regs[t]);              // matches the in-body `T := 0`
        addend = bignum_mul(regs[s], regs[c]);  // C * S
    } else {
        Bignum Cm1 = bignum_clone(regs[c]);
        bignum_dec(&Cm1);                       // C - 1 (C >= 1)
        Bignum cS   = bignum_mul(regs[s], regs[c]);  // C * S
        Bignum cm1T = bignum_mul(regs[t], Cm1);  // (C-1) * T
        addend = bignum_add(cS, cm1T);
        bignum_add_into(&regs[s], regs[t]);     // S += T (T untouched so far)
        bignum_free(&Cm1);
        bignum_free(&cS);
        bignum_free(&cm1T);
    }
    for (uint64_t i = 0; i < m; i++)
        bignum_add_into(&regs[dests[i]], addend);
    bignum_set_zero(&regs[t]);                  // T := 0 (no-op if precleared)
    bignum_set_zero(&regs[c]);                  // C := 0
    bignum_free(&addend);
}

// --- helpers invoked from generated code and the driver -------------------

// Numeric view of a register: the exact value if it fits in 64 bits, otherwise
// UINT64_MAX. (The driver prints via bc_bignum_to_string and never truncates.)
uint64_t bc_bignum_to_u64(Bignum x) {
    uint64_t out;
    return bignum_to_u64(x, &out) ? out : UINT64_MAX;
}

// Decimal string for a register value (caller frees with free()).
char *bc_bignum_to_string(Bignum x) {
    return bignum_to_string(x);
}

// Initialise a register to a given small value (used for REG=VALUE arguments).
void bc_init_reg(Bignum *reg, uint64_t value) {
    *reg = bignum_from_u64(value);
}

// Initialise a register from a decimal string -- arbitrary precision, so the
// huge Gödel numbers from `beancraft --emit-urm` work too.
void bc_init_reg_str(Bignum *reg, const char *s) {
    *reg = bignum_from_string(s);
}
