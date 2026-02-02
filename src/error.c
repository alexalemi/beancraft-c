#include "beancraft/error.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static const char *error_kind_str(BcErrorKind kind) {
    switch (kind) {
    case BC_ERR_NONE:     return "none";
    case BC_ERR_IO:       return "io error";
    case BC_ERR_SYNTAX:   return "syntax error";
    case BC_ERR_SEMANTIC: return "semantic error";
    case BC_ERR_RUNTIME:  return "runtime error";
    case BC_ERR_OVERFLOW: return "execution limit exceeded";
    default:              return "error";
    }
}

BcResult bc_err(Arena *arena, BcErrorKind kind, const char *filename,
                uint32_t line, uint32_t column, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    // Format the message
    va_list args_copy;
    va_copy(args_copy, args);
    int len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    char *message = NULL;
    if (len >= 0) {
        message = arena_alloc(arena, (size_t)len + 1);
        vsnprintf(message, (size_t)len + 1, fmt, args_copy);
    }
    va_end(args_copy);

    BcError err = {
        .kind = kind,
        .message = message,
        .filename = filename ? arena_strdup(arena, filename) : NULL,
        .line = line,
        .column = column,
    };

    return (BcResult){ .ok = false, .error = err };
}

BcResult bc_errf(Arena *arena, BcErrorKind kind, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    va_list args_copy;
    va_copy(args_copy, args);
    int len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    char *message = NULL;
    if (len >= 0) {
        message = arena_alloc(arena, (size_t)len + 1);
        vsnprintf(message, (size_t)len + 1, fmt, args_copy);
    }
    va_end(args_copy);

    BcError err = {
        .kind = kind,
        .message = message,
        .filename = NULL,
        .line = 0,
        .column = 0,
    };

    return (BcResult){ .ok = false, .error = err };
}

char *bc_error_format(Arena *arena, const BcError *err) {
    if (!err) return NULL;

    if (err->filename && err->line > 0 && err->column > 0) {
        return arena_sprintf(arena, "%s:%u:%u: %s: %s",
                            err->filename, err->line, err->column,
                            error_kind_str(err->kind), err->message);
    } else if (err->filename && err->line > 0) {
        return arena_sprintf(arena, "%s:%u: %s: %s",
                            err->filename, err->line,
                            error_kind_str(err->kind), err->message);
    } else if (err->filename) {
        return arena_sprintf(arena, "%s: %s: %s",
                            err->filename,
                            error_kind_str(err->kind), err->message);
    } else {
        return arena_sprintf(arena, "%s: %s",
                            error_kind_str(err->kind), err->message);
    }
}

void bc_error_print(const BcError *err) {
    if (!err) return;

    if (err->filename && err->line > 0 && err->column > 0) {
        fprintf(stderr, "%s:%u:%u: %s: %s\n",
                err->filename, err->line, err->column,
                error_kind_str(err->kind), err->message);
    } else if (err->filename && err->line > 0) {
        fprintf(stderr, "%s:%u: %s: %s\n",
                err->filename, err->line,
                error_kind_str(err->kind), err->message);
    } else if (err->filename) {
        fprintf(stderr, "%s: %s: %s\n",
                err->filename,
                error_kind_str(err->kind), err->message);
    } else {
        fprintf(stderr, "%s: %s\n",
                error_kind_str(err->kind), err->message);
    }
}
