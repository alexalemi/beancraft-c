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
#include "beancraft/debug.h"
#include "beancraft/dot.h"
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

// --- tally-encoding helpers (used only by --emit-tally) --------------------
// examples/tally.bc reads its program as a string of tallies: a field holding n
// is n one-bits followed by a zero, fields are concatenated least-significant
// bit first, and the bank of registers is one number, 2^v0 * 3^v1 * 5^v2 * ...

static uint64_t nth_prime(uint32_t n) {          // 0 -> 2, 1 -> 3, 2 -> 5, ...
    uint64_t c = 1;
    for (uint32_t found = 0; ; ) {
        c++;
        bool prime = true;
        for (uint64_t d = 2; d * d <= c; d++) if (c % d == 0) { prime = false; break; }
        if (prime && found++ == n) return c;
    }
}

typedef struct { uint8_t *bits; size_t n, cap; } BitVec;

static void bits_push(BitVec *v, uint8_t b) {
    if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 256; v->bits = realloc(v->bits, v->cap); }
    v->bits[v->n++] = b;
}

static void bits_tally(BitVec *v, uint64_t n) {   // n marks, then the zero that ends the field
    for (uint64_t i = 0; i < n; i++) bits_push(v, 1);
    bits_push(v, 0);
}

static Bignum bits_to_bignum(const BitVec *v) {   // bits[0] is the least significant
    Bignum acc = bignum_from_u64(0);
    for (size_t i = v->n; i-- > 0; ) {
        Bignum d = bignum_add(acc, acc);
        bignum_free(&acc);
        acc = d;
        if (v->bits[i]) bignum_inc(&acc);
    }
    return acc;
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
        "  --emit-tally        Output the program (and registers) tally-encoded for examples/tally.bc\n"
        "  --emit-dot          Output the program as a Graphviz graph (--show-dot is an alias)\n"
        "  -O, --optimize      Enable loop optimizations\n"
        "  --show-opt          Print the optimized IR (implies -O)\n"
        "  -c, --check         Run at -O0 and -O and compare results (differential test)\n"
        "  --trace[=N]         Print each executed instruction to stderr (first N steps)\n"
        "  --debug             Interactive debugger (step/continue/breakpoints/watch); runs unoptimized\n"
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
    {"emit-tally", no_argument,       NULL, 'Y'},
    {"emit-dot",   no_argument,       NULL, 'G'},
    {"show-dot",   no_argument,       NULL, 'G'},
    {"optimize",   no_argument,       NULL, 'O'},
    {"show-opt",   no_argument,       NULL, 'P'},
    {"check",      no_argument,       NULL, 'c'},
    {"trace",      optional_argument, NULL, 'T'},
    {"debug",      no_argument,       NULL, 'D'},
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
    bool emit_tally = false;
    bool emit_dot = false;
    bool optimize = false;
    bool show_opt = false;
    bool check = false;
    bool debug = false;
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
        case 'Y':
            emit_tally = true;
            break;
        case 'G':
            emit_dot = true;
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
        case 'D':
            debug = true;
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

    if (emit_dot) {
        ir_print_dot(stdout, prog, filename);
        arena_free(arena);
        return 0;
    }

    if (emit_tally) {
        // Encode for examples/tally.bc. Register i lives in the exponent of the
        // i-th prime of the bank R; an instruction is a run of tally fields:
        //   give r -> a        2*p_r,     a
        //   take r -> a / b    2*p_r + 1, a, b
        //   stop               0                (the empty tally)
        // where a jump target is the field offset of its instruction (the
        // machine's PC counts fields, so it can skip to any instruction).
        uint32_t *field_off = arena_alloc(arena, (prog->inst_count + 1) * sizeof(uint32_t));
        uint32_t off = 0;
        for (uint32_t i = 0; i < prog->inst_count; i++) {
            field_off[i] = off;
            off += prog->insts[i].op == IR_INC ? 2 : prog->insts[i].op == IR_DEB ? 3 : 1;
        }
        field_off[prog->inst_count] = off;

        BitVec bits = { 0 };
        for (uint32_t i = 0; i < prog->inst_count; i++) {
            const IrInst *in = &prog->insts[i];
            switch (in->op) {
            case IR_INC:
                bits_tally(&bits, 2 * nth_prime(in->reg));
                bits_tally(&bits, field_off[in->arg_a]);
                break;
            case IR_DEB:
                bits_tally(&bits, 2 * nth_prime(in->reg) + 1);
                bits_tally(&bits, field_off[in->arg_a]);
                bits_tally(&bits, field_off[in->arg_b]);
                break;
            case IR_END:
            default:
                bits_tally(&bits, 0);
                break;
            }
        }
        Bignum P = bits_to_bignum(&bits);

        Bignum R = bignum_from_u64(1);          // every exponent 0
        for (int i = optind + 1; i < argc; i++) {   // REG=VALUE -> p_reg^VALUE
            char *eq = strchr(argv[i], '=');
            if (!eq) continue;
            *eq = '\0';
            int32_t idx = ir_find_reg(prog, argv[i]);
            *eq = '=';
            if (idx < 0) continue;
            uint64_t v = (uint64_t)atoll(eq + 1);
            Bignum pr = bignum_from_u64(nth_prime((uint32_t)idx));
            for (uint64_t k = 0; k < v; k++) {
                Bignum m = bignum_mul(R, pr);
                bignum_free(&R);
                R = m;
            }
            bignum_free(&pr);
        }
        for (uint32_t i = 0; i < prog->reg_count; i++) {   // `use`-seeded initial values
            Bignum pr = bignum_from_u64(nth_prime(i));
            for (uint64_t k = 0; k < prog->reg_init[i]; k++) {
                Bignum m = bignum_mul(R, pr);
                bignum_free(&R);
                R = m;
            }
            bignum_free(&pr);
        }

        printf("# %s -> tally.bc encoding (%zu bits).  registers (prime):", filename, bits.n);
        for (uint32_t i = 0; i < prog->reg_count; i++)
            printf(" %s=%" PRIu64, prog->reg_names[i]->data, nth_prime(i));
        printf("\n");
        char *ps = bignum_to_string(P), *rs = bignum_to_string(R);
        printf("P=%s R=%s\n", ps, rs);
        free(ps); free(rs);
        bignum_free(&P); bignum_free(&R);
        free(bits.bits);
        arena_free(arena);
        return 0;
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
    if (debug && optimize) {
        fprintf(stderr, "note: --debug runs unoptimized so labels map 1:1 (-O ignored)\n");
        optimize = false;
    }
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

        // Register values are only comparable when both levels ran to halt:
        // if either was cut off mid-run the states diverge trivially (the
        // folds compress steps, so the two runs aren't in lockstep).
        if (!s_raw->halted || !s_opt->halted) {
            if (s_raw->halted != s_opt->halted) {
                fprintf(stderr, "INCONCLUSIVE: -O0 %s but -O %s at step cap %" PRIu64
                                "; raise -s to compare results\n",
                        s_raw->halted ? "halted" : "hit the step limit",
                        s_opt->halted ? "halted" : "hit the step limit", max_steps);
            } else {
                fprintf(stderr, "INCONCLUSIVE: step cap %" PRIu64 " reached at both "
                                "levels; raise -s to compare results\n", max_steps);
            }
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
        interp_cleanup(s_raw);
        interp_cleanup(s_opt);
        arena_free(arena);
        return mismatches == 0 ? 0 : 1;
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

    if (debug) {
        debug_repl(state, prog, max_steps);
    } else if (trace_steps > 0) {
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

        if (!state->halted && !debug) {   // quitting the debugger early is deliberate
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
