#include "beancraft/interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

InterpState *interp_new(Arena *arena, const IrProgram *prog) {
    InterpState *state = arena_alloc(arena, sizeof(InterpState));
    state->prog = prog;
    state->pc = 0;
    state->steps = 0;
    state->halted = false;

    // Allocate register array
    state->regs = arena_alloc(arena, sizeof(Bignum) * prog->reg_count);

    // Initialize all registers to zero
    for (uint32_t i = 0; i < prog->reg_count; i++) {
        state->regs[i] = bignum_zero();
    }

    return state;
}

void interp_init_regs(InterpState *state) {
    const IrProgram *prog = state->prog;
    for (uint32_t i = 0; i < prog->reg_count; i++) {
        if (prog->reg_init[i] != 0) {
            state->regs[i] = bignum_from_u64(prog->reg_init[i]);
        }
    }
}

bool interp_set_reg(InterpState *state, const char *name, uint64_t value) {
    int32_t idx = ir_find_reg(state->prog, name);
    if (idx < 0) return false;

    bignum_free(&state->regs[idx]);
    state->regs[idx] = bignum_from_u64(value);
    return true;
}

bool interp_set_reg_bignum(InterpState *state, const char *name, Bignum value) {
    int32_t idx = ir_find_reg(state->prog, name);
    if (idx < 0) return false;

    bignum_free(&state->regs[idx]);
    state->regs[idx] = bignum_clone(value);
    return true;
}

Bignum interp_get_reg(const InterpState *state, const char *name) {
    int32_t idx = ir_find_reg(state->prog, name);
    if (idx < 0) return bignum_zero();
    return state->regs[idx];
}

void interp_step(InterpState *state) {
    if (state->halted) return;
    if (state->pc >= state->prog->inst_count) {
        state->halted = true;
        return;
    }

    const IrInst *inst = &state->prog->insts[state->pc];

    switch (inst->op) {
    case IR_INC:
        bignum_inc(&state->regs[inst->reg]);
        state->pc = inst->arg_a;
        break;

    case IR_DEB:
        if (bignum_dec(&state->regs[inst->reg])) {
            // Was positive, decremented, go to next
            state->pc = inst->arg_b;
        } else {
            // Was zero, go to jump target
            state->pc = inst->arg_a;
        }
        break;

    case IR_END:
        state->halted = true;
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
    const IrProgram *prog = state->prog;

    for (uint32_t i = 0; i < prog->reg_count; i++) {
        const char *name = prog->reg_names[i]->data;

        // Skip internal registers like :nil
        if (name[0] == ':') continue;

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
