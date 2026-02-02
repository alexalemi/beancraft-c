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

// Apply register mappings to a register name
static Str *apply_reg_mapping(const AstNode *use_node, Str *reg,
                              StrPool *strings, const char *scope) {
    // Check if reg matches any mapping
    for (uint32_t i = 0; i < use_node->use.reg_mapping_count; i++) {
        RegMapping *m = &use_node->use.reg_mappings[i];
        if (m->local == reg) {
            if (m->is_value) {
                // Value mapping - return the original name (will be set in init)
                return reg;
            } else {
                // Register alias - return the import name (no scoping)
                return m->import;
            }
        }
    }
    // No mapping - apply scope prefix
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

// Transform a jump target with register/label mappings
// return_label is the label to use for done/halt (to return from module)
static Jump transform_jump(const AstNode *use_node, Jump jump,
                          StrPool *strings, const char *scope,
                          Str *return_label) {
    if (jump.kind == JUMP_LABEL) {
        Jump result = jump;
        result.label = apply_label_mapping(use_node, jump.label, strings, scope);
        return result;
    }
    // Transform done/halt to jump to return label
    if (jump.kind == JUMP_KEYWORD &&
        (jump.keyword == KW_DONE || jump.keyword == KW_HALT)) {
        return jump_label(return_label);
    }
    return jump;
}

// Clone and transform a single AST node from a module
// return_label is used to replace done/halt keywords
static void clone_and_transform_node(Ast *dest, const AstNode *src,
                                     const AstNode *use_node,
                                     StrPool *strings, const char *scope,
                                     Str *return_label) {
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
        node->inc.next = transform_jump(use_node, src->inc.next, strings, scope, return_label);
        break;

    case AST_DEB:
        node->deb.reg = apply_reg_mapping(use_node, src->deb.reg, strings, scope);
        node->deb.jump = transform_jump(use_node, src->deb.jump, strings, scope, return_label);
        node->deb.next = transform_jump(use_node, src->deb.next, strings, scope, return_label);
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

    // Get directory of module for nested imports
    char *module_dir = strdup(module_path);
    char *dir = dirname(module_dir);
    const char *old_base = ctx->base_path;
    ctx->base_path = dir;

    free(module_path);

    if (!load_result.ok) {
        free(module_dir);
        ctx->base_path = old_base;
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

    // Generate return label for this module invocation
    char return_buf[80];
    snprintf(return_buf, sizeof(return_buf), "%s/return", scope_buf);
    Str *return_label = str_intern_cstr(ctx->strings, return_buf);

    // Clone and transform each node from the module
    for (uint32_t i = 0; i < module_ast->node_count; i++) {
        clone_and_transform_node(dest, &module_ast->nodes[i], use_node,
                                 ctx->strings, scope_buf, return_label);
    }

    // Add the return label as a no-op (deb :nil next next)
    // This serves as the target for done/halt in the module
    AstNode *return_node = ast_add_node(dest);
    return_node->kind = AST_DEB;
    return_node->label = return_label;
    return_node->line = use_node->line;
    return_node->column = 0;
    return_node->deb.reg = str_intern_cstr(ctx->strings, ":nil");
    return_node->deb.jump = jump_keyword(KW_NEXT);
    return_node->deb.next = jump_keyword(KW_NEXT);

    free(module_dir);
    ctx->base_path = old_base;
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

    // Track use counter for this expansion
    uint32_t base_use_counter = ctx->use_counter;

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
