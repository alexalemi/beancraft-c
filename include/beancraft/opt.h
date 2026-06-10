#ifndef BC_OPT_H
#define BC_OPT_H

#include "ir.h"
#include "error.h"

// Optimization levels
typedef enum {
    OPT_NONE = 0,      // No optimization
    OPT_BASIC = 1,     // Basic optimizations (dead code, etc.)
    OPT_LOOPS = 2,     // Loop pattern recognition
    OPT_FULL = 3,      // All optimizations
} OptLevel;

// Cap on the fan-out of a TRANSFER (deb A; inc D1..Dn; jmp deb) and on the
// divisor of a DIVMOD (k contiguous `deb R`). Larger ones are left unoptimized.
#define IR_OPT_MAX_DESTS 32

// Extended IR opcodes for optimized operations.
// INC/DEB/END mirror the plain IR; the rest are O(1) folds of loops.
typedef enum {
    IR_OPT_INC = 0x10,    // inc REG; goto arg_a
    IR_OPT_DEB,           // dec REG; goto (was-positive ? arg_b : arg_a)
    IR_OPT_END,           // halt

    IR_OPT_ZERO,          // reg[REG] = 0; goto arg_a
    IR_OPT_TRANSFER,      // for d in dests: reg[d] += reg[REG];  reg[REG] = 0;  goto arg_a
    IR_OPT_DIVMOD,        // r = reg[REG] mod K;  for d in dests: reg[d] += reg[REG] / K;
                          //   reg[REG] = 0;  goto exits[r]    (K = arg_b)
    IR_OPT_MULADD,        // counter REG = C, dests = [S, T, D_1..D_m] (m >= 0).
                          //   if C != 0: for d in D_i: reg[d] += C*reg[S] + (C-1)*reg[T];
                          //              reg[S] += reg[T];  reg[T] = 0;  reg[C] = 0;
                          //   goto arg_a    (the unrolled `for C { TRANSFER S->{D..,T}; TRANSFER T->{S} }`)
                          //   arg_b != 0: the loop body also began with `reg[T] = 0` each round,
                          //   so reg[T] is provably 0 -- the (C-1)*reg[T] and reg[S]+=reg[T] terms drop.
    IR_OPT_ISZERO,        // goto (reg[REG] == 0 ? arg_a : arg_b)   -- REG is left unchanged
    IR_OPT_COPY,          // non-destructive copy: dests = [T, D_1..D_m];
                          //   reg[D_i] += reg[REG];  reg[REG] += reg[T];  reg[T] = 0;  goto arg_a
                          //   (the fold of TRANSFER REG->{D...,T} ; TRANSFER T->{REG})
} IrOptOp;

// Extended instruction for optimized IR.
//
// The `dests` array of IrOptProgram is a packed pool. A TRANSFER owns the slice
// dests[dest_off .. dest_off+dest_count) holding its destination register
// indices. A DIVMOD owns dests[dest_off .. dest_off+dest_count) for its quotient
// destination registers, immediately followed by dests[.. + arg_b) holding its
// arg_b exit instruction indices (one per remainder 0..arg_b-1). A MULADD owns
// dests[dest_off .. dest_off+dest_count) holding [S, T, D_1, ..., D_m] (so
// dest_count == m + 2, m >= 0); its `reg` is the counter C and `arg_a` the exit.
typedef struct {
    IrOptOp op;
    uint32_t reg;         // primary register (src for TRANSFER/DIVMOD, target otherwise)
    uint32_t dest_off;    // TRANSFER/DIVMOD: start offset into IrOptProgram.dests
    uint32_t dest_count;  // TRANSFER/DIVMOD: number of destination registers
    uint32_t arg_a;       // jump target / zero branch
    uint32_t arg_b;       // DEB/ISZERO: non-zero branch.  DIVMOD: the divisor K.
} IrOptInst;

// Optimized program
typedef struct {
    Arena *arena;
    StrPool *strings;

    IrOptInst *insts;
    uint32_t inst_count;
    uint32_t inst_capacity;

    // Packed operand pool: TRANSFER destination registers, DIVMOD quotient
    // registers, and DIVMOD exit-instruction indices (see IrOptInst).
    uint32_t *dests;
    uint32_t dest_total;

    // Register info (shared with the source IrProgram)
    Str **reg_names;
    uint64_t *reg_init;
    uint32_t reg_count;

    // Statistics
    uint32_t patterns_found;
    uint32_t instructions_removed;
} IrOptProgram;

// Pattern types detected during analysis
typedef enum {
    PATTERN_NONE = 0,
    PATTERN_ZERO,         // deb R exit self                       -> reg[R] = 0; goto exit
    PATTERN_TRANSFER,     // deb A exit; inc D1..Dn; jmp deb        -> Di += A; A = 0; goto exit
    PATTERN_DIVMOD,       // deb R e0; .. deb R e(k-1); inc Q1..Qm; jmp deb
                          //   -> Qi += R/k;  goto e_(R mod k);  R = 0    (k >= 2)
    PATTERN_MULADD,       // deb C exit (->next); [deb T self (->next);] deb S txexit (->next);
                          //   inc D1..Dm; inc T (->deb S);  deb T (->deb C) (->next); inc S (->deb T)
                          //   -> Di += C*S + (C-1)*T;  S += T;  T = 0;  C = 0;  goto exit
                          //   (the optional leading `deb T self` is a per-round `T := 0`; when
                          //    present, T is provably 0 and the (C-1)*T / S+=T terms vanish)
    PATTERN_ISZERO,       // deb R z; inc R nz   (deb's non-zero branch falls into the inc, which
                          //   undoes the decrement)  ->  goto (R == 0 ? z : nz)   -- R unchanged
    PATTERN_COPY,         // TRANSFER S -> {D..., T} immediately followed by TRANSFER T -> {S}
                          //   (the classic non-destructive copy: move S out through a temp,
                          //    then move the temp back)  ->  D_i += S;  S += T;  T := 0
                          //   dst_regs = [T, D_1..D_m]
} PatternType;

// Detected pattern info
typedef struct {
    PatternType type;
    uint32_t start_inst;  // first instruction of the pattern
    uint32_t end_inst;    // one past the last instruction of the pattern
    uint32_t src_reg;     // source / cleared / dividend / MULADD-counter / tested register
    uint32_t exit_inst;   // TRANSFER/ZERO/MULADD: continuation;  ISZERO: the zero branch
    uint32_t exit_inst2;  // ISZERO: the non-zero branch
    uint32_t dst_regs[IR_OPT_MAX_DESTS];  // TRANSFER/DIVMOD: inc/quotient targets; MULADD: [S, T, D_1..D_m]
    uint32_t dst_count;
    uint32_t div_k;                       // DIVMOD: the divisor
    uint32_t exit_insts[IR_OPT_MAX_DESTS];// DIVMOD: continuation per remainder 0..div_k-1
    bool muladd_preclear;                 // MULADD: loop body started with `T := 0` each round
} Pattern;

// Optimize an IR program. Returns a new optimized program; the original is
// unchanged. At OPT_NONE this is a faithful 1:1 lowering of the IR.
IrOptProgram *ir_optimize(Arena *arena, const IrProgram *prog, OptLevel level);

// Debug: print optimized IR
void ir_opt_print(const IrOptProgram *prog);

// Pattern detection (internal, but exposed for testing)
Pattern ir_detect_pattern(const IrProgram *prog, uint32_t start);

#endif // BC_OPT_H
