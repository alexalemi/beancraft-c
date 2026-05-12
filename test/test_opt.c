// Tests for the loop-pattern optimizer (src/opt.c), with a focus on the
// IR_OPT_MULADD fold: that it fires on the multiply-loop idiom, declines on
// near-misses (and degrades to TRANSFER folds), and produces results identical
// to running the unoptimized program.
#include "beancraft/parser.h"
#include "beancraft/ir.h"
#include "beancraft/opt.h"
#include "beancraft/interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define TEST(name) static void test_##name(void)
#define RUN(name) do { \
    printf("  " #name "..."); \
    fflush(stdout); \
    test_##name(); \
    printf(" OK\n"); \
} while(0)

// --- the multiply-loop sources under test --------------------------------

// mul.bc: Out = A * B, with tmp zeroed before the loop.
static const char *MUL_SRC =
    "clrTmp: - tmp clrOut self\n"
    "clrOut: - Out loop self\n"
    "loop: - B done\n"
    "addA: - A restoreA\n"
    "+ Out\n"
    "+ tmp addA\n"
    "restoreA: - tmp loop\n"
    "+ A prev\n";

// Like mul.bc but tmp holds 3 when the loop starts (exercises the (C-1)*T term).
static const char *MUL_TMPNZ_SRC =
    "clrTmp: - tmp clrOut self\n"
    "clrOut: - Out preset self\n"
    "preset: + tmp\n"
    "        + tmp\n"
    "        + tmp loop\n"
    "loop: - B done\n"
    "addA: - A restoreA\n"
    "+ Out\n"
    "+ tmp addA\n"
    "restoreA: - tmp loop\n"
    "+ A prev\n";

// mul.bc with the leading `clrTmp`/`clrOut` removed: Out and tmp are *not* zeroed
// before the loop, so the MULADD runs with whatever they hold (Out += A*B + ...).
// Counter B, multiplicand A, temp tmp, accumulator Out -- all distinct, so the
// fold fires; the `deb B` is instruction 0.
static const char *MUL_NOCLEAR_SRC =
    "loop: - B done\n"
    "addA: - A restoreA\n"
    "+ Out\n"
    "+ tmp addA\n"
    "restoreA: - tmp loop\n"
    "+ A prev\n";

// for B { Out += A; tmp += A; A := 0; tmp += A(=0)... } -- but the accumulator
// list reuses the counter (`+ B`), so the distinctness check must reject MULADD.
static const char *MUL_ACC_IS_COUNTER_SRC =
    "loop: - B done\n"
    "addA: - A restoreA\n"
    "+ B\n"
    "+ tmp addA\n"
    "restoreA: - tmp loop\n"
    "+ A prev\n";

// for B { tmp := 0; Out += A; tmp += A; A := 0; A += tmp } -- the leading
// `tmp := 0` makes this the "preclear" MULADD variant: tmp is provably 0 at the
// top of each round, so the result is just Out += B*A with A preserved (no junk
// from a nonzero tmp at loop entry can leak in).
static const char *MUL_PRECLEAR_SRC =
    "loop: - B done\n"
    "clrT: - tmp addA self\n"
    "addA: - A restoreA\n"
    "+ Out\n"
    "+ tmp addA\n"
    "restoreA: - tmp loop\n"
    "+ A prev\n";

// --- helpers --------------------------------------------------------------

static IrProgram *lower(Arena *arena, StrPool *strings, const char *src) {
    BcResult pr = parse(arena, strings, src, strlen(src), "test.bc");
    assert(pr.ok);
    BcResult ir = ir_from_ast(arena, strings, (const Ast *)pr.value);
    assert(ir.ok);
    return (IrProgram *)ir.value;
}

// Count MULADD/TRANSFER/ZERO ops in an optimized program.
static void count_ops(const IrOptProgram *opt, int *muladd, int *transfer, int *zero) {
    int ma = 0, tx = 0, z = 0;
    for (uint32_t i = 0; i < opt->inst_count; i++) {
        if (opt->insts[i].op == IR_OPT_MULADD) ma++;
        else if (opt->insts[i].op == IR_OPT_TRANSFER) tx++;
        else if (opt->insts[i].op == IR_OPT_ZERO) z++;
    }
    if (muladd) *muladd = ma;
    if (transfer) *transfer = tx;
    if (zero) *zero = z;
}

// Count instructions of a given op in an optimized program.
static int count_op(const IrOptProgram *opt, IrOptOp op) {
    int n = 0;
    for (uint32_t i = 0; i < opt->inst_count; i++) if (opt->insts[i].op == op) n++;
    return n;
}

// Run `src` at the given opt level with the given register inits, then read back
// the named registers into `out` (out[k] = value of names[k], as a uint64).
static void run_with(const char *src, OptLevel level,
                     const char *const *init_names, const uint64_t *init_vals, int n_init,
                     const char *const *names, uint64_t *out, int n) {
    Arena *arena = arena_new(1 << 16);
    StrPool *strings = strpool_new(arena);
    IrProgram *prog = lower(arena, strings, src);
    IrOptProgram *opt = ir_optimize(arena, prog, level);
    InterpState *st = interp_new(arena, opt);
    interp_init_regs(st);
    for (int i = 0; i < n_init; i++) {
        bool ok = interp_set_reg(st, init_names[i], init_vals[i]);
        assert(ok);
    }
    interp_run(st, 100000000ULL);
    assert(st->halted);
    for (int k = 0; k < n; k++) {
        Bignum v = interp_get_reg(st, names[k]);
        uint64_t u;
        bool fits = bignum_to_u64(v, &u);
        assert(fits);
        out[k] = u;
    }
    interp_cleanup(st);
    arena_free(arena);
}

// Assert that `src` gives the same result for the named registers at -O0 and -O.
static void assert_opt_agrees(const char *src,
                              const char *const *init_names, const uint64_t *init_vals, int n_init,
                              const char *const *names, int n) {
    uint64_t a[8], b[8];
    assert(n <= 8);
    run_with(src, OPT_NONE,  init_names, init_vals, n_init, names, a, n);
    run_with(src, OPT_LOOPS, init_names, init_vals, n_init, names, b, n);
    for (int k = 0; k < n; k++) assert(a[k] == b[k]);
}

// ============================================================
// MULADD detection
// ============================================================

TEST(muladd_fires_on_mul_shape) {
    Arena *arena = arena_new(1 << 16);
    StrPool *strings = strpool_new(arena);
    IrProgram *prog = lower(arena, strings, MUL_SRC);

    // The raw `deb B` (the `loop:` instruction) must be recognised as MULADD.
    // mul.bc lowers to: 0 ZERO tmp, 1 ZERO Out, 2 deb B, ..., 8 end -> the
    // outer counter loop starts at instruction 2.
    Pattern p = ir_detect_pattern(prog, 2);
    assert(p.type == PATTERN_MULADD);
    assert(strcmp(prog->reg_names[p.src_reg]->data, "B") == 0);    // counter C
    assert(p.dst_count == 3);                                       // [S, T, D_1]
    assert(strcmp(prog->reg_names[p.dst_regs[0]]->data, "A") == 0);   // S
    assert(strcmp(prog->reg_names[p.dst_regs[1]]->data, "tmp") == 0); // T
    assert(strcmp(prog->reg_names[p.dst_regs[2]]->data, "Out") == 0); // D_1
    assert(prog->insts[p.exit_inst].op == IR_END);

    // And the optimized program collapses the whole O(B) loop to one MULADD.
    IrOptProgram *opt = ir_optimize(arena, prog, OPT_LOOPS);
    int ma, tx, z;
    count_ops(opt, &ma, &tx, &z);
    assert(ma == 1);
    assert(tx == 0);   // the two inner TRANSFERs were subsumed
    assert(z == 2);    // the two ZERO clears remain

    arena_free(arena);
}

TEST(muladd_declines_when_accumulator_is_counter) {
    Arena *arena = arena_new(1 << 16);
    StrPool *strings = strpool_new(arena);
    IrProgram *prog = lower(arena, strings, MUL_ACC_IS_COUNTER_SRC);

    // No ZERO clears here, so the outer `deb B` is instruction 0.
    Pattern p = ir_detect_pattern(prog, 0);
    assert(p.type != PATTERN_MULADD);   // distinctness {C,S,T,D_i} violated (D_1 == C)

    // It still folds to plain TRANSFERs (graceful degradation).
    IrOptProgram *opt = ir_optimize(arena, prog, OPT_LOOPS);
    int ma, tx;
    count_ops(opt, &ma, &tx, NULL);
    assert(ma == 0);
    assert(tx == 2);

    arena_free(arena);
}

TEST(muladd_preclear_variant_fires) {
    Arena *arena = arena_new(1 << 16);
    StrPool *strings = strpool_new(arena);
    IrProgram *prog = lower(arena, strings, MUL_PRECLEAR_SRC);

    // The `deb B` is instruction 0; the body's leading `deb tmp self` is the
    // per-round `tmp := 0`, so this is the preclear MULADD shape.
    Pattern p = ir_detect_pattern(prog, 0);
    assert(p.type == PATTERN_MULADD);
    assert(p.muladd_preclear);
    assert(strcmp(prog->reg_names[p.src_reg]->data, "B") == 0);
    assert(p.dst_count == 3);
    assert(strcmp(prog->reg_names[p.dst_regs[0]]->data, "A") == 0);   // S
    assert(strcmp(prog->reg_names[p.dst_regs[1]]->data, "tmp") == 0); // T
    assert(strcmp(prog->reg_names[p.dst_regs[2]]->data, "Out") == 0); // D_1

    IrOptProgram *opt = ir_optimize(arena, prog, OPT_LOOPS);
    int ma, tx, z;
    count_ops(opt, &ma, &tx, &z);
    assert(ma == 1);   // the whole loop, including the in-body ZERO, folds to one MULADD
    assert(tx == 0);
    assert(z == 0);
    arena_free(arena);
}

TEST(muladd_preclear_matches_unoptimized) {
    static const char *inits[] = { "A", "B", "tmp", "Out" };
    static const char *check[] = { "Out", "A", "B", "tmp" };
    struct { uint64_t a, b, tmp0, out0; } cases[] = {
        { 7, 8, 0, 0 },     // Out := 56
        { 7, 8, 99, 0 },    // tmp's junk is wiped each round -> still Out := 56, A := 7
        { 0, 5, 3, 0 },     // Out := 0
        { 5, 0, 4, 0 },     // B == 0: nothing runs, tmp stays 4
        { 9, 6, 1, 10 },    // Out := 10 + 6*9 = 64;  A := 9
        { 1, 1, 7, 2 },     // Out := 2 + 1 = 3;  A := 1
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        uint64_t v[] = { cases[i].a, cases[i].b, cases[i].tmp0, cases[i].out0 };
        assert_opt_agrees(MUL_PRECLEAR_SRC, inits, v, 4, check, 4);
        uint64_t out[4];
        run_with(MUL_PRECLEAR_SRC, OPT_LOOPS, inits, v, 4, check, out, 4);
        if (cases[i].b == 0) {
            assert(out[0] == cases[i].out0);
            assert(out[1] == cases[i].a);
            assert(out[3] == cases[i].tmp0);   // tmp untouched (loop never runs)
        } else {
            assert(out[0] == cases[i].out0 + cases[i].b * cases[i].a);  // no (B-1)*tmp term
            assert(out[1] == cases[i].a);                                // A preserved (A += 0)
            assert(out[3] == 0);
        }
    }
}

TEST(muladd_not_at_opt_none) {
    Arena *arena = arena_new(1 << 16);
    StrPool *strings = strpool_new(arena);
    IrProgram *prog = lower(arena, strings, MUL_SRC);
    IrOptProgram *opt = ir_optimize(arena, prog, OPT_NONE);
    int ma;
    count_ops(opt, &ma, NULL, NULL);
    assert(ma == 0);   // OPT_NONE is a faithful 1:1 lowering, no folds
    assert(opt->inst_count == prog->inst_count);
    arena_free(arena);
}

// ============================================================
// MULADD execution: optimized result must equal the unoptimized one
// ============================================================

TEST(muladd_matches_unoptimized) {
    static const char *inits[] = { "A", "B" };
    static const char *check[] = { "Out", "A", "B", "tmp" };
    struct { uint64_t a, b; } cases[] = {
        { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 5 }, { 5, 0 }, { 1, 1 },
        { 7, 8 }, { 13, 17 }, { 12, 12 }, { 100, 200 }, { 9, 1 }, { 1, 9 },
        { 250, 250 },
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        uint64_t v[] = { cases[i].a, cases[i].b };
        assert_opt_agrees(MUL_SRC, inits, v, 2, check, 4);
        // Sanity: the value really is the product, and A is preserved.
        uint64_t out[4];
        run_with(MUL_SRC, OPT_LOOPS, inits, v, 2, check, out, 4);
        assert(out[0] == cases[i].a * cases[i].b);   // Out
        assert(out[1] == cases[i].a);                 // A preserved
        assert(out[2] == 0);                          // B consumed
        assert(out[3] == 0);                          // tmp cleared
    }
}

TEST(muladd_accumulates_into_nonzero_and_zero_counter_is_noop) {
    // MUL_NOCLEAR_SRC: Out and tmp are NOT zeroed first, so MULADD runs with them
    // holding live values -- the formula must add onto Out, not replace it.
    static const char *inits[] = { "A", "B", "Out", "tmp" };
    static const char *check[] = { "Out", "A", "B", "tmp" };

    // The fold must fire here (deb B at instruction 0, all of {B,A,tmp,Out} distinct).
    Arena *arena = arena_new(1 << 16);
    StrPool *strings = strpool_new(arena);
    IrProgram *prog = lower(arena, strings, MUL_NOCLEAR_SRC);
    Pattern p = ir_detect_pattern(prog, 0);
    assert(p.type == PATTERN_MULADD);
    assert(strcmp(prog->reg_names[p.dst_regs[2]]->data, "Out") == 0);  // accumulator
    int ma; count_ops(ir_optimize(arena, prog, OPT_LOOPS), &ma, NULL, NULL);
    assert(ma == 1);
    arena_free(arena);

    struct { uint64_t a, b, out0, tmp0; } cases[] = {
        { 5, 3, 7, 0 },     // Out := 7 + 3*5 + 2*0 = 22;  A := 5;  tmp := 0
        { 5, 3, 7, 2 },     // Out := 7 + 3*5 + 2*2 = 26;  A := 7;  tmp := 0
        { 9, 0, 7, 4 },     // B == 0: nothing happens at all
        { 0, 0, 0, 0 },
        { 6, 1, 100, 9 },   // B == 1: Out := 100 + 6 + 0;  A := 6 + 9 = 15
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        uint64_t v[] = { cases[i].a, cases[i].b, cases[i].out0, cases[i].tmp0 };
        assert_opt_agrees(MUL_NOCLEAR_SRC, inits, v, 4, check, 4);
        uint64_t out[4];
        run_with(MUL_NOCLEAR_SRC, OPT_LOOPS, inits, v, 4, check, out, 4);
        if (cases[i].b == 0) {
            assert(out[0] == cases[i].out0);   // Out untouched
            assert(out[1] == cases[i].a);       // A untouched
            assert(out[3] == cases[i].tmp0);    // tmp untouched
        } else {
            assert(out[0] == cases[i].out0 + cases[i].b * cases[i].a
                             + (cases[i].b - 1) * cases[i].tmp0);
            assert(out[1] == cases[i].a + cases[i].tmp0);
            assert(out[3] == 0);
        }
    }
}

TEST(muladd_tmp_nonzero_at_entry) {
    static const char *inits[] = { "A", "B" };
    static const char *check[] = { "Out", "A", "B", "tmp" };
    struct { uint64_t a, b; } cases[] = {
        { 0, 0 }, { 5, 0 }, { 0, 4 }, { 7, 8 }, { 9, 6 }, { 1, 1 }, { 50, 3 },
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        uint64_t v[] = { cases[i].a, cases[i].b };
        assert_opt_agrees(MUL_TMPNZ_SRC, inits, v, 2, check, 4);
        if (cases[i].b > 0) {
            uint64_t out[4];
            run_with(MUL_TMPNZ_SRC, OPT_LOOPS, inits, v, 2, check, out, 4);
            // After B iters: Out += B*A + (B-1)*3;  A := A + 3.
            assert(out[0] == cases[i].b * cases[i].a + (cases[i].b - 1) * 3);
            assert(out[1] == cases[i].a + 3);
            assert(out[3] == 0);
        }
    }
}

TEST(muladd_zero_counter_is_noop) {
    // B == 0: the loop never runs, so MULADD must leave every register alone.
    static const char *inits[] = { "A", "B", "Out", "tmp" };
    static const char *check[] = { "Out", "A", "B", "tmp" };
    uint64_t v[] = { 42, 0, 7, 5 };
    uint64_t out[4];
    run_with(MUL_SRC, OPT_LOOPS, inits, v, 4, check, out, 4);
    // mul.bc clears Out and tmp before the (never-taken) loop, so:
    assert(out[0] == 0);    // Out cleared by clrOut
    assert(out[1] == 42);   // A untouched
    assert(out[2] == 0);    // B was already 0
    assert(out[3] == 0);    // tmp cleared by clrTmp
    // And it agrees with -O0.
    assert_opt_agrees(MUL_SRC, inits, v, 4, check, 4);
}

// ============================================================
// The fold doesn't perturb other loop idioms
// ============================================================

// ============================================================
// ISZERO pattern  (deb R z; inc R nz  ->  goto R==0 ? z : nz)
// ============================================================

// iszero.bc:  Zero := (N == 0 ? 1 : 0)   (N preserved -- the dec is undone)
static const char *ISZERO_SRC =
    "clr:   - Zero check self\n"      // Zero := 0
    "check: - N setZero\n"             // \  N == 0 -> setZero ;  N > 0 -> N--, fall into the inc...
    "       + N done\n"                // /  ... which undoes the dec, then -> done
    "setZero: + Zero done\n";

TEST(iszero_pattern_fires) {
    Arena *arena = arena_new(1 << 16);
    StrPool *strings = strpool_new(arena);
    IrProgram *prog = lower(arena, strings, ISZERO_SRC);

    // IR:  0 clr (deb Zero), 1 check (deb N), 2 inc N, 3 setZero (inc Zero), 4 end.
    // Instruction 1 + 2 is the `deb N; inc N` ISZERO idiom.
    Pattern p = ir_detect_pattern(prog, 1);
    assert(p.type == PATTERN_ISZERO);
    assert(strcmp(prog->reg_names[p.src_reg]->data, "N") == 0);

    IrOptProgram *opt = ir_optimize(arena, prog, OPT_LOOPS);
    assert(count_op(opt, IR_OPT_ISZERO) == 1);
    assert(count_op(opt, IR_OPT_ZERO) == 1);     // the `clr`
    assert(count_op(opt, IR_OPT_INC) == 1);      // the `+ Zero`
    assert(count_op(opt, IR_OPT_DEB) == 0);      // no plain deb survives
    arena_free(arena);
}

TEST(iszero_matches_unoptimized) {
    static const char *inits[] = { "N" };
    static const char *check[] = { "Zero", "N" };
    uint64_t ns[] = { 0, 1, 2, 5, 100, 999 };
    for (size_t i = 0; i < sizeof(ns)/sizeof(ns[0]); i++) {
        uint64_t v[] = { ns[i] };
        assert_opt_agrees(ISZERO_SRC, inits, v, 1, check, 2);
        uint64_t out[2];
        run_with(ISZERO_SRC, OPT_LOOPS, inits, v, 1, check, out, 2);
        assert(out[0] == (ns[i] == 0 ? 1u : 0u));   // Zero
        assert(out[1] == ns[i]);                     // N preserved (the dec is undone)
    }
}

// ============================================================
// Jump threading + dead-code elimination of no-op `deb R X X`
// ============================================================

TEST(threading_removes_noop_jumps) {
    // `clr: - X done self` zeros X and jumps to the program's halt; the bare
    // `loop:` on its own line lowers to a `deb :nil next next` no-op, and `.` is
    // an explicit halt -- both become unreachable once the `clr` loop folds, so
    // -O threads past the no-op and DCE leaves just `ZERO X ; END`.
    static const char *SRC = "clr: - X done self\nloop:\n.\n";
    Arena *arena = arena_new(1 << 16);
    StrPool *strings = strpool_new(arena);

    IrOptProgram *raw = ir_optimize(arena, lower(arena, strings, SRC), OPT_NONE);
    IrOptProgram *opt = ir_optimize(arena, lower(arena, strings, SRC), OPT_LOOPS);

    // OPT_NONE: a faithful lowering -- DEB X, DEB :nil (the no-op), END, END.
    assert(raw->inst_count == 4);
    assert(count_op(raw, IR_OPT_DEB) == 2);
    // OPT_LOOPS: the deb-X loop folds to ZERO, threading skips the no-op, DCE
    // drops it (and the now-unreachable explicit END).
    assert(raw->inst_count > opt->inst_count);
    assert(count_op(opt, IR_OPT_DEB) == 0);
    assert(count_op(opt, IR_OPT_ZERO) == 1);
    assert(opt->inst_count == 2);                 // ZERO X ; END

    arena_free(arena);
}

TEST(transfer_and_zero_still_fold) {
    // A bare ZERO loop and a bare TRANSFER loop, neither of which is a MULADD.
    static const char *ZERO_SRC = "loop: - X done self\n";
    static const char *XFER_SRC = "loop: - X done\n+ A\n+ B loop\n";
    Arena *arena = arena_new(1 << 16);
    StrPool *strings = strpool_new(arena);

    IrOptProgram *z = ir_optimize(arena, lower(arena, strings, ZERO_SRC), OPT_LOOPS);
    int ma, tx, zc;
    count_ops(z, &ma, &tx, &zc);
    assert(ma == 0 && tx == 0 && zc == 1);

    IrOptProgram *x = ir_optimize(arena, lower(arena, strings, XFER_SRC), OPT_LOOPS);
    count_ops(x, &ma, &tx, &zc);
    assert(ma == 0 && tx == 1 && zc == 0);

    arena_free(arena);
}

// ============================================================
// Main
// ============================================================

int main(void) {
    printf("Running optimizer tests:\n");

    RUN(muladd_fires_on_mul_shape);
    RUN(muladd_preclear_variant_fires);
    RUN(muladd_declines_when_accumulator_is_counter);
    RUN(muladd_not_at_opt_none);

    RUN(muladd_matches_unoptimized);
    RUN(muladd_preclear_matches_unoptimized);
    RUN(muladd_accumulates_into_nonzero_and_zero_counter_is_noop);
    RUN(muladd_tmp_nonzero_at_entry);
    RUN(muladd_zero_counter_is_noop);

    RUN(iszero_pattern_fires);
    RUN(iszero_matches_unoptimized);
    RUN(threading_removes_noop_jumps);

    RUN(transfer_and_zero_still_fold);

    printf("\nAll optimizer tests passed!\n");
    return 0;
}
