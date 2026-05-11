#define _GNU_SOURCE
#include "beancraft/arena.h"
#include "beancraft/str.h"
#include "beancraft/parser.h"
#include "beancraft/loader.h"
#include "beancraft/ir.h"
#include "beancraft/interp.h"
#include "beancraft/qbe.h"
#include "beancraft/opt.h"
#include "beancraft/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <libgen.h>

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
        "  --emit-urm          Output the program encoded for examples/urm.bc\n"
        "  -O, --optimize      Enable loop optimizations\n"
        "  --show-opt          Print optimized IR\n"
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
    uint64_t max_steps = DEFAULT_MAX_STEPS;

    int opt;
    while ((opt = getopt_long(argc, argv, "vqns:lhO", long_options, NULL)) != -1) {
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
        // Encode the program for examples/urm.bc: four list elements per
        // instruction -- (t, r, g1, g2) -- printed as i0=.. i1=.. ...
        //   t=0: inc s_r; PC := g1     t=1: deb s_r (jz -> g1, nz -> g2)     t=2: halt
        // Registers map in order to s0, s1, ...; the URM has s0..s3 and room
        // for 16 instructions.
        if (prog->reg_count > 4) {
            fprintf(stderr,
                "Error: program uses %u registers; the urm.bc encoding supports at most 4\n",
                prog->reg_count);
            arena_free(arena);
            return 1;
        }
        if (prog->inst_count > 16) {
            fprintf(stderr,
                "Error: program has %u instructions; the urm.bc encoding supports at most 16\n",
                prog->inst_count);
            arena_free(arena);
            return 1;
        }
        printf("# %s -> urm.bc encoding.  registers:", filename);
        for (uint32_t i = 0; i < prog->reg_count; i++) {
            printf(" %s=s%u", prog->reg_names[i]->data, i);
        }
        printf("\n");
        for (uint32_t i = 0; i < prog->inst_count; i++) {
            const IrInst *in = &prog->insts[i];
            uint32_t t, r, g1, g2;
            switch (in->op) {
            case IR_INC: t = 0; r = in->reg; g1 = in->arg_a; g2 = 0;          break;
            case IR_DEB: t = 1; r = in->reg; g1 = in->arg_a; g2 = in->arg_b;  break;
            case IR_END:
            default:     t = 2; r = 0;       g1 = 0;          g2 = 0;          break;
            }
            printf("%si%u=%u i%u=%u i%u=%u i%u=%u",
                   i ? " " : "", 4 * i, t, 4 * i + 1, r, 4 * i + 2, g1, 4 * i + 3, g2);
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
        arena_free(arena);
        if (!qbe_result.ok) {
            bc_error_print(&qbe_result.error);
            return 1;
        }
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

    // Create interpreter state
    InterpState *state = interp_new(arena, opt_prog);
    interp_init_regs(state);

    // Process register assignments from command line
    for (int i = optind + 1; i < argc; i++) {
        char *arg = argv[i];
        char *eq = strchr(arg, '=');
        if (!eq) {
            fprintf(stderr, "Warning: invalid argument '%s' (expected REG=VALUE)\n", arg);
            continue;
        }

        *eq = '\0';
        const char *reg_name = arg;
        const char *value_str = eq + 1;

        // Parse value
        uint64_t value = (uint64_t)atoll(value_str);

        if (!interp_set_reg(state, reg_name, value)) {
            fprintf(stderr, "Warning: unknown register '%s'\n", reg_name);
        } else if (verbose) {
            printf("Set %s = %lu\n", reg_name, value);
        }
    }

    // Run
    if (verbose) {
        printf("Running (max %lu steps)...\n", max_steps);
    }

    interp_run(state, max_steps);

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
            fprintf(stderr, "\nWarning: execution limit reached (%lu steps)\n",
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
