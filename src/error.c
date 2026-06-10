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

// Format `fmt` into an arena-allocated message; never returns NULL (a
// vsnprintf encoding failure falls back to a placeholder rather than letting
// NULL reach a %s downstream).
static char *format_message(Arena *arena, const char *fmt, va_list args) {
    va_list args_copy;
    va_copy(args_copy, args);
    int len = vsnprintf(NULL, 0, fmt, args_copy);
    va_end(args_copy);

    if (len < 0) return "(error message formatting failed)";
    char *message = arena_alloc(arena, (size_t)len + 1);
    vsnprintf(message, (size_t)len + 1, fmt, args);
    return message;
}

BcResult bc_err(Arena *arena, BcErrorKind kind, const char *filename,
                uint32_t line, uint32_t column, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *message = format_message(arena, fmt, args);
    va_end(args);

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
    char *message = format_message(arena, fmt, args);
    va_end(args);

    BcError err = {
        .kind = kind,
        .message = message,
        .filename = NULL,
        .line = 0,
        .column = 0,
    };

    return (BcResult){ .ok = false, .error = err };
}

// Build the "file:line:col: " location prefix (possibly empty) into buf.
static const char *location_prefix(const BcError *err, char *buf, size_t n) {
    if (err->filename && err->line > 0 && err->column > 0) {
        snprintf(buf, n, "%s:%u:%u: ", err->filename, err->line, err->column);
    } else if (err->filename && err->line > 0) {
        snprintf(buf, n, "%s:%u: ", err->filename, err->line);
    } else if (err->filename) {
        snprintf(buf, n, "%s: ", err->filename);
    } else {
        buf[0] = '\0';
    }
    return buf;
}

char *bc_error_format(Arena *arena, const BcError *err) {
    if (!err) return NULL;
    char loc[512];
    return arena_sprintf(arena, "%s%s: %s",
                         location_prefix(err, loc, sizeof loc),
                         error_kind_str(err->kind), err->message);
}

void bc_error_print(const BcError *err) {
    if (!err) return;
    char loc[512];
    fprintf(stderr, "%s%s: %s\n",
            location_prefix(err, loc, sizeof loc),
            error_kind_str(err->kind), err->message);
}
