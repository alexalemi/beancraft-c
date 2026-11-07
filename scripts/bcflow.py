#!/usr/bin/env python3
"""Source-level flow graphs of beancraft programs, in Graphviz DOT.

`beancraft --emit-dot` draws the *lowered* counter machine: every `use` and
`func` call inlined, so a program that prints one sentence is already hundreds
of nodes.  This draws the program as written instead: `give` / `take` / `stop`
as `--emit-dot` draws them (circle, diamond with the empty-bin branch leaving
from the side, stop sign), but each call to a `func` as a single box, and each
run of `putc` calls as one box showing the text it prints.

    python3 scripts/bcflow.py examples/nim.bc            # the program's main body
    python3 scripts/bcflow.py examples/std.bc divmod     # one func's body
    python3 scripts/bcflow.py --list examples/nim.bc     # the funcs it defines/uses
"""
import os
import re
import sys

GIVE = {"+", "give", "inc"}
TAKE = {"-", "take", "deb"}
STOP = {".", "stop", "end"}
JUMP_WORDS = {"self", "next", "prev", "init", "done", "halt"}


class Stmt:
    def __init__(self, kind, args, labels, line):
        self.kind = kind          # give / take / stop / call / use
        self.args = args
        self.labels = labels
        self.line = line


class Func:
    def __init__(self, name, params, body, file):
        self.name, self.params, self.body, self.file = name, params, body, file


def tokenize(line):
    line = line.split("#", 1)[0]
    return re.findall(r'"[^"]*"|\{|\}|[A-Za-z_][\w/\-]*:|[^\s{}]+', line)


def parse(path, funcs=None, seen=None):
    """Parse a .bc file: returns (main body statements, funcs dict).  `use`d
    files are parsed for their func definitions (the code they'd inline is
    shown as a single `use` box)."""
    funcs = {} if funcs is None else funcs
    seen = set() if seen is None else seen
    seen.add(os.path.abspath(path))
    main = []
    stack = []                    # open func definitions: (name, params, body)
    pending = []                  # labels waiting for a statement
    for n, raw in enumerate(open(path), 1):
        toks = tokenize(raw)
        while toks:
            if toks[0] == "}":
                name, params, body, plabels = stack.pop()
                if pending:
                    body.append(Stmt("end", [], pending, n))
                pending = plabels
                funcs[name] = Func(name, params, body, path)
                toks = toks[1:]
                continue
            if toks[0] == "func":
                i = toks.index("{")
                stack.append((toks[1], toks[2:i], [], pending))
                pending = []
                toks = toks[i + 1:]
                continue
            if toks[0].endswith(":") and len(toks[0]) > 1:
                pending.append(toks[0][:-1])
                toks = toks[1:]
                continue
            # one statement, up to a closing brace on the same line
            j = toks.index("}") if "}" in toks else len(toks)
            st, toks = toks[:j], toks[j:]
            body = stack[-1][2] if stack else main
            w = st[0]
            if w in GIVE:
                kind = "give"
            elif w in TAKE:
                kind = "take"
            elif w in STOP:
                kind = "stop"
            elif w in ("use", "%"):
                kind = "use"
                f = st[1].strip('"')
                cand = os.path.join(os.path.dirname(path), f + ".bc")
                if os.path.exists(cand) and os.path.abspath(cand) not in seen:
                    parse(cand, funcs, seen)
            else:
                kind = "call"
            body.append(Stmt(kind, st if kind in ("call", "use") else st[1:], pending, n))
            pending = []
    if pending:
        main.append(Stmt("end", [], pending, 0))
    return main, funcs


def collapse_putc(body, funcs):
    """Merge runs of `putc <int>` calls (only the first may carry a label) into
    one 'print' statement."""
    out = []
    for st in body:
        is_putc = st.kind == "call" and st.args[0] == "putc" and len(st.args) == 2 \
            and re.fullmatch(r"\d+", st.args[1])
        if is_putc and out and out[-1].kind == "print" and not st.labels:
            out[-1].args.append(int(st.args[1]))
            continue
        if is_putc:
            out.append(Stmt("print", [int(st.args[1])], st.labels, st.line))
        else:
            out.append(st)
    return out


def esc(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


def show_text(codes):
    s = ""
    for c in codes:
        s += {10: "\\n", 32: " "}.get(c, chr(c) if 32 < c < 127 else f"\\x{c:02x}")
    return '"' + s + '"'


def to_dot(body, funcs, title, is_func=False):
    body = collapse_putc(body, funcs)
    labels = {}
    for i, st in enumerate(body):
        for l in st.labels:
            labels[l] = i
    n = len(body)
    end_id = n                    # the implicit stop / return point
    used_end = [False]

    def target(t, i):
        if t in ("self", "0"):
            return i
        if t == "next":
            return i + 1
        if t == "prev":
            return i - 1
        if t == "init":
            return 0
        if t in ("done", "halt"):
            used_end[0] = True
            return end_id
        if re.fullmatch(r"[+-]?\d+", t):
            return i + int(t)
        if t in labels:
            return labels[t]
        return ("ext", t)        # a label parameter / outside label

    def is_goto(i):
        """`take z X X` (or :nil): z is never given a bean, so it's a plain jump."""
        st = body[i] if 0 <= i < n else None
        return (st is not None and st.kind == "take" and st.args[0] in ("z", "nil")
                and (len(st.args) == 2 or st.args[1] == st.args[2]))

    raw_target = target

    def target(t, i):
        r = raw_target(t, i)
        hops = 0
        while not isinstance(r, tuple) and is_goto(r) and hops < n:
            r2 = raw_target(body[r].args[1], r)
            if r2 == r:
                break
            r, hops = r2, hops + 1
        return r

    nodes, edges, externals = [], [], {}
    moved_labels = {}             # a skipped jump's labels go on where it lands

    def node_id(t):
        if isinstance(t, tuple):
            externals[t[1]] = True
            return f'x_{re.sub(r"[^A-Za-z0-9_]", "_", t[1])}'
        if t >= n:
            used_end[0] = True
            return "iend"
        # skip over "end" pseudo-statements (labels at the very end of a body)
        if body[t].kind == "end":
            used_end[0] = True
            return "iend"
        return f"i{t}"

    def follow(r):
        hops = 0
        while not isinstance(r, tuple) and is_goto(r) and hops < n:
            r2 = raw_target(body[r].args[1], r)
            if r2 == r:
                break
            r, hops = r2, hops + 1
        return r

    def edge(i, t, *, zero=False, extra=""):
        t = follow(t)
        tid = node_id(t)
        nxt = (not isinstance(t, tuple)) and t == i + 1
        attrs = [f"weight={8 if nxt else 1}"]
        if zero:
            attrs.append("dir=both, arrowtail=odot")
            if t != i:
                attrs.append("tailport=e")
        elif not nxt and t != i:
            attrs.append("tailport=s")
        if t == i:
            attrs.append("tailport=w, headport=n")
        if extra:
            attrs.append(extra)
        edges.append(f"  i{i} -> {tid} [{', '.join(attrs)}];")

    for i, st in enumerate(body):
        if is_goto(i) and st.labels:
            t = follow(i)
            if not isinstance(t, tuple) and t < n and body[t].kind != "end":
                moved_labels.setdefault(t, []).extend(st.labels)
    for i, st in enumerate(body):
        if st.kind == "end" or is_goto(i):
            continue
        names = moved_labels.get(i, []) + st.labels
        xl = f', xlabel="{esc(", ".join(names))}"' if names else ""
        if st.kind == "give":
            nodes.append(f'  i{i} [shape=circle, label="{esc(st.args[0])}"{xl}];')
            edge(i, target(st.args[1], i) if len(st.args) > 1 else i + 1)
        elif st.kind == "take":
            nodes.append(f'  i{i} [shape=diamond, label="{esc(st.args[0])}"{xl}];')
            tz = target(st.args[1], i)
            tn = target(st.args[2], i) if len(st.args) > 2 else i + 1
            edge(i, tn)
            edge(i, tz, zero=True)
        elif st.kind == "stop":
            nodes.append(f'  i{i} [shape=octagon, label="stop"{xl}];')
        elif st.kind == "print":
            nodes.append(f'  i{i} [shape=note, style=filled, fillcolor="#f3f0e6", '
                         f'label="print {esc(show_text(st.args))}"{xl}];')
            edge(i, i + 1)
        elif st.kind == "use":
            nodes.append(f'  i{i} [shape=box, style="rounded,filled", fillcolor="#e8eef7", '
                         f'label="{esc(" ".join(st.args))}"{xl}];')
            edge(i, i + 1)
        else:  # a func call
            name, args = st.args[0], st.args[1:]
            f = funcs.get(name)
            shown = [a for k, a in enumerate(args)
                     if not (f and k < len(f.params) and f.params[k].startswith("~"))]
            nodes.append(f'  i{i} [shape=box, style="rounded,filled", fillcolor="#e8eef7", '
                         f'label="{esc(" ".join([name] + shown))}"{xl}];')
            edge(i, i + 1)
            if f:
                for k, a in enumerate(args):
                    if k < len(f.params) and f.params[k].startswith("~"):
                        edge(i, target(a, i), extra=f'style=dashed, label="{esc(f.params[k][1:])}", '
                                                     f'fontsize=9, tailport=e')
    # does control fall off the end?
    if n and body[-1].kind not in ("stop",):
        last = body[-1]
        if last.kind == "end" or not (last.kind == "give" and len(last.args) > 1) and \
                not (last.kind == "take" and len(last.args) > 2):
            used_end[0] = True
    out = ["digraph bcflow {",
           f'  label="{esc(title)}"; labelloc=t; labeljust=l;',
           '  graph [rankdir=TB, nodesep=0.3, ranksep=0.35, fontname="Helvetica", fontsize=11];',
           '  node  [fontname="Helvetica", fontsize=11, margin="0.06,0.03", width=0.45, height=0.45];',
           '  edge  [arrowsize=0.7, fontname="Helvetica"];',
           "  start [shape=point, width=0.12];"]
    if n:
        out.append(f"  start -> {node_id(follow(0))} [weight=8];")
    out += nodes
    if used_end[0]:
        endlabel = "return" if is_func else "stop"
        endxl = [l for st in body if st.kind == "end" for l in st.labels]
        xl = f', xlabel="{esc(", ".join(endxl))}"' if endxl else ""
        shape = "box, style=rounded" if is_func else "octagon"
        out.append(f'  iend [shape={shape}, label="{endlabel}"{xl}];')
    for name in externals:
        out.append(f'  x_{re.sub(r"[^A-Za-z0-9_]", "_", name)} [shape=plaintext, fontcolor="#666666", '
                   f'label="→ {esc(name)}"];')
    out += edges
    out.append("}")
    return "\n".join(out)


def funcs_used(body, funcs, acc=None):
    """Every func a body calls, transitively, in first-call order."""
    acc = [] if acc is None else acc
    for st in body:
        if st.kind == "call" and st.args[0] in funcs and st.args[0] not in acc:
            acc.append(st.args[0])
            funcs_used(funcs[st.args[0]].body, funcs, acc)
    return acc


def main():
    args = sys.argv[1:]
    if args and args[0] == "--list":
        main_body, funcs = parse(args[1])
        for name in funcs_used(main_body, funcs):
            f = funcs[name]
            print(name, os.path.basename(f.file), " ".join(f.params))
        return
    path = args[0]
    main_body, funcs = parse(path)
    if len(args) > 1:
        f = funcs[args[1]]
        print(to_dot(f.body, funcs, f"func {f.name} {' '.join(f.params)}", is_func=True))
    else:
        print(to_dot(main_body, funcs, os.path.basename(path)))


if __name__ == "__main__":
    main()
