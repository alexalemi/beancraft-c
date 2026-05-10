#ifndef BC_PARSER_H
#define BC_PARSER_H

#include "arena.h"
#include "str.h"
#include "ast.h"
#include "error.h"

// Parse source code into an AST
// Returns BC_OK with Ast* on success, or error on failure
BcResult parse(Arena *arena, StrPool *strings,
               const char *source, size_t len,
               const char *filename);

// Parse a file
BcResult parse_file(Arena *arena, StrPool *strings,
                    const char *filename);

#endif // BC_PARSER_H
