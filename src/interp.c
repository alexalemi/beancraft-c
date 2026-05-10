#include "beancraft/interp.h"
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
        bignum_inc(&state->regs[inst->reg]);
        state->pc = inst->arg_a;
        break;

    case IR_OPT_DEB:
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
        interp_step(state);
    }
}

void interp_print_state(const InterpState *state) {
    printf("PC: %u, Steps: %lu, Halted: %s\n",
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
