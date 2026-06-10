#ifndef BC_DEBUG_H
#define BC_DEBUG_H

#include "interp.h"
#include "ir.h"

// Interactive debugger REPL (the `--debug` flag). Drives `state` step by step,
// reading commands from stdin and writing the UI to stderr (program output on
// stdout stays clean). `prog` supplies label names for breakpoints and the
// listing; it must be the unoptimized lowering, whose instruction indices
// match the IrOptProgram 1:1 (main forces -O0 under --debug).
void debug_repl(InterpState *state, const IrProgram *prog, uint64_t max_steps);

#endif // BC_DEBUG_H
