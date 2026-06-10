// libFuzzer harness for the whole front half of the pipeline, plus a
// differential check of the optimizer:
//
//   bytes -> parse -> loader_expand -> ir_from_ast -> ir_optimize(O0, O)
//         -> interpret both (small step cap) -> registers must agree
//
// Build and run with `make fuzz` (needs clang; examples/ seeds the corpus).
// Devices are deliberately NOT wired up: their registers act as plain
// counters here, so fuzzed programs can't draw, sleep, or read stdin.
#include "beancraft/parser.h"
#include "beancraft/loader.h"
#include "beancraft/ir.h"
#include "beancraft/opt.h"
#include "beancraft/interp.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define FUZZ_MAX_INPUT (64 * 1024)
#define FUZZ_STEP_CAP  4096ULL

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > FUZZ_MAX_INPUT) return 0;

    char *src = malloc(size + 1);
    if (!src) return 0;
    memcpy(src, data, size);
    src[size] = '\0';

    Arena *arena = arena_new(1 << 16);
    StrPool *strings = strpool_new(arena);

    BcResult pr = parse(arena, strings, src, size, "<fuzz>");
    if (!pr.ok) goto done;
    Ast *ast = pr.value;

    // `use "file"` resolves against a directory with no modules in it, so
    // file inclusion fails cleanly instead of reading the fuzzer's cwd.
    LoaderContext *loader = loader_new(arena, strings, "/nonexistent-fuzz-base");
    BcResult ex = loader_expand(loader, ast);
    if (!ex.ok) goto done;

    BcResult ir = ir_from_ast(arena, strings, ast);
    if (!ir.ok) goto done;
    IrProgram *prog = ir.value;

    IrOptProgram *p_raw = ir_optimize(arena, prog, OPT_NONE);
    IrOptProgram *p_opt = ir_optimize(arena, prog, OPT_LOOPS);

    InterpState *s_raw = interp_new(arena, p_raw);
    InterpState *s_opt = interp_new(arena, p_opt);
    interp_init_regs(s_raw);
    interp_init_regs(s_opt);
    interp_run(s_raw, FUZZ_STEP_CAP);
    interp_run(s_opt, FUZZ_STEP_CAP);

    // Differential oracle: when both runs finished, every register must hold
    // the same value -- any disagreement is an optimizer miscompile.
    if (s_raw->halted && s_opt->halted) {
        for (uint32_t i = 0; i < prog->reg_count; i++) {
            if (!bignum_eq(s_raw->regs[i], s_opt->regs[i])) {
                fprintf(stderr, "MISCOMPILE: register '%s' differs between -O0 and -O\n",
                        prog->reg_names[i]->data);
                abort();
            }
        }
    }

    interp_cleanup(s_raw);
    interp_cleanup(s_opt);

done:
    arena_free(arena);
    free(src);
    return 0;
}
