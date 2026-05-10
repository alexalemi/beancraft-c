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

// Maximum fan-out of a single transfer (deb A; inc D1; inc D2; ...; jmp deb).
// Longer chains are simply left unoptimized.
#define IR_OPT_MAX_DESTS 8

// Extended IR opcodes for optimized operations.
// INC/DEB/END mirror the plain IR; the rest are O(1) folds of loops.
typedef enum {
    IR_OPT_INC = 0x10,    // inc REG; goto arg_a
    IR_OPT_DEB,           // dec REG; goto (was-positive ? arg_b : arg_a)
    IR_OPT_END,           // halt

    IR_OPT_ZERO,          // reg[REG] = 0; goto arg_a
    IR_OPT_TRANSFER,      // for d in dests: reg[d] += reg[REG];  reg[REG] = 0;  goto arg_a
    IR_OPT_COPY,          // reserved (non-destructive copy); not emitted yet
} IrOptOp;

// Extended instruction for optimized IR.
typedef struct {
    IrOptOp op;
    uint32_t reg;         // primary register (src for TRANSFER, target otherwise)
    uint32_t dest_off;    // TRANSFER: start offset into IrOptProgram.dests
    uint32_t dest_count;  // TRANSFER: number of destination registers
    uint32_t arg_a;       // jump target / zero branch
    uint32_t arg_b;       // non-zero branch (for DEB)
} IrOptInst;

// Optimized program
typedef struct {
    Arena *arena;
    StrPool *strings;

    IrOptInst *insts;
    uint32_t inst_count;
    uint32_t inst_capacity;

    // Packed destination-register lists for TRANSFER instructions.
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
    PATTERN_ZERO,         // deb R exit self           -> reg[R] = 0; goto exit
    PATTERN_TRANSFER,     // deb A exit; inc D1..Dn; jmp deb -> Di += A; A = 0; goto exit
} PatternType;

// Detected pattern info
typedef struct {
    PatternType type;
    uint32_t start_inst;  // first instruction of the pattern
    uint32_t end_inst;    // one past the last instruction of the pattern
    uint32_t src_reg;     // source / cleared register
    uint32_t exit_inst;   // where to continue after the pattern completes
    uint32_t dst_regs[IR_OPT_MAX_DESTS];  // TRANSFER: the increment targets, in order
    uint32_t dst_count;
} Pattern;

// Optimize an IR program. Returns a new optimized program; the original is
// unchanged. At OPT_NONE this is a faithful 1:1 lowering of the IR.
IrOptProgram *ir_optimize(Arena *arena, const IrProgram *prog, OptLevel level);

// Debug: print optimized IR
void ir_opt_print(const IrOptProgram *prog);

// Pattern detection (internal, but exposed for testing)
Pattern ir_detect_pattern(const IrProgram *prog, uint32_t start);

#endif // BC_OPT_H
