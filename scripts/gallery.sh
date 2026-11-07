#!/bin/bash
# Build docs/gallery.pdf: a description and a flow graph of each program in the
# gallery (docs/gallery.typ), drawn by scripts/bcflow.py and Graphviz `dot`.
# Needs python3, dot and typst.   Usage: scripts/gallery.sh
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=build/gallery
mkdir -p "$OUT"
make -s >/dev/null

PROGRAMS="euclid isqrt collatz e pi tictactoe guess nim wythoff"
render() {   # render NAME DOTSOURCE...
    local name="$1"; shift
    python3 scripts/bcflow.py "$@" | sed 's/fontname="Helvetica"/fontname="DejaVu Sans"/g' > "$OUT/$name.dot"
    dot -Tsvg "$OUT/$name.dot" -o "$OUT/$name.svg"
}

stats="$OUT/stats.csv"; : > "$stats"
for p in $PROGRAMS hexapawn; do
    src=$(grep -cvE '^\s*(#|$)' "examples/$p.bc")
    read -r insts regs < <(./beancraft -n "examples/$p.bc" | sed -E 's/.*OK: ([0-9]+) instructions, ([0-9]+) registers.*/\1 \2/')
    echo "$p,$src,$insts,$regs" >> "$stats"
done

# each program's main body, and the funcs it defines itself
for p in $PROGRAMS; do
    render "$p" "examples/$p.bc"
    for f in $(python3 scripts/bcflow.py --list "examples/$p.bc" | awk '$2 != "std.bc" {print $1}'); do
        render "$p-$f" "examples/$p.bc" "$f"
    done
done

# hexapawn.bc is ~2,500 generated lines: draw three representative excerpts
python3 - "$OUT" <<'PY'
import re, sys
out = sys.argv[1]
L = open('examples/hexapawn.bc').read().split('\n')
funcs, inf = [], False
for l in L:
    if l.startswith('func '): inf = True
    if inf: funcs.append(l)
    if inf and l.startswith('}'): inf = False
def grab(a_pat, b_pat):
    a = next(i for i, l in enumerate(L) if re.match(a_pat, l))
    b = next(i for i, l in enumerate(L) if i > a and re.match(b_pat, l))
    return L[a:b]
head = ['use "std"'] + funcs
box = grab(r'^# box 13:', r'^# box 14:')
end = grab(r'^bWins:', r'^pun3:')
end = [l for l in end if not re.match(r'\s+- last pun([3-9]|\d\d)\b', l)]
end += ['       # ... pun3 .. pun89, one per bead bin ...'] + grab(r'^pun90:', r'^eof:')
newg = grab(r'^newGame:', r'^# ---- your move')[:50]
for name, body in (('hx-box', box), ('hx-end', end), ('hx-tree', newg)):
    open(f'{out}/{name}.bc', 'w').write('\n'.join(head + body) + '\n')
PY
cp examples/std.bc "$OUT/std.bc"
for x in hx-box hx-end hx-tree; do render "$x" "$OUT/$x.bc"; done
for f in putc if1 show; do render "hexapawn-$f" examples/hexapawn.bc "$f"; done

# the standard-library funcs the gallery uses
for f in clear copy addr addn subn half inrange mul divmod sub isqrt printnum readnum; do
    render "std-$f" examples/std.bc "$f"
done

# the legend's pictures
legend() {   # legend NAME 'node attrs'
    printf 'digraph { node [fontname="DejaVu Sans", fontsize=11, margin="0.06,0.03", width=0.45, height=0.45]; n [%s]; }' "$2" \
        | dot -Tsvg -o "$OUT/legend-$1.svg"
}
legend give 'shape=circle, label="R"'
legend take 'shape=diamond, label="R"'
legend stop 'shape=octagon, label="stop"'
legend call 'shape=box, style="rounded,filled", fillcolor="#e8eef7", label="divmod n 2 h r"'
legend print 'shape=note, style=filled, fillcolor="#f3f0e6", label="print \"I win.\\n\""'

# natural sizes of the drawings, for the layout
python3 - "$OUT" <<'PY'
import glob, json, os, re, sys
out = sys.argv[1]
sizes = {}
for f in glob.glob(f'{out}/*.svg'):
    head = open(f).read(2000)
    w = float(re.search(r'width="([\d.]+)pt"', head).group(1))
    h = float(re.search(r'height="([\d.]+)pt"', head).group(1))
    sizes[os.path.basename(f)[:-4]] = [w, h]
json.dump(sizes, open(f'{out}/sizes.json', 'w'), indent=0)
PY

typst compile --root . docs/gallery.typ docs/gallery.pdf
echo "wrote docs/gallery.pdf"
