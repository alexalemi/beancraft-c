# Architecture

This is the implementation guide — the source-level companion to
[LANGUAGE.md](LANGUAGE.md) (what the language *is*) and [DEVICES.md](DEVICES.md)
(the I/O surface). It walks the pipeline a `.bc` file goes through, the data
structures at each stage, and where to plug in a new optimization or device.

## The pipeline

```
 source text
   │  src/lexer.c  +  src/parser.c
   ▼
 AST  (src/ast.h)                       inc / deb / end / use / funcdef / call nodes
   │  src/loader.c   — loader_expand()
   ▼
 AST  (flattened)                       only inc / deb / end remain; use & func gone
   │  src/ir.c       — ir_from_ast()
   ▼
 IrProgram  (src/ir.h)                  a counter machine: IrInst[] {op, reg, arg_a, arg_b}
   │  src/opt.c      — ir_optimize(level)
   ▼
 IrOptProgram  (include/beancraft/opt.h)  IrOptInst[]; at -O, loops folded to O(1) ops
   │
   ├── src/interp.c   — tree-walking VM      ./beancraft file.bc
   │
   └── src/qbe.c      — emit QBE IL          ./beancraft file.bc --emit-qbe
          │  qbe(1)  then  cc(1)
          ▼
       native binary  +  src/qbe_runtime.c (the bc_* shims)  +  src/qbe_driver.c (its main)
```

Everything in the front end is allocated from a single bump **`Arena`**
(`src/arena.c`) and freed in one shot; strings are interned in a **`StrPool`**
(`src/str.c`) so identifiers, labels, and module paths compare by pointer.

`src/main.c` is the CLI: parse args, run the pipeline, optionally dump
`--show-ast` / `--show-ir` / `--show-opt`, then either interpret or `--emit-qbe`.

## Stage 1 — lexer + parser → AST

`src/lexer.c` is a hand-written scanner. Notable rules: `#` to end-of-line is a
comment; `inc`/`deb`/`end`/`use` have the aliases `+`/`-`/`.`/`%`; a `+` or `-`
*immediately* before a digit is a signed number (a relative jump offset),
otherwise it's the `inc`/`deb` token; identifier characters include `-` and `/`
so device names like `con/byte` and scoped names like `copy-0/tmp` lex as one
token. Newlines are real tokens (statements are line-oriented).

`src/parser.c` is a small recursive-descent parser producing `Ast` — a flat
array of `AstNode`s, one per source statement, each optionally carrying a
`label`. Node kinds: `AST_INC`, `AST_DEB`, `AST_END`, `AST_USE`, `AST_FUNCDEF`,
`AST_CALL`. Jump targets are kept symbolic at this stage (`JUMP_LABEL` /
`JUMP_KEYWORD` / `JUMP_OFFSET` / `JUMP_NONE`). Two small desugarings happen here:
a `label:` alone on a line becomes a fall-through no-op (`deb :nil next next`),
and a `label:` at end of file/`func`-body becomes an `AST_END` (so you can name
your exit point).

## Stage 2 — the loader: `use` and `func` expansion

`src/loader.c`'s `loader_expand()` turns the AST into a single flat program with
no `use`, `func`, or call nodes:

- **`func` definitions** are collected into a table (`FuncEntry` list in
  `LoaderContext`) and dropped from the program — they generate no code on their
  own. A `use`d module's `func`s are registered too, so importing a module makes
  its subroutines callable (`use "std"` → `clear`, `copy`, `addn`, …).
- **`use "file" [:scope] [mappings]`** resolves the path (current dir relative to
  the importer, then CWD, then `BEANCRAFT_PATH`), parses+caches the module, and
  inlines a fresh copy: every private register/label is renamed `scope/name`
  (auto-scope `file-N` if no `:scope` given); `local=caller_reg` mappings rename
  to the caller's register instead (shared storage); `local=N` mappings make a
  private register that's re-seeded to `N` on entry; `local~caller_label`
  mappings redirect a label. The inlined copy is bracketed by two labelled
  no-ops — an *entry* point (target of `init` inside the body, and of any label
  on the `use` statement) and a *return* point (target of `done`/`halt` and the
  body's own `end`).
- **`name arg ...` calls** bind each argument to a parameter (a register param
  gets a register-alias or a literal-value mapping; a `~`-param gets a label),
  then inline the `func` body exactly like a `use`.
- Expansion is iterated to a fixpoint: a `func` body that itself `use`s or calls
  is handled by re-running `loader_expand` on the partially-expanded result
  (capped at `MAX_USE_DEPTH = 32`).

Device registers are exempt from all renaming — `con/byte` is always `con/byte`.

## Stage 3 — lowering to IR: `ir_from_ast`

`src/ir.c` turns the flat AST into an `IrProgram` (`src/ir.h`):

- one `IrInst` per node — `op ∈ {IR_INC, IR_DEB, IR_END}`, a `reg` index, and
  `arg_a`/`arg_b` (for `IR_INC`: `arg_a` = next instruction; for `IR_DEB`:
  `arg_a` = jump-if-zero target, `arg_b` = next-after-decrement target);
- register names get small integer indices (`reg_names[]`, `reg_init[]`);
- symbolic jump targets are resolved to instruction indices: `self`→i,
  `next`→i+1, `prev`→i-1, `init`→0, `done`/`halt`→the last instruction, a label
  to its instruction, an offset `N`→i+N, omitted→i+1;
- an implicit `IR_END` is appended (so falling off the end halts);
- for each device register used, its implicit data-register dependencies are
  created (`con/emit` ⇒ `con/byte`, `screen/plot` ⇒ `screen/{x,y,color}`, …);
- the internal `:nil` register is added (used by the no-op desugarings);
- a `label_names[]` table (instruction → label) is kept for `--show-ir`/`--show-opt`.

This `IrProgram` is the actual counter machine. `--show-ir` prints it.

## Stage 4 — the optimizer: `ir_optimize`

`src/opt.c` produces an `IrOptProgram` (`include/beancraft/opt.h`). The extended
instruction set adds, on top of `INC`/`DEB`/`END`, four O(1) folds: `ZERO`,
`TRANSFER`, `DIVMOD`, `MULADD`. An `IrOptInst` carries `reg`, `arg_a`, `arg_b`,
and `dest_off`/`dest_count` indexing a packed `dests[]` pool that holds
TRANSFER's destination registers / DIVMOD's quotient registers + per-remainder
exit indices / MULADD's `[S, T, D₁…Dₘ]`.

At **`OPT_NONE`** (no `-O`): a faithful 1:1 lowering — each `IrInst` becomes the
corresponding `IR_OPT_INC`/`IR_OPT_DEB`/`IR_OPT_END`. (This is why `--show-ir`
and `--show-opt` look the same without `-O`.)

At **`OPT_LOOPS`** (`-O` / `--show-opt`), three passes:

1. **Detect.** For each not-yet-consumed instruction `i`, `ir_detect_pattern`
   tries, in order, `detect_muladd_pattern` → `detect_transfer_pattern` →
   `detect_divmod_pattern` → `detect_zero_pattern`. A match that doesn't touch a
   device register (those have side effects on `inc`/`deb`, so their loops are
   left alone) is recorded and the instructions it covers, `[start, end)`, are
   marked consumed. Helpers: `is_deb`/`is_inc` (op check), `collect_inc_run`
   (a contiguous `inc`-chain looping back to a given instruction), and
   `body_is_private` (no jump from *outside* the loop lands in its interior, and
   no exit target points strictly inside it — both would dangle once folded).
2. **Emit.** Walk the instructions; at a pattern's `start` emit the folded
   `IrOptInst` (packing its dest registers / exit indices into `dests[]`),
   recording `inst_map[old] = new`; for non-consumed instructions, copy 1:1.
3. **Remap.** Rewrite jump targets through `inst_map`. DIVMOD's per-remainder
   exit indices live in the `dests[]` pool and are remapped there; TRANSFER/MULADD
   dests are *register* indices and are left alone.

### The patterns

| pattern | shape (raw IR) | folds to |
| --- | --- | --- |
| **ZERO** | `deb R exit self` | `R := 0; goto exit` |
| **TRANSFER** | `deb A exit; inc D₁; …; inc Dₙ; jmp deb` | `Dᵢ += A; A := 0; goto exit` |
| **DIVMOD** | `deb R e₀; deb R e₁; …; deb R e_{k-1}; inc Q₁; …; inc Qₘ; jmp deb`  (k ≥ 2, m ≥ 0) | `Qᵢ += ⌊R/k⌋;  goto e_{R mod k};  R := 0` |
| **MULADD** | `deb C exit; [deb T self;] deb S tx; inc D₁…Dₘ; inc T (→deb S); deb T (→deb C); inc S (→deb T)` | `if C≠0: Dᵢ += C·S + (C−1)·T;  S += T;  T := 0;  C := 0;  goto exit` |

MULADD is the multiply idiom — `for C { [T:=0;] TRANSFER S→{D…,T}; TRANSFER T→{S} }`
— so `Out = A*B` stops being O(A·B). Constraints: the loop body is one contiguous
block, `{C, S, T, D₁…Dₘ}` are pairwise distinct, fan-out ≤ `IR_OPT_MAX_DESTS`
(32), `body_is_private` holds, and the optional leading `deb T self` (a per-round
`T := 0`) sets the "preclear" flag — then `T` is provably 0 and the `(C−1)·T` /
`S += T` terms drop. The `(C−1)·T` term exists because `TRANSFER T→{S}` copies
`T+S` back into `S` each round; if `T` is junk at loop entry it gets folded in
`C−1` times. C==0 must be special-cased (the `(C−1)` would underflow).

If any constraint fails the fold simply doesn't fire and the inner loops still
fold to TRANSFERs on their own — every pattern degrades gracefully.

## Stage 5a — the interpreter

`src/interp.c` walks the `IrOptProgram` directly. `interp_step` is a switch on
the op: `IR_OPT_INC`/`IR_OPT_DEB` consult `inc_mask`/`deb_mask` and call
`device_on_inc`/`device_on_deb` for device registers, otherwise `bignum_inc`/
`bignum_dec`; the folded ops do the arithmetic with `bignum_add_into`,
`bignum_divmod_small`, `bignum_mul`. `interp_run(state, max_steps)` loops until
the program halts or the step cap is hit; `interp_init_regs` applies `reg_init`
and `interp_set_reg` handles `REG=VALUE`. `interp_cleanup` frees any heap
bignums before the arena goes away (the registers' bignums are `malloc`'d, not
arena-owned).

## Stage 5b — the QBE backend

`src/qbe.c`'s `qbe_generate_opt` writes [QBE](https://c9x.me/compile/) IL: a
global `data $bc_regs` (one pointer-tagged `Bignum` word per register), a name
table, small numeric accessors, and one function `$bc_run(max_steps)` whose body
is a basic block `@inst_<i>` per IR instruction. Each block bumps a step
counter, bails to `@exit` on the cap, then: `INC`/`DEB` call `$bc_inc`/`$bc_dec`
(or `$bc_dev_inc`/`$bc_dev_deb` for device regs); `ZERO`/`TRANSFER`/`DIVMOD`/
`MULADD` call `$bc_zero`/`$bc_add_into`/`$bc_divmod`/`$bc_muladd`. MULADD's
variable-length accumulator list is emitted as a top-level `data $bc_muladd_d<i>`
array of register indices (QBE only allows `alloc` in the entry block, so a
per-instruction stack array isn't possible); a fan-out of zero passes a null
pointer.

Those `$bc_*` functions are in `src/qbe_runtime.c` — thin wrappers over the
**same `src/bignum.c`** the interpreter uses, which is what guarantees a compiled
program and the interpreter agree on every result. `src/qbe_driver.c` is the
`main()` for compiled binaries: it parses `REG=VALUE` args, calls `device_init`,
calls `bc_run`, and prints the registers — exactly like the interpreter's CLI.

`scripts/bccompile` runs `beancraft --emit-qbe [-O]` → `qbe` → `cc`, compiling
`qbe_runtime.c` / `bignum.c` / `devices.c` / `qbe_driver.c` alongside the
generated assembly. `make libbcruntime.a` bundles the runtime as a static lib if
you'd rather link by hand (`qbe_link`).

## Devices

`src/devices.c` has a static `MAGIC[]` catalogue (name → `DEV_*` op). At startup
`device_init` scans the program's register-name table, builds `inc_mask` /
`deb_mask` / `op_of` arrays plus a cache of the indices it'll need, and — if any
`screen/*`, `kbd/*`, `mouse/*`, or `audio/*` register appears — brings up the
relevant backend (SDL window if built with `-DBC_SDL`, otherwise the terminal
alternate-screen renderer / raw-stdin keyboard), registering an `atexit` to put
the terminal back. `device_on_inc` and `device_on_deb` are the side-effect
dispatchers. The interpreter and the QBE runtime both link this file, so devices
behave identically whether interpreted or compiled. See [DEVICES.md](DEVICES.md)
for the register reference.

## The bignum representation

`include/beancraft/bignum.h` + `src/bignum.c`. A `Bignum` is a 64-bit word with
a tag in the low bit:

- **LSB = 1** → an *immediate*: the value is `x >> 1` (so up to ~4.6×10¹⁸ fits
  inline, no allocation — `inc`/`dec` are branch-free header functions). Zero is
  the word `1` (`0 << 1 | 1`), **not** `0` — worth remembering when reading the
  QBE code.
- **LSB = 0** → a pointer to a heap `BigLimbs { uint32_t len, cap; uint64_t limbs[] }`,
  a base-2⁶⁴ little-endian magnitude. Operations promote to heap on overflow and
  demote back when a result fits; `inc`, `dec`, `add`, `add_into`, `mul`,
  `divmod_small`, compare, and decimal `to_string`/`from_string` are all
  provided. There's no subtraction below zero (`dec` of `0` is a no-op) and no
  signedness — beancraft naturals only.

## Tests

- `make test` builds everything with `-fsanitize=address,undefined` and runs:
  - `test/test_parser.c` — lexer/parser/AST cases;
  - `test/test_bignum.c` — `inc`/`dec`/`add`/`mul`/compare/string round-trips,
    including overflow-to-heap and big-number cases;
  - `test/test_opt.c` — the optimizer: pattern detection (incl. the MULADD
    preclear variant), graceful degradation on near-misses, and — the gold
    standard for an optimizer — *the `OPT_NONE` result equals the `OPT_LOOPS`
    result for the same inputs* (plus closed-form spot checks);
  - then `test/run_examples.sh` (also runnable standalone as
    `bash test/run_examples.sh ./beancraft`): ~50 example-program checks at
    `-O0` and `-O`, plus a block that — if `qbe` and `scripts/bccompile` are
    available — compiles a few programs and runs the binaries.

## Adding things

**A new optimizer fold.** Add a `PATTERN_*` and `IR_OPT_*` to `opt.h`; write a
`detect_*_pattern` in `opt.c` (reuse `is_deb`/`is_inc`/`collect_inc_run`/
`body_is_private`) and add it to `ir_detect_pattern`'s cascade; add an emission
case to pass 2 (pack any operands into `dests[]`) and a remap case to pass 3 (if
it has jump-target operands); add an interpreter case to `interp.c` and a QBE
case to `qbe.c` (plus a shim in `qbe_runtime.c` if it needs runtime help).
Cover it in `test/test_opt.c` — at minimum a detection assertion, a couple of
near-misses that must *not* fold, and `OPT_NONE == OPT_LOOPS` over a range of
inputs. The MULADD fold (git: `d8333b2`, `6fe6635`) is the worked example.

**A new device register.** Add it to `MAGIC[]` in `devices.c` (as `DEV_DATA` for
plain storage, or a fresh `DEV_*` op for a trigger; add it to the
`op_is_inc_trigger`/`op_is_deb_poll` range as appropriate). List its data-register
dependencies in `device_dependencies`. Handle the op in `device_on_inc` /
`device_on_deb`. If it needs a backend, wire it into `device_init` /
`device_shutdown`. It works in both the interpreter and compiled binaries
automatically — both link `devices.c`. Document it in [DEVICES.md](DEVICES.md).
