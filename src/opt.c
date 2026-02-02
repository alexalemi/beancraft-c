#include "beancraft/opt.h"
#include <stdio.h>
#include <string.h>

// Helper: check if instruction at idx is a DEB
static bool is_deb(const IrProgram *prog, uint32_t idx) {
    return idx < prog->inst_count && prog->insts[idx].op == IR_DEB;
}

// Helper: check if instruction at idx is an INC
static bool is_inc(const IrProgram *prog, uint32_t idx) {
    return idx < prog->inst_count && prog->insts[idx].op == IR_INC;
}

// Helper: check if instruction at idx is END
static bool is_end(const IrProgram *prog, uint32_t idx) {
    return idx < prog->inst_count && prog->insts[idx].op == IR_END;
}

// Detect pattern: ZERO
// Pattern: deb R self X  (decrements R until zero, then jumps to X)
// This is: while(R > 0) R--; goto X
static Pattern detect_zero_pattern(const IrProgram *prog, uint32_t start) {
    Pattern p = {.type = PATTERN_NONE};

    if (!is_deb(prog, start)) return p;

    const IrInst *inst = &prog->insts[start];

    // deb R ZERO NEXT where NEXT == start (loops back to self)
    // arg_a = jump if zero, arg_b = jump if decremented
    if (inst->arg_b == start) {
        // This is: deb R exit self - loops until R is zero
        p.type = PATTERN_ZERO;
        p.start_inst = start;
        p.end_inst = start + 1;
        p.src_reg = inst->reg;
        p.exit_inst = inst->arg_a;
        return p;
    }

    return p;
}

// Detect pattern: TRANSFER
// Pattern: A loop where we decrement A and increment B each iteration
//   deb A exit loop
//   loop: inc B
//         jmp to deb (either directly or via prev)
//
// In IR form:
//   inst[i]:   DEB reg=A, arg_a=exit, arg_b=i+1
//   inst[i+1]: INC reg=B, arg_a=i
static Pattern detect_transfer_pattern(const IrProgram *prog, uint32_t start) {
    Pattern p = {.type = PATTERN_NONE};

    if (!is_deb(prog, start)) return p;

    const IrInst *deb = &prog->insts[start];
    uint32_t inc_idx = deb->arg_b;  // Where we jump after successful decrement

    // Check that the next instruction after dec is an INC
    if (!is_inc(prog, inc_idx)) return p;

    const IrInst *inc = &prog->insts[inc_idx];

    // The INC must jump back to the DEB
    if (inc->arg_a != start) return p;

    // The registers must be different
    if (deb->reg == inc->reg) return p;

    // Found a transfer pattern!
    p.type = PATTERN_TRANSFER;
    p.start_inst = start;
    p.end_inst = inc_idx + 1;
    p.src_reg = deb->reg;
    p.dst_reg = inc->reg;
    p.exit_inst = deb->arg_a;

    return p;
}

// Detect pattern: COPY (non-destructive copy via temporary)
// This is more complex - we need to find:
//   1. Transfer A -> tmp
//   2. Transfer tmp -> B and tmp -> A (restore)
//
// Common pattern in copy.bc:
//   deb tmp loop self     ; clear tmp first
//   loop: deb From refill
//         inc To
//         inc tmp
//         jmp loop
//   refill: deb tmp done
//           inc From
//           jmp refill
//
// For now, let's detect a simpler version: two consecutive transfers
// where the second one restores the source
static Pattern detect_copy_pattern(const IrProgram *prog, uint32_t start) {
    Pattern p = {.type = PATTERN_NONE};

    // First, try to find a transfer pattern
    Pattern t1 = detect_transfer_pattern(prog, start);
    if (t1.type != PATTERN_TRANSFER) return p;

    // After the first transfer completes, look for a second transfer
    // that uses the destination as source and restores to original
    uint32_t next = t1.exit_inst;

    // Actually, copy patterns are quite complex in Beancraft.
    // Let's look for the specific pattern in copy.bc:
    //
    // The real copy.bc does:
    //   deb tmp loop1 self        ; zero tmp
    //   loop1: deb From loop2     ; transfer From -> (To, tmp)
    //          inc To
    //          inc tmp
    //          jmp loop1
    //   loop2: deb tmp done       ; transfer tmp -> From (restore)
    //          inc From
    //          jmp loop2
    //
    // This is actually TWO consecutive transfer patterns where:
    // - First: From -> To AND From -> tmp (dual increment)
    // - Second: tmp -> From (restore)

    // For now, skip complex copy detection - focus on transfer
    return p;
}

// Main pattern detection entry point
Pattern ir_detect_pattern(const IrProgram *prog, uint32_t start) {
    Pattern p;

    // Try patterns in order of complexity (most specific first)

    // Copy pattern (most complex)
    p = detect_copy_pattern(prog, start);
    if (p.type != PATTERN_NONE) return p;

    // Transfer pattern
    p = detect_transfer_pattern(prog, start);
    if (p.type != PATTERN_NONE) return p;

    // Zero pattern (simplest)
    p = detect_zero_pattern(prog, start);
    if (p.type != PATTERN_NONE) return p;

    return (Pattern){.type = PATTERN_NONE};
}

// Create optimized program
static IrOptProgram *opt_prog_new(Arena *arena, const IrProgram *prog) {
    IrOptProgram *opt = arena_alloc(arena, sizeof(IrOptProgram));
    opt->arena = arena;
    opt->strings = prog->strings;

    opt->inst_capacity = prog->inst_count;
    opt->insts = arena_alloc(arena, opt->inst_capacity * sizeof(IrOptInst));
    opt->inst_count = 0;

    // Copy register info
    opt->reg_count = prog->reg_count;
    opt->reg_names = prog->reg_names;
    opt->reg_init = prog->reg_init;

    opt->patterns_found = 0;
    opt->instructions_removed = 0;

    return opt;
}

// Add instruction to optimized program
static void opt_emit(IrOptProgram *opt, IrOptInst inst) {
    if (opt->inst_count >= opt->inst_capacity) {
        // This shouldn't happen if we allocated enough
        return;
    }
    opt->insts[opt->inst_count++] = inst;
}

// Convert standard IR instruction to optimized format
static IrOptInst convert_inst(const IrInst *inst) {
    IrOptInst opt = {0};
    switch (inst->op) {
    case IR_INC:
        opt.op = IR_OPT_INC;
        opt.reg = inst->reg;
        opt.arg_a = inst->arg_a;
        break;
    case IR_DEB:
        opt.op = IR_OPT_DEB;
        opt.reg = inst->reg;
        opt.arg_a = inst->arg_a;  // zero branch
        opt.arg_b = inst->arg_b;  // non-zero branch
        break;
    case IR_END:
        opt.op = IR_OPT_END;
        break;
    }
    return opt;
}

// Optimize the program
IrOptProgram *ir_optimize(Arena *arena, const IrProgram *prog, OptLevel level) {
    IrOptProgram *opt = opt_prog_new(arena, prog);

    if (level == OPT_NONE) {
        // Just convert without optimization
        for (uint32_t i = 0; i < prog->inst_count; i++) {
            opt_emit(opt, convert_inst(&prog->insts[i]));
        }
        return opt;
    }

    // Build a map: old instruction index -> new instruction index
    // We need this because optimizations change instruction numbering
    uint32_t *inst_map = arena_alloc(arena, prog->inst_count * sizeof(uint32_t));
    bool *consumed = arena_alloc(arena, prog->inst_count * sizeof(bool));
    memset(consumed, 0, prog->inst_count * sizeof(bool));

    // First pass: detect patterns and mark consumed instructions
    typedef struct {
        Pattern pattern;
        uint32_t new_idx;  // Where the optimized instruction will be
    } PatternInfo;

    PatternInfo *patterns = arena_alloc(arena, prog->inst_count * sizeof(PatternInfo));
    uint32_t pattern_count = 0;

    for (uint32_t i = 0; i < prog->inst_count; i++) {
        if (consumed[i]) continue;

        if (level >= OPT_LOOPS) {
            Pattern p = ir_detect_pattern(prog, i);
            if (p.type != PATTERN_NONE) {
                patterns[pattern_count].pattern = p;
                patterns[pattern_count].new_idx = 0;  // Will be set in second pass
                pattern_count++;
                opt->patterns_found++;

                // Mark all instructions in pattern as consumed
                for (uint32_t j = p.start_inst; j < p.end_inst; j++) {
                    consumed[j] = true;
                    opt->instructions_removed++;
                }
                // Don't double-count the start instruction
                opt->instructions_removed--;
            }
        }
    }

    // Second pass: emit optimized instructions and build the map
    uint32_t pattern_idx = 0;
    for (uint32_t i = 0; i < prog->inst_count; i++) {
        inst_map[i] = opt->inst_count;

        // Check if this is the start of a pattern
        if (pattern_idx < pattern_count &&
            patterns[pattern_idx].pattern.start_inst == i) {

            Pattern *p = &patterns[pattern_idx].pattern;
            patterns[pattern_idx].new_idx = opt->inst_count;

            IrOptInst opt_inst = {0};
            switch (p->type) {
            case PATTERN_ZERO:
                opt_inst.op = IR_OPT_ZERO;
                opt_inst.reg = p->src_reg;
                opt_inst.arg_a = p->exit_inst;  // Will be remapped
                break;

            case PATTERN_TRANSFER:
                opt_inst.op = IR_OPT_TRANSFER;
                opt_inst.reg = p->src_reg;
                opt_inst.reg2 = p->dst_reg;
                opt_inst.arg_a = p->exit_inst;  // Will be remapped
                break;

            case PATTERN_COPY:
                opt_inst.op = IR_OPT_COPY;
                opt_inst.reg = p->src_reg;
                opt_inst.reg2 = p->dst_reg;
                opt_inst.arg_a = p->exit_inst;
                break;

            default:
                break;
            }

            opt_emit(opt, opt_inst);
            pattern_idx++;

            // Skip consumed instructions
            i = p->end_inst - 1;  // -1 because loop will increment
            continue;
        }

        // Not a pattern - emit regular instruction
        if (!consumed[i]) {
            opt_emit(opt, convert_inst(&prog->insts[i]));
        }
    }

    // Third pass: remap all jump targets
    for (uint32_t i = 0; i < opt->inst_count; i++) {
        IrOptInst *inst = &opt->insts[i];
        switch (inst->op) {
        case IR_OPT_INC:
            if (inst->arg_a < prog->inst_count) {
                inst->arg_a = inst_map[inst->arg_a];
            }
            break;
        case IR_OPT_DEB:
            if (inst->arg_a < prog->inst_count) {
                inst->arg_a = inst_map[inst->arg_a];
            }
            if (inst->arg_b < prog->inst_count) {
                inst->arg_b = inst_map[inst->arg_b];
            }
            break;
        case IR_OPT_ZERO:
        case IR_OPT_TRANSFER:
        case IR_OPT_COPY:
            if (inst->arg_a < prog->inst_count) {
                inst->arg_a = inst_map[inst->arg_a];
            }
            break;
        default:
            break;
        }
    }

    return opt;
}

// Print optimized IR
void ir_opt_print(const IrOptProgram *prog) {
    printf("Optimized IR: %u instructions, %u registers\n",
           prog->inst_count, prog->reg_count);
    printf("Patterns found: %u, Instructions removed: %u\n\n",
           prog->patterns_found, prog->instructions_removed);

    for (uint32_t i = 0; i < prog->inst_count; i++) {
        const IrOptInst *inst = &prog->insts[i];
        printf("%3u: ", i);

        switch (inst->op) {
        case IR_OPT_INC:
            printf("INC %s -> %u\n",
                   prog->reg_names[inst->reg]->data, inst->arg_a);
            break;
        case IR_OPT_DEB:
            printf("DEB %s -> zero:%u nonzero:%u\n",
                   prog->reg_names[inst->reg]->data, inst->arg_a, inst->arg_b);
            break;
        case IR_OPT_END:
            printf("END\n");
            break;
        case IR_OPT_ZERO:
            printf("ZERO %s -> %u\n",
                   prog->reg_names[inst->reg]->data, inst->arg_a);
            break;
        case IR_OPT_TRANSFER:
            printf("TRANSFER %s -> %s, then %u\n",
                   prog->reg_names[inst->reg]->data,
                   prog->reg_names[inst->reg2]->data,
                   inst->arg_a);
            break;
        case IR_OPT_COPY:
            printf("COPY %s -> %s, then %u\n",
                   prog->reg_names[inst->reg]->data,
                   prog->reg_names[inst->reg2]->data,
                   inst->arg_a);
            break;
        default:
            printf("UNKNOWN op=%d\n", inst->op);
            break;
        }
    }
}

// Convert optimized IR back to standard IR (for interpreter compatibility)
IrProgram *ir_opt_to_standard(Arena *arena, const IrOptProgram *opt) {
    // This would expand optimized ops back to sequences
    // For now, we'll implement this when needed
    (void)arena;
    (void)opt;
    return NULL;
}
