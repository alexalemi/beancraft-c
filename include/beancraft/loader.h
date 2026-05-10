#ifndef BC_LOADER_H
#define BC_LOADER_H

#include "arena.h"
#include "str.h"
#include "ast.h"
#include "error.h"
#include <stdbool.h>

// Maximum recursion depth for nested use statements
#define MAX_USE_DEPTH 32

// Module cache entry
typedef struct ModuleCacheEntry {
    Str *path;                      // Absolute path to module
    Ast *ast;                       // Parsed AST
    struct ModuleCacheEntry *next;  // Hash chain
} ModuleCacheEntry;

// Module cache (hash table)
typedef struct {
    Arena *arena;
    StrPool *strings;
    ModuleCacheEntry **buckets;
    uint32_t bucket_count;
} ModuleCache;

// Registered `func` definition (a linked-list entry).
typedef struct FuncEntry {
    Str *name;
    const AstNode *def;        // the AST_FUNCDEF node
    struct FuncEntry *next;
} FuncEntry;

// Loader context
typedef struct {
    Arena *arena;
    StrPool *strings;
    ModuleCache *cache;
    FuncEntry *funcs;           // registered `func` definitions
    const char *base_path;      // Base directory for relative imports
    uint32_t use_counter;       // Counter for generating unique scope prefixes
    uint32_t depth;             // Current recursion depth
} LoaderContext;

// Create a new module cache
ModuleCache *module_cache_new(Arena *arena, StrPool *strings);

// Get or load a module (returns cached if already loaded)
BcResult module_cache_get(ModuleCache *cache, const char *path);

// Create a new loader context
LoaderContext *loader_new(Arena *arena, StrPool *strings, const char *base_path);

// Expand all use statements in an AST, producing a flat AST
// This recursively loads and inlines all modules
BcResult loader_expand(LoaderContext *ctx, Ast *ast);

// Resolve a module path relative to a base path
// Returns allocated string that should be freed by caller (or NULL if not found)
char *loader_resolve_path(const char *module_name, const char *base_path);

#endif // BC_LOADER_H
