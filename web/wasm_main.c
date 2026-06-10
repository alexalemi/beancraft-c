// WebAssembly entry point for the beancraft interpreter.
//
// Built by `make wasm` (needs Emscripten on PATH) into web/beancraft.mjs +
// web/beancraft.wasm; web/index.html is a small demo page that uses it.
//
// The QBE backend (and `bccompile`) shell out to `qbe`/`cc`, which can't run in
// a browser, so this build leaves them out -- it's the interpreter only:
// source string -> parse -> expand `use`/`func` -> IR -> optimize -> interpret.

#define _GNU_SOURCE
#include "beancraft/arena.h"
#include "beancraft/str.h"
#include "beancraft/parser.h"
#include "beancraft/loader.h"
#include "beancraft/ir.h"
#include "beancraft/opt.h"
#include "beancraft/interp.h"
#include "beancraft/devices.h"
#include "beancraft/error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

#define BC_DEFAULT_MAX_STEPS 100000000ULL

// A small growable string.
typedef struct { char *p; size_t len, cap; } Buf;

static void buf_puts(Buf *b, const char *s) {
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        do { b->cap = b->cap ? b->cap * 2 : 256; } while (b->len + n + 1 > b->cap);
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->len, s, n + 1);
    b->len += n;
}
static char *buf_take(Buf *b) { return b->p ? b->p : strdup(""); }

// Format a BcResult error as "error: ...", free the arena, return a heap string.
static char *fail(Arena *arena, const BcError *err) {
    char *m = bc_error_format(arena, err);
    Buf b = { 0 };
    buf_puts(&b, "error: ");
    buf_puts(&b, m && *m ? m : "unknown error");
    arena_free(arena);
    return buf_take(&b);
}

// Final-frame snapshot of the screen device, taken just before shutdown so the
// page can draw it on a canvas after the (synchronous) run finishes.
static uint8_t *g_fb_rgba = NULL;   // w*h*4 bytes, row-major RGBA
static uint32_t g_fb_w = 0, g_fb_h = 0;

uint32_t EMSCRIPTEN_KEEPALIVE bc_fb_width(void)  { return g_fb_w; }
uint32_t EMSCRIPTEN_KEEPALIVE bc_fb_height(void) { return g_fb_h; }
uint8_t *EMSCRIPTEN_KEEPALIVE bc_fb_rgba(void)   { return g_fb_rgba; }

static void snapshot_screen(void) {
    free(g_fb_rgba);
    g_fb_rgba = NULL;
    g_fb_w = g_fb_h = 0;

    uint32_t w, h;
    const uint8_t *fb = device_screen_fb(&w, &h);
    if (!fb) return;
    const uint32_t *pal = device_screen_palette();
    uint8_t *rgba = malloc((size_t)w * h * 4);
    if (!rgba) return;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint32_t c = pal[fb[i]];
        rgba[i * 4 + 0] = (uint8_t)(c >> 16);
        rgba[i * 4 + 1] = (uint8_t)(c >> 8);
        rgba[i * 4 + 2] = (uint8_t)c;
        rgba[i * 4 + 3] = 0xff;
    }
    g_fb_rgba = rgba;
    g_fb_w = w;
    g_fb_h = h;
}

// Run a beancraft program given as a source string, with the optimizer on.
//
//   source           the .bc program text
//   reg_assignments   space-separated NAME=VALUE pairs (values may be
//                     arbitrary-precision integers); unknown names are ignored
//   max_steps         step cap; 0 means "use the default"
//
// Returns a heap-allocated, NUL-terminated string (free it with bc_free): the
// final register dump, or "error: <message>", optionally preceded by a
// "(execution limit reached ...)" note. Anything the program prints via
// `con/emit` / `con/err` goes to stdout/stderr -> the host's print callbacks.
char *EMSCRIPTEN_KEEPALIVE bc_run_source(const char *source,
                                         const char *reg_assignments,
                                         unsigned long long max_steps) {
    if (!source) source = "";
    if (max_steps == 0) max_steps = BC_DEFAULT_MAX_STEPS;

    Arena *arena = arena_new(64 * 1024);
    StrPool *strings = strpool_new(arena);

    BcResult pr = parse(arena, strings, source, strlen(source), "<input>");
    if (!pr.ok) return fail(arena, &pr.error);
    Ast *ast = pr.value;

    // Expand `use`/`func`. There's no real base path for a source *string*, and
    // the virtual FS is empty by default, so `use "file"` won't resolve -- but
    // `func`-based programs are fully self-contained and work fine.
    LoaderContext *loader = loader_new(arena, strings, ".");
    BcResult ex = loader_expand(loader, ast);
    if (!ex.ok) return fail(arena, &ex.error);

    BcResult ir = ir_from_ast(arena, strings, ast);
    if (!ir.ok) return fail(arena, &ir.error);
    IrProgram *prog = ir.value;

    IrOptProgram *opt = ir_optimize(arena, prog, OPT_LOOPS);

    InterpState *st = interp_new(arena, opt);
    interp_init_regs(st);

    if (reg_assignments && *reg_assignments) {
        char *copy = strdup(reg_assignments), *save = NULL;
        for (char *tok = strtok_r(copy, " \t\r\n", &save); tok;
             tok = strtok_r(NULL, " \t\r\n", &save)) {
            char *eq = strchr(tok, '=');
            if (!eq) continue;
            *eq = '\0';
            Bignum v = bignum_from_string(eq + 1);
            interp_set_reg_bignum(st, tok, v);
            bignum_free(&v);
        }
        free(copy);
    }

    // Wire the device subsystem (no-op unless the program names a magic reg).
    const char **dev_names = arena_alloc(arena, opt->reg_count * sizeof(char *));
    for (uint32_t i = 0; i < opt->reg_count; i++) dev_names[i] = opt->reg_names[i]->data;
    if (device_init(dev_names, opt->reg_count, st->regs)) {
        st->inc_mask = device_inc_mask();
        st->deb_mask = device_deb_mask();
    }

    interp_run(st, max_steps);
    fflush(stdout);
    fflush(stderr);

    Buf out = { 0 };
    if (!st->halted) {
        char note[80];
        snprintf(note, sizeof note, "(execution limit reached after %llu steps)\n",
                 (unsigned long long)st->steps);
        buf_puts(&out, note);
    }
    for (uint32_t i = 0; i < opt->reg_count; i++) {
        const char *name = opt->reg_names[i]->data;
        // Skip internals (`:nil`), device registers and loader-generated scoped
        // scratch (`con/byte`, `copy-0/tmp`, …) -- both have a '/'.
        if (name[0] == ':' || strchr(name, '/')) continue;
        char *val = bignum_to_string(st->regs[i]);
        buf_puts(&out, name);
        buf_puts(&out, " = ");
        buf_puts(&out, val);
        buf_puts(&out, "\n");
        free(val);
    }

    snapshot_screen();   // before shutdown frees the framebuffer
    interp_cleanup(st);
    device_shutdown();
    arena_free(arena);
    return buf_take(&out);
}

void EMSCRIPTEN_KEEPALIVE bc_free(char *p) { free(p); }

// Present only to keep Emscripten happy; the runtime is driven from JS.
int main(void) { return 0; }
