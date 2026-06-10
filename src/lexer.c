#include "beancraft/lexer.h"
#include <string.h>
#include <ctype.h>

void lexer_init(Lexer *lex, Arena *arena, StrPool *strings,
                const char *source, size_t len, const char *filename) {
    lex->arena = arena;
    lex->strings = strings;
    lex->source = source;
    lex->filename = filename;
    lex->pos = 0;
    lex->len = len;
    lex->line = 1;
    lex->column = 1;
    lex->line_start = 0;
}

static inline bool is_at_end(const Lexer *lex) {
    return lex->pos >= lex->len;
}

static inline char peek_char(const Lexer *lex) {
    if (is_at_end(lex)) return '\0';
    return lex->source[lex->pos];
}

static inline char peek_char_ahead(const Lexer *lex, size_t offset) {
    if (lex->pos + offset >= lex->len) return '\0';
    return lex->source[lex->pos + offset];
}

static char advance(Lexer *lex) {
    if (is_at_end(lex)) return '\0';
    char c = lex->source[lex->pos++];
    if (c == '\n') {
        lex->line++;
        lex->column = 1;
        lex->line_start = lex->pos;
    } else {
        lex->column++;
    }
    return c;
}

static void skip_whitespace(Lexer *lex) {
    while (!is_at_end(lex)) {
        char c = peek_char(lex);
        switch (c) {
        case ' ':
        case '\t':
        case '\r':
        case '\f':
        case '\v':
            advance(lex);
            break;
        default:
            return;
        }
    }
}

static void skip_comment(Lexer *lex) {
    // Skip until end of line or file
    while (!is_at_end(lex) && peek_char(lex) != '\n') {
        advance(lex);
    }
}

static bool is_ident_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

static bool is_ident_char(char c) {
    // '/' lets device registers be written `mouse/x`, `con/byte`, etc.
    return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '/';
}

static Token make_token(TokenKind kind, uint32_t start_line, uint32_t start_col) {
    return (Token){
        .kind = kind,
        .line = start_line,
        .column = start_col,
    };
}

static Token make_error(const char *msg, uint32_t line, uint32_t col) {
    return (Token){
        .kind = TOK_ERROR,
        .line = line,
        .column = col,
        .error = msg,
    };
}

static TokenKind check_keyword(const char *start, size_t len) {
    // Check for instruction keywords
    if (len == 3) {
        if (memcmp(start, "inc", 3) == 0) return TOK_INC;
        if (memcmp(start, "deb", 3) == 0) return TOK_DEB;
        if (memcmp(start, "end", 3) == 0) return TOK_END;
        if (memcmp(start, "use", 3) == 0) return TOK_USE;
    }

    // Check for jump keywords
    if (len == 4) {
        if (memcmp(start, "self", 4) == 0) return TOK_SELF;
        if (memcmp(start, "next", 4) == 0) return TOK_NEXT;
        if (memcmp(start, "prev", 4) == 0) return TOK_PREV;
        if (memcmp(start, "init", 4) == 0) return TOK_INIT;
        if (memcmp(start, "done", 4) == 0) return TOK_DONE;
        if (memcmp(start, "halt", 4) == 0) return TOK_HALT;
        if (memcmp(start, "func", 4) == 0) return TOK_FUNC;
    }

    return TOK_IDENT;
}

static Token scan_identifier(Lexer *lex) {
    uint32_t start_line = lex->line;
    uint32_t start_col = lex->column;
    size_t start = lex->pos;

    while (!is_at_end(lex) && is_ident_char(peek_char(lex))) {
        advance(lex);
    }

    size_t len = lex->pos - start;
    const char *text = lex->source + start;

    TokenKind kind = check_keyword(text, len);
    Token tok = make_token(kind, start_line, start_col);

    // Always intern the string for identifiers
    tok.str = str_intern(lex->strings, text, len);

    return tok;
}

static Token scan_number(Lexer *lex) {
    uint32_t start_line = lex->line;
    uint32_t start_col = lex->column;
    size_t start = lex->pos;

    // Handle optional sign
    bool negative = false;
    if (peek_char(lex) == '+' || peek_char(lex) == '-') {
        negative = (peek_char(lex) == '-');
        advance(lex);
    }

    while (!is_at_end(lex) && isdigit((unsigned char)peek_char(lex))) {
        advance(lex);
    }

    size_t len = lex->pos - start;
    if (len == 0 || (len == 1 && (lex->source[start] == '+' || lex->source[start] == '-'))) {
        return make_error("expected number", start_line, start_col);
    }

    // Parse the number, rejecting anything that won't fit in int64_t (the
    // unchecked multiply-add is signed-overflow UB on 20+ digit literals).
    int64_t value = 0;
    for (size_t i = start; i < lex->pos; i++) {
        char c = lex->source[i];
        if (c == '+' || c == '-') continue;
        int digit = c - '0';
        if (value > (INT64_MAX - digit) / 10) {
            return make_error("number literal too large", start_line, start_col);
        }
        value = value * 10 + digit;
    }
    if (negative) value = -value;

    Token tok = make_token(TOK_NUMBER, start_line, start_col);
    tok.number = value;
    return tok;
}

static Token scan_string(Lexer *lex) {
    uint32_t start_line = lex->line;
    uint32_t start_col = lex->column;

    // Skip opening quote
    advance(lex);

    size_t start = lex->pos;
    while (!is_at_end(lex) && peek_char(lex) != '"' && peek_char(lex) != '\n') {
        advance(lex);
    }

    if (is_at_end(lex) || peek_char(lex) == '\n') {
        return make_error("unterminated string", start_line, start_col);
    }

    size_t len = lex->pos - start;

    // Skip closing quote
    advance(lex);

    Token tok = make_token(TOK_STRING, start_line, start_col);
    tok.str = str_intern(lex->strings, lex->source + start, len);
    return tok;
}

Token lexer_next(Lexer *lex) {
    skip_whitespace(lex);

    if (is_at_end(lex)) {
        return make_token(TOK_EOF, lex->line, lex->column);
    }

    uint32_t start_line = lex->line;
    uint32_t start_col = lex->column;
    char c = peek_char(lex);

    // Comments
    if (c == '#') {
        skip_comment(lex);
        return lexer_next(lex);  // Recurse to get next real token
    }

    // Newlines
    if (c == '\n') {
        advance(lex);
        return make_token(TOK_NEWLINE, start_line, start_col);
    }

    // Single-character tokens
    switch (c) {
    case ':':
        advance(lex);
        return make_token(TOK_COLON, start_line, start_col);
    case '=':
        advance(lex);
        return make_token(TOK_EQUALS, start_line, start_col);
    case '~':
        advance(lex);
        return make_token(TOK_TILDE, start_line, start_col);
    case '.':
        advance(lex);
        return make_token(TOK_END, start_line, start_col);
    case '%':
        advance(lex);
        return make_token(TOK_USE, start_line, start_col);
    case '{':
        advance(lex);
        return make_token(TOK_LBRACE, start_line, start_col);
    case '}':
        advance(lex);
        return make_token(TOK_RBRACE, start_line, start_col);
    case '"':
        return scan_string(lex);
    }

    // + and - can be instructions or numeric offsets
    if (c == '+' || c == '-') {
        // Check if followed by a digit
        char next = peek_char_ahead(lex, 1);
        if (isdigit((unsigned char)next)) {
            // It's a numeric offset like +1 or -2
            return scan_number(lex);
        }
        // It's an instruction
        advance(lex);
        return make_token(c == '+' ? TOK_INC : TOK_DEB, start_line, start_col);
    }

    // Numbers
    if (isdigit((unsigned char)c)) {
        return scan_number(lex);
    }

    // Identifiers and keywords
    if (is_ident_start(c)) {
        return scan_identifier(lex);
    }

    // Unknown character
    advance(lex);
    return make_error("unexpected character", start_line, start_col);
}

Token lexer_peek(Lexer *lex) {
    // Save state
    size_t pos = lex->pos;
    uint32_t line = lex->line;
    uint32_t column = lex->column;
    uint32_t line_start = lex->line_start;

    Token tok = lexer_next(lex);

    // Restore state
    lex->pos = pos;
    lex->line = line;
    lex->column = column;
    lex->line_start = line_start;

    return tok;
}

const char *token_kind_name(TokenKind kind) {
    switch (kind) {
    case TOK_EOF:     return "end of file";
    case TOK_NEWLINE: return "newline";
    case TOK_INC:     return "inc";
    case TOK_DEB:     return "deb";
    case TOK_END:     return "end";
    case TOK_USE:     return "use";
    case TOK_FUNC:    return "func";
    case TOK_COLON:   return ":";
    case TOK_EQUALS:  return "=";
    case TOK_TILDE:   return "~";
    case TOK_LBRACE:  return "{";
    case TOK_RBRACE:  return "}";
    case TOK_IDENT:   return "identifier";
    case TOK_NUMBER:  return "number";
    case TOK_STRING:  return "string";
    case TOK_SELF:    return "self";
    case TOK_NEXT:    return "next";
    case TOK_PREV:    return "prev";
    case TOK_INIT:    return "init";
    case TOK_DONE:    return "done";
    case TOK_HALT:    return "halt";
    case TOK_ERROR:   return "error";
    default:          return "unknown";
    }
}

bool token_is_keyword(TokenKind kind) {
    return kind >= TOK_SELF && kind <= TOK_HALT;
}

bool token_is_jump_target(TokenKind kind) {
    return kind == TOK_IDENT ||
           kind == TOK_NUMBER ||
           token_is_keyword(kind);
}
