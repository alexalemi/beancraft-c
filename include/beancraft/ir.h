#ifndef BC_IR_H
#define BC_IR_H

#include "arena.h"
#include "str.h"
#include "ast.h"
#include "error.h"
#include <stdint.h>
#include <stdbool.h>

// Opcodes for the IR
typedef enum {
    IR_INC,   // inc REG NEXT
    IR_DEB,   // deb REG JUMP NEXT
    IR_END,   // halt
} IrOp;

// A single IR instruction (after label resolution)
typedef struct {
    IrOp op;
    uint32_t reg;     // Register index
    uint32_t arg_a;   // INC: next addr, DEB: jump-if-zero addr
    uint32_t arg_b;   // DEB: next-after-dec addr
} IrInst;

// The complete program in IR form
typedef struct {
    Arena *arena;
    StrPool *strings;

    // Instructions
    IrInst *insts;
    uint32_t inst_count;
    uint32_t inst_capacity;

    // Register info
    Str **reg_names;       // Index -> name
    uint64_t *reg_init;    // Initial values (0 if unset)
    uint32_t reg_count;
    uint32_t reg_capacity;

    // Label table (for error messages)
    Str **label_names;     // Index -> label name (NULL if none)
} IrProgram;

// Create a new empty IR program
IrProgram *ir_new(Arena *arena, StrPool *strings);

// Lower AST to IR (resolves labels, assigns register indices)
// This is a simple lowering without module expansion
BcResult ir_from_ast(Arena *arena, StrPool *strings, const Ast *ast);

// Find register index by name (-1 if not found)
int32_t ir_find_reg(const IrProgram *prog, const char *name);

// Set register initial value
void ir_set_reg_init(IrProgram *prog, uint32_t reg_idx, uint64_t value);

// Add the :nil register (used for use statements)
void ir_add_nil_reg(IrProgram *prog);

// Debug: print IR
void ir_print(const IrProgram *prog);

#endif // BC_IR_H
