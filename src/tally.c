#include "beancraft/tally.h"
#include <stdlib.h>
#include <string.h>

uint64_t tally_nth_prime(uint32_t n) {
    uint64_t c = 1;
    for (uint32_t found = 0; ; ) {
        c++;
        bool prime = true;
        for (uint64_t d = 2; d * d <= c; d++) if (c % d == 0) { prime = false; break; }
        if (prime && found++ == n) return c;
    }
}

static bool is_noop(const IrProgram *prog, uint32_t i) {
    const IrInst *in = &prog->insts[i];
    return in->op == IR_DEB && in->arg_a == in->arg_b &&
           strcmp(prog->reg_names[in->reg]->data, ":nil") == 0;
}

static uint32_t thread(const IrProgram *prog, uint32_t t) {
    for (uint32_t hops = 0; hops < prog->inst_count && t < prog->inst_count && is_noop(prog, t); hops++) {
        uint32_t next = prog->insts[t].arg_a;
        if (next == t) break;
        t = next;
    }
    return t;
}

void tally_layout(Arena *arena, const IrProgram *prog, TallyLayout *l) {
    uint32_t n = prog->inst_count;
    memset(l, 0, sizeof *l);
    l->prog = prog;

    // Reachability from the (threaded) entry, through threaded targets. An
    // instruction past the end of the cleaned program reads as `stop`, so a
    // target at n stays at n.
    bool *reach = arena_alloc_zero(arena, (n + 1) * sizeof(bool));
    uint32_t *stack = arena_alloc(arena, (n + 1) * sizeof(uint32_t));
    uint32_t sp = 0;
    uint32_t entry = n ? thread(prog, 0) : 0;
    if (entry < n) { reach[entry] = true; stack[sp++] = entry; }
    while (sp) {
        uint32_t i = stack[--sp];
        const IrInst *in = &prog->insts[i];
        uint32_t t[2]; uint32_t nt = 0;
        if (in->op == IR_INC) t[nt++] = in->arg_a;
        if (in->op == IR_DEB) { t[nt++] = in->arg_a; t[nt++] = in->arg_b; }
        for (uint32_t k = 0; k < nt; k++) {
            uint32_t x = thread(prog, t[k]);
            if (x < n && !reach[x]) { reach[x] = true; stack[sp++] = x; }
        }
    }

    // Renumber, keeping the original order. The entry is the lowest kept
    // instruction: a leading bare label threads forward to the first real one,
    // and nothing before it can be reachable.
    uint32_t *newidx = arena_alloc(arena, (n + 1) * sizeof(uint32_t));
    uint32_t count = 0;
    for (uint32_t i = 0; i < n; i++) { newidx[i] = count; if (reach[i]) count++; }
    newidx[n] = count;
    l->insts = arena_alloc(arena, (count ? count : 1) * sizeof(IrInst));
    l->inst_count = count;
    for (uint32_t i = 0, k = 0; i < n; i++) {
        if (!reach[i]) continue;
        IrInst in = prog->insts[i];
        if (in.op == IR_INC) in.arg_a = newidx[thread(prog, in.arg_a)];
        if (in.op == IR_DEB) { in.arg_a = newidx[thread(prog, in.arg_a)]; in.arg_b = newidx[thread(prog, in.arg_b)]; }
        l->insts[k++] = in;
    }
    // Trailing `stop`s cost nothing: a target at or past the end reads as
    // empty tallies, i.e. stop, so they can simply be dropped.
    while (l->inst_count && l->insts[l->inst_count - 1].op == IR_END) l->inst_count--;

    // Field offsets.
    l->field_off = arena_alloc(arena, (l->inst_count + 1) * sizeof(uint32_t));
    uint32_t off = 0;
    for (uint32_t i = 0; i < l->inst_count; i++) {
        l->field_off[i] = off;
        off += l->insts[i].op == IR_INC ? 2 : l->insts[i].op == IR_DEB ? 3 : 1;
    }
    l->field_off[l->inst_count] = off;

    // Primes by mention count (ties: lower register index first).
    uint32_t rc = prog->reg_count;
    uint32_t *mentions = arena_alloc_zero(arena, (rc ? rc : 1) * sizeof(uint32_t));
    for (uint32_t i = 0; i < l->inst_count; i++)
        if (l->insts[i].op != IR_END) mentions[l->insts[i].reg]++;
    l->by_prime = arena_alloc(arena, (rc ? rc : 1) * sizeof(uint32_t));
    l->prime_of = arena_alloc_zero(arena, (rc ? rc : 1) * sizeof(uint64_t));
    uint32_t used = 0;
    for (uint32_t r = 0; r < rc; r++) if (mentions[r]) l->by_prime[used++] = r;
    for (uint32_t i = 1; i < used; i++) {            // insertion sort, stable
        uint32_t r = l->by_prime[i]; uint32_t j = i;
        while (j > 0 && mentions[l->by_prime[j - 1]] < mentions[r]) { l->by_prime[j] = l->by_prime[j - 1]; j--; }
        l->by_prime[j] = r;
    }
    for (uint32_t i = 0; i < used; i++) l->prime_of[l->by_prime[i]] = tally_nth_prime(i);
    l->used_count = used;

    size_t bits = 0;
    for (uint32_t i = 0; i < l->inst_count; i++) {
        const IrInst *in = &l->insts[i];
        switch (in->op) {
        case IR_INC: bits += 2 * l->prime_of[in->reg] + 1 + l->field_off[in->arg_a] + 1; break;
        case IR_DEB: bits += 2 * l->prime_of[in->reg] + 2 + l->field_off[in->arg_a] + 1 + l->field_off[in->arg_b] + 1; break;
        default:     bits += 1; break;
        }
    }
    l->bit_count = bits;
}

// P is built most-significant field first: acc = acc * 2^(n+1) + (2^n - 1)
// appends the field "n ones then a zero" *below* what is already there --
// so we walk the fields backwards.
static void prepend_field(Bignum *acc, uint64_t n) {
    for (uint64_t i = 0; i < n + 1; i++) { Bignum d = bignum_add(*acc, *acc); bignum_free(acc); *acc = d; }
    Bignum ones = bignum_from_u64(1);
    for (uint64_t i = 0; i < n; i++) { Bignum d = bignum_add(ones, ones); bignum_free(&ones); ones = d; }
    bignum_dec(&ones);
    bignum_add_into(acc, ones);
    bignum_free(&ones);
}

Bignum tally_program(const TallyLayout *l) {
    Bignum P = bignum_from_u64(0);
    for (uint32_t i = l->inst_count; i-- > 0; ) {
        const IrInst *in = &l->insts[i];
        switch (in->op) {
        case IR_INC:
            prepend_field(&P, l->field_off[in->arg_a]);
            prepend_field(&P, 2 * l->prime_of[in->reg]);
            break;
        case IR_DEB:
            prepend_field(&P, l->field_off[in->arg_b]);
            prepend_field(&P, l->field_off[in->arg_a]);
            prepend_field(&P, 2 * l->prime_of[in->reg] + 1);
            break;
        default:
            prepend_field(&P, 0);
            break;
        }
    }
    return P;
}

// acc *= base^e by square-and-multiply (e may be a bignum: a register value).
static void mul_pow(Bignum *acc, uint64_t base, Bignum e) {
    Bignum ee = bignum_clone(e);
    Bignum b = bignum_from_u64(base);
    while (!bignum_is_zero(ee)) {
        uint64_t bit = bignum_divmod_small(&ee, 2);
        if (bit) { Bignum m = bignum_mul(*acc, b); bignum_free(acc); *acc = m; }
        if (!bignum_is_zero(ee)) { Bignum s = bignum_mul(b, b); bignum_free(&b); b = s; }
    }
    bignum_free(&ee);
    bignum_free(&b);
}

Bignum tally_bank(const TallyLayout *l, const Bignum *values) {
    Bignum R = bignum_from_u64(1);
    for (uint32_t r = 0; r < l->prog->reg_count; r++)
        if (l->prime_of[r]) mul_pow(&R, l->prime_of[r], values[r]);
    return R;
}

Bignum tally_decode(const TallyLayout *l, Bignum R, Bignum *values) {
    Bignum rest = bignum_clone(R);
    for (uint32_t r = 0; r < l->prog->reg_count; r++) {
        values[r] = bignum_zero();
        if (!l->prime_of[r]) continue;
        for (;;) {
            Bignum c = bignum_clone(rest);
            if (bignum_divmod_small(&c, l->prime_of[r]) != 0 || bignum_is_zero(c)) { bignum_free(&c); break; }
            bignum_free(&rest);
            rest = c;
            bignum_inc(&values[r]);
        }
    }
    return rest;
}
