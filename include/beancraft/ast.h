#ifndef BC_AST_H
#define BC_AST_H

#include "arena.h"
#include "str.h"
#include <stdint.h>
#include <stdbool.h>

// AST node types
typedef enum {
    AST_INC,
    AST_DEB,
    AST_END,
    AST_USE,
} AstKind;

// Jump target (before label resolution)
typedef enum {
    JUMP_NONE,      // Not specified (implicit next)
    JUMP_LABEL,     // Named label
    JUMP_KEYWORD,   // self, next, prev, init, done, halt
    JUMP_OFFSET,    // Relative offset like +1 or -2
} JumpKind;

typedef enum {
    KW_SELF,
    KW_NEXT,
    KW_PREV,
    KW_INIT,
    KW_DONE,
    KW_HALT,
} Keyword;

typedef struct {
    JumpKind kind;
    union {
        Str *label;       // For JUMP_LABEL
        Keyword keyword;  // For JUMP_KEYWORD
        int32_t offset;   // For JUMP_OFFSET
    };
} Jump;

// Register mapping in use statement: local=imported or local=value
typedef struct {
    Str *local;
    union {
        Str *import;      // If is_value is false
        int64_t value;    // If is_value is true
    };
    bool is_value;
} RegMapping;

// Label mapping in use statement: local~imported
typedef struct {
    Str *local;
    Str *import;
} LabelMapping;

// AST node for a single instruction
typedef struct AstNode {
    AstKind kind;
    Str *label;           // Optional label for this instruction
    uint32_t line;
    uint32_t column;

    union {
        struct {          // INC
            Str *reg;
            Jump next;
        } inc;

        struct {          // DEB
            Str *reg;
            Jump jump;    // Where to go if zero
            Jump next;    // Where to go after decrement
        } deb;

        // END has no additional data

        struct {          // USE
            Str *filename;
            Str *scope;   // Optional scope prefix
            RegMapping *reg_mappings;
            uint32_t reg_mapping_count;
            LabelMapping *label_mappings;
            uint32_t label_mapping_count;
        } use;
    };
} AstNode;

// The complete AST for a program
typedef struct {
    Arena *arena;
    StrPool *strings;
    AstNode *nodes;
    uint32_t node_count;
    uint32_t node_capacity;
} Ast;

// Create a new empty AST
Ast *ast_new(Arena *arena, StrPool *strings);

// Add a node to the AST (returns pointer to the added node)
AstNode *ast_add_node(Ast *ast);

// Create an empty jump (implicit next)
static inline Jump jump_none(void) {
    return (Jump){ .kind = JUMP_NONE };
}

// Create a label jump
static inline Jump jump_label(Str *label) {
    return (Jump){ .kind = JUMP_LABEL, .label = label };
}

// Create a keyword jump
static inline Jump jump_keyword(Keyword kw) {
    return (Jump){ .kind = JUMP_KEYWORD, .keyword = kw };
}

// Create an offset jump
static inline Jump jump_offset(int32_t offset) {
    return (Jump){ .kind = JUMP_OFFSET, .offset = offset };
}

// Debug: print AST
void ast_print(const Ast *ast);

#endif // BC_AST_H
