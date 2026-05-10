#include "beancraft/opt.h"
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Pattern detection
// ---------------------------------------------------------------------------

static bool is_deb(const IrProgram *prog, uint32_t idx) {
    return idx < prog->inst_count && prog->insts[idx].op == IR_DEB;
}

static bool is_inc(const IrProgram *prog, uint32_t idx) {
    return idx < prog->inst_count && prog->insts[idx].op == IR_INC;
}

// Is the instruction at idx the target of a jump originating outside [lo, hi)?
// (Jumps from within the range, plus the instruction at lo itself, are fine -
// those are the loop's own back-edges.)  Instruction 0 is the program entry.
static bool targeted_from_outside(const IrProgram *prog, uint32_t idx,
                                  uint32_t lo, uint32_t hi) {
    if (idx == 0) return true;
    for (uint32_t i = 0; i < prog->inst_count; i++) {
        if (i >= lo && i < hi) continue;
        const IrInst *in = &prog->insts[i];
        if (in->op == IR_INC && in->arg_a == idx) return true;
        if (in->op == IR_DEB && (in->arg_a == idx || in->arg_b == idx)) return true;
    }
    return false;
}

// ZERO:  deb R exit self   ->   reg[R] = 0; goto exit
static Pattern detect_zero_pattern(const IrProgram *prog, uint32_t start) {
    Pattern p = { .type = PATTERN_NONE };
    if (!is_deb(prog, start)) return p;

    const IrInst *deb = &prog->insts[start];
    if (deb->arg_b != start) return p;   // the "decremented" branch must loop back here

    p.type = PATTERN_ZERO;
    p.start_inst = start;
    p.end_inst = start + 1;
    p.src_reg = deb->reg;
    p.exit_inst = deb->arg_a;
    p.dst_count = 0;
    return p;
}

// TRANSFER:  deb A exit; inc D1; inc D2; ...; inc Dn; jmp deb
//   ->  D1 += A; D2 += A; ...; Dn += A;  A = 0;  goto exit
// The increments must be the contiguous block immediately after the deb, and
// none of them may be the deb's own register (that would never terminate).
static Pattern detect_transfer_pattern(const IrProgram *prog, uint32_t start) {
    Pattern p = { .type = PATTERN_NONE };
    if (!is_deb(prog, start)) return p;

    const IrInst *deb = &prog->insts[start];
    if (deb->arg_b != start + 1) return p;   // decremented branch enters the inc chain

    uint32_t n = 0;
    for (uint32_t idx = start + 1; ; idx++) {
        if (!is_inc(prog, idx)) return p;
        const IrInst *inc = &prog->insts[idx];
        if (inc->reg == deb->reg) return p;          // inc of the source -> not a transfer
        if (n >= IR_OPT_MAX_DESTS) return p;         // chain too long; leave it alone
        p.dst_regs[n++] = inc->reg;
        if (inc->arg_a == start) break;              // last inc loops back to the deb
        if (inc->arg_a != idx + 1) return p;         // otherwise the run must continue
    }

    uint32_t end_inst = start + 1 + n;
    for (uint32_t j = start + 1; j < end_inst; j++) {
        if (targeted_from_outside(prog, j, start, end_inst)) return p;
    }

    p.type = PATTERN_TRANSFER;
    p.start_inst = start;
    p.end_inst = end_inst;
    p.src_reg = deb->reg;
    p.exit_inst = deb->arg_a;
    p.dst_count = n;
    return p;
}

Pattern ir_detect_pattern(const IrProgram *prog, uint32_t start) {
    Pattern p = detect_transfer_pattern(prog, start);
    if (p.type != PATTERN_NONE) return p;
    return detect_zero_pattern(prog, start);
}

// ---------------------------------------------------------------------------
// Building the optimized program
// ---------------------------------------------------------------------------

static IrOptProgram *opt_prog_new(Arena *arena, const IrProgram *prog) {
    IrOptProgram *opt = arena_alloc(arena, sizeof(IrOptProgram));
    opt->arena = arena;
    opt->strings = prog->strings;

    opt->inst_capacity = prog->inst_count;
    opt->insts = arena_alloc(arena, opt->inst_capacity * sizeof(IrOptInst));
    opt->inst_count = 0;

    // Upper bound on packed dests: every instruction could (in the worst case)
    // be an increment folded into some transfer.
    opt->dests = arena_alloc(arena, (prog->inst_count ? prog->inst_count : 1) * sizeof(uint32_t));
    opt->dest_total = 0;

    opt->reg_names = prog->reg_names;
    opt->reg_init = prog->reg_init;
    opt->reg_count = prog->reg_count;

    opt->patterns_found = 0;
    opt->instructions_removed = 0;
    return opt;
}

static void opt_emit(IrOptProgram *opt, IrOptInst inst) {
    if (opt->inst_count < opt->inst_capacity) {
        opt->insts[opt->inst_count++] = inst;
    }
}

static IrOptInst convert_inst(const IrInst *inst) {
    IrOptInst opt = { 0 };
    switch (inst->op) {
    case IR_INC:
        opt.op = IR_OPT_INC;
        opt.reg = inst->reg;
        opt.arg_a = inst->arg_a;
        break;
    case IR_DEB:
        opt.op = IR_OPT_DEB;
        opt.reg = inst->reg;
        opt.arg_a = inst->arg_a;   // zero branch
        opt.arg_b = inst->arg_b;   // non-zero branch
        break;
    case IR_END:
        opt.op = IR_OPT_END;
        break;
    }
    return opt;
}

IrOptProgram *ir_optimize(Arena *arena, const IrProgram *prog, OptLevel level) {
    IrOptProgram *opt = opt_prog_new(arena, prog);

    if (level < OPT_LOOPS) {
        for (uint32_t i = 0; i < prog->inst_count; i++) {
            opt_emit(opt, convert_inst(&prog->insts[i]));
        }
        return opt;
    }

    // old instruction index -> new instruction index. Zero-initialised so that
    // instructions folded into a pattern map to instruction 0 rather than
    // garbage (in practice no surviving jump targets them).
    uint32_t *inst_map = arena_alloc_zero(arena, prog->inst_count * sizeof(uint32_t));
    bool *consumed = arena_alloc_zero(arena, prog->inst_count * sizeof(bool));

    // Pass 1: detect patterns, mark the instructions they cover as consumed.
    Pattern *patterns = arena_alloc(arena, prog->inst_count * sizeof(Pattern));
    uint32_t pattern_count = 0;
    for (uint32_t i = 0; i < prog->inst_count; i++) {
        if (consumed[i]) continue;
        Pattern p = ir_detect_pattern(prog, i);
        if (p.type == PATTERN_NONE) continue;

        patterns[pattern_count++] = p;
        opt->patterns_found++;
        for (uint32_t j = p.start_inst; j < p.end_inst; j++) consumed[j] = true;
        opt->instructions_removed += (p.end_inst - p.start_inst) - 1;  // -1: the fold itself stays
    }

    // Pass 2: emit instructions, recording where each old index lands.
    uint32_t pattern_idx = 0;
    for (uint32_t i = 0; i < prog->inst_count; i++) {
        inst_map[i] = opt->inst_count;

        if (pattern_idx < pattern_count && patterns[pattern_idx].start_inst == i) {
            Pattern *p = &patterns[pattern_idx++];
            IrOptInst out = { 0 };
            out.arg_a = p->exit_inst;   // remapped in pass 3
            switch (p->type) {
            case PATTERN_ZERO:
                out.op = IR_OPT_ZERO;
                out.reg = p->src_reg;
                break;
            case PATTERN_TRANSFER:
                out.op = IR_OPT_TRANSFER;
                out.reg = p->src_reg;
                out.dest_off = opt->dest_total;
                out.dest_count = p->dst_count;
                for (uint32_t d = 0; d < p->dst_count; d++) {
                    opt->dests[opt->dest_total++] = p->dst_regs[d];
                }
                break;
            default:
                break;
            }
            opt_emit(opt, out);
            i = p->end_inst - 1;   // skip the consumed instructions (loop will ++)
            continue;
        }

        if (!consumed[i]) opt_emit(opt, convert_inst(&prog->insts[i]));
    }

    // Pass 3: remap jump targets through inst_map.
    for (uint32_t i = 0; i < opt->inst_count; i++) {
        IrOptInst *inst = &opt->insts[i];
        if (inst->arg_a < prog->inst_count) inst->arg_a = inst_map[inst->arg_a];
        if (inst->op == IR_OPT_DEB && inst->arg_b < prog->inst_count) {
            inst->arg_b = inst_map[inst->arg_b];
        }
    }

    return opt;
}

// ---------------------------------------------------------------------------
// Debug printing
// ---------------------------------------------------------------------------

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
            printf("INC %s -> %u\n", prog->reg_names[inst->reg]->data, inst->arg_a);
            break;
        case IR_OPT_DEB:
            printf("DEB %s -> zero:%u nonzero:%u\n",
                   prog->reg_names[inst->reg]->data, inst->arg_a, inst->arg_b);
            break;
        case IR_OPT_END:
            printf("END\n");
            break;
        case IR_OPT_ZERO:
            printf("ZERO %s -> %u\n", prog->reg_names[inst->reg]->data, inst->arg_a);
            break;
        case IR_OPT_TRANSFER:
            printf("TRANSFER %s -> {", prog->reg_names[inst->reg]->data);
            for (uint32_t d = 0; d < inst->dest_count; d++) {
                printf("%s%s", d ? ", " : "",
                       prog->reg_names[prog->dests[inst->dest_off + d]]->data);
            }
            printf("}, then %u\n", inst->arg_a);
            break;
        case IR_OPT_COPY:
            printf("COPY %s -> ... (not implemented)\n", prog->reg_names[inst->reg]->data);
            break;
        default:
            printf("UNKNOWN op=%d\n", inst->op);
            break;
        }
    }
}
