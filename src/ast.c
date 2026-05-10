#include "beancraft/ast.h"
#include <stdio.h>
#include <string.h>

#define INITIAL_CAPACITY 16

Ast *ast_new(Arena *arena, StrPool *strings) {
    Ast *ast = arena_alloc(arena, sizeof(Ast));
    ast->arena = arena;
    ast->strings = strings;
    ast->node_count = 0;
    ast->node_capacity = INITIAL_CAPACITY;
    ast->nodes = arena_alloc(arena, sizeof(AstNode) * ast->node_capacity);
    return ast;
}

AstNode *ast_add_node(Ast *ast) {
    if (ast->node_count >= ast->node_capacity) {
        uint32_t new_capacity = ast->node_capacity * 2;
        AstNode *new_nodes = arena_alloc(ast->arena, sizeof(AstNode) * new_capacity);
        memcpy(new_nodes, ast->nodes, sizeof(AstNode) * ast->node_count);
        ast->nodes = new_nodes;
        ast->node_capacity = new_capacity;
    }

    AstNode *node = &ast->nodes[ast->node_count++];
    memset(node, 0, sizeof(AstNode));
    return node;
}

static void print_jump(const Jump *j) {
    switch (j->kind) {
    case JUMP_NONE:
        printf("(implicit next)");
        break;
    case JUMP_LABEL:
        printf("%s", j->label->data);
        break;
    case JUMP_KEYWORD:
        switch (j->keyword) {
        case KW_SELF: printf("self"); break;
        case KW_NEXT: printf("next"); break;
        case KW_PREV: printf("prev"); break;
        case KW_INIT: printf("init"); break;
        case KW_DONE: printf("done"); break;
        case KW_HALT: printf("halt"); break;
        }
        break;
    case JUMP_OFFSET:
        printf("%+d", j->offset);
        break;
    }
}

void ast_print(const Ast *ast) {
    for (uint32_t i = 0; i < ast->node_count; i++) {
        const AstNode *node = &ast->nodes[i];

        printf("[%u] ", i);
        if (node->label) {
            printf("%s: ", node->label->data);
        }

        switch (node->kind) {
        case AST_INC:
            printf("inc %s ", node->inc.reg->data);
            print_jump(&node->inc.next);
            break;

        case AST_DEB:
            printf("deb %s ", node->deb.reg->data);
            print_jump(&node->deb.jump);
            printf(" ");
            print_jump(&node->deb.next);
            break;

        case AST_END:
            printf("end");
            break;

        case AST_USE:
            printf("use \"%s\"", node->use.filename->data);
            if (node->use.scope) {
                printf(":%s", node->use.scope->data);
            }
            for (uint32_t j = 0; j < node->use.reg_mapping_count; j++) {
                RegMapping *rm = &node->use.reg_mappings[j];
                if (rm->is_value) {
                    printf(" %s=%ld", rm->local->data, rm->value);
                } else {
                    printf(" %s=%s", rm->local->data, rm->import->data);
                }
            }
            for (uint32_t j = 0; j < node->use.label_mapping_count; j++) {
                LabelMapping *lm = &node->use.label_mappings[j];
                printf(" %s~%s", lm->local->data, lm->import->data);
            }
            break;

        case AST_FUNCDEF:
            printf("func %s", node->funcdef.name->data);
            for (uint32_t j = 0; j < node->funcdef.param_count; j++) {
                printf(" %s%s", node->funcdef.param_is_label[j] ? "~" : "",
                       node->funcdef.params[j]->data);
            }
            printf(" { %u statements }", node->funcdef.body_count);
            break;

        case AST_CALL:
            printf("call %s", node->call.name->data);
            for (uint32_t j = 0; j < node->call.arg_count; j++) {
                printf(" %s", node->call.args[j]->data);
            }
            break;
        }

        printf(" (line %u)\n", node->line);
    }
}
