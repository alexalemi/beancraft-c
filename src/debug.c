#include "beancraft/debug.h"
#include "beancraft/bignum.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BREAKPOINTS 32

// --- formatting helpers -----------------------------------------------------

static const char *op_name(IrOptOp op) {
    switch (op) {
    case IR_OPT_INC:      return "inc";
    case IR_OPT_DEB:      return "deb";
    case IR_OPT_END:      return "end";
    case IR_OPT_ZERO:     return "zero";
    case IR_OPT_TRANSFER: return "transfer";
    case IR_OPT_DIVMOD:   return "divmod";
    case IR_OPT_MULADD:   return "muladd";
    case IR_OPT_ISZERO:   return "iszero";
    case IR_OPT_COPY:     return "copy";
    }
    return "?";
}

// Print one instruction line: "  >  12  label:  deb B -> z:14 nz:13".
static void print_inst(const InterpState *st, const IrProgram *prog, uint32_t pc) {
    const IrOptProgram *p = st->prog;
    if (pc >= p->inst_count) {
        fprintf(stderr, "  %s %4u  <past end: halt>\n", pc == st->pc ? ">" : " ", pc);
        return;
    }
    const IrOptInst *in = &p->insts[pc];
    const char *label = (prog->label_names && pc < prog->inst_count && prog->label_names[pc])
                        ? prog->label_names[pc]->data : "";
    fprintf(stderr, "  %s %4u  %-12s ", pc == st->pc ? ">" : " ", pc, label);
    switch (in->op) {
    case IR_OPT_INC:
        fprintf(stderr, "inc %s -> %u\n", p->reg_names[in->reg]->data, in->arg_a);
        break;
    case IR_OPT_DEB:
        fprintf(stderr, "deb %s -> z:%u nz:%u\n",
                p->reg_names[in->reg]->data, in->arg_a, in->arg_b);
        break;
    case IR_OPT_END:
        fprintf(stderr, "end\n");
        break;
    default:
        fprintf(stderr, "%s %s -> %u\n", op_name(in->op),
                p->reg_names[in->reg]->data, in->arg_a);
        break;
    }
}

static void show_stop(const InterpState *st, const IrProgram *prog) {
    if (st->halted) {
        fprintf(stderr, "halted after %" PRIu64 " steps\n", st->steps);
        return;
    }
    fprintf(stderr, "step %" PRIu64 ":\n", st->steps);
    print_inst(st, prog, st->pc);
}

// --- name lookups -----------------------------------------------------------

static int32_t find_label(const IrProgram *prog, const char *name) {
    if (!prog->label_names) return -1;
    for (uint32_t i = 0; i < prog->inst_count; i++) {
        if (prog->label_names[i] && strcmp(prog->label_names[i]->data, name) == 0)
            return (int32_t)i;
    }
    return -1;
}

static int32_t find_register(const InterpState *st, const char *name) {
    for (uint32_t i = 0; i < st->prog->reg_count; i++) {
        if (strcmp(st->prog->reg_names[i]->data, name) == 0) return (int32_t)i;
    }
    return -1;
}

static void print_reg(const InterpState *st, uint32_t idx) {
    char *v = bignum_to_string(st->regs[idx]);
    fprintf(stderr, "%s = %s\n", st->prog->reg_names[idx]->data, v);
    free(v);
}

// --- the REPL ----------------------------------------------------------------

void debug_repl(InterpState *st, const IrProgram *prog, uint64_t max_steps) {
    uint32_t bps[MAX_BREAKPOINTS];
    uint32_t bp_count = 0;
    int32_t watch_reg = -1;
    Bignum watch_last = bignum_zero();

    fprintf(stderr, "beancraft debugger: %u instructions, %u registers. "
                    "'h' for help, 'q' to quit.\n",
            st->prog->inst_count, st->prog->reg_count);
    show_stop(st, prog);

    char line[256], last[256] = "s";
    for (;;) {
        fprintf(stderr, "(bdb) ");
        fflush(stderr);
        if (!fgets(line, sizeof line, stdin)) break;

        // Blank line repeats the previous command (gdb-style).
        char *cmd = line + strspn(line, " \t");
        cmd[strcspn(cmd, "\n")] = '\0';
        if (*cmd == '\0') cmd = last;
        else snprintf(last, sizeof last, "%s", cmd);

        char op[16] = "", arg[200] = "";
        sscanf(cmd, "%15s %199s", op, arg);

        if (strcmp(op, "q") == 0 || strcmp(op, "quit") == 0) break;

        if (strcmp(op, "h") == 0 || strcmp(op, "help") == 0) {
            fprintf(stderr,
                "  s [N]        step N instructions (default 1)\n"
                "  c            continue to breakpoint / watch change / halt\n"
                "  b LABEL|PC   set a breakpoint (no arg: list them)\n"
                "  w REG        watch REG: break when its value changes (no arg: clear)\n"
                "  p REG        print one register\n"
                "  regs         print all registers\n"
                "  l            list instructions around the pc\n"
                "  q            quit (skips the rest of the run)\n"
                "  <enter>      repeat the previous command\n");
            continue;
        }

        if (strcmp(op, "s") == 0 || strcmp(op, "step") == 0) {
            uint64_t n = arg[0] ? strtoull(arg, NULL, 10) : 1;
            if (n == 0) n = 1;
            for (uint64_t k = 0; k < n && !st->halted && st->steps < max_steps; k++)
                interp_step(st);
            show_stop(st, prog);
            if (st->halted) break;
            continue;
        }

        if (strcmp(op, "c") == 0 || strcmp(op, "continue") == 0) {
            if (watch_reg >= 0) {
                bignum_free(&watch_last);
                watch_last = bignum_clone(st->regs[watch_reg]);
            }
            while (!st->halted && st->steps < max_steps) {
                interp_step(st);
                bool stop = false;
                for (uint32_t i = 0; i < bp_count; i++) {
                    if (st->pc == bps[i]) {
                        fprintf(stderr, "breakpoint at %u\n", bps[i]);
                        stop = true;
                    }
                }
                if (watch_reg >= 0 && !bignum_eq(st->regs[watch_reg], watch_last)) {
                    fprintf(stderr, "watch: ");
                    print_reg(st, (uint32_t)watch_reg);
                    bignum_free(&watch_last);
                    watch_last = bignum_clone(st->regs[watch_reg]);
                    stop = true;
                }
                if (stop) break;
            }
            if (!st->halted && st->steps >= max_steps)
                fprintf(stderr, "step limit (%" PRIu64 ") reached\n", max_steps);
            show_stop(st, prog);
            if (st->halted) break;
            continue;
        }

        if (strcmp(op, "b") == 0 || strcmp(op, "break") == 0) {
            if (!arg[0]) {
                if (bp_count == 0) fprintf(stderr, "no breakpoints\n");
                for (uint32_t i = 0; i < bp_count; i++) print_inst(st, prog, bps[i]);
                continue;
            }
            char *end;
            long pc = strtol(arg, &end, 10);
            if (*end != '\0' || pc < 0) pc = find_label(prog, arg);
            if (pc < 0 || (uint32_t)pc >= st->prog->inst_count) {
                fprintf(stderr, "no such label or pc: '%s'\n", arg);
                continue;
            }
            if (bp_count >= MAX_BREAKPOINTS) {
                fprintf(stderr, "too many breakpoints (max %d)\n", MAX_BREAKPOINTS);
                continue;
            }
            bps[bp_count++] = (uint32_t)pc;
            fprintf(stderr, "breakpoint %u set:\n", bp_count);
            print_inst(st, prog, (uint32_t)pc);
            continue;
        }

        if (strcmp(op, "w") == 0 || strcmp(op, "watch") == 0) {
            if (!arg[0]) {
                watch_reg = -1;
                fprintf(stderr, "watch cleared\n");
                continue;
            }
            int32_t r = find_register(st, arg);
            if (r < 0) {
                fprintf(stderr, "no such register: '%s'\n", arg);
                continue;
            }
            watch_reg = r;
            bignum_free(&watch_last);
            watch_last = bignum_clone(st->regs[r]);
            fprintf(stderr, "watching ");
            print_reg(st, (uint32_t)r);
            continue;
        }

        if (strcmp(op, "p") == 0 || strcmp(op, "print") == 0) {
            int32_t r = find_register(st, arg);
            if (r < 0) fprintf(stderr, "no such register: '%s'\n", arg);
            else print_reg(st, (uint32_t)r);
            continue;
        }

        if (strcmp(op, "regs") == 0) {
            for (uint32_t i = 0; i < st->prog->reg_count; i++) {
                if (st->prog->reg_names[i]->data[0] == ':') continue;
                fprintf(stderr, "  ");
                print_reg(st, i);
            }
            continue;
        }

        if (strcmp(op, "l") == 0 || strcmp(op, "list") == 0) {
            uint32_t lo = st->pc > 2 ? st->pc - 2 : 0;
            uint32_t hi = st->pc + 5;
            if (hi > st->prog->inst_count) hi = st->prog->inst_count;
            for (uint32_t i = lo; i < hi; i++) print_inst(st, prog, i);
            continue;
        }

        fprintf(stderr, "unknown command '%s' ('h' for help)\n", op);
    }

    bignum_free(&watch_last);
}
