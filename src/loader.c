#define _GNU_SOURCE
#include "beancraft/loader.h"
#include "beancraft/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>

// FNV-1a hash for strings
static uint32_t hash_string(const char *s) {
    uint32_t hash = 2166136261u;
    while (*s) {
        hash ^= (uint8_t)*s++;
        hash *= 16777619u;
    }
    return hash;
}

ModuleCache *module_cache_new(Arena *arena, StrPool *strings) {
    ModuleCache *cache = arena_alloc(arena, sizeof(ModuleCache));
    cache->arena = arena;
    cache->strings = strings;
    cache->bucket_count = 64;
    cache->buckets = arena_alloc(arena, sizeof(ModuleCacheEntry *) * cache->bucket_count);
    for (uint32_t i = 0; i < cache->bucket_count; i++) {
        cache->buckets[i] = NULL;
    }
    return cache;
}

// Check if module is in cache
static Ast *cache_lookup(ModuleCache *cache, Str *path) {
    uint32_t bucket = hash_string(path->data) % cache->bucket_count;
    ModuleCacheEntry *entry = cache->buckets[bucket];
    while (entry) {
        if (entry->path == path) {
            return entry->ast;
        }
        entry = entry->next;
    }
    return NULL;
}

// Add module to cache
static void cache_insert(ModuleCache *cache, Str *path, Ast *ast) {
    uint32_t bucket = hash_string(path->data) % cache->bucket_count;
    ModuleCacheEntry *entry = arena_alloc(cache->arena, sizeof(ModuleCacheEntry));
    entry->path = path;
    entry->ast = ast;
    entry->next = cache->buckets[bucket];
    cache->buckets[bucket] = entry;
}

char *loader_resolve_path(const char *module_name, const char *base_path) {
    // Add .bc extension if needed
    size_t name_len = strlen(module_name);
    bool has_ext = name_len > 3 && strcmp(module_name + name_len - 3, ".bc") == 0;

    char *filename;
    if (has_ext) {
        filename = strdup(module_name);
    } else {
        filename = malloc(name_len + 4);
        sprintf(filename, "%s.bc", module_name);
    }

    // Try relative to base path first
    if (base_path) {
        size_t base_len = strlen(base_path);
        size_t file_len = strlen(filename);
        char *full_path = malloc(base_len + file_len + 2);
        sprintf(full_path, "%s/%s", base_path, filename);

        struct stat st;
        if (stat(full_path, &st) == 0) {
            free(filename);
            // Return realpath for canonical form
            char *real = realpath(full_path, NULL);
            free(full_path);
            return real;
        }
        free(full_path);
    }

    // Try current directory
    struct stat st;
    if (stat(filename, &st) == 0) {
        char *real = realpath(filename, NULL);
        free(filename);
        return real;
    }

    // Try BEANCRAFT_PATH
    const char *env_path = getenv("BEANCRAFT_PATH");
    if (env_path) {
        char *paths = strdup(env_path);
        char *saveptr;
        char *dir = strtok_r(paths, ":", &saveptr);
        while (dir) {
            size_t dir_len = strlen(dir);
            size_t file_len = strlen(filename);
            char *full_path = malloc(dir_len + file_len + 2);
            sprintf(full_path, "%s/%s", dir, filename);

            if (stat(full_path, &st) == 0) {
                free(paths);
                free(filename);
                char *real = realpath(full_path, NULL);
                free(full_path);
                return real;
            }
            free(full_path);
            dir = strtok_r(NULL, ":", &saveptr);
        }
        free(paths);
    }

    free(filename);
    return NULL;
}

LoaderContext *loader_new(Arena *arena, StrPool *strings, const char *base_path) {
    LoaderContext *ctx = arena_alloc(arena, sizeof(LoaderContext));
    ctx->arena = arena;
    ctx->strings = strings;
    ctx->cache = module_cache_new(arena, strings);
    ctx->funcs = NULL;
    ctx->base_path = base_path;
    ctx->use_counter = 0;
    ctx->depth = 0;
    return ctx;
}

// Create a scoped name: "scope/name" or just "name" if no scope
static Str *make_scoped_name(StrPool *strings, const char *scope, Str *name) {
    if (!scope || scope[0] == '\0') {
        return name;
    }

    size_t scope_len = strlen(scope);
    size_t name_len = name->len;
    char *buf = malloc(scope_len + 1 + name_len + 1);
    sprintf(buf, "%s/%s", scope, name->data);
    Str *result = str_intern(strings, buf, scope_len + 1 + name_len);
    free(buf);
    return result;
}

// Everything needed to inline one module / func body: how to rename its
// registers and labels, plus the synthetic entry/return points.
typedef struct {
    StrPool *strings;
    const RegMapping *rmaps;     uint32_t rmap_count;
    const LabelMapping *lmaps;   uint32_t lmap_count;
    const char *scope;
    Str *return_label;           // target of done/halt and the body's own END
    Str *entry_label;            // target of init
} InlineCtx;

// Rename a register reference inside the inlined body:
//   local=import  -> the caller's register `import` (used directly, unscoped)
//   local=value   -> a private scoped register (expand_module_body seeds it)
//   (unmapped)    -> a private scoped register
static Str *apply_reg_mapping(const InlineCtx *ic, Str *reg) {
    for (uint32_t i = 0; i < ic->rmap_count; i++) {
        if (ic->rmaps[i].local == reg) {
            if (!ic->rmaps[i].is_value) return ic->rmaps[i].import;
            break;
        }
    }
    return make_scoped_name(ic->strings, ic->scope, reg);
}

// Rename a label reference inside the inlined body.
static Str *apply_label_mapping(const InlineCtx *ic, Str *label) {
    for (uint32_t i = 0; i < ic->lmap_count; i++) {
        if (ic->lmaps[i].local == label) return ic->lmaps[i].import;
    }
    return make_scoped_name(ic->strings, ic->scope, label);
}

// Rewrite a jump target inside the inlined body: labels are renamed, done/halt
// become a jump to the return point, and init becomes a jump to the entry point.
static Jump transform_jump(const InlineCtx *ic, Jump jump) {
    if (jump.kind == JUMP_LABEL) {
        return jump_label(apply_label_mapping(ic, jump.label));
    }
    if (jump.kind == JUMP_KEYWORD) {
        if (jump.keyword == KW_DONE || jump.keyword == KW_HALT) return jump_label(ic->return_label);
        if (jump.keyword == KW_INIT) return jump_label(ic->entry_label);
    }
    return jump;
}

// Clone one statement of an inlined body into `dest`, renaming as per `ic`.
static void clone_and_transform_node(Ast *dest, const AstNode *src, const InlineCtx *ic) {
    AstNode *node = ast_add_node(dest);
    node->kind = src->kind;
    node->line = src->line;
    node->column = src->column;
    node->label = src->label ? apply_label_mapping(ic, src->label) : NULL;

    switch (src->kind) {
    case AST_INC:
        node->inc.reg = apply_reg_mapping(ic, src->inc.reg);
        node->inc.next = transform_jump(ic, src->inc.next);
        break;

    case AST_DEB:
        node->deb.reg = apply_reg_mapping(ic, src->deb.reg);
        node->deb.jump = transform_jump(ic, src->deb.jump);
        node->deb.next = transform_jump(ic, src->deb.next);
        break;

    case AST_END:
        // The body's END means "return to caller".
        node->kind = AST_DEB;
        node->deb.reg = str_intern_cstr(ic->strings, ":nil");
        node->deb.jump = jump_label(ic->return_label);
        node->deb.next = jump_label(ic->return_label);
        break;

    case AST_USE:
        // Nested use -- copy as-is (expanded in a later pass); just re-scope it.
        node->use = src->use;
        if (src->use.scope) node->use.scope = make_scoped_name(ic->strings, ic->scope, src->use.scope);
        break;

    case AST_CALL: {
        // Nested call -- rename its argument registers; expanded in a later pass.
        node->call.name = src->call.name;
        node->call.arg_count = src->call.arg_count;
        if (src->call.arg_count > 0) {
            node->call.args = arena_alloc(dest->arena, src->call.arg_count * sizeof(Str *));
            for (uint32_t i = 0; i < src->call.arg_count; i++)
                node->call.args[i] = apply_reg_mapping(ic, src->call.args[i]);
        } else {
            node->call.args = NULL;
        }
        break;
    }

    case AST_FUNCDEF:
        // Functions are top-level only; a body shouldn't contain one. Drop it.
        node->kind = AST_END;
        break;
    }
}

// Load and parse a module file
static BcResult load_module(LoaderContext *ctx, const char *path) {
    // Check cache first
    Str *path_str = str_intern(ctx->strings, path, strlen(path));
    Ast *cached = cache_lookup(ctx->cache, path_str);
    if (cached) {
        return BC_OK(cached);
    }

    // Parse the file
    BcResult result = parse_file(ctx->arena, ctx->strings, path);
    if (!result.ok) {
        return result;
    }

    Ast *ast = result.value;
    cache_insert(ctx->cache, path_str, ast);
    return BC_OK(ast);
}

// Append a labelled no-op (deb :nil next next) to dest. Used for the synthetic
// entry and return points of an inlined module.
static void add_nop_label(LoaderContext *ctx, Ast *dest, Str *label,
                          uint32_t line) {
    AstNode *node = ast_add_node(dest);
    node->kind = AST_DEB;
    node->label = label;
    node->line = line;
    node->column = 0;
    node->deb.reg = str_intern_cstr(ctx->strings, ":nil");
    node->deb.jump = jump_keyword(KW_NEXT);
    node->deb.next = jump_keyword(KW_NEXT);
}

// Inline a module / func body into `dest`, surrounded by entry and return
// no-ops. `body`/`body_count` is the body's statements; `rmaps`/`lmaps` bind its
// registers/labels; `scope` namespaces everything private; `my_label`, if
// non-NULL, is placed on the entry no-op (so callers can jump to the inlining
// site); `base_dir`, if non-NULL, is used to resolve nested `use` paths.
static void expand_module_body(LoaderContext *ctx, Ast *dest,
                               const AstNode *body, uint32_t body_count,
                               const RegMapping *rmaps, uint32_t rmap_count,
                               const LabelMapping *lmaps, uint32_t lmap_count,
                               const char *scope, Str *my_label, uint32_t line,
                               const char *base_dir) {
    char buf[96];
    Str *entry_label = my_label;
    if (!entry_label) {
        snprintf(buf, sizeof(buf), "%s/entry", scope);
        entry_label = str_intern_cstr(ctx->strings, buf);
    }
    snprintf(buf, sizeof(buf), "%s/return", scope);
    Str *return_label = str_intern_cstr(ctx->strings, buf);

    InlineCtx ic = {
        .strings = ctx->strings,
        .rmaps = rmaps, .rmap_count = rmap_count,
        .lmaps = lmaps, .lmap_count = lmap_count,
        .scope = scope,
        .return_label = return_label,
        .entry_label = entry_label,
    };

    // Entry no-op: target of `init` inside the body and of external jumps to
    // `my_label` -- not the program's instruction 0.
    add_nop_label(ctx, dest, entry_label, line);

    // Seed value-mapped registers: scoped L := N every time control enters here.
    for (uint32_t i = 0; i < rmap_count; i++) {
        if (!rmaps[i].is_value || rmaps[i].value <= 0) continue;
        Str *r = make_scoped_name(ctx->strings, scope, rmaps[i].local);

        AstNode *clr = ast_add_node(dest);
        clr->kind = AST_DEB;
        clr->line = line;
        clr->deb.reg = r;
        clr->deb.jump = jump_keyword(KW_NEXT);   // when already 0, fall through
        clr->deb.next = jump_keyword(KW_SELF);   // otherwise keep decrementing
        for (int64_t v = 0; v < rmaps[i].value; v++) {
            AstNode *inc = ast_add_node(dest);
            inc->kind = AST_INC;
            inc->line = line;
            inc->inc.reg = r;
            inc->inc.next = jump_none();
        }
    }

    uint32_t first_cloned = dest->node_count;
    for (uint32_t i = 0; i < body_count; i++) {
        clone_and_transform_node(dest, &body[i], &ic);
    }

    // Nested `use` statements should resolve relative to base_dir; make their
    // paths absolute now so the next expansion pass finds them.
    if (base_dir) {
        for (uint32_t i = first_cloned; i < dest->node_count; i++) {
            AstNode *n = &dest->nodes[i];
            if (n->kind != AST_USE) continue;
            char *resolved = loader_resolve_path(n->use.filename->data, base_dir);
            if (resolved) {
                n->use.filename = str_intern_cstr(ctx->strings, resolved);
                free(resolved);
            }
        }
    }

    // Return no-op: target of done/halt and the body's own END.
    add_nop_label(ctx, dest, return_label, line);
}

// Expand a `use "file"` statement into `dest`.
static BcResult expand_use(LoaderContext *ctx, Ast *dest,
                           const AstNode *use_node, uint32_t use_index) {
    char *module_path = loader_resolve_path(use_node->use.filename->data, ctx->base_path);
    if (!module_path) {
        return bc_err(ctx->arena, BC_ERR_SEMANTIC, NULL, use_node->line, 0,
                     "module '%s' not found", use_node->use.filename->data);
    }

    BcResult load_result = load_module(ctx, module_path);
    char *module_dir = strdup(module_path);
    char *dir = dirname(module_dir);
    free(module_path);
    if (!load_result.ok) {
        free(module_dir);
        return load_result;
    }
    Ast *module_ast = load_result.value;

    char scope_buf[64];
    if (use_node->use.scope) {
        snprintf(scope_buf, sizeof(scope_buf), "%s", use_node->use.scope->data);
    } else {
        snprintf(scope_buf, sizeof(scope_buf), "%s-%u",
                 use_node->use.filename->data, use_index);
    }

    expand_module_body(ctx, dest, module_ast->nodes, module_ast->node_count,
                       use_node->use.reg_mappings, use_node->use.reg_mapping_count,
                       use_node->use.label_mappings, use_node->use.label_mapping_count,
                       scope_buf, use_node->label, use_node->line, dir);
    free(module_dir);
    return BC_OK(NULL);
}

// Find a registered `func` by name (interned-pointer compare).
static const AstNode *func_lookup(LoaderContext *ctx, Str *name) {
    for (FuncEntry *e = ctx->funcs; e; e = e->next) {
        if (e->name == name) return e->def;
    }
    return NULL;
}

// Expand a `name arg arg ...` call into `dest`.
static BcResult expand_call(LoaderContext *ctx, Ast *dest,
                            const AstNode *call_node, uint32_t call_index) {
    const AstNode *def = func_lookup(ctx, call_node->call.name);
    if (!def) {
        return bc_err(ctx->arena, BC_ERR_SEMANTIC, NULL, call_node->line, 0,
                     "undefined function '%s'", call_node->call.name->data);
    }
    if (call_node->call.arg_count != def->funcdef.param_count) {
        return bc_err(ctx->arena, BC_ERR_SEMANTIC, NULL, call_node->line, 0,
                     "function '%s' takes %u argument(s) but %u given",
                     call_node->call.name->data, def->funcdef.param_count,
                     call_node->call.arg_count);
    }

    // Bind each positional argument to its parameter: register params become
    // register aliases, label params (~name) become label mappings.
    uint32_t np = def->funcdef.param_count;
    RegMapping *rmaps = np ? arena_alloc(ctx->arena, np * sizeof(RegMapping)) : NULL;
    LabelMapping *lmaps = np ? arena_alloc(ctx->arena, np * sizeof(LabelMapping)) : NULL;
    uint32_t rn = 0, ln = 0;
    for (uint32_t i = 0; i < np; i++) {
        if (def->funcdef.param_is_label[i]) {
            lmaps[ln].local = def->funcdef.params[i];
            lmaps[ln].import = call_node->call.args[i];
            ln++;
        } else {
            rmaps[rn].local = def->funcdef.params[i];
            rmaps[rn].import = call_node->call.args[i];
            rmaps[rn].is_value = false;
            rn++;
        }
    }

    char scope_buf[64];
    snprintf(scope_buf, sizeof(scope_buf), "%s-%u", call_node->call.name->data, call_index);

    expand_module_body(ctx, dest, def->funcdef.body, def->funcdef.body_count,
                       rmaps, rn, lmaps, ln, scope_buf,
                       call_node->label, call_node->line, NULL);
    return BC_OK(NULL);
}

BcResult loader_expand(LoaderContext *ctx, Ast *ast) {
    if (ctx->depth >= MAX_USE_DEPTH) {
        return bc_err(ctx->arena, BC_ERR_SEMANTIC, NULL, 0, 0,
                     "maximum expansion depth (%d) exceeded", MAX_USE_DEPTH);
    }
    ctx->depth++;

    // Register any `func` definitions found at this level. (On recursive calls
    // there are none -- they were dropped from `expanded` on the first pass --
    // and the table persists in ctx, so order of definition does not matter.)
    for (uint32_t i = 0; i < ast->node_count; i++) {
        if (ast->nodes[i].kind != AST_FUNCDEF) continue;
        FuncEntry *e = arena_alloc(ctx->arena, sizeof(FuncEntry));
        e->name = ast->nodes[i].funcdef.name;
        e->def = &ast->nodes[i];
        e->next = ctx->funcs;
        ctx->funcs = e;
    }

    Ast *expanded = ast_new(ctx->arena, ctx->strings);
    for (uint32_t i = 0; i < ast->node_count; i++) {
        AstNode *node = &ast->nodes[i];
        BcResult result;
        switch (node->kind) {
        case AST_FUNCDEF:
            continue;  // declarations: drop from the program
        case AST_USE:
            result = expand_use(ctx, expanded, node, ctx->use_counter++);
            if (!result.ok) { ctx->depth--; return result; }
            continue;
        case AST_CALL:
            result = expand_call(ctx, expanded, node, ctx->use_counter++);
            if (!result.ok) { ctx->depth--; return result; }
            continue;
        default: {
            AstNode *dst = ast_add_node(expanded);
            *dst = *node;
            continue;
        }
        }
    }

    // If expansion produced more uses/calls (a func body that uses/calls), keep
    // going. The recursive call mutates `expanded` in place.
    bool more = false;
    for (uint32_t i = 0; i < expanded->node_count && !more; i++) {
        AstKind k = expanded->nodes[i].kind;
        more = (k == AST_USE || k == AST_CALL);
    }
    if (more) {
        BcResult result = loader_expand(ctx, expanded);
        if (!result.ok) { ctx->depth--; return result; }
    }

    ctx->depth--;
    ast->nodes = expanded->nodes;
    ast->node_count = expanded->node_count;
    ast->node_capacity = expanded->node_capacity;
    return BC_OK(ast);
}
