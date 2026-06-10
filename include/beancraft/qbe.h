#ifndef BC_QBE_H
#define BC_QBE_H

#include "ir.h"
#include "opt.h"
#include "error.h"
#include <stdio.h>

// QBE code generation options
typedef struct {
    bool emit_debug_info;    // Include comments with original labels
    bool optimize_immediates; // Use inline arithmetic for small values
} QbeOptions;

// Default options
static inline QbeOptions qbe_default_options(void) {
    return (QbeOptions){
        .emit_debug_info = true,
        .optimize_immediates = true,
    };
}

// Generate QBE IL from IR program
// Writes QBE IL to the given file stream
BcResult qbe_generate(FILE *out, const IrProgram *prog, QbeOptions opts);

// Generate QBE IL from optimized IR program
// This can emit O(1) operations for TRANSFER, ZERO, etc.
BcResult qbe_generate_opt(FILE *out, const IrOptProgram *prog, QbeOptions opts);

#endif // BC_QBE_H
