#include "beancraft/ir.h"
#include "beancraft/devices.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define INITIAL_CAPACITY 16

IrProgram *ir_new(Arena *arena, StrPool *strings) {
    IrProgram *prog = arena_alloc(arena, sizeof(IrProgram));
    prog->arena = arena;
    prog->strings = strings;

    prog->inst_count = 0;
    prog->inst_capacity = INITIAL_CAPACITY;
    prog->insts = arena_alloc(arena, sizeof(IrInst) * prog->inst_capacity);

    prog->reg_count = 0;
    prog->reg_capacity = INITIAL_CAPACITY;
    prog->reg_names = arena_alloc(arena, sizeof(Str *) * prog->reg_capacity);
    prog->reg_init = arena_alloc_zero(arena, sizeof(uint64_t) * prog->reg_capacity);

    prog->label_names = NULL;  // Allocated later if needed

    return prog;
}

static void ensure_inst_capacity(IrProgram *prog) {
    if (prog->inst_count >= prog->inst_capacity) {
        uint32_t new_cap = prog->inst_capacity * 2;
        IrInst *new_insts = arena_alloc(prog->arena, sizeof(IrInst) * new_cap);
        memcpy(new_insts, prog->insts, sizeof(IrInst) * prog->inst_count);
        prog->insts = new_insts;
        prog->inst_capacity = new_cap;
    }
}

static void ensure_reg_capacity(IrProgram *prog) {
    if (prog->reg_count >= prog->reg_capacity) {
        uint32_t new_cap = prog->reg_capacity * 2;

        Str **new_names = arena_alloc(prog->arena, sizeof(Str *) * new_cap);
        memcpy(new_names, prog->reg_names, sizeof(Str *) * prog->reg_count);
        prog->reg_names = new_names;

        uint64_t *new_init = arena_alloc_zero(prog->arena, sizeof(uint64_t) * new_cap);
        memcpy(new_init, prog->reg_init, sizeof(uint64_t) * prog->reg_count);
        prog->reg_init = new_init;

        prog->reg_capacity = new_cap;
    }
}

// ---------------------------------------------------------------------------
// Str* -> u32 map. Interned strings compare by pointer (str_eq), so an
// open-addressing table on the pointer keeps register and label lookup O(1)
// instead of O(n) per reference -- loader expansions (urm.bc-scale programs)
// produce tens of thousands of distinct names.
// ---------------------------------------------------------------------------

typedef struct {
    Arena *arena;
    Str **keys;        // NULL = empty slot
    uint32_t *vals;
    uint32_t cap;      // power of two
    uint32_t count;
} PtrMap;

static uint64_t ptr_hash(const Str *p) {
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    return x;
}

static PtrMap *ptrmap_new(Arena *arena, uint32_t cap) {
    PtrMap *m = arena_alloc(arena, sizeof(PtrMap));
    m->arena = arena;
    m->cap = cap;          // must be a power of two
    m->count = 0;
    m->keys = arena_alloc_zero(arena, cap * sizeof(Str *));
    m->vals = arena_alloc(arena, cap * sizeof(uint32_t));
    return m;
}

static bool ptrmap_get(const PtrMap *m, Str *key, uint32_t *out) {
    uint32_t mask = m->cap - 1;
    for (uint32_t i = (uint32_t)ptr_hash(key) & mask; m->keys[i]; i = (i + 1) & mask) {
        if (m->keys[i] == key) { *out = m->vals[i]; return true; }
    }
    return false;
}

static void ptrmap_put(PtrMap *m, Str *key, uint32_t val);

static void ptrmap_grow(PtrMap *m) {
    Str **old_keys = m->keys;
    uint32_t *old_vals = m->vals;
    uint32_t old_cap = m->cap;
    m->cap *= 2;
    m->count = 0;
    m->keys = arena_alloc_zero(m->arena, m->cap * sizeof(Str *));
    m->vals = arena_alloc(m->arena, m->cap * sizeof(uint32_t));
    for (uint32_t i = 0; i < old_cap; i++) {
        if (old_keys[i]) ptrmap_put(m, old_keys[i], old_vals[i]);
    }
}

static void ptrmap_put(PtrMap *m, Str *key, uint32_t val) {
    if (m->count * 4 >= m->cap * 3) ptrmap_grow(m);   // <= 75% load
    uint32_t mask = m->cap - 1;
    uint32_t i = (uint32_t)ptr_hash(key) & mask;
    while (m->keys[i] && m->keys[i] != key) i = (i + 1) & mask;
    if (!m->keys[i]) m->count++;
    m->keys[i] = key;
    m->vals[i] = val;
}

// Find or add a register, returns index. `map` caches name -> index; pass NULL
// for one-off additions (ir_add_nil_reg).
static uint32_t find_or_add_reg(IrProgram *prog, PtrMap *map, Str *name) {
    uint32_t idx;
    if (map && ptrmap_get(map, name, &idx)) return idx;
    if (!map) {
        // Linear fallback for the rare un-mapped call.
        for (uint32_t i = 0; i < prog->reg_count; i++) {
            if (str_eq(prog->reg_names[i], name)) return i;
        }
    }

    // Add new
    ensure_reg_capacity(prog);
    idx = prog->reg_count++;
    prog->reg_names[idx] = name;
    prog->reg_init[idx] = 0;
    if (map) ptrmap_put(map, name, idx);
    return idx;
}

int32_t ir_find_reg(const IrProgram *prog, const char *name) {
    for (uint32_t i = 0; i < prog->reg_count; i++) {
        if (strcmp(prog->reg_names[i]->data, name) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

void ir_set_reg_init(IrProgram *prog, uint32_t reg_idx, uint64_t value) {
    if (reg_idx < prog->reg_count) {
        prog->reg_init[reg_idx] = value;
    }
}

void ir_add_nil_reg(IrProgram *prog) {
    Str *nil_name = str_intern_cstr(prog->strings, ":nil");
    find_or_add_reg(prog, NULL, nil_name);
}

// Resolve a jump target to an address. `labels` maps label Str* -> address.
static BcResult resolve_jump(Arena *arena, const Jump *jump, uint32_t current_addr,
                             uint32_t inst_count, const PtrMap *labels,
                             const char *filename, uint32_t line) {
    switch (jump->kind) {
    case JUMP_NONE:
        // Implicit next
        return BC_OK((void *)(uintptr_t)(current_addr + 1));

    case JUMP_LABEL: {
        uint32_t addr;
        if (!ptrmap_get(labels, jump->label, &addr)) {
            // Loader-scoped names look like "f-0/dest" (or "<path>-N/dest");
            // report the user's label name and where it was being expanded
            // instead of leaking the mangled form.
            const char *full = jump->label->data;
            const char *slash = strrchr(full, '/');
            if (slash && slash != full && slash[1] != '\0') {
                return bc_err(arena, BC_ERR_SEMANTIC, filename, line, 0,
                             "undefined label '%s' (inside the expansion of '%.*s')",
                             slash + 1, (int)(slash - full), full);
            }
            return bc_err(arena, BC_ERR_SEMANTIC, filename, line, 0,
                         "undefined label '%s'", full);
        }
        return BC_OK((void *)(uintptr_t)addr);
    }

    case JUMP_KEYWORD:
        switch (jump->keyword) {
        case KW_SELF:
            return BC_OK((void *)(uintptr_t)current_addr);
        case KW_NEXT:
            return BC_OK((void *)(uintptr_t)(current_addr + 1));
        case KW_PREV:
            return BC_OK((void *)(uintptr_t)(current_addr > 0 ? current_addr - 1 : 0));
        case KW_INIT:
            return BC_OK((void *)(uintptr_t)0);
        case KW_DONE:
        case KW_HALT:
            // Jump to halt instruction (last instruction)
            return BC_OK((void *)(uintptr_t)(inst_count - 1));
        }
        break;

    case JUMP_OFFSET: {
        // 64-bit arithmetic: an offset near INT32_MAX must clamp, not overflow.
        int64_t target = (int64_t)current_addr + jump->offset;
        if (target < 0) target = 0;
        // A past-the-end offset means halt. Clamp to the implicit trailing END
        // so every backend agrees: the interpreter treated pc >= inst_count as
        // a silent halt, but the QBE backend emitted jumps to nonexistent
        // blocks and the optimizer had to bounds-check on its own.
        if (target >= (int64_t)inst_count) target = (int64_t)inst_count - 1;
        return BC_OK((void *)(uintptr_t)target);
    }
    }

    return bc_err(arena, BC_ERR_SEMANTIC, filename, line, 0, "invalid jump target");
}

BcResult ir_from_ast(Arena *arena, StrPool *strings, const Ast *ast) {
    IrProgram *prog = ir_new(arena, strings);

    // First pass: collect all labels
    PtrMap *labels = ptrmap_new(arena, 64);
    for (uint32_t i = 0; i < ast->node_count; i++) {
        const AstNode *node = &ast->nodes[i];
        if (node->label) {
            uint32_t prev;
            if (ptrmap_get(labels, node->label, &prev)) {
                return bc_err(arena, BC_ERR_SEMANTIC, NULL, node->line, 0,
                              "duplicate label '%s'", node->label->data);
            }
            ptrmap_put(labels, node->label, i);
        }
    }

    // Add implicit halt at end
    uint32_t total_insts = ast->node_count + 1;

    // Register name -> index cache for the second pass.
    PtrMap *regs = ptrmap_new(arena, 64);

    // Second pass: convert AST nodes to IR instructions
    for (uint32_t i = 0; i < ast->node_count; i++) {
        const AstNode *node = &ast->nodes[i];
        ensure_inst_capacity(prog);
        IrInst *inst = &prog->insts[prog->inst_count++];

        switch (node->kind) {
        case AST_INC: {
            inst->op = IR_INC;
            inst->reg = find_or_add_reg(prog, regs, node->inc.reg);

            BcResult res = resolve_jump(arena, &node->inc.next, i, total_insts,
                                        labels, NULL, node->line);
            if (!res.ok) return res;
            inst->arg_a = (uint32_t)(uintptr_t)res.value;
            inst->arg_b = 0;
            break;
        }

        case AST_DEB: {
            inst->op = IR_DEB;
            inst->reg = find_or_add_reg(prog, regs, node->deb.reg);

            BcResult res_jump = resolve_jump(arena, &node->deb.jump, i, total_insts,
                                             labels, NULL, node->line);
            if (!res_jump.ok) return res_jump;
            inst->arg_a = (uint32_t)(uintptr_t)res_jump.value;

            BcResult res_next = resolve_jump(arena, &node->deb.next, i, total_insts,
                                             labels, NULL, node->line);
            if (!res_next.ok) return res_next;
            inst->arg_b = (uint32_t)(uintptr_t)res_next.value;
            break;
        }

        case AST_END:
            inst->op = IR_END;
            inst->reg = 0;
            inst->arg_a = 0;
            inst->arg_b = 0;
            break;

        case AST_USE:
        case AST_FUNCDEF:
        case AST_CALL:
            // These are removed/expanded by the loader; reaching IR means a bug.
            return bc_err(arena, BC_ERR_SEMANTIC, NULL, node->line, 0,
                         "internal error: %s not expanded by loader",
                         node->kind == AST_USE ? "use statement"
                       : node->kind == AST_CALL ? "function call"
                       : "function definition");
        }
    }

    // Add final halt instruction
    ensure_inst_capacity(prog);
    IrInst *halt = &prog->insts[prog->inst_count++];
    halt->op = IR_END;
    halt->reg = 0;
    halt->arg_a = 0;
    halt->arg_b = 0;

    // For each device register used, make sure its implicit data registers exist
    // (e.g. con/emit needs con/byte) so the runtime has somewhere to put data.
    {
        uint32_t orig = prog->reg_count;
        for (uint32_t i = 0; i < orig; i++) {
            uint32_t dep_count;
            const char *const *deps = device_dependencies(prog->reg_names[i]->data, &dep_count);
            for (uint32_t d = 0; d < dep_count; d++) {
                find_or_add_reg(prog, regs, str_intern_cstr(strings, deps[d]));
            }
        }
    }

    // Add the :nil register
    ir_add_nil_reg(prog);

    // Store label names for debugging
    prog->label_names = arena_alloc_zero(arena, sizeof(Str *) * prog->inst_count);
    for (uint32_t i = 0; i < labels->cap; i++) {
        if (labels->keys[i]) prog->label_names[labels->vals[i]] = labels->keys[i];
    }

    return BC_OK(prog);
}

void ir_print(const IrProgram *prog) {
    printf("IR Program: %u instructions, %u registers\n\n",
           prog->inst_count, prog->reg_count);

    printf("Registers:\n");
    for (uint32_t i = 0; i < prog->reg_count; i++) {
        printf("  [%u] %s = %" PRIu64 "\n", i, prog->reg_names[i]->data, prog->reg_init[i]);
    }
    printf("\n");

    printf("Instructions:\n");
    for (uint32_t i = 0; i < prog->inst_count; i++) {
        const IrInst *inst = &prog->insts[i];

        printf("  %3u: ", i);
        if (prog->label_names && prog->label_names[i]) {
            printf("%-10s ", prog->label_names[i]->data);
        } else {
            printf("           ");
        }

        switch (inst->op) {
        case IR_INC:
            printf("inc r%u -> %u\n", inst->reg, inst->arg_a);
            break;
        case IR_DEB:
            printf("deb r%u ? %u : %u\n", inst->reg, inst->arg_a, inst->arg_b);
            break;
        case IR_END:
            printf("end\n");
            break;
        }
    }
}
