#include "beancraft/interp.h"
#include "beancraft/devices.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Find a register index by name, or -1 if there is no such register.
static int32_t find_reg(const IrOptProgram *prog, const char *name) {
    for (uint32_t i = 0; i < prog->reg_count; i++) {
        if (strcmp(prog->reg_names[i]->data, name) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

InterpState *interp_new(Arena *arena, const IrOptProgram *prog) {
    InterpState *state = arena_alloc(arena, sizeof(InterpState));
    state->prog = prog;
    state->pc = 0;
    state->steps = 0;
    state->halted = false;
    state->inc_mask = NULL;
    state->deb_mask = NULL;
    state->stop_flag = NULL;

    state->regs = arena_alloc(arena, sizeof(Bignum) * prog->reg_count);
    for (uint32_t i = 0; i < prog->reg_count; i++) {
        state->regs[i] = bignum_zero();
    }
    return state;
}

void interp_init_regs(InterpState *state) {
    const IrOptProgram *prog = state->prog;
    for (uint32_t i = 0; i < prog->reg_count; i++) {
        if (prog->reg_init[i] != 0) {
            state->regs[i] = bignum_from_u64(prog->reg_init[i]);
        }
    }
}

bool interp_set_reg(InterpState *state, const char *name, uint64_t value) {
    int32_t idx = find_reg(state->prog, name);
    if (idx < 0) return false;
    bignum_free(&state->regs[idx]);
    state->regs[idx] = bignum_from_u64(value);
    return true;
}

bool interp_set_reg_bignum(InterpState *state, const char *name, Bignum value) {
    int32_t idx = find_reg(state->prog, name);
    if (idx < 0) return false;
    bignum_free(&state->regs[idx]);
    state->regs[idx] = bignum_clone(value);
    return true;
}

Bignum interp_get_reg(const InterpState *state, const char *name) {
    int32_t idx = find_reg(state->prog, name);
    if (idx < 0) return bignum_zero();
    return state->regs[idx];
}

void interp_step(InterpState *state) {
    if (state->halted) return;
    if (state->pc >= state->prog->inst_count) {
        state->halted = true;
        return;
    }

    const IrOptProgram *prog = state->prog;
    const IrOptInst *inst = &prog->insts[state->pc];

    switch (inst->op) {
    case IR_OPT_INC:
        if (state->inc_mask && state->inc_mask[inst->reg]) {
            device_on_inc(inst->reg);  // a device trigger: side effect, no actual increment
        } else {
            bignum_inc(&state->regs[inst->reg]);
        }
        state->pc = inst->arg_a;
        break;

    case IR_OPT_DEB:
        if (state->deb_mask && state->deb_mask[inst->reg]) {
            device_on_deb(inst->reg);  // a device poll: let it refresh its registers first
        }
        if (bignum_dec(&state->regs[inst->reg])) {
            state->pc = inst->arg_b;   // was positive, decremented
        } else {
            state->pc = inst->arg_a;   // was zero
        }
        break;

    case IR_OPT_END:
        state->halted = true;
        break;

    case IR_OPT_ZERO:
        bignum_set_zero(&state->regs[inst->reg]);
        state->pc = inst->arg_a;
        break;

    case IR_OPT_TRANSFER: {
        // No destination aliases the source (the optimizer guarantees this), so
        // the source value stays valid while we add it into each destination.
        Bignum src = state->regs[inst->reg];
        for (uint32_t i = 0; i < inst->dest_count; i++) {
            bignum_add_into(&state->regs[prog->dests[inst->dest_off + i]], src);
        }
        bignum_set_zero(&state->regs[inst->reg]);
        state->pc = inst->arg_a;
        break;
    }

    case IR_OPT_DIVMOD: {
        uint32_t k = inst->arg_b;
        uint64_t r = bignum_divmod_small(&state->regs[inst->reg], k);
        Bignum q = state->regs[inst->reg];   // now the quotient; no dest aliases it
        for (uint32_t i = 0; i < inst->dest_count; i++) {
            bignum_add_into(&state->regs[prog->dests[inst->dest_off + i]], q);
        }
        bignum_set_zero(&state->regs[inst->reg]);
        state->pc = prog->dests[inst->dest_off + inst->dest_count + r];
        break;
    }

    case IR_OPT_MULADD: {
        // Folded `for C { [T := 0;] D_i += S; T += S; S := 0; S += T; T := 0 }`.
        // After C >= 1 iterations: D_i += C*S + (C-1)*T;  S := S + T;  T := 0;  C := 0
        // (using the initial S and T). When the body re-zeroed T each round
        // (arg_b != 0) T is provably 0, so the (C-1)*T and S+=T terms drop out.
        // C == 0 is a no-op. dests = [S, T, D_1..D_m]; none of {C, S, T, D_i}
        // alias each other, so reading regs is order-safe.
        uint32_t Ci = inst->reg;
        if (!bignum_is_zero(state->regs[Ci])) {
            uint32_t Si = prog->dests[inst->dest_off];
            uint32_t Ti = prog->dests[inst->dest_off + 1];
            Bignum addend;
            if (inst->arg_b) {                                         // T precleared each round
                bignum_set_zero(&state->regs[Ti]);                     // (matches the in-body T := 0)
                addend = bignum_mul(state->regs[Si], state->regs[Ci]);  // C * S
            } else {
                Bignum Cm1 = bignum_clone(state->regs[Ci]);
                bignum_dec(&Cm1);                                      // C - 1 (C >= 1)
                Bignum cS   = bignum_mul(state->regs[Si], state->regs[Ci]);  // C * S
                Bignum cm1T = bignum_mul(state->regs[Ti], Cm1);         // (C-1) * T
                addend = bignum_add(cS, cm1T);
                bignum_add_into(&state->regs[Si], state->regs[Ti]);    // S += T (T untouched so far)
                bignum_free(&Cm1);
                bignum_free(&cS);
                bignum_free(&cm1T);
            }
            for (uint32_t d = inst->dest_off + 2; d < inst->dest_off + inst->dest_count; d++)
                bignum_add_into(&state->regs[prog->dests[d]], addend);
            bignum_set_zero(&state->regs[Ti]);                         // T := 0 (no-op if precleared)
            bignum_set_zero(&state->regs[Ci]);                         // C := 0
            bignum_free(&addend);
        }
        state->pc = inst->arg_a;
        break;
    }

    case IR_OPT_ISZERO:
        // goto (regs[reg] == 0 ? arg_a : arg_b); reg unchanged.
        state->pc = bignum_is_zero(state->regs[inst->reg]) ? inst->arg_a : inst->arg_b;
        break;

    case IR_OPT_COPY:
    default:
        // Not emitted by the current optimizer; fall through harmlessly.
        state->pc = inst->arg_a;
        break;
    }

    state->steps++;
}

void interp_run(InterpState *state, uint64_t max_steps) {
    while (!state->halted && state->steps < max_steps) {
        if (state->stop_flag && *state->stop_flag) break;
        interp_step(state);
    }
}

static const char *opt_op_name(IrOptOp op) {
    switch (op) {
    case IR_OPT_INC:      return "inc";
    case IR_OPT_DEB:      return "deb";
    case IR_OPT_END:      return "end";
    case IR_OPT_ZERO:     return "zero";
    case IR_OPT_TRANSFER: return "transfer";
    case IR_OPT_DIVMOD:   return "divmod";
    case IR_OPT_MULADD:   return "muladd";
    case IR_OPT_ISZERO:   return "iszero";
    case IR_OPT_COPY:     return "copy";
    }
    return "?";
}

void interp_run_trace(InterpState *state, uint64_t max_steps,
                      uint64_t trace_limit, FILE *out) {
    while (!state->halted && state->steps < max_steps) {
        if (state->stop_flag && *state->stop_flag) break;
        if (state->steps >= trace_limit) {
            fprintf(out, "... trace limit reached; continuing silently\n");
            interp_run(state, max_steps);
            return;
        }
        if (state->pc >= state->prog->inst_count) {
            interp_step(state);
            continue;
        }
        const IrOptInst *in = &state->prog->insts[state->pc];
        uint32_t pc = state->pc;

        if (in->op == IR_OPT_END) {
            fprintf(out, "%8" PRIu64 "  @%-4u end => halt\n", state->steps, pc);
            interp_step(state);
            continue;
        }

        const char *rn = state->prog->reg_names[in->reg]->data;
        char *before = bignum_to_string(state->regs[in->reg]);
        interp_step(state);
        char *after = bignum_to_string(state->regs[in->reg]);
        fprintf(out, "%8" PRIu64 "  @%-4u %-8s %s: %s -> %s   => @%u\n",
                state->steps - 1, pc, opt_op_name(in->op), rn, before, after,
                state->pc);
        free(before);
        free(after);
    }
}

void interp_print_state(const InterpState *state) {
    printf("PC: %u, Steps: %" PRIu64 ", Halted: %s\n",
           state->pc, state->steps, state->halted ? "yes" : "no");
}

void interp_print_regs(const InterpState *state) {
    const IrOptProgram *prog = state->prog;
    for (uint32_t i = 0; i < prog->reg_count; i++) {
        const char *name = prog->reg_names[i]->data;
        if (name[0] == ':') continue;  // skip internal registers like :nil
        char *value = bignum_to_string(state->regs[i]);
        printf("%s = %s\n", name, value);
        free(value);
    }
}

void interp_cleanup(InterpState *state) {
    for (uint32_t i = 0; i < state->prog->reg_count; i++) {
        bignum_free(&state->regs[i]);
    }
}
