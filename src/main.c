#define _GNU_SOURCE
#include "beancraft/arena.h"
#include "beancraft/str.h"
#include "beancraft/parser.h"
#include "beancraft/loader.h"
#include "beancraft/ir.h"
#include "beancraft/interp.h"
#include "beancraft/qbe.h"
#include "beancraft/opt.h"
#include "beancraft/devices.h"
#include "beancraft/error.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <libgen.h>

// --- Gödel-encoding helpers (used only by --emit-urm) ----------------------
// pair(x, y) = 2^x * (2y + 1);  list([]) = 0;  list(h :: t) = pair(h, list(t)).

static Bignum bn_pow2(uint64_t e) {           // 2^e  (e is small in practice)
    Bignum r = bignum_from_u64(1);
    for (uint64_t i = 0; i < e; i++) {
        Bignum d = bignum_add(r, r);
        bignum_free(&r);
        r = d;
    }
    return r;
}

static Bignum bn_cons(uint64_t head, Bignum tail) {   // pair(head, tail); consumes `tail`
    Bignum two_t   = bignum_add(tail, tail);
    Bignum two_t_1 = bignum_add(two_t, bignum_from_u64(1));
    Bignum p2      = bn_pow2(head);
    Bignum out     = bignum_mul(p2, two_t_1);
    bignum_free(&two_t);
    bignum_free(&two_t_1);
    bignum_free(&p2);
    bignum_free(&tail);
    return out;
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] <file.bc> [REG=VALUE...]\n"
        "\n"
        "Options:\n"
        "  -v, --verbose       Show execution stats\n"
        "  -q, --quiet         Suppress output\n"
        "  -n, --dry-run       Parse only, don't execute\n"
        "  -s, --max-steps N   Maximum execution steps (default: 10000000)\n"
        "  -l, --list-regs     List available registers\n"
        "  --show-ir           Print intermediate representation\n"
        "  --show-ast          Print abstract syntax tree\n"
        "  --emit-qbe          Output QBE intermediate language (for compilation)\n"
        "  --emit-urm          Output the program (and registers) Gödel-encoded for examples/urm.bc\n"
        "  -O, --optimize      Enable loop optimizations\n"
        "  --show-opt          Print the optimized IR (implies -O)\n"
        "  -c, --check         Run at -O0 and -O and compare results (differential test)\n"
        "  --trace[=N]         Print each executed instruction to stderr (first N steps)\n"
        "  -h, --help          Show this help\n"
        "\n"
        "Register initialization:\n"
        "  REG=VALUE           Set register REG to VALUE before execution\n"
        "\n"
        "Examples:\n"
        "  %s add.bc A=10 B=5\n"
        "  %s mul.bc A=7 B=8 --verbose\n"
        "  %s fib.bc --emit-qbe > fib.ssa\n",
        prog, prog, prog, prog);
}

// Apply REG=VALUE command-line assignments to one or two interpreter states
// (b may be NULL). argv is left intact so the assignments can be re-read.
static void apply_assignments(InterpState *a, InterpState *b,
                              int argc, char *argv[], int from, bool verbose) {
    for (int i = from; i < argc; i++) {
        char *arg = argv[i];
        char *eq = strchr(arg, '=');
        if (!eq) {
            fprintf(stderr, "Warning: invalid argument '%s' (expected REG=VALUE)\n", arg);
            continue;
        }

        *eq = '\0';
        const char *reg_name = arg;
        const char *value_str = eq + 1;

        // Parse value as an arbitrary-precision non-negative integer (so the
        // huge Gödel numbers from --emit-urm work, not just 64-bit ones).
        Bignum value = bignum_from_string(value_str);
        if (!interp_set_reg_bignum(a, reg_name, value)) {
            fprintf(stderr, "Warning: unknown register '%s'\n", reg_name);
        } else if (verbose) {
            char *vs = bignum_to_string(value);
            printf("Set %s = %s\n", reg_name, vs);
            free(vs);
        }
        if (b) interp_set_reg_bignum(b, reg_name, value);
        bignum_free(&value);
        *eq = '=';
    }
}

static struct option long_options[] = {
    {"verbose",    no_argument,       NULL, 'v'},
    {"quiet",      no_argument,       NULL, 'q'},
    {"dry-run",    no_argument,       NULL, 'n'},
    {"max-steps",  required_argument, NULL, 's'},
    {"list-regs",  no_argument,       NULL, 'l'},
    {"show-ir",    no_argument,       NULL, 'I'},
    {"show-ast",   no_argument,       NULL, 'A'},
    {"emit-qbe",   no_argument,       NULL, 'Q'},
    {"emit-urm",   no_argument,       NULL, 'U'},
    {"optimize",   no_argument,       NULL, 'O'},
    {"show-opt",   no_argument,       NULL, 'P'},
    {"check",      no_argument,       NULL, 'c'},
    {"trace",      optional_argument, NULL, 'T'},
    {"help",       no_argument,       NULL, 'h'},
    {NULL,         0,                 NULL, 0}
};

int main(int argc, char *argv[]) {
    bool verbose = false;
    bool quiet = false;
    bool dry_run = false;
    bool list_regs = false;
    bool show_ir = false;
    bool show_ast = false;
    bool emit_qbe = false;
    bool emit_urm = false;
    bool optimize = false;
    bool show_opt = false;
    bool check = false;
    bool steps_set = false;
    uint64_t max_steps = DEFAULT_MAX_STEPS;
    uint64_t trace_steps = 0;   // 0 = no tracing

    int opt;
    while ((opt = getopt_long(argc, argv, "vqns:lhOc", long_options, NULL)) != -1) {
        switch (opt) {
        case 'v':
            verbose = true;
            break;
        case 'q':
            quiet = true;
            break;
        case 'n':
            dry_run = true;
            break;
        case 's':
            max_steps = (uint64_t)atoll(optarg);
            steps_set = true;
            break;
        case 'l':
            list_regs = true;
            break;
        case 'I':
            show_ir = true;
            break;
        case 'A':
            show_ast = true;
            break;
        case 'Q':
            emit_qbe = true;
            break;
        case 'U':
            emit_urm = true;
            break;
        case 'O':
            optimize = true;
            break;
        case 'P':
            show_opt = true;
            optimize = true;  // "the optimized IR" only differs from --show-ir once folding is on
            break;
        case 'c':
            check = true;
            break;
        case 'T':
            trace_steps = optarg ? (uint64_t)atoll(optarg) : UINT64_MAX;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: no input file specified\n");
        print_usage(argv[0]);
        return 1;
    }

    const char *filename = argv[optind];

    // Create arena and string pool
    Arena *arena = arena_new(64 * 1024);
    StrPool *strings = strpool_new(arena);

    // Parse file
    BcResult parse_result = parse_file(arena, strings, filename);
    if (!parse_result.ok) {
        bc_error_print(&parse_result.error);
        arena_free(arena);
        return 1;
    }

    Ast *ast = parse_result.value;

    // Get directory of input file for module resolution
    char *filename_copy = strdup(filename);
    char *base_dir = dirname(filename_copy);

    // Expand use statements
    LoaderContext *loader = loader_new(arena, strings, base_dir);
    BcResult expand_result = loader_expand(loader, ast);
    free(filename_copy);

    if (!expand_result.ok) {
        bc_error_print(&expand_result.error);
        arena_free(arena);
        return 1;
    }

    if (show_ast) {
        printf("=== AST (after expansion) ===\n");
        ast_print(ast);
        printf("\n");
    }

    // Lower to IR
    BcResult ir_result = ir_from_ast(arena, strings, ast);
    if (!ir_result.ok) {
        bc_error_print(&ir_result.error);
        arena_free(arena);
        return 1;
    }

    IrProgram *prog = ir_result.value;

    if (show_ir) {
        printf("=== IR ===\n");
        ir_print(prog);
        printf("\n");
    }

    if (emit_urm) {
        // Gödel-encode the program (and the initial registers, if any REG=VALUE
        // args are given) for examples/urm.bc. No size limits -- any number of
        // registers, any number of instructions. An instruction is four list
        // elements (t, r, g1, g2): t=0 inc s_r; t=1 deb s_r (jz->g1, nz->g2);
        // t=2 halt. Registers map in order to s0, s1, ...
        Bignum P = bignum_from_u64(0);
        for (int i = (int)prog->inst_count - 1; i >= 0; i--) {
            const IrInst *in = &prog->insts[i];
            uint64_t t, r, g1, g2;
            switch (in->op) {
            case IR_INC: t = 0; r = in->reg; g1 = in->arg_a; g2 = 0;          break;
            case IR_DEB: t = 1; r = in->reg; g1 = in->arg_a; g2 = in->arg_b;  break;
            case IR_END:
            default:     t = 2; r = 0;       g1 = 0;          g2 = 0;          break;
            }
            uint64_t f[4] = { t, r, g1, g2 };
            for (int k = 3; k >= 0; k--) P = bn_cons(f[k], P);
        }

        uint64_t *rv = arena_alloc_zero(arena, (prog->reg_count ? prog->reg_count : 1) * sizeof(uint64_t));
        bool any_reg = false;
        for (uint32_t i = 0; i < prog->reg_count; i++) {
            rv[i] = prog->reg_init[i];
            if (rv[i]) any_reg = true;
        }
        for (int i = optind + 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (!eq) continue;
            *eq = '\0';
            int32_t idx = ir_find_reg(prog, argv[i]);
            if (idx >= 0) { rv[idx] = (uint64_t)atoll(eq + 1); if (rv[idx]) any_reg = true; }
            *eq = '=';
        }

        printf("# %s -> urm.bc encoding.  registers:", filename);
        for (uint32_t i = 0; i < prog->reg_count; i++) printf(" %s=s%u", prog->reg_names[i]->data, i);
        printf("\n");

        char *ps = bignum_to_string(P);
        printf("P=%s", ps);
        free(ps);
        bignum_free(&P);

        if (any_reg) {
            Bignum R = bignum_from_u64(0);
            for (int i = (int)prog->reg_count - 1; i >= 0; i--) R = bn_cons(rv[i], R);
            char *rs = bignum_to_string(R);
            printf(" R=%s", rs);
            free(rs);
            bignum_free(&R);
        }
        printf("\n");
        arena_free(arena);
        return 0;
    }

    // Lower to the optimized IR form. At -O0 this is a faithful 1:1 lowering;
    // at -O it folds recognized loop idioms (ZERO, TRANSFER) into O(1) ops.
    // The interpreter runs this form directly, so -O speeds up interpretation
    // too, not just `--emit-qbe`.
    OptLevel level = optimize ? OPT_LOOPS : OPT_NONE;
    IrOptProgram *opt_prog = ir_optimize(arena, prog, level);

    if (show_opt) {
        printf("=== Optimized IR ===\n");
        ir_opt_print(opt_prog);
        printf("\n");
    }

    if (emit_qbe) {
        QbeOptions opts = qbe_default_options();
        opts.emit_debug_info = verbose;
        BcResult qbe_result;
        if (optimize) {
            qbe_result = qbe_generate_opt(stdout, opt_prog, opts);
        } else {
            qbe_result = qbe_generate(stdout, prog, opts);
        }
        // Print before freeing: the error message is arena-allocated.
        if (!qbe_result.ok) {
            bc_error_print(&qbe_result.error);
            arena_free(arena);
            return 1;
        }
        arena_free(arena);
        return 0;
    }

    if (list_regs) {
        printf("Available registers:\n");
        for (uint32_t i = 0; i < prog->reg_count; i++) {
            const char *name = prog->reg_names[i]->data;
            if (name[0] != ':') {  // Skip internal registers
                printf("  %s\n", name);
            }
        }
        arena_free(arena);
        return 0;
    }

    if (dry_run) {
        if (!quiet) {
            printf("Parse OK: %u instructions, %u registers\n",
                   prog->inst_count, prog->reg_count);
        }
        arena_free(arena);
        return 0;
    }

    // Differential test: run the same program and inputs at -O0 and -O and
    // compare every register. Catches optimizer folds that change semantics.
    if (check) {
        for (uint32_t i = 0; i < prog->reg_count; i++) {
            if (device_name_is_known(prog->reg_names[i]->data)) {
                fprintf(stderr, "Error: --check cannot run device programs "
                                "(register '%s' has side effects, and the program "
                                "would run twice)\n", prog->reg_names[i]->data);
                arena_free(arena);
                return 1;
            }
        }

        IrOptProgram *p_raw = ir_optimize(arena, prog, OPT_NONE);
        IrOptProgram *p_opt = ir_optimize(arena, prog, OPT_LOOPS);
        InterpState *s_raw = interp_new(arena, p_raw);
        InterpState *s_opt = interp_new(arena, p_opt);
        interp_init_regs(s_raw);
        interp_init_regs(s_opt);
        apply_assignments(s_raw, s_opt, argc, argv, optind + 1, verbose);

        interp_run(s_raw, max_steps);
        interp_run(s_opt, max_steps);

        // If one level finished and the other ran out of steps, the register
        // values aren't comparable -- the slower one was cut off mid-run.
        // (-O typically needs far fewer steps, so the raw run caps out first.)
        if (s_raw->halted != s_opt->halted) {
            fprintf(stderr, "INCONCLUSIVE: -O0 %s but -O %s at step cap %" PRIu64
                            "; raise -s to compare results\n",
                    s_raw->halted ? "halted" : "hit the step limit",
                    s_opt->halted ? "halted" : "hit the step limit", max_steps);
            interp_cleanup(s_raw);
            interp_cleanup(s_opt);
            arena_free(arena);
            return 2;
        }

        int mismatches = 0;
        // Both states come from the same IrProgram, so register i means the
        // same name in each.
        for (uint32_t i = 0; i < prog->reg_count; i++) {
            if (bignum_eq(s_raw->regs[i], s_opt->regs[i])) continue;
            char *a = bignum_to_string(s_raw->regs[i]);
            char *b = bignum_to_string(s_opt->regs[i]);
            fprintf(stderr, "MISMATCH: %s = %s at -O0 but %s at -O\n",
                    prog->reg_names[i]->data, a, b);
            free(a); free(b);
            mismatches++;
        }

        if (mismatches == 0) {
            printf("OK: %u registers agree between -O0 and -O "
                   "(%" PRIu64 " steps vs %" PRIu64 ")\n",
                   prog->reg_count, s_raw->steps, s_opt->steps);
        }
        bool ok = (mismatches == 0) && s_raw->halted;
        if (!s_raw->halted && mismatches == 0) {
            fprintf(stderr, "Warning: step limit reached at both levels; "
                            "results compared at the cap, not at halt\n");
        }
        interp_cleanup(s_raw);
        interp_cleanup(s_opt);
        arena_free(arena);
        return ok ? 0 : 1;
    }

    // Create interpreter state
    InterpState *state = interp_new(arena, opt_prog);
    interp_init_regs(state);

    // Process register assignments from command line
    apply_assignments(state, NULL, argc, argv, optind + 1, verbose);

    // Wire up devices (if the program references any magic register). A program
    // that does I/O is typically an interactive loop, so drop the step cap
    // unless one was given explicitly.
    {
        const char **dev_names = arena_alloc(arena, opt_prog->reg_count * sizeof(char *));
        for (uint32_t i = 0; i < opt_prog->reg_count; i++) dev_names[i] = opt_prog->reg_names[i]->data;
        if (device_init(dev_names, opt_prog->reg_count, state->regs)) {
            state->inc_mask = device_inc_mask();
            state->deb_mask = device_deb_mask();
            if (!steps_set) max_steps = UINT64_MAX;
            if (!verbose) quiet = true;   // a device program's output is its output; no register dump
        }
    }

    // Run
    if (verbose) {
        printf("Running (max %" PRIu64 " steps)...\n", max_steps);
    }

    if (trace_steps > 0) {
        interp_run_trace(state, max_steps, trace_steps, stderr);
    } else {
        interp_run(state, max_steps);
    }
    device_shutdown();

    // Output results
    if (!quiet) {
        if (verbose) {
            printf("\nExecution completed:\n");
            interp_print_state(state);
            printf("\n");
        }

        printf("Results:\n");
        interp_print_regs(state);

        if (!state->halted) {
            fprintf(stderr, "\nWarning: execution limit reached (%" PRIu64 " steps)\n",
                    state->steps);
        }
    }

    // Save return status before cleanup
    bool halted = state->halted;

    // Cleanup
    interp_cleanup(state);
    arena_free(arena);

    return halted ? 0 : 1;
}
