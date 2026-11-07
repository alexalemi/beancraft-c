// A gallery of beancraft programs: what each one does, and its flow graph.
// Build with scripts/gallery.sh (it draws the graphs into build/gallery/ first).

#let G = "/build/gallery/"
#let sizes = json(G + "sizes.json")
#let stats = {
  let d = (:)
  for row in csv(G + "stats.csv") { d.insert(row.at(0), row.slice(1)) }
  d
}

#set document(title: "Beans that think: a beancraft gallery", author: "beancraft")
#set page(paper: "us-letter", margin: (x: 0.8in, y: 0.75in),
  footer: context [#set text(8pt, fill: gray); beancraft gallery #h(1fr) #counter(page).display()])
#set text(font: "Libertinus Serif", size: 10.5pt)
#set par(justify: true, leading: 0.6em)
#show raw: set text(font: "DejaVu Sans Mono", size: 8.5pt)
#show heading.where(level: 1): it => { pagebreak(weak: true); v(0.3em); text(18pt, it) ; v(0.2em) }
#show heading.where(level: 2): set text(12.5pt)
#show link: set text(fill: rgb("#1f4e8c"))

// The width/height the text column allows, and the smallest scale at which a
// graph is drawn inline; anything that would need to shrink further gets a
// page of its own, sized to fit it.
#let maxw = 6.9in
#let maxh = 8.6in
#let inline-min = 0.62
#let own-scale = 0.7

#let graph(name, caption: none) = {
  let (w, h) = sizes.at(name)
  let s = calc.min(1.0, maxw / (w * 1pt), maxh / (h * 1pt))
  if s >= inline-min {
    figure(image(G + name + ".svg", width: w * s * 1pt), caption: caption, kind: image)
  } else {
    // its own page, as big as the drawing at a readable scale
    page(width: w * own-scale * 1pt + 1in, height: auto,
         margin: 0.5in, footer: none)[
      #set align(center)
      #if caption != none [#text(9pt, style: "italic", caption) #v(0.2em)]
      #image(G + name + ".svg", width: w * own-scale * 1pt)
    ]
  }
}

#let facts(name) = {
  let (src, insts, regs) = stats.at(name)
  block(inset: (x: 8pt, y: 5pt), fill: rgb("#f4f2ec"), radius: 3pt, width: 100%)[
    #set text(9pt)
    `examples/`#raw(name + ".bc") #h(1fr)
    #src source lines #sym.dot.c #insts counter-machine instructions #sym.dot.c #regs bins
  ]
}

#let run(body) = block(inset: (left: 10pt, y: 3pt), stroke: (left: 2pt + rgb("#c9c2ae")), body)

// ---------------------------------------------------------------------------------
#align(center)[
  #v(1.2in)
  #text(26pt)[Beans that think]
  #v(0.4em)
  #text(14pt, style: "italic")[a gallery of beancraft programs, drawn as machines]
  #v(2em)
]

*beancraft* has one kind of value, a bin of beans, and three instructions:
#raw("give R") puts a bean in bin `R`; #raw("take R z") takes one out, or jumps
to `z` if the bin is empty; and `stop`. That is a counter machine (a Minsky
register machine), and everything below, from long division to a game player
that learns, is built from those three moves.

Each program comes with a short description and a *flow graph* of the program
as written. The graphs use the same pictures as `beancraft --emit-dot`:

#table(columns: (auto, 1fr), stroke: none, inset: (x: 6pt, y: 4pt), align: (center + horizon, left + horizon),
  image(G + "legend-give.svg", height: 0.42in), [*give R*: put a bean in bin `R`, then carry on down.],
  image(G + "legend-take.svg", height: 0.5in), [*take R*: if `R` has a bean, take it and carry on _down_; if it is empty, leave by the _side_ arrow, the one with the small open circle at its tail.],
  image(G + "legend-stop.svg", height: 0.42in), [*stop*.],
  image(G + "legend-call.svg", height: 0.36in), [A call of a `func` (a subroutine, inlined where it is called). Dashed side arrows are its _label arguments_, the places it can jump to instead of returning: `sub A B R` jumps to `under` when `B > A`, `readnum` to `eof` at end of input. The funcs a program defines are drawn after it; the standard library's are at the back.],
  image(G + "legend-print.svg", height: 0.36in), [A run of `putc` calls, shown as the text it prints.],
  [#text(fill: rgb("#666666"))[→ name]], [A jump to a label outside the picture (only in excerpts).],
)

Text beside a node is its label. Unconditional jumps (`take z X X` on the
never-filled bin `z`) are drawn as plain arrows. These graphs are the program
_as written_; the machine that actually runs has every call expanded, which is
the "counter-machine instructions" count given with each program.

*Run them with* `./beancraft examples/NAME.bc [REG=VALUE ...]`. Most are much
faster with `-O`, which recognises the standard loops (zeroing, moving,
multiplying, dividing) and does each in one big-number step; that is what lets
`pi.bc` print hundreds of digits and `nim.bc` play with heaps of $10^21$ beans.

= Arithmetic

== `euclid.bc`: greatest common divisor

#facts("euclid")

Euclid's algorithm with remainders: while $B > 0$, replace $(A, B)$ by
$(B, A mod B)$; then $A$ is the gcd. The older `gcd.bc` does the same job by
repeated subtraction, which takes about $max(A, B) slash gcd$ rounds; this takes
$O(log min(A, B))$ rounds, and each `divmod` is a single folded division under `-O`.

Note how little is left once `divmod` exists: the loop is a test of `B`, a call,
and two _transfers_ (the `take X ... / give Y` pairs that pour one bin into another).

#run[`./beancraft -O examples/euclid.bc A=1071 B=462` #h(1fr) `Out = 21`]

#graph("euclid")

== `isqrt.bc`: integer square root

#facts("isqrt")

$"Out" = floor(sqrt(N))$ by Newton's method on integers. Start from $x = N$ and
repeat $x <- (x + N div x) div 2$; it falls steadily until it lands on the answer.
The new $x$ is smaller exactly when $t = N div x < x$, and the program tests
that by dividing again: $t div x = 0$. That trick, _compare by dividing_, turns up
in several programs below, because a division is one step under `-O` while
counting two bins down side by side is not.

#run[`./beancraft -O examples/isqrt.bc N=2000000000000000000000000000000000000000` \
  #h(1fr) `Out = 44721359549995793928` (that's $sqrt 2$ to 20 digits)]

#graph("isqrt")

== `collatz.bc`: the $3n + 1$ problem

#facts("collatz")

Print the Collatz sequence from $N$ (halve it if even, $3n+1$ if odd) until it
reaches 1, then the number of steps and the highest point. `divmod n 2 h r`
splits $n = 2h + r$ at once, so the odd case can go straight to $6h + 4$: six
`give n`s per bean of `h`, plus four. Whether every $N$ gets to 1 is the
Collatz conjecture, open since 1937.

#run[`./beancraft -O examples/collatz.bc N=27` \
  `27 82 41 124 62 31 94 47 ... 4 2 1` \
  `111 steps, peak 9232`]

#graph("collatz")
#graph("collatz-putc", caption: [`func putc C`: print the character with code `C`. Every program that talks uses this.])

= Digits of $e$ and $pi$

A _spigot_ produces digits one at a time and never looks at them again. The
textbook spigots keep an array of digits, and a counter machine has no arrays;
but Gibbons' _unbounded_ spigot (2006) keeps only a few numbers, each growing
without limit, and beancraft's bins are exactly that. Both programs keep the
part of the series consumed so far as one fraction $f(y) = (q y + b) slash t$,
where $y in [0, 1]$ stands for the rest of the series, and alternate two moves:

- *consume* the next term, which multiplies into $q$, $b$, $t$;
- *produce* a digit $d = b div t$, as soon as it is certain: when
  $f(0)$ and $f(1)$ agree on it, i.e. $(b mod t) + q < t$. Print it, and
  shift: $b <- 10 (b mod t)$, $q <- 10 q$.

The digit test is the compare-by-dividing trick again. The fraction is set up
(with $y$ measured from the bottom of its range) so that every quantity stays
non-negative, since a bin can't hold a negative number.

== `e.bc`

#facts("e")

$e = 1 + 1/1 (1 + 1/2 (1 + 1/3 (1 + dots.c)))$, so term $k$ is the map
$y |-> (1 + y) slash k$, and consuming it is $b <- b k + q$, $t <- t k$.

#run[`./beancraft -O examples/e.bc N=60` \
  `2.71828182845904523536028747135266249775724709369995957496696`]

#graph("e")
#graph("e-move", caption: [`func move From To`: pour `From` into `To` (`To` cleared first).])

== `pi.bc`

#facts("pi")

Euler's series $pi = 2 + 1/3 (2 + 2/5 (2 + 3/7 (2 + dots.c)))$; with the tail
written as $3 + y$, term $k$ becomes $b <- b(2k+1) + q(k-1)$, $q <- q k$,
$t <- t(2k+1)$. It is the same machine as `e.bc`, fed a different series.
Without `-O` even `3.1` takes twenty seconds; with it, 400 digits take about one.

#run[`./beancraft -O examples/pi.bc N=60` \
  `3.14159265358979323846264338327950288419716939937510582097494`]

#graph("pi")

= Games against the machine

Can a few bins of beans play a game well? Five ways of saying yes.

== `tictactoe.bc`: rules that never lose

#facts("tictactoe")

You are X and move first; the machine is O. Each square is two bins, `x`$i$ and
`o`$i$. The machine plays the first rule that applies: win if it can; block
your line; take the centre; if you hold two opposite corners, take a side (a
corner loses to a fork); four specific "fork-stopper" replies; else a corner,
else a side. The rules were found by trial in Python and then checked by
playing the actual program against every possible sequence of X moves: 489
games, 348 won by O, 141 drawn, none lost.

`func line` tries to complete one line in each of its three orientations; the
long column of `line` calls in the middle of the graph is the win check and
then the block check, eight lines each.

#run[`./beancraft examples/tictactoe.bc` #h(1fr) type a square 1--9]

#graph("tictactoe")
#grid(columns: 2, gutter: 12pt,
  graph("tictactoe-line", caption: [`func line`: complete line ABC whichever square is open.]),
  graph("tictactoe-complete", caption: [`func complete`: if `PA` and `PB` are held and C is empty, take C.]),
  graph("tictactoe-free", caption: [`func free`: fall through if a square is empty.]),
  graph("tictactoe-take1", caption: [`func take1`: take a square if it is free.]),
  graph("tictactoe-three", caption: [`func three`: are A, B, C all held?]),
  graph("tictactoe-if1", caption: [`func if1`: fall through if R holds a bean.]),
  graph("tictactoe-show", caption: [`func show`: print X, O, or the digit.]),
  graph("tictactoe-row", caption: [`func row`: print one row of the board.]),
)
#graph("tictactoe-won", caption: [`func won`: has P completed any of the eight lines?])
#graph("tictactoe-rule", caption: [`func rule`: print `---+---+---`.])

== `guess.bc`: binary search

#facts("guess")

Think of a number from 1 to $N$ (default 100). The machine keeps the range
$[l o, h i]$ that your number must lie in and always asks about the middle, so
each answer halves the range: at most $ceil(log_2 (N + 1))$ questions, which it
works out and tells you first. If the range ever empties, your answers
contradicted each other, and it says so. With `-O`, $N = 10^30$ takes at most
100 questions.

#run[`./beancraft examples/guess.bc` #h(1fr) answer `h`, `l` or `y`]

#graph("guess")

== `nim.bc`: perfect play from a theorem

#facts("nim")

Three heaps (default 3, 5, 7); take any number from one heap; taking the last
bean wins. Bouton (1901): XOR the heap sizes in binary. If that _nim-sum_ is 0,
the player to move loses against good play; otherwise shrinking some heap $h$
to $h xor s$ makes it 0. The machine never looks ahead. `func xor` peels off
one bit of each number per round with `divmod a 2`, and `sub` (which jumps to
`under` when the result would be negative) both finds the heap to shrink and
says how much to take.

In 40 games from random starting heaps, the machine won every one in which the
human started from a nim-sum of 0. From 3, 5, 7 a perfect human wins.

#run[`./beancraft examples/nim.bc` #h(1fr) type e.g. `3 2`: take 2 from heap 3 \
     `./beancraft -O examples/nim.bc A=1000000000000000000000 B=12345678987654321 C=3`]

#graph("nim")
#graph("nim-xor", caption: [`func xor A B R`: $R = A xor B$, one bit per round.])

== `wythoff.bc`: the golden ratio, from beans

#facts("wythoff")

Two heaps (default 13, 21); take any number from one heap, or the same number
from both. Wythoff (1907) showed that the losing positions are the pairs
$(a_k, a_k + k)$ with $a_k = floor(k phi)$, $phi = (1 + sqrt 5) slash 2$:
(0,0), (1,2), (3,5), (4,7), (6,10), (8,13), ... From any other position there is
a move onto one, found by comparing the smaller heap with $a_d$ where $d$ is
the difference. The machine has no $phi$, and needs none:
$ floor(k phi) = (k + floor(sqrt(5 k^2))) div 2 $
exactly, so `func phi` computes the golden ratio, to just the precision needed,
from integer square roots of beans.

Checked against a brute-force solution for every position up to $60 times 60$,
and in about 60 played games every machine reply landed on a losing pair.

#run[`./beancraft examples/wythoff.bc` #h(1fr) type `a b`: take `a` from heap 1, `b` from heap 2]

#graph("wythoff")
#graph("wythoff-phi", caption: [`func phi K R`: $R = floor(K phi)$.])

== `hexapawn.bc`: a machine that learns

#facts("hexapawn")

The others play by rules someone worked out. This one is told nothing about
strategy. Hexapawn is played with three pawns each on a 3×3 board; a pawn steps
forward onto an empty square or diagonally onto an enemy pawn; you win by
reaching the far row, taking every enemy pawn, or leaving the other side with no
move. You are White and move first.

The machine is Martin Gardner's _Hexapawn Educable Robot_ (1962), after Donald
Michie's MENACE (1961), which learned noughts and crosses from matchboxes full of
coloured beads. There is a matchbox for each of the 37 positions the machine can
face, and in each box one bin per legal move (90 bins), each starting with one
bean. To move, it draws a bean at random from the box for the current position.
When it loses, the bean it played last is removed, so that move is never made
again; if a box runs empty it resigns, and the move that led there loses its bean
in turn. Losses push the blame backwards, game by game.

With `T=n` it first plays $n$ practice games against a random opponent,
printing W or L for each. Measured from the program's own bins over ten runs
each: after 300 practice games, 5 of 10 runs could still be beaten by some line
of play; after 1000, none could. It keeps learning from your games too.

#run[`./beancraft examples/hexapawn.bc T=1000` \
  `practice (W: B won, L: B lost):` \
  `LLLWWWWLLLWWLLWLWWWWWLWWWLWLLWWWWWWLLWLWWWWWWWWWWW` \
  `LWWWWWWWLWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWWW` ...]

The program is about 2,500 lines, generated by `scripts/gen_hexapawn.py` (the
move rules and the position recognition; the learning is all in the bins), so
its whole graph is too big to draw. Three excerpts:

#graph("hx-box", caption: [One matchbox, box 13 (board `.BB / W.W / W..`, three moves). It adds up the beans, resigns if there are none, draws a random number `r` below the total, and walks the bins until `r` runs out inside one of them. That bin's number is remembered in `last`.])
#graph("hx-end", caption: [The end of a game. On a loss, `punish` finds bin number `last` with a chain of `take`s (one per bin; most of the chain is cut from this picture) and removes one bean from it. A practice game then prints W or L; a real one prints the board, the verdict and the score.])
#graph("hx-tree", caption: [A new game, and the start of the decision tree that recognises the position: one square at a time, it asks whether that square's W bin or B bin holds a bean, and branches three ways.])

= The standard library

The `func`s from `examples/std.bc` that these programs use. `mul`, `divmod` and
`sub` are written in exactly the shapes the `-O` optimizer folds into single
big-number operations.

#let stdf = (
  ("clear", [`clear X`: $X <- 0$.]),
  ("copy", [`copy From To`: $"To" <- "From"$ (via a scratch bin).]),
  ("addr", [`addr From To`: $"To" += "From"$.]),
  ("addn", [`addn N R`: $R += N$ ($N$ a literal, usually).]),
  ("subn", [`subn N R`: $R <- max(0, R - N)$.]),
  ("half", [`half X`: $X <- floor(X slash 2)$.]),
  ("inrange", [`inrange A B SPAN R`: is $B <= A < B + "SPAN"$?]),
  ("mul", [`mul A B R`: $R <- A B$.]),
  ("divmod", [`divmod N D Q REM`: long division.]),
  ("sub", [`sub A B R ~under`: $R <- A - B$, or jump to `under`.]),
  ("isqrt", [`isqrt N R`: Newton's method, as in `isqrt.bc`.]),
  ("printnum", [`printnum N`: print $N$ in decimal (digits stacked in base 11).]),
  ("readnum", [`readnum N ~eof`: read a decimal number from the console.]),
)
#for (f, cap) in stdf { graph("std-" + f, caption: cap) }
