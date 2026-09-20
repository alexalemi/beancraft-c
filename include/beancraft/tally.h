#ifndef BC_TALLY_H
#define BC_TALLY_H

#include "ir.h"
#include "bignum.h"

// The tally encoding read by examples/tally.bc (see docs/LANGUAGE.md):
//   a field holding n is n one-bits then a zero, laid end to end LSB first;
//   give r -> a  is (2p, a);  take r -> a / b  is (2p+1, a, b);  stop is 0;
//   p is the prime standing for register r and a target is a field offset.
// The register bank is one number  prod_r  p_r ^ value_r.
//
// Before encoding, the program is cleaned: bare-label no-ops (`take :nil X X`)
// are threaded away, unreachable instructions dropped, and primes handed out
// by how often a register is named (most-named gets 2), so the encoding is as
// short as the program allows. `prime_of[r]` is 0 for a register that no
// surviving instruction names.
typedef struct {
    const IrProgram *prog;
    IrInst *insts;          // the cleaned program (targets renumbered)
    uint32_t inst_count;
    uint32_t *field_off;    // inst -> its first field's offset; [inst_count] = one past the end
    uint64_t *prime_of;     // register -> prime (0 = unused)
    uint32_t *by_prime;     // registers in prime order (index i has the i-th smallest prime)
    uint32_t used_count;
    size_t bit_count;       // length of the program's bit string
} TallyLayout;

void   tally_layout(Arena *arena, const IrProgram *prog, TallyLayout *out);
Bignum tally_program(const TallyLayout *l);                       // P
Bignum tally_bank(const TallyLayout *l, const Bignum *values);    // R from per-register values
// Factor R against the layout's primes: values[r] := exponent of prime_of[r]
// (0 for unused registers); returns what is left of R (1 when fully decoded).
Bignum tally_decode(const TallyLayout *l, Bignum R, Bignum *values);

uint64_t tally_nth_prime(uint32_t n);   // 0 -> 2, 1 -> 3, 2 -> 5, ...

#endif // BC_TALLY_H
