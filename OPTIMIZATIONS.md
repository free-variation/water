# Mini-plan: post-DTC interpreter optimizations

Combined target: ~25-30% on the dispatch-heavy phases of `bench/synth.l4`,
on top of the gain already obtained from direct threading. Each step is
independently testable; the system runs cleanly after every step. Order
is roughly easiest-first.

Reference profile: run `./logicforth < bench/profile.l4` under `sample`
(macOS) or `perf record` (Linux); refresh the per-primitive ranking
before committing to candidate selection beyond what's listed here.

---

## Step 1 — Float fast-path on `lt` / `gt` / `eq`

Inline the `T_FLOAT` / `T_FLOAT` case at the top of `p_lt`, `p_gt`,
`p_eq` so the common numeric case skips the call to `val_cmp` and its
tag-and-kind switch.

- **Files**: `src/c/words.c`
- **Form**: each primitive becomes
  ```c
  POP(right);
  POP(left);
  if (left.tag == T_FLOAT && right.tag == T_FLOAT)
      push(interp, make_bool(unpack_float(left) <op> unpack_float(right)));
  else
      push(interp, make_bool(val_cmp(interp, left, right) <op> 0));
  ```
- **LOC**: ~20 lines total across three primitives
- **Expected gain**: 3-5% on numeric loops. `val_cmp` accounts for
  ~3% of CPU across `p_lt` / `p_gt` / `p_eq` per the current profile.
- **Risk**: ~zero. Pure local optimization, behaviour preserved by the
  fallback branch.

---

## Step 2 — Inline frame access helpers

Move `frame_find` and `frame_walk` from `collections.c` to
`logicforth.h` as `static inline`. The compiler can then integrate them
into `p_frame_get` / `p_frame_set` (and the upcoming `(@k)` / `(!k)`).

- **Files**: declarations in `logicforth.h`, definitions move from
  `collections.c` to the header.
- **LOC**: ~50 lines relocated, no logic changes.
- **Expected gain**: ~2 ns per frame access from removing the
  function-call overhead. Small but real on phases 4 and 6 of the bench.
- **Risk**: minimal. Pure code organization.

---

## Step 3 — Peephole framework

Shared utility for the super-instructions to use. Two helpers:

```c
static int  peephole_can_rewind(Interpreter *interp, int cells);
static void peephole_rewind(Interpreter *interp, int cells);
```

`peephole_can_rewind(n)` returns true iff `here - n >=
local_scope_dict_starts[current_scope]` — never rewind past the start
of the body currently being compiled.

Called from `emit_call` (and from the per-token compile logic in
`run_outer`) before emitting the second op of a known pattern: read the
last N cells, match against patterns, rewind and emit the
super-instruction if matched, otherwise proceed normally.

- **Files**: `src/c/core.c` (helpers, plus dispatch hook in `emit_call`
  / `run_outer`).
- **LOC**: ~40 lines.
- **Expected gain**: zero standalone; enables Steps 5-8.
- **Risk**: minimal. Never rewinds across scope boundaries by
  construction.

---

## Step 4 — `(local@0)` / `(local!0)`

Depth-zero local fetch/store. Two new primitives, plus emit-site
detection.

- **New primitives in `src/c/core.c`**: `p_local_fetch_0`,
  `p_local_store_0`. Each reads one operand (slot only, no depth) and
  accesses `return_stack[local_base + slot]` directly — no depth-walk
  loop, no second operand fetch.
- **Emit-site change**: in `run_outer`'s locals-read branch and in
  `p_to`'s locals-write branch, check if the resolved local depth is 0
  and emit the new primitive in that case. Compile-time check, no
  peephole needed.
- **`mark_body` update**: add the two new ops to the operand-skipping
  table (2 cells each: handler + slot).
- **LOC**: ~80 lines (two primitives + two emit-site changes +
  `mark_body` + register in `main`).
- **Expected gain**: 5-7%. The profile shows `p_local_fetch` and
  `p_local_store` consuming ~10% of CPU between them; halving that is
  the upper bound for the depth-zero case (which is the universal case
  outside closures).
- **Risk**: low. Well-scoped, doesn't touch the dispatch loop itself.

---

## Step 5 — `(@k)` / `(!k)`

Single-key frame get/set. Peephole-detected when `@` / `!` follows a
literal of a 1-element symbol array (the result of compiling a path
like `/key`).

- **New primitives in `src/c/collections.c`**: `p_frame_get_key`,
  `p_frame_set_key`. Each takes a frame on the data stack and a
  symbol's CFA as an inline operand; calls `frame_find` directly,
  skipping the path-validation loop and the `frame_walk` outer loop.
- **Peephole detection**: when about to emit the `@` / `!` primitive,
  check the prior 3 cells — `literal_handler`, `T_ARRAY` tag, array
  handle. If the array has `len == 1` and `items[0].tag == T_SYMBOL`,
  rewind 3 cells and emit `(@k) <symbol_value>` or `(!k) <symbol_value>`.
- **`mark_body` update**: add the two new ops (2 cells each: handler +
  symbol).
- **LOC**: ~100 lines.
- **Expected gain**: 10-15 ns per single-key lookup. The scale-frames
  benchmark in `bench/synth.l4` reports ~37 ns per single-key lookup
  pre-optimization; this should reduce it to ~22-27 ns.
- **Risk**: low-medium. Peephole logic needs careful handling of the
  array-literal lookback — only rewrite when the literal's array
  Object satisfies the shape constraint.

---

## Step 6 — `?0branch`

Collapse `dup 0= if` into one op. Peephole-detected when `(0branch)`
(emitted by `if`) follows `dup` then `0=`.

- **New primitive in `src/c/words.c`**: `p_qzbranch`. Reads its
  branch-offset operand. Tests `data_stack[dsp - 1]` without popping;
  if zero, branches.
- **Peephole detection**: at `p_if`'s emit point, check prior 2 cells —
  `dup_handler`, `zeq_handler`. If both match, rewind 2 cells and emit
  `(?0branch) <slot>` instead of `(0branch) <slot>`.
- **`mark_body` update**: add the new op (2 cells: handler + offset,
  same as existing branches).
- **LOC**: ~30 lines.
- **Expected gain**: 2-3%. Saves 2 dispatches per non-destructive
  zero-test; common in `begin … until` loops.
- **Risk**: low. `p_if`'s emit logic is small and well-scoped.

---

## Step 7 — `1+`

Collapse `1 +` into one op. Peephole-detected when `+` follows a
`(literal) T_FLOAT 1.0`.

- **New primitive in `src/c/words.c`**: `p_inc`. Pops one float, pushes
  float + 1.
- **Peephole detection**: at `+`'s emit point, check prior 3 cells —
  `literal_handler`, `T_FLOAT` tag, float-bits-of-1.0. If all match,
  rewind 3 cells and emit `(1+)`.
- **`mark_body` update**: add the new op (1 cell, no operand).
- **LOC**: ~30 lines.
- **Expected gain**: ~1%. Common inner-loop pattern (`i 1 +`) but each
  instance saves only one dispatch.
- **Risk**: low.

---

## Step 8 — `dup *`

Collapse `dup *` (squaring) into one op. Peephole-detected when `*`
follows `dup`.

- **New primitive in `src/c/words.c`**: `p_sq`. Peeks the top, pushes
  top × top, replacing top.
- **Peephole detection**: at `*`'s emit point, check prior cell —
  `dup_handler`. If match, rewind 1 cell and emit `(dup*)`.
- **`mark_body` update**: add the new op (1 cell, no operand).
- **LOC**: ~25 lines.
- **Expected gain**: 1-2% on numeric-kernel phases that use squaring.
- **Risk**: minimal.

---

## Notes

**Body-start tracking for peephole**: `interp->vocab->latest_cfa + 1`
is the start of the current outermost colon-def's body. For quotations,
`interp->local_scope_dict_starts[interp->n_local_scopes - 1]` marks
the current scope's start. The peephole helpers refuse to rewind
across either boundary, so a super-instruction can never be emitted
that crosses a `[: … :]` boundary or extends past the beginning of
its containing definition.

**Stale items deliberately skipped**: PLAN.md lists `+!` (in-place add
to variable) and `@+` (read-then-increment) as super-instruction
candidates flagged STALE — they targeted patterns that no longer exist
after the `@` / `!` repurposing for frames. If equivalent post-rename
patterns surface in a refreshed profile (`x <expr> + to x` for the
accumulator, `x 1 + to x` for the read-then-increment), re-derive
before implementing. Not in this mini-plan.

**Verification per step**: `make test` passes after each step.
`./logicforth < bench/synth.l4` numbers should improve monotonically —
if any step regresses a phase, suspect the peephole logic and inspect
with `see`.

**Combined estimate**: ~380 lines of C across all 8 steps. Combined
expected gain over current direct-threaded build: ~25-30% on
`bench/synth.l4` phases 1, 2, 4, 5, 6. Phase 3 (DGEMM kernel)
unaffected throughout — it's a single C-kernel call, not dispatch-bound.

**Order rationale**: Step 1 and Step 2 are independent of the peephole
framework and can be implemented first as warm-up. Step 3 builds the
shared infrastructure. Steps 4-8 are independent of each other and
can be implemented in any order (or in parallel), each adding ~25-100
lines.
