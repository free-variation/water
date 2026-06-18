# Superinstructions in logicforth

This document is a primer on how logicforth collapses common sequences of
floating-point operations into single dispatched ops — and how it does so
without asking the programmer to write anything unusual. By the end you should
understand:

- Why a threaded interpreter spends most of a tight float loop on dispatch
  rather than on arithmetic, and why that is the lever
- What a superinstruction is, and the four families logicforth defines
- How the compiler fuses idiomatic source into superinstructions at compile
  time, with no new syntax for the programmer to learn
- How a store (`<expr> to <var>`) fuses into the producing op as well
- How the read-modify-write of a local accumulator (`<value> acc <fop> to acc`)
  fuses the same way, collapsing the per-element cost of every reduction
- How every superinstruction is generated from a short operator list, so adding
  one is a single line that cannot fall out of sync
- Why the fused cells need care to stay safe for the garbage collector's
  body-walk, and how `see-compiled` lets you see the fused form

The variable superinstructions live in `src/c/superwords.c`; the accumulator
(local) fusion lives in `src/c/core.c`, since it reads and writes the locals
frame rather than dictionary slots. Both plug into the same compile-time hooks:
the emit-time lookback in `run_outer` and the store/accumulate checks in `p_to`,
with operand-width entries in `op_cell_count`. The aim of this document is to
make the fusion feel like an obvious bookkeeping trick rather than a special
case — because that is all it is.

---

## Part 1: Why one dispatch per scalar op is the cost

logicforth is a direct-threaded interpreter (see `docs/threading.md`). A
compiled word body is an array of cells; each cell is a handler pointer, and
running the body means a `musttail` chain of indirect calls, one per operation.
That dispatch is cheap, but it is not free: an indirect call plus the per-op
bookkeeping costs more than a single floating-point add.

For arithmetic-heavy code, that ratio is unfavorable. Consider a line that
computes one coordinate difference in a tight float loop:

```forth
x1 x2 f- to dx
```

Compiled naively, that is four dispatched ops: push the variable `x1`, push the
variable `x2`, subtract, then store into `dx` (the store is two cells but one
dispatch). Three of the four do
almost no arithmetic — they move a value or read a variable. In that loop the
variable pushes (`dovar`) and the store dominate; the actual subtraction is a
small slice.

A modern bytecode interpreter like CPython narrows this gap with a
*specializing* interpreter: it fuses and specializes opcodes at runtime so each
bytecode does more real work. logicforth has no runtime specializer and does not
compile to native code. The only lever available is to **do more per dispatch**
— to recognize, at compile time, that a run of cheap ops can be replaced by one
op that does all their work.

That replacement is a *superinstruction*.

---

## Part 2: What a superinstruction is

A superinstruction is a single primitive whose handler reads its operands inline
from the instruction stream and performs what used to be several ops.

The idea generalizes a familiar one: a compound op like `f*+` (fused
multiply-add) or `fsq` (square) does the work of two or three stack ops in one
dispatch. Superinstructions extend that to the operations a float loop spends
its time on, and crucially encode the *operands* inline.

The key fact that makes inline operands possible: a variable's value lives in
the dictionary at a fixed slot. A `variable` defined at cfa `V` keeps its current
value in `dict[V + 1]`. So instead of compiling `x1` as "dispatch `dovar`, which
reads `dict[V+1]` and pushes it," a superinstruction can hold the slot index
`V+1` as one of its own inline operand cells and read the value directly.

A two-variable subtract, then, compiles to three cells:

```
[ handler for (vvf-) ] [ x1's value slot ] [ x2's value slot ]
```

One dispatch, two direct reads, one subtraction, one push — replacing the three
ops of `x1 x2 f-`.

---

## Part 3: The four families

logicforth defines four families of float superinstruction, named by how many
variable operands they carry. `vv` means two inline variables, `vf` means one;
then `f` for float, then the operator.

| family | form | reads | does |
|:-------|:-----|:------|:-----|
| two-var binary  | `vvf+ vvf- vvf* vvf/` | two var slots          | push `a op b` |
| one-var binary  | `vf+ vf- vf* vf/`     | one var slot + stack   | `top op v` |
| unary function  | `vfsq vfneg vfsqrt vfsin vfcos vftan vftanh vfexp vflog vfabs` | one var slot | push `fn(v)` |
| fused multiply  | `vvf*- vvf*+`         | two var slots + stack  | `c - a*b` / `a*b + c` |

The two-variable forms read both operands from variables; the one-variable forms
take their other operand from the data stack (whatever the preceding code left
there). The unary functions cover the scalar math words. The fused-multiply
forms carry the `f*-`/`f*+` semantics (`c - a*b` / `a*b + c`) but read their two
trailing operands from variables.

Every one of these is a normal dictionary word with a normal name, so it can be
typed directly — but in practice nobody does, because the compiler inserts them
for you.

---

## Part 4: Auto-fusion — the compiler inserts them

The programmer writes idiomatic stack code:

```forth
x1 x2 f- to dx
dx fsq dy fsq f+ dz fsq f+ to d2
```

and the compiler rewrites the fusable runs into superinstructions as it emits
them. There is no new syntax.

The mechanism is an emit-time lookback in `run_outer`. As a colon definition
compiles, the interpreter keeps a two-deep history of what it just emitted: for
each, either "a push of variable `V`" or "a barrier." When the next token is a
float operator, it consults that history before emitting the operator plainly:

- `f-`/`f+`/`f*`/`f/` after two variable pushes → rewind the four cells of the
  two pushes and emit the `vv` form;
- the same operator after one variable push → emit the `vf` form;
- `fsq` (and the other unary functions) after one variable push → emit the `vf`
  unary form;
- `f*-`/`f*+` after two variable pushes → emit the fused `vvf*-`/`vvf*+` form.

The history resets to a barrier at every control-flow word, literal, local, and
string. This is what keeps fusion safe around branches: a fused group is always
a contiguous run with nothing jumping into its middle, so the rewind never
disturbs a branch target, and branch offsets — which are computed later, from
the post-fusion position — stay correct. No offset fixup is needed.

The rewinding only ever touches the tail just emitted, never code that some
earlier branch already points at.

---

## Part 5: Store fusion

A computed value is usually stored straight back into a variable:

```forth
x1 x2 f- to dx
```

After Part 4, the `x1 x2 f-` is already one op (`vvf-`) that pushes its result;
the `to dx` is then a second op (`(to-var)`) that pops the result and writes it.
Store fusion removes that second dispatch: the producing op writes the variable
directly.

Each superinstruction has a **store variant** carrying one extra inline operand,
the destination slot. Instead of pushing its result it writes `dict[dst]`.

The fusion happens in `p_to`. When `to <var>` compiles, it inspects the tail of
the code just emitted: if a superinstruction sits there (a three-cell form
ending at the current position, or a two-cell form), it rewinds that op and
re-emits its store variant with the destination slot appended — and skips
emitting `(to-var)` entirely.

So `x1 x2 f- to dx` ends up as a single op:

```
[ handler for (vvf-!) ] [ x1 slot ] [ x2 slot ] [ dx slot ]
```

The store variant reads all its source operands *before* it writes the
destination, so it is correct even when the destination is also a source — as in
a velocity update like `vvf*- mag vx1 to vx1`, where `vx1` is both read and
written.

---

## Part 6: Accumulator fusion

A reduction folds a sequence into a single running value:

```
acc  ←  acc ⊕ xᵢ      for each element xᵢ
```

The running value lives in a local — the natural scope-bounded mutable cell —
and the fold is written as an update inside a loop:

```forth
| acc |
0 to acc
[: |> i | i acc f+ to acc :] n i-times
acc
```

Compiled literally, the update `i acc f+ to acc` is three dispatched ops: fetch
the local `acc`, apply the operator, store the result back into `acc`. Two of
the three only move the accumulator in and out of the stack; the arithmetic is
the third. In the innermost loop of a reduction — a sum, a dot product, a mean,
any fold — that fetch-and-store is paid on every element.

Accumulator fusion collapses the read-modify-write of a local into a single op.
At emit time the compiler recognizes the shape

```
[ fetch local L ]  [ float op ]  [ store local L ]      (same L)
```

and replaces it with one instruction that reads `L` in place, combines it with
the value already on the stack, and writes the result back — no intermediate
push, no separate fetch or store dispatch. It is the local analogue of store
fusion (Part 5): store fusion folds a write into a *global* variable;
accumulator fusion folds a read-modify-write of a *local*.

```
i acc f+ to acc      →      [ handler for (acc+) ] [ acc depth ] [ acc slot ]
```

The fused op computes `acc ← value ⊕ acc`, preserving operand order: the value
on the stack is the left operand and the accumulator the right, exactly as the
unfused `value acc op` sequence computed it. So the non-commutative operators
stay correct — `f-` yields `value − acc`, `f/` yields `value ÷ acc`.

**Only the monomorphic float operators fuse.** The fused op works on raw doubles
in place. The polymorphic operators (`+`, `*`, …) dispatch on tag and may act on
matrices or other types, so folding one into a float read-modify-write would be
unsound. Fusion fires only for the `f`-prefixed forms (`f+ f- f* f/`), which are
float-by-contract.

**Depth.** A local is addressed by a (scope depth, slot) pair. The characteristic
case is the one above: the accumulator is declared in an enclosing scope and
updated inside a loop quotation, so it is reached at depth ≥ 1, and the fused op
carries the depth alongside the slot. An accumulator updated in the same scope
it was declared is the depth-0 variant of the same op.

This is the interpreter form of a familiar compiler transform — turning
`x = x ⊕ e` into a single read-modify-write of `x`'s storage rather than a load,
a compute, and a store. In a dispatch-bound interpreter the gain is concrete and
universal: two fewer dispatches per element in the hottest loop of every
reduction.

Like the other inline-operand superinstructions, the fused op carries its
operands — the slot, and the depth for the nested form — in the instruction
stream, so the structure-walk the garbage collector and the decompiler rely on
must know its width (Part 8).

---

## Part 7: One list, every touch-point

A superinstruction is not one piece of code; it is six. Each needs:

1. a runtime handler,
2. a store-variant handler,
3. a compile-time word (so it can be typed),
4. registration in the dictionary,
5. an entry in `op_cell_count` so the body-walk knows its width (Part 8),
6. a line in the fuse logic mapping a base operator to the superinstruction.

Maintaining six parallel lists by hand is how a subtle bug enters: forget the
`op_cell_count` entry and the garbage collector silently mis-walks the body.
logicforth avoids that by deriving all six from a single operator list per
family, using X-macros:

```c
#define FLOAT_BINOPS(X) \
	X(add, +, p_add_f) \
	X(sub, -, p_sub_f) \
	X(mul, *, p_mul_f) \
	X(div, /, p_div_f)
```

The operator token does triple duty: it is the C operator in the handler
(`a.number op b.number`), it stringizes into the word name (`"vvf" #op` →
`"vvf+"`), and the third argument names the base handler the fuse logic keys on.
A template macro expands the list once per purpose — handler, compile word,
registration, cell count, fuse mapping, store variant — so the six parts cannot
drift apart. Adding `vfcbrt` or a new operator is one row, fully wired.

There are three lists: `FLOAT_BINOPS` (which drives both the `vv` and `vf`
binary forms), `FLOAT_UNARY_FNS` (the unary functions), and `FLOAT_FUSED` (the
fused multiplies).

---

## Part 8: Keeping the body-walk correct

A compiled body is a flat array of cells, and nothing in a cell marks it as a
handler versus an inline operand. Code that walks a body — the garbage
collector's `mark_body`, looking for object references baked into the body, and
`inline_word_body`, copying a word for inlining — reconstructs the structure by
knowing, for each handler, how many operand cells follow it. That knowledge lives
in one function, `op_cell_count`.

Superinstructions carry inline operand cells (one to three of them), so
`op_cell_count` must report their true widths, or the walk desynchronizes the
moment it crosses one. `op_cell_count` asks `superword_cell_count` first, which
returns the width of any superinstruction (3 for a two-var push, 2 for a one-var
push, 4 for a two-var store, and so on) and 0 for anything else.

This works because each superinstruction has a *unique* handler and a *fixed*
width. That is worth contrasting with `docol`, the colon-word entry handler,
which deliberately is *not* in `op_cell_count`: a `docol` cell is two cells when
it is a call to another word but one cell when it marks the start of an embedded
quotation, and the two are indistinguishable by their handler pointer. The
body-walk tolerates that ambiguity for `docol` because its stray operand is a
small integer that never collides with the object-bearing handlers `mark_body`
acts on. Superinstructions have no such ambiguity, so adding them to
`op_cell_count` is both necessary and safe.

---

## Part 9: Seeing the fused form — `see-compiled`

Because fusion happens silently, `see` does not reveal it: `see` reprints a
word's stored source text, so it shows exactly what the programmer typed —
`x1 x2 f- to dx`. That is usually what you want, but it does not tell you whether
fusion fired.

`see-compiled` decompiles the actual cell body instead. It walks the body with
`op_cell_count` (so it agrees with the GC about where each op begins), prints
`docol`/`dovar`/`dosym` calls by their target's name, literals by value, and
superinstructions by name with their variable operands resolved back to names:

```
: pair-step   \ ...
   0: (vvf-!) x1 x2 dx
   4: (vfsq) dx
   ...
```

That is the tool for confirming a hot word fused the way you expected.

---

## Part 10: Where fusion pays off

Fusion removes dispatch, so the gain tracks how much of a loop is dispatch
versus real work.

- In a tight float loop over variables, the variable-push dispatch (`dovar`) is
  the single largest cost, and fusing the arithmetic folds those pushes into the
  operator op. Store fusion further removes the per-element writebacks.
- A loop with a tiny body and a store per iteration — `X negate to X`,
  `PI f+ to PI` — folds each store into a store variant, removing a
  push-and-store round trip from every iteration.
- Loops written with `begin`/`until` over scalar locals are unaffected: fusion
  keys on variable pushes and float operators, not on local fetches or
  hand-rolled loop control.

A tight loop of trivial float ops over variables gains the most; a loop
dominated by array indexing, branching, or genuine arithmetic gains little,
because there is less dispatch to remove.

---

## Part 11: Where to look in the source

- **`src/c/superwords.c`** — the variable superinstructions: the three operator
  lists, the template macros that generate the handlers / compile words / store
  variants, `superword_cell_count`, `superword_try_fuse` (the auto-fusion logic),
  and `superword_try_fuse_store` (the store-fusion logic).
- **`src/c/core.c`** — the accumulator-fusion ops (a macro over the four float
  operators generating the depth-0 and nested read-modify-write handlers) and
  `try_fuse_local_acc`, the match-and-rewind for `<value> acc <fop> to acc`.
  These live here, not in `superwords.c`, because they read and write the locals
  frame rather than dictionary slots.
- **`run_outer`** in `src/c/core.c` — the emit-time lookback that maintains the
  variable-push history and calls `superword_try_fuse` before emitting a plain
  operator.
- **`p_to`** in `src/c/words.c` — calls `superword_try_fuse_store` (variable
  stores) and `try_fuse_local_acc` (local accumulators) before emitting a plain
  store.
- **`op_cell_count`** in `src/c/core.c` — defers to `superword_cell_count` so
  the GC body-walk and the inliner step over superinstructions correctly.
- **`p_see_compiled`** in `src/c/core.c` — the `see-compiled` decompiler.

For broader context:

- **`docs/threading.md`** — the dispatch model these ops live in, and the
  trampoline `execute_cfa` uses.
- **`docs/gc.md`** — the body-walk (`mark_body`) that `op_cell_count` serves.
