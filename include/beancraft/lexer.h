#ifndef BC_LEXER_H
#define BC_LEXER_H

#include "arena.h"
#include "str.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Token types
typedef enum {
    TOK_EOF = 0,
    TOK_NEWLINE,

    // Instructions
    TOK_INC,          // 'inc' or '+'
    TOK_DEB,          // 'deb' or '-'
    TOK_END,          // 'end' or '.'
    TOK_USE,          // 'use' or '%'
    TOK_FUNC,         // 'func'

    // Punctuation
    TOK_COLON,        // ':'
    TOK_EQUALS,       // '='
    TOK_TILDE,        // '~'
    TOK_LBRACE,       // '{'
    TOK_RBRACE,       // '}'

    // Literals
    TOK_IDENT,        // Register name, label, keyword
    TOK_NUMBER,       // Integer literal
    TOK_STRING,       // "quoted string"

    // Keywords (these are also identifiers but have special meaning in jumps)
    TOK_SELF,
    TOK_NEXT,
    TOK_PREV,
    TOK_INIT,
    TOK_DONE,
    TOK_HALT,

    // Error
    TOK_ERROR,
} TokenKind;

typedef struct {
    TokenKind kind;
    uint32_t line;
    uint32_t column;
    union {
        Str *str;         // For IDENT, STRING
        int64_t number;   // For NUMBER (can be negative for offsets like -1)
        const char *error; // For ERROR
    };
} Token;

typedef struct {
    Arena *arena;
    StrPool *strings;
    const char *source;
    const char *filename;
    size_t pos;
    size_t len;
    uint32_t line;
    uint32_t column;
    uint32_t line_start;  // Position of current line start
} Lexer;

// Initialize a lexer
void lexer_init(Lexer *lex, Arena *arena, StrPool *strings,
                const char *source, size_t len, const char *filename);

// Get the next token
Token lexer_next(Lexer *lex);

// Peek at the next token without consuming it
Token lexer_peek(Lexer *lex);

// Get a string representation of a token kind
const char *token_kind_name(TokenKind kind);

// Check if a token is a keyword (self, next, prev, etc.)
bool token_is_keyword(TokenKind kind);

// Check if a token can be a jump target
bool token_is_jump_target(TokenKind kind);

#endif // BC_LEXER_H
