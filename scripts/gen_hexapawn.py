#!/usr/bin/env python3
"""Generate examples/hexapawn.bc: a matchbox-learning (MENACE / HER) Hexapawn player.

    python3 scripts/gen_hexapawn.py > examples/hexapawn.bc

The game tree is small enough to enumerate here, so the generator writes out one
"matchbox" per position the machine can face, with one bin of beads per legal
move, plus the code that recognises a position (a decision tree over the
board's bins) and plays a move.  Everything that *learns* happens in the .bc.
"""
import re

# ---- the game -----------------------------------------------------------------
# A board is a 9-char string, squares 1..9 = indices 0..8, row 1 on top:
#     1 2 3        B = the machine's pawns (start on top, move down)
#     4 5 6        W = yours (start at the bottom, move up)
#     7 8 9
START = "BBB...WWW"

def moves(b, side):
    """Legal moves for side, as (from, to) indices: one step forward onto an
    empty square, or one step diagonally forward onto an enemy pawn."""
    out = []
    d = -3 if side == "W" else 3
    opp = "B" if side == "W" else "W"
    for s in range(9):
        if b[s] != side:
            continue
        t = s + d
        if not 0 <= t < 9:
            continue
        if b[t] == ".":
            out.append((s, t))
        for dc in (-1, 1):
            c = s % 3 + dc
            if 0 <= c < 3 and b[t - t % 3 + c] == opp:
                out.append((s, t - t % 3 + c))
    return out

def apply(b, m):
    s, t = m
    l = list(b)
    l[t], l[s] = l[s], "."
    return "".join(l)

def winner_after(b, mover):
    """mover just moved: they win by reaching the far row, taking every enemy
    pawn, or leaving the enemy no legal move."""
    if mover == "W" and "W" in b[0:3]:
        return True
    if mover == "B" and "B" in b[6:9]:
        return True
    opp = "B" if mover == "W" else "W"
    return opp not in b or not moves(b, opp)

# Enumerate every position either side can face.
Wpos, Bpos = {}, {}           # board -> legal moves, for the side to move
afterW, afterB = set(), set()  # every board right after a W / B move (terminal or not)

def walk(b, side):
    D = Wpos if side == "W" else Bpos
    if b in D:
        return
    D[b] = moves(b, side)
    for m in D[b]:
        b2 = apply(b, m)
        (afterW if side == "W" else afterB).add(b2)
        if not winner_after(b2, side):
            walk(b2, "B" if side == "W" else "W")

walk(START, "W")
afterB.add(START)              # a new game "arrives" at the start position

wpos = sorted(Wpos)            # positions where it's your move
bpos = sorted(Bpos)            # the matchboxes
box_of = {b: i for i, b in enumerate(bpos)}
beads = [(i, j, m) for i, b in enumerate(bpos) for j, m in enumerate(Bpos[b])]
gidx = {(i, j): g + 1 for g, (i, j, m) in enumerate(beads)}   # bead bin -> 1..90

def bead(i, j):
    return f"box{i}m{j + 1}"

def sq(x):
    return x + 1

def board_comment(b):
    return " / ".join(b[r * 3:r * 3 + 3] for r in range(3))

# ---- emit ------------------------------------------------------------------------
out = []
def emit(line=""):
    out.append(line)

uid = [0]
def fresh(prefix):
    uid[0] += 1
    return f"{prefix}{uid[0]}"

def tree_at(boards, leaf_label, name, label):
    """Emit, at `label`, a decision tree that tells the boards apart by testing
    the w_i / b_i bins of one square at a time, ending at leaf_label(board)."""
    def score(c):
        groups = {}
        for b in boards:
            groups[b[c]] = groups.get(b[c], 0) + 1
        return (max(groups.values()), c)
    c = min((c for c in range(9) if len({b[c] for b in boards}) > 1), key=score)
    kids, pending = {}, []
    for k in "WB.":
        part = [b for b in boards if b[c] == k]
        if not part:
            kids[k] = "bug"
        elif len(part) == 1:
            kids[k] = leaf_label(part[0])
        else:
            kids[k] = fresh(name)
            pending.append((kids[k], part))
    nb = fresh(name)
    emit(f"{(label + ':').ljust(7)}- w{sq(c)} {nb}".ljust(34) + f"# square {sq(c)}: W?")
    emit(f"       + w{sq(c)} {kids['W']}")
    emit(f"{(nb + ':').ljust(7)}- b{sq(c)} {kids['.']}".ljust(34) + "#   B?")
    emit(f"       + b{sq(c)} {kids['B']}")
    for lab, part in pending:
        tree_at(part, leaf_label, name, lab)

def wleaf(b):
    if b in Wpos:
        return f"wpos{wpos.index(b)}"
    return "bWins"           # you have no move: the machine wins

def bleaf(b):
    if b in Bpos:
        return f"box{box_of[b]}"
    return "wWins"           # the machine has no move / you reached row 1 / ...

HEADER = f"""\
# Hexapawn against a machine that learns -- a bins-and-beans MENACE.
#
#   B B B       Three pawns each on a 3x3 board.  You are W, at the bottom, and
#   4 5 6       move first.  A pawn steps straight forward onto an empty square,
#   W W W       or diagonally forward onto an enemy pawn, capturing it.  You win
#               by reaching the far row, by taking every enemy pawn, or by leaving
#               the other side with no move.  Type a move as "from to", e.g. "8 5".
#
#   ./beancraft examples/hexapawn.bc               # play; it starts out clueless
#   ./beancraft examples/hexapawn.bc T=1000        # ...or let it practise first
#
# The machine (B) has no strategy written into it at all.  It has a matchbox for
# each of the {len(bpos)} positions it can face, and in each box one bin per legal move,
# holding one bean to start with ({len(beads)} bins in all).  To move, it draws a bean
# at random from the box for the current position and plays that move.  When it
# loses, it takes away the bean it played last -- that move will never be made
# again -- and if a box runs empty it resigns (and the move that led there loses
# its bean in turn).  Losses push the blame backwards, game by game, until every
# losing line is gone.  This is Martin Gardner's HER ("Hexapawn Educable Robot",
# 1962) after Donald Michie's MENACE (1961), which learned noughts and crosses
# from matchboxes and glass beads.  Here the beads are beans and the boxes bins.
#
# With T=n it first plays n practice games against a random opponent, printing W
# (it won) or L (it lost) for each: watch the Ls thin out.  A random opponent
# rarely finds the trickier lines, so it takes a while to be thorough: after 300
# practice games about half the runs can still be beaten (if you find the hole),
# after 1000 essentially none can -- the second player wins Hexapawn with perfect
# play.  It keeps learning from your games too, so a hole you exploit closes.  The game-specific code (move rules,
# position recognition) is generated by scripts/gen_hexapawn.py; the learning is
# the bins.
"""

for line in HEADER.rstrip("\n").split("\n"):
    emit(line)
emit()
emit('use "std"')
emit()
emit("func putc C {                     # print the character with code C")
emit("  addn C con/byte")
emit("  give con/emit")
emit("}")
emit()
emit("func if1 R ~no {                  # fall through if R holds a bean, else jump to no")
emit("  - R no")
emit("  + R")
emit("}")
emit()
emit("func show W B CH {                # print ' W', ' B', or ' ' and the square's digit CH")
emit("  putc 32")
emit("  - W notW")
emit("  + W")
emit("  putc 87")
emit("  - z done")
emit("  notW: - B notB")
emit("  + B")
emit("  putc 66")
emit("  - z done")
emit("  notB: putc CH")
emit("}")
emit()
emit("# ---- fill the matchboxes: one bean per move -------------------------------------")
first = True
for i, b in enumerate(bpos):
    for j in range(len(Bpos[b])):
        emit(f"{'init:' if first else ''}".ljust(7) + f"+ {bead(i, j)}")
        first = False
emit("       clear won")
emit("       clear lost")
emit("       clear cd")
emit("       addn 50 cd                 # practice results: 50 to a line")
emit("       - T newGame                # practice games?")
emit("       + T")
emit("       @SAY practice (W: B won, L: B lost):\\n")
emit()
emit("# ---- a new game -----------------------------------------------------------------")
emit("newGame: clear last")
for s_ in range(1, 10):
    emit(f"       clear w{s_}")
    emit(f"       clear b{s_}")
for s_ in (1, 2, 3):
    emit(f"       + b{s_}")
for s_ in (7, 8, 9):
    emit(f"       + w{s_}")
emit()
emit("# ---- your turn: recognise the position (a decision tree over the squares' bins) --")
entry = fresh("t")
emit(f"yourTurn: - z {entry} {entry}")
tree_at(sorted(afterB), wleaf, "t", entry)
emit()
emit("# ---- your move: a practice game picks one at random; otherwise you type it -----")
for k, b in enumerate(wpos):
    ms = Wpos[b]
    emit(f"{('wpos' + str(k) + ':').ljust(7)}- T human".ljust(34) + f"# {board_comment(b)}")
    emit("       + T")
    if len(ms) > 1:
        emit("       give rand/next")
        emit(f"       divmod rand/byte {len(ms)} q r")
        for s_, t_ in ms[:-1]:
            emit(f"       - r w{sq(s_)}to{sq(t_)}")
    s_, t_ = ms[-1]
    emit(f"       - z w{sq(s_)}to{sq(t_)} w{sq(s_)}to{sq(t_)}")
emit()
emit("human: clear res")
emit("board: show w1 b1 49               # print the board ...")
emit("       show w2 b2 50")
emit("       show w3 b3 51")
emit("       putc 10")
emit("       show w4 b4 52")
emit("       show w5 b5 53")
emit("       show w6 b6 54")
emit("       putc 10")
emit("       show w7 b7 55")
emit("       show w8 b8 56")
emit("       show w9 b9 57")
emit("       putc 10")
emit("       - res ask                  # ... then: your move (res = 0),")
emit("       - res msgB                 #   or B won (1),")
emit("       - res msgW                 #   you won (2),")
emit("       - res msgR msgR            #   B resigned (3)")
emit()
emit("ask:   @SAY your move (from to): ")
emit("       readnum f eof")
emit("       readnum t eof")
shapes = sorted({(s_, s_ - 3) for s_ in range(3, 9)} |
                {(s_, s_ - 4) for s_ in range(3, 9) if s_ % 3 > 0} |
                {(s_, s_ - 2) for s_ in range(3, 9) if s_ % 3 < 2})
emit("       - f bad                    # dispatch on (from, to)")
for f in range(1, 10):
    emit(f"       - f from{f}")
emit("bad:   clear f")
emit("       clear t")
emit("       @SAY not a legal move\n")
emit("       - z ask ask")
for f in range(1, 10):
    emit(f"{('from' + str(f) + ':').ljust(7)}- t bad")
    for t in range(1, 10):
        target = f"chk{f}to{t}" if (f - 1, t - 1) in shapes else "bad"
        emit(f"       - t {target}")
    emit("       - z bad bad")
for (s_, t_) in shapes:
    S, T_ = sq(s_), sq(t_)
    emit(f"{('chk' + str(S) + 'to' + str(T_) + ':').ljust(7)}if1 w{S} bad".ljust(34) + "# your pawn there?")
    emit(f"       - w{T_} free{S}{T_}")
    emit(f"       + w{T_} bad")
    if t_ == s_ - 3:
        emit(f"{('free' + str(S) + str(T_) + ':').ljust(7)}- b{T_} w{S}to{T_}".ljust(34) + "# straight on: must be empty")
        emit(f"       + b{T_} bad")
    else:
        emit(f"{('free' + str(S) + str(T_) + ':').ljust(7)}if1 b{T_} bad".ljust(34) + "# diagonal: must capture")
        emit(f"       - z w{S}to{T_} w{S}to{T_}")
emit()
emit("# W moves (yours, or the practice opponent's), then it's B's turn")
for (s_, t_) in shapes:
    S, T_ = sq(s_), sq(t_)
    emit(f"{('w' + str(S) + 'to' + str(T_) + ':').ljust(7)}- w{S} bug")
    emit(f"       - b{T_} wp{S}{T_} wp{S}{T_}".ljust(34) + "# (a capture empties the B bin)")
    emit(f"{('wp' + str(S) + str(T_) + ':').ljust(7)}+ w{T_} myTurn")
emit()
emit("# ---- B's turn: recognise the position, open its matchbox, draw a bean -----------")
entry = fresh("m")
emit(f"myTurn: - z {entry} {entry}")
tree_at(sorted(afterW), bleaf, "m", entry)
emit()
for i, b in enumerate(bpos):
    ms = Bpos[b]
    emit(f"# box {i}: {board_comment(b)}   " +
         "  ".join(f"m{j + 1}: {sq(s_)}-{sq(t_)}" for j, (s_, t_) in enumerate(ms)))
    emit(f"{('box' + str(i) + ':').ljust(7)}clear tot")
    for j in range(len(ms)):
        emit(f"       addr {bead(i, j)} tot")
    emit("       - tot resign               # an empty box: no move left worth making")
    emit("       + tot")
    if len(ms) == 1:
        emit(f"       - z b{i}p0 b{i}p0")
    else:
        emit("       give rand/next             # r := a random bean, 0 .. tot-1")
        emit("       divmod rand/byte tot q r")
        for j in range(len(ms)):
            lj = f"b{i}d{j}"
            nxt = f"b{i}d{j + 1}" if j + 1 < len(ms) else f"b{i}p{j}"
            emit(f"{(lj + ':').ljust(7)}copy {bead(i, j)} c")
            emit(f"{(lj + 'l:').ljust(7)}- c {nxt}")
            emit(f"       - r b{i}p{j} {lj}l")
    for j, (s_, t_) in enumerate(ms):
        emit(f"{('b' + str(i) + 'p' + str(j) + ':').ljust(7)}clear c")
        emit("       clear last")
        emit(f"       addn {gidx[(i, j)]} last".ljust(34) + f"# remember the bean: {bead(i, j)}")
        emit(f"       - z b{sq(s_)}to{sq(t_)} b{sq(s_)}to{sq(t_)}")
emit()
bshapes = sorted({m for b in Bpos for m in Bpos[b]})
for (s_, t_) in bshapes:
    S, T_ = sq(s_), sq(t_)
    emit(f"{('b' + str(S) + 'to' + str(T_) + ':').ljust(7)}- b{S} bug")
    emit(f"       - w{T_} bp{S}{T_} bp{S}{T_}")
    emit(f"{('bp' + str(S) + str(T_) + ':').ljust(7)}+ b{T_} yourTurn")
emit()
emit("# ---- the end of a game -----------------------------------------------------------")
emit("bWins: clear kind")
emit("       + kind over")
emit("resign: clear kind")
emit("       addn 3 kind")
emit("       - z punish punish")
emit("wWins: clear kind")
emit("       addn 2 kind")
emit("punish: - last over               # B lost: take away the bean it played last")
for g in range(1, len(beads)):
    emit(f"       - last pun{g}")
emit(f"       - z pun{len(beads)} pun{len(beads)}")
for g, (i, j, m) in enumerate(beads, start=1):
    emit(f"{('pun' + str(g) + ':').ljust(7)}clear last")
    emit(f"       - {bead(i, j)} over over")
emit()
emit("over:  - T real")
emit("       - kind lost1               # a practice game: print W or L")
emit("       - kind won1")
emit("lost1: clear kind")
emit("       @SAY L")
emit("       - z col col")
emit("won1:  @SAY W")
emit("col:   - cd row row               # 50 to a line")
emit("row:   - cd nl")
emit("       + cd more")
emit("nl:    putc 10")
emit("       addn 50 cd")
emit("more:  - T done1")
emit("       + T newGame")
emit("done1: @SAY \\nnow it's your turn.\\n\\n")
emit("       - z newGame newGame")
emit("real:  copy kind res              # show the final board, then the verdict")
emit("       - kind bug")
emit("       - kind iWon")
emit("       + lost board")
emit("iWon:  clear kind")
emit("       + won board")
emit("msgB:  @SAY I win.\\n")
emit("       - z score score")
emit("msgW:  @SAY You win. I'll remember that.\\n")
emit("       - z score score")
emit("msgR:  @SAY I resign. I'll remember that.\\n")
emit("score: @SAY me ")
emit("       printnum won")
emit("       @SAY , you ")
emit("       printnum lost")
emit("       @SAY \\n\\n")
emit("       - z newGame newGame")
emit()
emit("bug:   stop")
emit("eof:")

# expand @SAY lines into putc runs
final = []
for line in out:
    if "@SAY " in line:
        lab, txt = line.split("@SAY ", 1)
        lab = lab.strip()
        txt_dec = txt.encode().decode("unicode_escape")
        for n, ch in enumerate(txt_dec):
            stmt = (lab.ljust(7) if (n == 0 and lab) else "       ") + f"putc {ord(ch)}"
            if n == 0:
                stmt = stmt.ljust(34) + "# " + repr(txt_dec)
            final.append(stmt)
    else:
        final.append(line)
# a label that fills its column still needs a space before the statement
final = [re.sub(r"^([A-Za-z_]\w*:)(\S)", r"\1 \2", l) for l in final]
print("\n".join(final))
