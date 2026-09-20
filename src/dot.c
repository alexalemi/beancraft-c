#include "beancraft/dot.h"
#include <string.h>

// Write s as a DOT double-quoted string, escaping the two characters that
// matter inside one ('"' and '\').
static void dot_str(FILE *out, const char *s) {
    fputc('"', out);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', out);
        fputc(*s, out);
    }
    fputc('"', out);
}

// A bare `label:` line lowers to `take :nil next next` -- a no-op jump that
// only exists to carry the label. Those are not drawn: every edge into one is
// threaded through to where it lands, and its label is shown on that node.
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

// The number of instructions whose (threaded) control flow can reach idx, or
// 0 for an orphan. The implicit trailing `stop` of a program that never falls
// off its end has none, and is left out of the picture.
static uint32_t predecessors(const IrProgram *prog, uint32_t idx) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < prog->inst_count; i++) {
        const IrInst *in = &prog->insts[i];
        if (is_noop(prog, i)) continue;
        if (in->op == IR_INC && thread(prog, in->arg_a) == idx) n++;
        if (in->op == IR_DEB && (thread(prog, in->arg_a) == idx || thread(prog, in->arg_b) == idx)) n++;
    }
    return n;
}

// The node's own label plus the labels of every no-op that threads into it,
// comma-separated ("loop, again"), or NULL when there are none.
static void print_xlabel(FILE *out, const IrProgram *prog, uint32_t idx) {
    if (!prog->label_names) return;
    bool any = false;
    for (uint32_t i = 0; i < prog->inst_count; i++) {
        if (!prog->label_names[i]) continue;
        if (i != idx && !(is_noop(prog, i) && thread(prog, i) == idx)) continue;
        if (!any) fputs(", xlabel=\"", out);
        else fputs(", ", out);
        for (const char *c = prog->label_names[i]->data; *c; c++) {
            if (*c == '"' || *c == '\\') fputc('\\', out);
            fputc(*c, out);
        }
        any = true;
    }
    if (any) fputc('"', out);
}

// One edge. The edge to the textually next instruction is the "main flow": it
// gets a heavy weight so dot keeps it vertical; every other edge is a light
// side branch. A take's zero branch also leaves from the diamond's side and
// carries the open-circle tail that marks "if empty".
static void edge(FILE *out, uint32_t from, uint32_t to, bool zero_branch) {
    fprintf(out, "  i%u -> i%u [", from, to);
    bool next = (to == from + 1);
    fprintf(out, "weight=%d", next ? 8 : 1);
    if (zero_branch) {
        fputs(", dir=both, arrowtail=odot", out);
        if (to != from) fputs(", tailport=e", out);
    } else if (!next && to != from) {
        fputs(", tailport=s", out);
    }
    if (to == from) fputs(", tailport=w, headport=n", out);   // a `self` loop
    fputs("];\n", out);
}

void ir_print_dot(FILE *out, const IrProgram *prog, const char *title) {
    fputs("digraph beancraft {\n", out);
    if (title) { fputs("  label=", out); dot_str(out, title); fputs("; labelloc=t; labeljust=l;\n", out); }
    fputs("  graph [rankdir=TB, nodesep=0.3, ranksep=0.4, fontname=\"Helvetica\", fontsize=10];\n"
          "  node  [fontname=\"Helvetica\", fontsize=11, margin=\"0.05,0.03\", width=0.5, height=0.5];\n"
          "  edge  [arrowsize=0.7];\n"
          "  start [shape=point, width=0.12];\n", out);
    if (prog->inst_count) fputs("  start -> i0 [weight=8];\n", out);

    if (prog->inst_count) fprintf(out, "  start -> i%u [weight=8];\n", thread(prog, 0));

    for (uint32_t i = 0; i < prog->inst_count; i++) {
        const IrInst *in = &prog->insts[i];
        if (is_noop(prog, i)) continue;
        if (in->op == IR_END && predecessors(prog, i) == 0 && thread(prog, 0) != i) continue;   // unused implicit stop
        fprintf(out, "  i%u [", i);
        switch (in->op) {
        case IR_INC: fputs("shape=circle, label=", out);  dot_str(out, prog->reg_names[in->reg]->data); break;
        case IR_DEB: fputs("shape=diamond, label=", out); dot_str(out, prog->reg_names[in->reg]->data); break;
        case IR_END: fputs("shape=octagon, label=\"stop\"", out); break;
        }
        print_xlabel(out, prog, i);
        fputs("];\n", out);
    }
    for (uint32_t i = 0; i < prog->inst_count; i++) {
        const IrInst *in = &prog->insts[i];
        if (is_noop(prog, i)) continue;
        if (in->op == IR_INC) edge(out, i, thread(prog, in->arg_a), false);
        if (in->op == IR_DEB) {
            edge(out, i, thread(prog, in->arg_b), false);   // took a bean: fall through
            edge(out, i, thread(prog, in->arg_a), true);    // bin was empty
        }
    }
    fputs("}\n", out);
}
