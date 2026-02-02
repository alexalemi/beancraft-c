#include "beancraft/parser.h"
#include "beancraft/lexer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Arena *arena;
    StrPool *strings;
    Lexer lexer;
    Token current;
    Token previous;
    Ast *ast;
    const char *filename;
    bool had_error;
    char error_msg[256];
} Parser;

// Forward declarations
static bool parse_line(Parser *p);

static void advance(Parser *p) {
    p->previous = p->current;
    p->current = lexer_next(&p->lexer);
}

static bool check(Parser *p, TokenKind kind) {
    return p->current.kind == kind;
}

static bool match(Parser *p, TokenKind kind) {
    if (!check(p, kind)) return false;
    advance(p);
    return true;
}

static void error_at(Parser *p, Token *tok, const char *message) {
    if (p->had_error) return;  // Suppress cascading errors
    p->had_error = true;
    snprintf(p->error_msg, sizeof(p->error_msg), "%s", message);
}

static void error(Parser *p, const char *message) {
    error_at(p, &p->current, message);
}

static bool consume(Parser *p, TokenKind kind, const char *message) {
    if (check(p, kind)) {
        advance(p);
        return true;
    }
    error(p, message);
    return false;
}

static Jump parse_jump(Parser *p) {
    switch (p->current.kind) {
    case TOK_IDENT:
        advance(p);
        return jump_label(p->previous.str);

    case TOK_NUMBER:
        advance(p);
        return jump_offset((int32_t)p->previous.number);

    case TOK_SELF:
        advance(p);
        return jump_keyword(KW_SELF);
    case TOK_NEXT:
        advance(p);
        return jump_keyword(KW_NEXT);
    case TOK_PREV:
        advance(p);
        return jump_keyword(KW_PREV);
    case TOK_INIT:
        advance(p);
        return jump_keyword(KW_INIT);
    case TOK_DONE:
        advance(p);
        return jump_keyword(KW_DONE);
    case TOK_HALT:
        advance(p);
        return jump_keyword(KW_HALT);

    default:
        return jump_none();
    }
}

static bool parse_inc(Parser *p, AstNode *node) {
    node->kind = AST_INC;
    node->line = p->previous.line;
    node->column = p->previous.column;

    // Expect register name
    if (!check(p, TOK_IDENT) && !token_is_keyword(p->current.kind)) {
        error(p, "expected register name");
        return false;
    }
    advance(p);
    node->inc.reg = p->previous.str;

    // Optional jump target
    node->inc.next = parse_jump(p);

    return true;
}

static bool parse_deb(Parser *p, AstNode *node) {
    node->kind = AST_DEB;
    node->line = p->previous.line;
    node->column = p->previous.column;

    // Expect register name
    if (!check(p, TOK_IDENT) && !token_is_keyword(p->current.kind)) {
        error(p, "expected register name");
        return false;
    }
    advance(p);
    node->deb.reg = p->previous.str;

    // First jump target (where to go if zero)
    node->deb.jump = parse_jump(p);
    if (node->deb.jump.kind == JUMP_NONE) {
        error(p, "expected jump target for deb instruction");
        return false;
    }

    // Optional second jump target (where to go after decrement)
    node->deb.next = parse_jump(p);

    return true;
}

static bool parse_end(Parser *p, AstNode *node) {
    node->kind = AST_END;
    node->line = p->previous.line;
    node->column = p->previous.column;
    return true;
}

static bool parse_use(Parser *p, AstNode *node) {
    node->kind = AST_USE;
    node->line = p->previous.line;
    node->column = p->previous.column;

    // Expect filename string
    if (!consume(p, TOK_STRING, "expected filename string after 'use'")) {
        return false;
    }
    node->use.filename = p->previous.str;

    // Optional scope: :scopename
    if (match(p, TOK_COLON)) {
        if (!check(p, TOK_IDENT) && !token_is_keyword(p->current.kind)) {
            error(p, "expected scope name after ':'");
            return false;
        }
        advance(p);
        node->use.scope = p->previous.str;
    } else {
        node->use.scope = NULL;
    }

    // Parse register and label mappings
    // Temporary storage - we'll copy to arena-allocated arrays
    RegMapping reg_mappings[64];
    LabelMapping label_mappings[64];
    uint32_t reg_count = 0;
    uint32_t label_count = 0;

    while ((check(p, TOK_IDENT) || token_is_keyword(p->current.kind)) &&
           !check(p, TOK_NEWLINE) && !check(p, TOK_EOF)) {
        advance(p);
        Str *name = p->previous.str;

        if (match(p, TOK_EQUALS)) {
            // Register mapping: name=value or name=alias
            if (reg_count >= 64) {
                error(p, "too many register mappings");
                return false;
            }
            RegMapping *rm = &reg_mappings[reg_count++];
            rm->local = name;

            if (check(p, TOK_NUMBER)) {
                advance(p);
                rm->is_value = true;
                rm->value = p->previous.number;
            } else if (check(p, TOK_IDENT) || token_is_keyword(p->current.kind)) {
                advance(p);
                rm->is_value = false;
                rm->import = p->previous.str;
            } else {
                error(p, "expected value or register name after '='");
                return false;
            }
        } else if (match(p, TOK_TILDE)) {
            // Label mapping: name~alias
            if (label_count >= 64) {
                error(p, "too many label mappings");
                return false;
            }
            LabelMapping *lm = &label_mappings[label_count++];
            lm->local = name;

            if (!check(p, TOK_IDENT) && !token_is_keyword(p->current.kind)) {
                error(p, "expected label name after '~'");
                return false;
            }
            advance(p);
            lm->import = p->previous.str;
        } else {
            error(p, "expected '=' or '~' after identifier in use statement");
            return false;
        }
    }

    // Copy mappings to arena
    if (reg_count > 0) {
        node->use.reg_mappings = arena_alloc(p->arena,
                                              sizeof(RegMapping) * reg_count);
        memcpy(node->use.reg_mappings, reg_mappings,
               sizeof(RegMapping) * reg_count);
    }
    node->use.reg_mapping_count = reg_count;

    if (label_count > 0) {
        node->use.label_mappings = arena_alloc(p->arena,
                                                sizeof(LabelMapping) * label_count);
        memcpy(node->use.label_mappings, label_mappings,
               sizeof(LabelMapping) * label_count);
    }
    node->use.label_mapping_count = label_count;

    return true;
}

static bool parse_instruction(Parser *p, AstNode *node) {
    switch (p->current.kind) {
    case TOK_INC:
        advance(p);
        return parse_inc(p, node);
    case TOK_DEB:
        advance(p);
        return parse_deb(p, node);
    case TOK_END:
        advance(p);
        return parse_end(p, node);
    case TOK_USE:
        advance(p);
        return parse_use(p, node);
    default:
        error(p, "expected instruction");
        return false;
    }
}

static bool parse_line(Parser *p) {
    // Skip empty lines
    while (match(p, TOK_NEWLINE)) {}

    if (check(p, TOK_EOF)) {
        return true;  // Done
    }

    AstNode *node = ast_add_node(p->ast);

    // Check for optional label
    if ((check(p, TOK_IDENT) || token_is_keyword(p->current.kind))) {
        // Peek ahead to see if this is a label (followed by ':')
        Token saved = p->current;
        advance(p);
        if (check(p, TOK_COLON)) {
            // It's a label
            node->label = saved.str;
            advance(p);  // Skip ':'
        } else {
            // Not a label, this must be an instruction or we have an error
            // Put back the token (we can't really put back, so handle inline)
            // Actually we already advanced, so just check if it's an instruction
            // The saved token should have been an instruction keyword

            // Reset - but we can't easily. Instead, re-check what we have
            // If the previous token was an instruction, we're in trouble
            // Let's handle this differently...
            p->ast->node_count--;  // Remove the node we added

            // Go back to saved state by re-parsing
            // This is a bit hacky but works
            error(p, "expected ':' after label or valid instruction");
            return false;
        }
    }

    // Parse instruction
    if (!parse_instruction(p, node)) {
        return false;
    }

    // Expect newline or EOF
    if (!check(p, TOK_NEWLINE) && !check(p, TOK_EOF)) {
        error(p, "expected newline after instruction");
        return false;
    }

    return true;
}

BcResult parse(Arena *arena, StrPool *strings,
               const char *source, size_t len,
               const char *filename) {
    Parser p;
    p.arena = arena;
    p.strings = strings;
    p.filename = filename;
    p.had_error = false;
    p.error_msg[0] = '\0';

    lexer_init(&p.lexer, arena, strings, source, len, filename);
    p.ast = ast_new(arena, strings);

    // Prime the parser
    advance(&p);

    // Parse all lines
    while (!check(&p, TOK_EOF) && !p.had_error) {
        if (!parse_line(&p)) {
            break;
        }
    }

    if (p.had_error) {
        return bc_err(arena, BC_ERR_SYNTAX, filename,
                      p.current.line, p.current.column,
                      "%s", p.error_msg);
    }

    return BC_OK(p.ast);
}

BcResult parse_file(Arena *arena, StrPool *strings, const char *filename) {
    // Read file
    FILE *f = fopen(filename, "rb");
    if (!f) {
        return bc_errf(arena, BC_ERR_IO, "cannot open file '%s'", filename);
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fclose(f);
        return bc_errf(arena, BC_ERR_IO, "cannot determine size of '%s'", filename);
    }

    char *source = arena_alloc(arena, (size_t)size + 1);
    size_t read = fread(source, 1, (size_t)size, f);
    fclose(f);

    if (read != (size_t)size) {
        return bc_errf(arena, BC_ERR_IO, "cannot read file '%s'", filename);
    }
    source[size] = '\0';

    return parse(arena, strings, source, (size_t)size, filename);
}
