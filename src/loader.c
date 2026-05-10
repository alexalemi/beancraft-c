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

// Apply register mappings to a register name.
//   local=import  -> the importer's register `import` (used directly, unscoped)
//   local=value   -> a private scoped register (expand_use seeds it to `value`)
//   (unmapped)    -> a private scoped register
static Str *apply_reg_mapping(const AstNode *use_node, Str *reg,
                              StrPool *strings, const char *scope) {
    for (uint32_t i = 0; i < use_node->use.reg_mapping_count; i++) {
        RegMapping *m = &use_node->use.reg_mappings[i];
        if (m->local == reg) {
            if (!m->is_value) return m->import;   // register alias: no scoping
            break;                                // value mapping: scoped local (seeded below)
        }
    }
    return make_scoped_name(strings, scope, reg);
}

// Apply label mappings to a label
static Str *apply_label_mapping(const AstNode *use_node, Str *label,
                                StrPool *strings, const char *scope) {
    // Check if label matches any mapping
    for (uint32_t i = 0; i < use_node->use.label_mapping_count; i++) {
        LabelMapping *m = &use_node->use.label_mappings[i];
        if (m->local == label) {
            return m->import;
        }
    }
    // No mapping - apply scope prefix
    return make_scoped_name(strings, scope, label);
}

// Transform a jump target with register/label mappings.
//   return_label - target for done/halt (returns from the module)
//   entry_label  - target for init (the module's own first instruction)
static Jump transform_jump(const AstNode *use_node, Jump jump,
                          StrPool *strings, const char *scope,
                          Str *return_label, Str *entry_label) {
    if (jump.kind == JUMP_LABEL) {
        Jump result = jump;
        result.label = apply_label_mapping(use_node, jump.label, strings, scope);
        return result;
    }
    if (jump.kind == JUMP_KEYWORD) {
        // done/halt return from the module; init jumps to its first instruction
        if (jump.keyword == KW_DONE || jump.keyword == KW_HALT) {
            return jump_label(return_label);
        }
        if (jump.keyword == KW_INIT) {
            return jump_label(entry_label);
        }
    }
    return jump;
}

// Clone and transform a single AST node from a module.
//   return_label - replaces done/halt keywords (and the module's own END)
//   entry_label  - replaces the init keyword
static void clone_and_transform_node(Ast *dest, const AstNode *src,
                                     const AstNode *use_node,
                                     StrPool *strings, const char *scope,
                                     Str *return_label, Str *entry_label) {
    AstNode *node = ast_add_node(dest);
    node->kind = src->kind;
    node->line = src->line;
    node->column = src->column;

    // Transform label if present
    if (src->label) {
        node->label = apply_label_mapping(use_node, src->label, strings, scope);
    } else {
        node->label = NULL;
    }

    switch (src->kind) {
    case AST_INC:
        node->inc.reg = apply_reg_mapping(use_node, src->inc.reg, strings, scope);
        node->inc.next = transform_jump(use_node, src->inc.next, strings, scope,
                                        return_label, entry_label);
        break;

    case AST_DEB:
        node->deb.reg = apply_reg_mapping(use_node, src->deb.reg, strings, scope);
        node->deb.jump = transform_jump(use_node, src->deb.jump, strings, scope,
                                        return_label, entry_label);
        node->deb.next = transform_jump(use_node, src->deb.next, strings, scope,
                                        return_label, entry_label);
        break;

    case AST_END:
        // Transform END to jump to return label
        node->kind = AST_DEB;
        node->deb.reg = str_intern_cstr(strings, ":nil");
        node->deb.jump = jump_label(return_label);
        node->deb.next = jump_label(return_label);
        break;

    case AST_USE:
        // Nested use - copy as-is (will be expanded in next pass)
        node->use = src->use;
        // But transform the scope if present
        if (src->use.scope) {
            node->use.scope = make_scoped_name(strings, scope, src->use.scope);
        }
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

// Expand a single use statement into dest AST
static BcResult expand_use(LoaderContext *ctx, Ast *dest,
                           const AstNode *use_node, uint32_t use_index) {
    // Resolve module path
    char *module_path = loader_resolve_path(use_node->use.filename->data, ctx->base_path);
    if (!module_path) {
        return bc_err(ctx->arena, BC_ERR_SEMANTIC, NULL, use_node->line, 0,
                     "module '%s' not found", use_node->use.filename->data);
    }

    // Load and parse module
    BcResult load_result = load_module(ctx, module_path);

    // Directory of the module, used to resolve its own (nested) imports.
    char *module_dir = strdup(module_path);
    char *dir = dirname(module_dir);
    free(module_path);

    if (!load_result.ok) {
        free(module_dir);
        return load_result;
    }

    Ast *module_ast = load_result.value;

    // Generate scope prefix
    char scope_buf[64];
    if (use_node->use.scope) {
        snprintf(scope_buf, sizeof(scope_buf), "%s", use_node->use.scope->data);
    } else {
        // Generate unique scope from filename
        snprintf(scope_buf, sizeof(scope_buf), "%s-%u",
                 use_node->use.filename->data, use_index);
    }

    // Synthetic entry/return labels for this module invocation. If the `use`
    // statement itself carried a label, that label *is* the module's entry
    // point (so other code can jump to it).
    char buf[80];
    Str *entry_label;
    if (use_node->label) {
        entry_label = use_node->label;
    } else {
        snprintf(buf, sizeof(buf), "%s/entry", scope_buf);
        entry_label = str_intern_cstr(ctx->strings, buf);
    }
    snprintf(buf, sizeof(buf), "%s/return", scope_buf);
    Str *return_label = str_intern_cstr(ctx->strings, buf);

    // Entry no-op: `init` inside the module, and any external jump to the use
    // statement's label, both resolve here -- not to the program's first
    // instruction.
    add_nop_label(ctx, dest, entry_label, use_node->line);

    // Seed value-mapped registers (use "mod" L=N -> scoped L is set to N every
    // time control enters the module): clear, then increment N times.
    for (uint32_t i = 0; i < use_node->use.reg_mapping_count; i++) {
        RegMapping *m = &use_node->use.reg_mappings[i];
        if (!m->is_value || m->value <= 0) continue;
        Str *r = make_scoped_name(ctx->strings, scope_buf, m->local);

        AstNode *clr = ast_add_node(dest);
        clr->kind = AST_DEB;
        clr->line = use_node->line;
        clr->deb.reg = r;
        clr->deb.jump = jump_keyword(KW_NEXT);   // when already 0, fall through
        clr->deb.next = jump_keyword(KW_SELF);   // otherwise keep decrementing

        for (int64_t v = 0; v < m->value; v++) {
            AstNode *inc = ast_add_node(dest);
            inc->kind = AST_INC;
            inc->line = use_node->line;
            inc->inc.reg = r;
            inc->inc.next = jump_none();         // implicit next
        }
    }

    // Clone and transform each node from the module
    uint32_t first_cloned = dest->node_count;
    for (uint32_t i = 0; i < module_ast->node_count; i++) {
        clone_and_transform_node(dest, &module_ast->nodes[i], use_node,
                                 ctx->strings, scope_buf, return_label, entry_label);
    }

    // Nested `use` statements must resolve relative to *this* module's
    // directory in the next expansion pass, so rewrite their paths to absolute.
    for (uint32_t i = first_cloned; i < dest->node_count; i++) {
        AstNode *n = &dest->nodes[i];
        if (n->kind != AST_USE) continue;
        char *resolved = loader_resolve_path(n->use.filename->data, dir);
        if (resolved) {
            n->use.filename = str_intern_cstr(ctx->strings, resolved);
            free(resolved);
        }
    }

    // Return no-op: target of done/halt (and the module's own END).
    add_nop_label(ctx, dest, return_label, use_node->line);

    free(module_dir);
    return BC_OK(NULL);
}

BcResult loader_expand(LoaderContext *ctx, Ast *ast) {
    if (ctx->depth >= MAX_USE_DEPTH) {
        return bc_err(ctx->arena, BC_ERR_SEMANTIC, NULL, 0, 0,
                     "maximum module nesting depth (%d) exceeded", MAX_USE_DEPTH);
    }
    ctx->depth++;

    // Create a new AST for the expanded result
    Ast *expanded = ast_new(ctx->arena, ctx->strings);

    // Process each node
    for (uint32_t i = 0; i < ast->node_count; i++) {
        AstNode *node = &ast->nodes[i];

        if (node->kind == AST_USE) {
            // Expand the use statement
            BcResult result = expand_use(ctx, expanded, node, ctx->use_counter++);
            if (!result.ok) {
                ctx->depth--;
                return result;
            }
        } else {
            // Copy non-use nodes directly
            AstNode *dest = ast_add_node(expanded);
            *dest = *node;
        }
    }

    // Check for nested use statements and expand recursively
    bool has_use = false;
    for (uint32_t i = 0; i < expanded->node_count; i++) {
        if (expanded->nodes[i].kind == AST_USE) {
            has_use = true;
            break;
        }
    }

    if (has_use) {
        BcResult result = loader_expand(ctx, expanded);
        ctx->depth--;
        return result;
    }

    ctx->depth--;

    // Replace original AST contents
    ast->nodes = expanded->nodes;
    ast->node_count = expanded->node_count;
    ast->node_capacity = expanded->node_capacity;

    return BC_OK(ast);
}
