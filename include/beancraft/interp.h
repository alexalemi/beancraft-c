#ifndef BC_INTERP_H
#define BC_INTERP_H

#include "arena.h"
#include "opt.h"
#include "bignum.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// Default maximum steps
#define DEFAULT_MAX_STEPS 10000000ULL

// Interpreter state. The interpreter runs the optimized IR form; at OPT_NONE
// that is just a faithful lowering of the plain IR.
typedef struct {
    const IrOptProgram *prog;  // Program (not owned)
    Bignum *regs;              // Register values (owned)
    uint32_t pc;               // Program counter
    uint64_t steps;            // Step count
    bool halted;               // Whether program has halted
    const bool *inc_mask;      // inc_mask[r] => `inc r` is a device trigger (NULL if no devices)
    const bool *deb_mask;      // deb_mask[r] => `deb r` is a device poll
    const volatile bool *stop_flag;  // optional host abort flag, checked each step
                                     // (set e.g. by the wasm Stop button while the
                                     // run is suspended in a device yield)
} InterpState;

// Create interpreter state from an optimized IR program
InterpState *interp_new(Arena *arena, const IrOptProgram *prog);

// Initialize register values from program defaults
void interp_init_regs(InterpState *state);

// Set register value by name
bool interp_set_reg(InterpState *state, const char *name, uint64_t value);

// Set register value by name (bignum)
bool interp_set_reg_bignum(InterpState *state, const char *name, Bignum value);

// Get register value by name
Bignum interp_get_reg(const InterpState *state, const char *name);

// Execute a single step
void interp_step(InterpState *state);

// Execute until halt or max_steps
void interp_run(InterpState *state, uint64_t max_steps);

// Like interp_run, but print one line per executed instruction to `out`
// (step number, pc, op, the touched register's value before/after, next pc)
// for the first `trace_limit` steps, then continue silently.
void interp_run_trace(InterpState *state, uint64_t max_steps,
                      uint64_t trace_limit, FILE *out);

// Print current state
void interp_print_state(const InterpState *state);

// Print final register values
void interp_print_regs(const InterpState *state);

// Free bignum resources (call before arena_free if bignums may be on heap)
void interp_cleanup(InterpState *state);

#endif // BC_INTERP_H
