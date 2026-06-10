# beancraft

**beancraft** is a tiny programming language whose only data type is the
non-negative integer and whose only operations are *increment* and
*decrement-or-branch* — a [counter machine](https://en.wikipedia.org/wiki/Counter_machine)
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

`make wasm` (with [Emscripten](https://emscripten.org/) on `PATH`) compiles the
interpreter — parser → IR → optimizer → interpreter, no QBE backend — to a
WebAssembly module. `web/index.html` is a small demo page that loads it: pick an
example or type your own counter-machine program, set register values, hit Run.
WASM won't load over `file://`, so serve the repo over HTTP:

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
| `--show-ast` / `--show-ir` / `--show-opt` | dump the AST / IR / optimized IR, then run (use `-n` to just dump) |
| `--emit-qbe` | emit QBE IL to stdout, then exit (combine with `-O`) |
| `--emit-urm` | emit the program (and registers) Gödel-encoded for `examples/urm.bc`, then exit |

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

- **ZERO** — `deb R exit self` ⟶ `R := 0`
- **TRANSFER** — `deb A exit; inc D₁…Dₙ; jmp` ⟶ `Dᵢ += A; A := 0`
- **DIVMOD** — `k` chained `deb R` plus an inc-run ⟶ `Qᵢ += R/k; goto exit[R mod k]; R := 0`
- **MULADD** — the two-transfer multiply loop ⟶ `Dᵢ += C·S + (C−1)·T; S += T; T,C := 0`
- **ISZERO** — `deb R z; inc R nz` (the "is R zero?" idiom) ⟶ `goto R==0 ? z : nz` (R unchanged)

It also threads no-op jumps (`deb R X X`, which a bare `label:` and `use`/`func`
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
the standard library (`std.bc`), and the universal machine (`urm.bc` — it runs
any beancraft program supplied as one Gödel number; see `--emit-urm`).

```console
$ ./beancraft examples/factorial.bc N=10 -O | grep Out
Out = 3628800
$ ./beancraft examples/hello.bc
Hi
$ make sdl && ./beancraft examples/life.bc      # needs SDL; press q to quit
```

## Documentation

- **[docs/LANGUAGE.md](docs/LANGUAGE.md)** — the `.bc` language reference: instructions, jump targets, register init, `use` modules, `func` definitions, the URM encoding.
- **[docs/DEVICES.md](docs/DEVICES.md)** — the magic device registers: console, screen/palette, audio, keyboard, mouse, clock, RNG, `sys/*`; terminal vs. SDL rendering.
- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — the implementation: source → AST → loader → IR → optimizer → interpreter / QBE backend + runtime, and how to add an optimization or a device.

## Layout

```
src/                C sources               include/beancraft/   public headers
  lexer,parser,ast    front end             test/                unit + example tests
  loader              use/func expansion    scripts/bccompile    .bc -> native binary
  ir, opt             IR + optimizer        examples/            ~50 .bc programs
  interp              tree-walking VM        web/wasm_main.c +    WebAssembly build
  qbe, qbe_runtime,   native backend +          web/index.html      (`make wasm`) + demo page
  qbe_driver          its C runtime          docs/                language / devices / architecture
  devices             I/O (console/screen/audio/...)
  bignum, arena, str  pointer-tagged bignums; bump allocator; interned strings
```

This C implementation is a port of an earlier Janet implementation; the language
and example set are shared.
