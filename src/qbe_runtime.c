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
