# The beancraft language

beancraft is a [counter machine](https://en.wikipedia.org/wiki/Counter_machine):
a finite list of instructions, a set of registers each holding a non-negative
integer (arbitrary precision — they're bignums), and a program counter. There
are exactly two real instructions — *increment* and *decrement-or-branch* — plus
a *halt*. Everything else (loops, conditionals, copy, add, multiply, …) is built
out of those, which is the whole point.

On top of that core the language adds three conveniences that disappear before
execution: **labels** (symbolic jump targets), **`use`** (textual module
inclusion with renaming), and **`func`** (parameterized inline subroutines).
None of them add expressive power; they make programs writable.

## Lexical structure

- One statement per line. Blank lines are ignored.
- `#` starts a comment that runs to end of line.
- Whitespace (spaces, tabs, `\r`, `\f`, `\v`) separates tokens and is otherwise
  insignificant. Newlines are significant (they end statements).
- **Identifiers** start with a letter or `_` and may then contain letters,
  digits, `_`, `-`, and `/`. The `/` is what lets device registers be written
  `con/byte`, `screen/x`, `mouse/buttons`; the `-` shows up in auto-generated
  scoped names like `copy-0/tmp`.
- **Numbers** are decimal, optionally signed (`5`, `+1`, `-2`). A `+` or `-`
  *immediately followed by a digit* is a number; otherwise `+` and `-` are the
  `inc` / `deb` instructions.
- **Strings** are `"…"` (used only as `use` filenames; no escapes, no newlines).
- Instruction words have one-character aliases: `inc` = `+`, `deb` = `-`,
  `end` = `.`, `use` = `%`. `func` has no alias.
- **Reserved jump words**: `self`, `next`, `prev`, `init`, `done`, `halt`. (They
  are only special as jump targets; you can still use them as register names.)

## Registers

A register is named by an identifier and holds a non-negative integer. Every
register a program mentions is created automatically, initialized to `0`. There
is no declaration, no scope (within one program), no type other than "natural
number" — and no fixed width: a register that overflows 63 bits silently spills
to a heap bignum, so `2^1000` is fine, `0 - 1` is just `0` (decrement of zero
does nothing).

Set initial values from the command line:

```console
$ ./beancraft examples/add.bc A=10 B=5
Results:
A = 0
Out = 15
B = 0
```

When the program halts, every named register's final value is printed under a
`Results:` header (registers whose name begins with `:` are internal and
hidden). A program that uses an I/O device prints just its output instead.
`--quiet` suppresses the dump; `--list-regs` shows the register set without
running.

Some register *names* are **device registers** — touching them has side effects
(printing a byte, plotting a pixel, reading the clock, …). They behave like
ordinary registers to the language but are intercepted by the runtime. See
[DEVICES.md](DEVICES.md). They are never affected by `use`/`func` renaming —
`con/byte` always means the console's byte register.

## Instructions

Execution starts at instruction `0` and proceeds until a `halt` is reached (or
control falls off the end — there is always an implicit `halt` appended). There
is no notion of "calling" or "returning" at the instruction level; there is only
*goto*.

### `inc R [target]` — increment

Add 1 to register `R`, then jump to `target`. `target` defaults to the next
instruction.

```bc
+ counter        # counter++; fall through
+ counter loop   # counter++; goto loop
inc Out          # 'inc' spelled out
```

### `deb R target_zero [target_nonzero]` — decrement-or-branch

If `R` is `0`: jump to `target_zero` (R is left at 0).
If `R` is `> 0`: subtract 1 from `R`, then jump to `target_nonzero`.
`target_zero` is **required**; `target_nonzero` defaults to the next instruction.

This single instruction is the language's only conditional, only loop primitive,
and only subtraction:

```bc
- N done         # if N==0 goto done, else N--, fall through   (a countdown loop)
- X clear self   # while X>0: X--    -- i.e. X := 0   (jump back to self)
- A neg pos      # branch on whether A was zero (consuming one from A if not)
```

### `end` (or `.`) — halt

Stop the machine. A bare `label:` at the very end of a file (or `func` body)
also acts as a halt, which lets you give your exit point a name:

```bc
loop: - N done
+ Out loop
done:            # <- the program halts here; 'done' is a valid jump target
```

### `use` and `func`

See the [Modules](#modules-use) and [Functions](#functions-func) sections; both
are removed by the loader before the program becomes a counter machine.

## Jump targets

Anywhere an instruction takes a `target`:

| target | goes to |
| --- | --- |
| *(omitted)* | the next instruction (`inc` always; `deb`'s second target) |
| `name` | the instruction labelled `name:` |
| `self` | this instruction (re-execute it — the standard "loop until zero" idiom) |
| `next` | the next instruction (explicit form of omitting) |
| `prev` | the previous instruction |
| `init` | instruction `0` (the program start; inside a `use`/`func` body, the inclusion's entry point) |
| `done`, `halt` | the program's halt instruction (inside a `use`/`func` body, the inclusion's return point) |
| `N` (a number) | a **relative** offset: this instruction's index + N. So `0` = self, `1` = next, `-1` = prev, `+3` = three forward. Rarely used directly; labels are clearer. |

### Labels

Prefix any statement with `label:`. A label on its own line is a no-op that
falls through to the next statement — that's how `loop:` written on its own line
works:

```bc
loop:                # <- a labelled no-op
  - N done
  + Out loop
```

A label may appear on the same line as its statement too (`loop: - N done`).

## Putting it together

A few canonical idioms:

```bc
# X := 0
- X done self

# To += From, From preserved   (needs a scratch register tmp, initially 0)
- tmp loop self                # make sure tmp is 0
loop:    - From restore
         + To
         + tmp loop            # From -> {To, tmp}
restore: - tmp done
         + From restore        # tmp -> {From}

# Zero := (N == 0 ? 1 : 0)   (examples/iszero.bc; N consumed)
clr:   - Zero check self       # Zero := 0
check: - N setZero             # N==0 -> goto setZero;  N>0 -> N--, fall through
       + N done                #   (N>0) put it back and halt with Zero==0
setZero: + Zero done           #   (N==0) Zero := 1
```

The `examples/` directory and `examples/std.bc` (the standard library) are the
best reference for real code; start with `add.bc`, `copy.bc`, `mul.bc`,
`div.bc`, `iseven.bc`.

## Modules: `use`

```
use "file" [: scope] [ mapping ... ]
```

`use "foo"` finds `foo.bc` (relative to the current file, then the current
directory, then each entry of the `BEANCRAFT_PATH` environment variable), and
**inlines a copy of it** at that point. The copy's private registers and labels
are renamed so they can't collide with the importer's; `mapping`s override that
renaming to plug the module into the caller. A module is parsed once and cached,
but each `use` of it produces a fresh inlined copy.

`scope` (`use "foo":bar`) is the prefix used for the module's private names
(`bar/tmp`, `bar/loop`, …). Without it, an automatic scope `foo-N` is used.

A `mapping` is one of:

| form | meaning |
| --- | --- |
| `local = caller_reg` | inside the module, the register written `local` *is* the caller's register `caller_reg` — they share storage. The way you pass data in and out. |
| `local = N` (a literal) | `local` is a **private** register, but it is set to `N` every time control enters this inclusion (so a module can take "value parameters"). |
| `local ~ caller_label` | inside the module, a jump to `local` goes to the caller's label `caller_label`. |

Device registers (`con/byte`, `screen/x`, …) are global — they are never scoped
and never need a mapping.

`init` inside a module body jumps to that inclusion's *entry* point, not the
program's instruction 0; `done`/`halt` (and the module's own `end`) jump to its
*return* point — the instruction right after the inlined copy. A `use` statement
may itself carry a label, which is placed on the entry point, so external code
can jump in. `examples/mul_with_use.bc` multiplies by copying `A` into `Out` `B`
times, using the library `copy` module each round:

```bc
init: - B clr               # B times:
use "copy" From=A To=Out     #   Out += A   (copy adds From into To... see examples/copy.bc)
- nil init self              #   loop  (nil is 0, so `- nil init self` is just `goto init`)
clr:  - A halt self          # B exhausted: clear A and halt
```

## Functions: `func`

```
func name [~]param ... { statement* }
```

A `func` is a named, parameterized body that is **inlined at every call site**.
Definitions are **top-level only**. A bare `param` is a *register parameter*; a
`~param` is a *label parameter*. The body uses `done`/`halt` to "return" (jump
to just after the inlined copy) and may reference its own labels and private
registers freely — they're scoped per call.

Call it by writing its name followed by arguments:

```
name arg ...
```

- a register parameter takes either **another register name** (the call shares
  that register) or a **literal integer** (a fresh private register seeded to
  that value each time the call is entered);
- a label parameter takes a **label name**.

```bc
func addn N R {        # R += N   (N is a value argument, consumed)
  loop: - N done
  + R loop
}

addn 72 con/byte       # con/byte += 72   ('H')
inc con/emit           # print it
addn 105 con/byte      # con/byte += 105  ('i')
inc con/emit
```

A `use`d module that contains `func` definitions registers them so the importer
(and anything *it* pulls in) can call them — `use "std"` brings in `clear`,
`copy`, `addr`, `addn`, `subn`, `half`, `iseq`, `inrange`, `note`, … without
generating any code of its own. Functions can `use` other modules and call other
functions; the loader keeps expanding until nothing is left but `inc`/`deb`/`end`.

See `examples/std.bc` for the standard library, and `examples/hello.bc`,
`examples/dayOfWeek-refactored.bc`, `examples/urm.bc` for `func` in anger.

## Running and inspecting

```console
$ ./beancraft file.bc [REG=VALUE ...] [options]
```

- `--show-ast` — the parsed AST (before module/func expansion).
- `--show-ir` — the lowered counter machine (after expansion, labels resolved to
  instruction indices). This is what actually runs.
- `--show-opt` — the optimized IR (implies `-O`); shows which loops folded into
  `ZERO` / `TRANSFER` / `DIVMOD` / `MULADD` ops.
- `-O` — run with those folds enabled (much faster for arithmetic-heavy code).
- `-s N` — cap execution at N steps (default 10 million; a program that uses an
  I/O device runs uncapped unless you pass `-s`).
- `-v` — also print steps executed and elapsed time.
- `--emit-qbe` (+ `-O`) — emit [QBE](https://c9x.me/compile/) IL; `scripts/bccompile`
  wraps `beancraft --emit-qbe | qbe | cc` into one command.
- `--emit-urm` — print the program (and, if you pass `REG=VALUE` args, the
  initial registers) Gödel-encoded for `examples/urm.bc` (the universal machine;
  see below).

## The universal register machine

`examples/urm.bc` is a universal register machine: it runs *another* beancraft
program supplied as data. The simulated machine has a `t = 0/1/2`-tagged
instruction set and any number of registers `s0, s1, …`:

| `t` | meaning |
| --- | --- |
| `0` | `inc s_r; PC := g1` |
| `1` | `if s_r == 0 then PC := g1 else (s_r--; PC := g2)` |
| `2` | halt |

Both the simulated **program** and its **registers** are passed as single
integers, Gödel-encoded with the pairing code:

- `pair(x, y) = 2^x · (2y + 1)` — always ≥ 1, so `0` unambiguously means the
  empty list;
- `list([]) = 0`, `list(h :: t) = pair(h, list(t))`;
- the program is `list([t0, r0, g1_0, g2_0, t1, r1, g1_1, g2_1, …])` — four list
  elements per instruction;
- the registers are `list([v0, v1, v2, …])` (a missing tail is treated as zeros).

So there's no fixed register count or instruction count: register `r` is "the
`r`-th element of the register integer," and `urm.bc` accesses it by walking the
list (pop `r` elements onto a scratch stack, touch element `r`, push them back).
Building and walking those numbers is all `inc`/`deb` loops — so `urm.bc` is
exponential-time without `-O` (a single list pop is exponential in the encoded
number's bit length) and merely *very slow* with it (every list op folds to an
O(1) bignum op, but each simulated `inc`/`deb` still walks the register list).

`beancraft --emit-urm file.bc [REG=VALUE...]` prints the encoding — `P=…` (the
program) and, if you pass register values, `R=…` (the initial registers).
Registers map in order to `s0, s1, …`. At halt `urm.bc` unpacks the first eight
simulated registers into `out0..out7` so the result is readable:

```console
$ ./beancraft --emit-urm examples/iseven.bc N=7
# examples/iseven.bc -> urm.bc encoding.  registers: Even=s0 N=s1 :nil=s2
P=68125900126858486 R=769

$ ./beancraft examples/urm.bc -O $(./beancraft --emit-urm examples/iseven.bc N=7 | tail -1)
out0 = 0     # Even  (7 is odd)
out1 = 0     # N     (consumed)
...

$ ./beancraft examples/urm.bc -O $(./beancraft --emit-urm examples/mul.bc A=2 B=3 | tail -1)
out0 = 0     # tmp
out1 = 6     # Out  (= 2 * 3)
out2 = 0     # B
out3 = 2     # A
...
```
