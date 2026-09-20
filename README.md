# beancraft

[![ci](https://github.com/alexalemi/beancraft-c/actions/workflows/ci.yml/badge.svg)](https://github.com/alexalemi/beancraft-c/actions/workflows/ci.yml)
[![playground](https://github.com/alexalemi/beancraft-c/actions/workflows/pages.yml/badge.svg)](https://www.alexalemi.com/beancraft-c/)

**beancraft** is a tiny programming language whose only data type is the
non-negative integer — a *bin* of beans — and whose only operations are
**`give`** (put a bean in a bin) and **`take`** (take one out, or branch if the
bin is empty) — a [counter machine](https://en.wikipedia.org/wiki/Counter_machine)
(a.k.a. a Minsky register machine). It is Turing-complete, painfully so. This
repository is the C implementation: a parser, an optimizer, a tree-walking
interpreter, and a [QBE](https://c9x.me/compile/)-based native compiler — plus a
small zoo of I/O "devices" (console, a 256-colour framebuffer, audio, keyboard,
mouse, clock, RNG) exposed as magic registers, so you can actually *do* things
with it.

```bc
# examples/mul.bc — Out := A * B  (add A to Out, B times)
clrTmp: - tmp clrOut self
clrOut: - Out loop self
loop: - B done
addA: - A restoreA
+ Out
+ tmp addA
restoreA: - tmp loop
+ A prev
```

```console
$ ./beancraft examples/mul.bc A=7 B=8
Results:
tmp = 0
Out = 56
B = 0
A = 7
```

## Build

Needs a C11 compiler and `make`. For native compilation you also want
[`qbe`](https://c9x.me/compile/) on your `PATH`; for a windowed framebuffer,
SDL2 (`pkg-config --exists sdl2`).

```console
$ make            # builds ./beancraft
$ make test       # parser + bignum + optimizer unit tests (ASan/UBSan), then test/run_examples.sh
$ make debug      # ./beancraft with -fsanitize=address,undefined
$ make sdl        # ./beancraft with the SDL framebuffer backend
$ make wasm       # web/beancraft.{mjs,wasm} — the interpreter as a WebAssembly module (needs Emscripten)
$ make clean
```

Each configuration compiles into its own subdirectory of `build/`
(`release`, `debug`, `sdl`), so switching between `make`, `make test`, and
`make sdl` never mixes incompatible objects — no `make clean` needed.

### Web playground

**Try it live: <https://www.alexalemi.com/beancraft-c/>** (built and deployed
from `main` by `.github/workflows/pages.yml`).

`make wasm` (with [Emscripten](https://emscripten.org/) on `PATH`) compiles the
interpreter — parser → IR → optimizer → interpreter, no QBE backend — to a
WebAssembly module (built with Asyncify). `web/index.html` is a small demo page
that loads it: pick an example or type your own counter-machine program, set
register values, hit Run. Programs that draw via `screen/*` animate live on a
256×192 canvas below the output panes — each `give screen/flush` paints a frame
and yields to the browser. While a run is live, your keyboard feeds `kbd/event`
(same key encoding as the SDL backend), pointer events over the canvas feed
`mouse/*`, and the console-input box feeds `con/read` (which blocks until you
Send a line, press EOF, or Stop). Console output streams live. WASM won't load
over `file://`, so serve the repo over HTTP:

```console
$ make wasm && python3 -m http.server
# then open http://localhost:8000/web/index.html
```

## Run

```console
$ ./beancraft [options] file.bc [REG=VALUE ...]
```

| flag | meaning |
| --- | --- |
| `-v, --verbose` | print step count and timing |
| `-q, --quiet` | suppress the final register dump |
| `-n, --dry-run` | parse + lower only, don't execute (pair with `--show-*`) |
| `-s, --max-steps N` | execution step cap (default 10,000,000; raised to ∞ when the program uses an I/O device) |
| `-l, --list-regs` | list the program's registers |
| `-O, --optimize` | enable the loop-folding optimizer (see below) |
| `-c, --check` | differential test: run at `-O0` *and* `-O`, compare every register, exit non-zero on mismatch |
| `--trace[=N]` | print every executed instruction to stderr (step, pc, op, register before → after); `=N` stops tracing after N steps |
| `--debug` | interactive debugger: `s`tep, `c`ontinue, `b`reakpoints by label or pc, `w`atch a register, `p`rint/`regs`, `l`ist; runs unoptimized so labels map 1:1 |
| `--show-ast` / `--show-ir` / `--show-opt` | dump the AST / IR / optimized IR, then run (use `-n` to just dump) |
| `--emit-qbe` | emit QBE IL to stdout, then exit (combine with `-O`) |
| `--emit-urm` | emit the program (and registers) Gödel-encoded for `examples/urm.bc`, then exit |
| `--emit-tally` | emit the program (and registers) tally-encoded for `examples/tally.bc`, the 62-move universal machine, then exit |
| `--decode-tally` | read a `tally.bc` register bank (`R=VALUE`) back into the program's named registers |
| `--emit-dot` (alias `--show-dot`) | emit the program as a [Graphviz](https://graphviz.org/) graph, then exit — pipe into `dot -Tsvg`. `give`s are circles, `take`s are diamonds whose empty-bin branch leaves from the side with a small open circle at its tail |

`REG=VALUE` arguments set a register's initial value before the run (the value
may be an arbitrary-precision integer — e.g. the huge numbers from `--emit-urm`);
when the program halts, every named register's final value is printed under a
`Results:` header — *except* for programs that use an I/O device, which print
just their output (a register dump would clobber it). `--quiet` suppresses the
dump; `--verbose` adds step counts and forces it on.

### Compiling to a native binary

```console
$ scripts/bccompile -O examples/mul.bc            # -> ./examples/mul
$ ./examples/mul A=12 B=12 | grep Out
Out = 144
```

`bccompile` runs `beancraft --emit-qbe`, then `qbe`, then `cc`, linking against
the small C runtime in `src/qbe_runtime.c`. Flags: `-O`/`-O0` (optimizer),
`-g`, `--sdl`, `-v`. See `scripts/bccompile -h`.

## The `-O` optimizer

A counter machine adds by looping and multiplies by looping over *that* — so the
naïve cost of `Out := A*B` is O(A·B) increments. The optimizer recognizes a
handful of loop idioms and folds each into one O(1) bignum operation:

- **ZERO** — `take R exit self` ⟶ `R := 0`
- **TRANSFER** — `take A exit; give D₁…Dₙ; jmp` ⟶ `Dᵢ += A; A := 0`
- **DIVMOD** — `k` chained `take R` plus a give-run ⟶ `Qᵢ += R/k; goto exit[R mod k]; R := 0`
- **MULADD** — the two-transfer multiply loop ⟶ `Dᵢ += C·S + (C−1)·T; S += T; T,C := 0`
- **ISZERO** — `take R z; give R nz` (the "is R zero?" idiom) ⟶ `goto R==0 ? z : nz` (R unchanged)
- **COPY** — `TRANSFER S→{D…,T}; TRANSFER T→{S}` (the non-destructive copy) ⟶ `Dᵢ += S` (S kept)
- **DIVBIN** — the "divide by a bin" loop: `take P full; take R out; give REM; jmp` with `full: take REM q; give P; jmp` / `q: give Q; jmp` ⟶ `Q += R div P; REM := R mod P` — a long division by the *value* of another register, in one bignum op

It also threads no-op jumps (`take R X X`, which a bare `label:` and `use`/`func`
inlining produce a lot of) and dead-code-eliminates what that leaves behind.
Both the interpreter and the QBE backend use the folded form, so `-O` speeds up
interpretation as well as compilation. `examples/urm.bc` (a universal register
machine that runs another beancraft program — and its registers — supplied as a
[Gödel-style](https://en.wikipedia.org/wiki/G%C3%B6del_numbering) number) is
unusable without it and merely-slow with it.

## Examples

`examples/` has ~50 programs: arithmetic (`add`, `mul`, `div`, `pow`,
`factorial`, `fib`, `gcd`), predicates (`iseven`, `iszero`), text (`hello`,
`cat`, `clock`, `dayOfWeek`), graphics demos (`life`, `langton`, `sierpinski`,
`stars`, `gravity`, `dvd`, `bounce`, `paint`, `pong`), a chiptune (`chime`),
the standard library (`std.bc`), and two universal machines: `urm.bc` (runs
any beancraft program supplied as one Gödel number; see `--emit-urm`) and
`tally.bc` — **a universal machine in 62 moves**: the program is a string of
tallies and the whole register bank is one number `2^v₀·3^v₁·5^v₂·…`, so `give`
is a multiply and `take` is a divisibility test (see `--emit-tally` and
[docs/LANGUAGE.md](docs/LANGUAGE.md#the-tally-machine)).

```console
$ ./beancraft --emit-tally examples/mul.bc A=7 B=8
# examples/mul.bc -> tally.bc encoding: 8 instructions, 218 bits.  registers (prime): tmp=2 Out=3 A=5 B=7
P=210622975614431643255944712187008394406281938408708122559079250399 R=450375078125
$ ./beancraft examples/tally.bc -O $(./beancraft --emit-tally examples/mul.bc A=7 B=8 | tail -1) | grep '^R ='
R = 40886533830262541969805587578125
$ ./beancraft --decode-tally examples/mul.bc R=40886533830262541969805587578125
Results:
tmp = 0
Out = 56
A = 7
B = 0
$ ./beancraft --emit-dot examples/mul.bc | dot -Tsvg > mul.svg
```

```console
$ ./beancraft examples/factorial.bc N=10 -O | grep Out
Out = 3628800
$ ./beancraft examples/hello.bc
Hi
$ make sdl && ./beancraft examples/life.bc      # needs SDL; press q to quit
```

## Documentation

- **[docs/LANGUAGE.md](docs/LANGUAGE.md)** — the `.bc` language reference: instructions, jump targets, register init, `use` modules, `func` definitions, the URM and tally-machine encodings.
- **[docs/DEVICES.md](docs/DEVICES.md)** — the magic device registers: console, screen/palette, audio, keyboard, mouse, clock, RNG, `sys/*`; terminal vs. SDL rendering.
- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — the implementation: source → AST → loader → IR → optimizer → interpreter / QBE backend + runtime, and how to add an optimization or a device.

## Layout

```
src/                C sources               include/beancraft/   public headers
  lexer,parser,ast    front end             test/                unit + example tests
  loader              use/func expansion    scripts/bccompile    .bc -> native binary
  ir, opt             IR + optimizer        examples/            ~50 .bc programs
  dot, tally          --emit-dot graphs; tally.bc encode/decode
  interp              tree-walking VM        web/wasm_main.c +    WebAssembly build
  qbe, qbe_runtime,   native backend +          web/index.html      (`make wasm`) + demo page
  qbe_driver          its C runtime          docs/                language / devices / architecture
  devices             I/O (console/screen/audio/...)
  bignum, arena, str  pointer-tagged bignums; bump allocator; interned strings
```

This C implementation is a port of an earlier Janet implementation; the language
and example set are shared.
