// Tests for the loader (src/loader.c): `use`/`func` expansion, register and
// label mappings, value seeding, scope generation -- plus the IR-level
// diagnostics (duplicate labels, out-of-range offsets) that loader bugs tend
// to turn into. Module files are written to a mkdtemp directory per test.
#include "beancraft/parser.h"
#include "beancraft/loader.h"
#include "beancraft/ir.h"
#include "beancraft/opt.h"
#include "beancraft/interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST(name) static void test_##name(void)
#define RUN(name) do { \
    printf("  " #name "..."); \
    fflush(stdout); \
    test_##name(); \
    printf(" OK\n"); \
} while(0)

// --- helpers --------------------------------------------------------------

// Create a fresh scratch directory for module files.
static char *make_tmpdir(void) {
    static char templ[] = "/tmp/bc_loader_test_XXXXXX";
    char *dir = strdup(templ);
    assert(mkdtemp(dir));
    return dir;
}

static void write_module(const char *dir, const char *name, const char *src) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(src, f);
    fclose(f);
}

// parse + loader_expand + ir_from_ast; returns the stage that failed (or the
// IrProgram on success).
static BcResult pipeline(Arena *arena, StrPool *strings, const char *dir,
                         const char *src) {
    BcResult pr = parse(arena, strings, src, strlen(src), "test.bc");
    if (!pr.ok) return pr;
    LoaderContext *loader = loader_new(arena, strings, dir ? dir : ".");
    BcResult ex = loader_expand(loader, (Ast *)pr.value);
    if (!ex.ok) return ex;
    return ir_from_ast(arena, strings, (const Ast *)pr.value);
}

// Run `src` (with modules resolved relative to `dir`) and read back the named
// registers. Asserts the program halts within the step cap.
static void run_src(const char *dir, const char *src,
                    const char *const *names, uint64_t *out, int n) {
    Arena *arena = arena_new(1 << 16);
    StrPool *strings = strpool_new(arena);
    BcResult r = pipeline(arena, strings, dir, src);
    assert(r.ok);
    IrOptProgram *opt = ir_optimize(arena, (IrProgram *)r.value, OPT_NONE);
    InterpState *st = interp_new(arena, opt);
    interp_init_regs(st);
    interp_run(st, 10000000ULL);
    assert(st->halted);
    for (int k = 0; k < n; k++) {
        uint64_t u;
        assert(bignum_to_u64(interp_get_reg(st, names[k]), &u));
        out[k] = u;
    }
    interp_cleanup(st);
    arena_free(arena);
}

// Run `src` expecting the pipeline to fail; assert the error message contains
// `needle`.
static void expect_error(const char *dir, const char *src, const char *needle) {
    Arena *arena = arena_new(1 << 16);
    StrPool *strings = strpool_new(arena);
    BcResult r = pipeline(arena, strings, dir, src);
    assert(!r.ok);
    assert(r.error.message && strstr(r.error.message, needle));
    arena_free(arena);
}

// --- module composition ----------------------------------------------------

TEST(nested_use_binds_enclosing_scope) {
    // outer's body increments its own P and passes that *same* P into inner.
    // A correct expansion gives Result = 1 (outer) + 1 (inner) = 2; binding
    // inner's X to a caller-global P instead leaves Result = 1.
    char *dir = make_tmpdir();
    write_module(dir, "inner.bc", "+ X\n");
    write_module(dir, "outer.bc", "+ P\nuse \"inner\" X=P\n");

    static const char *const names[] = {"Result"};
    uint64_t out[1];
    run_src(dir, "use \"outer\" P=Result\n", names, out, 1);
    assert(out[0] == 2);
    free(dir);
}

TEST(label_param_forwards_through_nested_call) {
    // f forwards its label parameter `dest` to g; g jumps there. A broken
    // forward turns `dest` into a nonexistent scoped label (semantic error).
    static const char *SRC =
        "func g ~out {\n"
        "- nil out out\n"
        "}\n"
        "func f ~dest {\n"
        "g dest\n"
        "}\n"
        "f target\n"
        "target: + B\n";
    static const char *const names[] = {"B"};
    uint64_t out[1];
    run_src(NULL, SRC, names, out, 1);
    assert(out[0] == 1);
}

TEST(value_mapping_reseeds_on_reentry) {
    // `x=0` means "x := 0 every time control enters", not "leave the previous
    // entry's value". Two inclusions of a `+ x / + x` body must each end at 2.
    char *dir = make_tmpdir();
    write_module(dir, "mod.bc", "+ x\n+ x\n");

    static const char *const names[] = {"mod-0/x", "mod-1/x"};
    uint64_t out[2];
    run_src(dir, "use \"mod\" x=0\nuse \"mod\" x=0\n", names, out, 2);
    assert(out[0] == 2 && out[1] == 2);
    free(dir);
}

TEST(value_mapping_seeds_nonzero) {
    char *dir = make_tmpdir();
    write_module(dir, "mod.bc", "+ x\n");

    static const char *const names[] = {"mod-0/x"};
    uint64_t out[1];
    run_src(dir, "use \"mod\" x=5\n", names, out, 1);
    assert(out[0] == 6);
    free(dir);
}

TEST(long_module_paths_get_distinct_scopes) {
    // Nested `use` filenames are rewritten to absolute paths; scope names must
    // not truncate or the two inner inclusions merge (label collisions turned
    // this exact shape into an infinite loop before the str_internf fix).
    char *dir = make_tmpdir();
    char longdir[512];
    snprintf(longdir, sizeof(longdir),
             "%s/a_very_long_directory_name_made_to_push_module_paths_well_past_sixty_four_characters",
             dir);
    assert(mkdir(longdir, 0755) == 0);
    write_module(longdir, "inner.bc", "lp: - c done\n+ out lp\n");
    write_module(longdir, "mid.bc",
                 "use \"inner\" c=2 out=G\nuse \"inner\" c=3 out=G\n");

    char src[600];
    snprintf(src, sizeof(src), "use \"%s/mid.bc\" G=Total\n", longdir);
    static const char *const names[] = {"Total"};
    uint64_t out[1];
    run_src(dir, src, names, out, 1);
    assert(out[0] == 5);
    free(dir);
}

// --- diagnostics -----------------------------------------------------------

TEST(duplicate_label_is_error) {
    expect_error(NULL, "x: + A\n- B x\nx: + C\n", "duplicate label");
}

TEST(negative_value_mapping_is_error) {
    char *dir = make_tmpdir();
    write_module(dir, "mod.bc", "+ x\n");
    expect_error(dir, "use \"mod\" x=-3\n", "non-negative");
    free(dir);
}

TEST(huge_literal_is_lexer_error) {
    expect_error(NULL, "- A +99999999999999999999999999 done\n",
                 "number literal too large");
}

TEST(missing_module_is_error) {
    char *dir = make_tmpdir();
    expect_error(dir, "use \"no_such_module\"\n", "not found");
    free(dir);
}

TEST(runaway_expansion_is_error) {
    // Mutually-calling funcs multiply the AST every expansion round; the node
    // cap must fire (cleanly) before memory does. Found by the fuzzer.
    expect_error(NULL,
                 "func a { b }\n"
                 "func b {\n"
                 "a\n"
                 "a\n"
                 "a\n"
                 "a\n"
                 "}\n"
                 "a\n",
                 "expands to more than");
}

TEST(oversized_value_seed_is_error) {
    // A value seed of N emits N inc instructions; huge seeds must error
    // instead of exhausting memory.
    char *dir = make_tmpdir();
    write_module(dir, "mod.bc", "+ x\n");
    expect_error(dir, "use \"mod\" x=99999999\n", "too large");
    free(dir);
}

// --- jump-offset semantics ---------------------------------------------------

TEST(past_the_end_offset_halts) {
    // `+5` from instruction 1 lands past the program; that must mean "halt"
    // (after the deb's decrement) in every backend.
    static const char *const names[] = {"x"};
    uint64_t out[1];
    run_src(NULL, "+ x\n- x done +5\n", names, out, 1);
    assert(out[0] == 0);
}

int main(void) {
    printf("Running loader tests:\n");

    RUN(nested_use_binds_enclosing_scope);
    RUN(label_param_forwards_through_nested_call);
    RUN(value_mapping_reseeds_on_reentry);
    RUN(value_mapping_seeds_nonzero);
    RUN(long_module_paths_get_distinct_scopes);

    RUN(duplicate_label_is_error);
    RUN(negative_value_mapping_is_error);
    RUN(huge_literal_is_lexer_error);
    RUN(missing_module_is_error);
    RUN(runaway_expansion_is_error);
    RUN(oversized_value_seed_is_error);

    RUN(past_the_end_offset_halts);

    printf("\nAll loader tests passed!\n");
    return 0;
}
