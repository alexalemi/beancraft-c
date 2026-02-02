#include "beancraft/parser.h"
#include "beancraft/ast.h"
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

// ============================================================
// Basic parsing tests
// ============================================================

TEST(empty_program) {
    Arena *arena = arena_new(4096);
    StrPool *strings = strpool_new(arena);

    BcResult result = parse(arena, strings, "", 0, "test.bc");
    assert(result.ok);

    Ast *ast = result.value;
    assert(ast->node_count == 0);

    arena_free(arena);
}

TEST(simple_inc) {
    Arena *arena = arena_new(4096);
    StrPool *strings = strpool_new(arena);

    const char *source = "+ A\n";
    BcResult result = parse(arena, strings, source, strlen(source), "test.bc");
    assert(result.ok);

    Ast *ast = result.value;
    assert(ast->node_count == 1);
    assert(ast->nodes[0].kind == AST_INC);
    assert(strcmp(ast->nodes[0].inc.reg->data, "A") == 0);
    assert(ast->nodes[0].inc.next.kind == JUMP_NONE);

    arena_free(arena);
}

TEST(inc_with_jump) {
    Arena *arena = arena_new(4096);
    StrPool *strings = strpool_new(arena);

    const char *source = "+ A next\n";
    BcResult result = parse(arena, strings, source, strlen(source), "test.bc");
    assert(result.ok);

    Ast *ast = result.value;
    assert(ast->node_count == 1);
    assert(ast->nodes[0].inc.next.kind == JUMP_KEYWORD);
    assert(ast->nodes[0].inc.next.keyword == KW_NEXT);

    arena_free(arena);
}

TEST(simple_deb) {
    Arena *arena = arena_new(4096);
    StrPool *strings = strpool_new(arena);

    const char *source = "- A done\n";
    BcResult result = parse(arena, strings, source, strlen(source), "test.bc");
    assert(result.ok);

    Ast *ast = result.value;
    assert(ast->node_count == 1);
    assert(ast->nodes[0].kind == AST_DEB);
    assert(strcmp(ast->nodes[0].deb.reg->data, "A") == 0);
    assert(ast->nodes[0].deb.jump.kind == JUMP_KEYWORD);
    assert(ast->nodes[0].deb.jump.keyword == KW_DONE);
    assert(ast->nodes[0].deb.next.kind == JUMP_NONE);

    arena_free(arena);
}

TEST(deb_with_both_jumps) {
    Arena *arena = arena_new(4096);
    StrPool *strings = strpool_new(arena);

    const char *source = "- A done prev\n";
    BcResult result = parse(arena, strings, source, strlen(source), "test.bc");
    assert(result.ok);

    Ast *ast = result.value;
    assert(ast->node_count == 1);
    assert(ast->nodes[0].deb.jump.kind == JUMP_KEYWORD);
    assert(ast->nodes[0].deb.jump.keyword == KW_DONE);
    assert(ast->nodes[0].deb.next.kind == JUMP_KEYWORD);
    assert(ast->nodes[0].deb.next.keyword == KW_PREV);

    arena_free(arena);
}

TEST(simple_end) {
    Arena *arena = arena_new(4096);
    StrPool *strings = strpool_new(arena);

    const char *source = ".\n";
    BcResult result = parse(arena, strings, source, strlen(source), "test.bc");
    assert(result.ok);

    Ast *ast = result.value;
    assert(ast->node_count == 1);
    assert(ast->nodes[0].kind == AST_END);

    arena_free(arena);
}

TEST(labeled_instruction) {
    Arena *arena = arena_new(4096);
    StrPool *strings = strpool_new(arena);

    const char *source = "start: + A\n";
    BcResult result = parse(arena, strings, source, strlen(source), "test.bc");
    assert(result.ok);

    Ast *ast = result.value;
    assert(ast->node_count == 1);
    assert(ast->nodes[0].label != NULL);
    assert(strcmp(ast->nodes[0].label->data, "start") == 0);
    assert(ast->nodes[0].kind == AST_INC);

    arena_free(arena);
}

TEST(numeric_offset) {
    Arena *arena = arena_new(4096);
    StrPool *strings = strpool_new(arena);

    const char *source = "- A +2 -1\n";
    BcResult result = parse(arena, strings, source, strlen(source), "test.bc");
    assert(result.ok);

    Ast *ast = result.value;
    assert(ast->node_count == 1);
    assert(ast->nodes[0].deb.jump.kind == JUMP_OFFSET);
    assert(ast->nodes[0].deb.jump.offset == 2);
    assert(ast->nodes[0].deb.next.kind == JUMP_OFFSET);
    assert(ast->nodes[0].deb.next.offset == -1);

    arena_free(arena);
}

TEST(label_jump) {
    Arena *arena = arena_new(4096);
    StrPool *strings = strpool_new(arena);

    const char *source = "- A myLabel\n";
    BcResult result = parse(arena, strings, source, strlen(source), "test.bc");
    assert(result.ok);

    Ast *ast = result.value;
    assert(ast->node_count == 1);
    assert(ast->nodes[0].deb.jump.kind == JUMP_LABEL);
    assert(strcmp(ast->nodes[0].deb.jump.label->data, "myLabel") == 0);

    arena_free(arena);
}

TEST(comments_ignored) {
    Arena *arena = arena_new(4096);
    StrPool *strings = strpool_new(arena);

    const char *source = "# This is a comment\n+ A\n# Another comment\n";
    BcResult result = parse(arena, strings, source, strlen(source), "test.bc");
    assert(result.ok);

    Ast *ast = result.value;
    assert(ast->node_count == 1);
    assert(ast->nodes[0].kind == AST_INC);

    arena_free(arena);
}

TEST(multiple_instructions) {
    Arena *arena = arena_new(4096);
    StrPool *strings = strpool_new(arena);

    const char *source =
        "init: - A copyB\n"
        "+ Out prev\n"
        "copyB: - B done\n"
        "+ Out prev\n";

    BcResult result = parse(arena, strings, source, strlen(source), "test.bc");
    assert(result.ok);

    Ast *ast = result.value;
    assert(ast->node_count == 4);

    assert(ast->nodes[0].kind == AST_DEB);
    assert(strcmp(ast->nodes[0].label->data, "init") == 0);

    assert(ast->nodes[1].kind == AST_INC);
    assert(ast->nodes[1].label == NULL);

    assert(ast->nodes[2].kind == AST_DEB);
    assert(strcmp(ast->nodes[2].label->data, "copyB") == 0);

    assert(ast->nodes[3].kind == AST_INC);

    arena_free(arena);
}

TEST(use_statement) {
    Arena *arena = arena_new(4096);
    StrPool *strings = strpool_new(arena);

    const char *source = "use \"add\" A=10 B=x\n";
    BcResult result = parse(arena, strings, source, strlen(source), "test.bc");
    assert(result.ok);

    Ast *ast = result.value;
    assert(ast->node_count == 1);
    assert(ast->nodes[0].kind == AST_USE);
    assert(strcmp(ast->nodes[0].use.filename->data, "add") == 0);
    assert(ast->nodes[0].use.scope == NULL);
    assert(ast->nodes[0].use.reg_mapping_count == 2);

    // First mapping: A=10 (value)
    assert(strcmp(ast->nodes[0].use.reg_mappings[0].local->data, "A") == 0);
    assert(ast->nodes[0].use.reg_mappings[0].is_value == true);
    assert(ast->nodes[0].use.reg_mappings[0].value == 10);

    // Second mapping: B=x (alias)
    assert(strcmp(ast->nodes[0].use.reg_mappings[1].local->data, "B") == 0);
    assert(ast->nodes[0].use.reg_mappings[1].is_value == false);
    assert(strcmp(ast->nodes[0].use.reg_mappings[1].import->data, "x") == 0);

    arena_free(arena);
}

TEST(use_with_scope) {
    Arena *arena = arena_new(4096);
    StrPool *strings = strpool_new(arena);

    const char *source = "use \"mul\":multiply A=x\n";
    BcResult result = parse(arena, strings, source, strlen(source), "test.bc");
    assert(result.ok);

    Ast *ast = result.value;
    assert(ast->node_count == 1);
    assert(strcmp(ast->nodes[0].use.filename->data, "mul") == 0);
    assert(strcmp(ast->nodes[0].use.scope->data, "multiply") == 0);

    arena_free(arena);
}

TEST(long_form_instructions) {
    Arena *arena = arena_new(4096);
    StrPool *strings = strpool_new(arena);

    const char *source =
        "inc A next\n"
        "deb B done prev\n"
        "end\n";

    BcResult result = parse(arena, strings, source, strlen(source), "test.bc");
    assert(result.ok);

    Ast *ast = result.value;
    assert(ast->node_count == 3);
    assert(ast->nodes[0].kind == AST_INC);
    assert(ast->nodes[1].kind == AST_DEB);
    assert(ast->nodes[2].kind == AST_END);

    arena_free(arena);
}

// ============================================================
// Error tests
// ============================================================

TEST(error_missing_register) {
    Arena *arena = arena_new(4096);
    StrPool *strings = strpool_new(arena);

    const char *source = "+ \n";
    BcResult result = parse(arena, strings, source, strlen(source), "test.bc");
    assert(!result.ok);
    assert(result.error.kind == BC_ERR_SYNTAX);

    arena_free(arena);
}

TEST(error_missing_jump) {
    Arena *arena = arena_new(4096);
    StrPool *strings = strpool_new(arena);

    const char *source = "- A\n";  // deb requires at least one jump
    BcResult result = parse(arena, strings, source, strlen(source), "test.bc");
    assert(!result.ok);
    assert(result.error.kind == BC_ERR_SYNTAX);

    arena_free(arena);
}

// ============================================================
// Main
// ============================================================

int main(void) {
    printf("Running parser tests:\n");

    RUN(empty_program);
    RUN(simple_inc);
    RUN(inc_with_jump);
    RUN(simple_deb);
    RUN(deb_with_both_jumps);
    RUN(simple_end);
    RUN(labeled_instruction);
    RUN(numeric_offset);
    RUN(label_jump);
    RUN(comments_ignored);
    RUN(multiple_instructions);
    RUN(use_statement);
    RUN(use_with_scope);
    RUN(long_form_instructions);
    RUN(error_missing_register);
    RUN(error_missing_jump);

    printf("\nAll parser tests passed!\n");
    return 0;
}
