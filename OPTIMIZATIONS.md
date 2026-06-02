# Post-DTC interpreter optimizations

Reference profile: run `./logicforth < bench/profile.l4` under `sample`
(macOS) or `perf record` (Linux); refresh the per-primitive ranking
before committing to additional candidates.

---

## Done

- **Float fast-path on `lt` / `gt` / `eq`** — `p_lt`, `p_gt`, `p_eq`
  inline the `T_FLOAT` / `T_FLOAT` case and skip `val_cmp` for it.
  Factored into a `COMPARISON_PRIMITIVE` macro.

- **Inline frame access helpers** — `frame_find` and `frame_walk` are
  `static inline __attribute__((always_inline))` in `logicforth.h`.
  `nm` confirms both fully inlined.

- **`times` and `i-times` combinators** — counted-loop primitives in
  `src/c/functional.c`, factored through a `COUNTED_LOOP` macro.
  `bench/synth-defs.l4`'s phase 1, `bench-build`, and `bench-lookup`
  rewritten to use them.

---

## `1+` ( n -- n+1 )

Float increment as a primitive. `i 1+ to i` in place of `i 1 + to i`.

- **File**: `src/c/words.c`. ~5 lines + prototype + registration.
- **Expected gain**: ~1 ns per call; meaningful in tight loops with
  many increments.

## `1-` ( n -- n-1 )

Float decrement. Mirror of `1+`.

- **File**: `src/c/words.c`. ~5 lines + prototype + registration.

## `sq` ( n -- n² )

Squaring as a primitive. `sq` in place of `dup *`.

- **File**: `src/c/words.c`. ~5 lines + prototype + registration.

## `key@` ( frame symbol -- value )

Single-key frame fetch taking the symbol directly. Calls `frame_find`
once; skips path validation and `frame_walk`'s outer loop. The symbol
can be a dynamic stack value.

- **File**: `src/c/collections.c`. ~15 lines + prototype + registration.
- **Expected gain**: ~10-15 ns vs `/key @` for a single-key access.

## `key!` ( frame value symbol -- frame )

Single-key frame store. Symmetric to `key@`.

- **File**: `src/c/collections.c`. ~15 lines + prototype + registration.

## `(local@0)` / `(local!0)`

Depth-zero local fetch/store. Compiler knows the resolved local depth
at emit time; when depth is 0, emit a primitive that takes only a slot
operand and skips the depth-walk loop in `local_slot`.

- **New primitives in `src/c/core.c`**: `p_local_fetch_0`,
  `p_local_store_0`. Each ~10 lines.
- **Emit-site changes**: `run_outer`'s locals-read branch and `p_to`'s
  locals-write branch add `if (local_depth == 0)` and emit the new
  variant. ~4 lines per site.
- **`mark_body` update**: two new ops, each 2 cells (handler + slot).
- **LOC**: ~50 lines.
- **Expected gain**: 5-7%. `p_local_fetch` + `p_local_store` are
  ~10% of CPU per the profile.

## `(?0branch)`

Collapse `dup 0= if` into one op.

- **New primitive in `src/c/words.c`**: `p_qzbranch`. Reads its
  branch-offset operand; tests `data_stack[dsp - 1]` without popping;
  branches if zero. ~10 lines.
- **Emit-site change in `p_if`**: before emitting `(0branch)`, check
  the prior 2 cells against the cached `dup` and `0=` handler pointers.
  If both match, verify `here - 2 >=
  local_scope_dict_starts[current_scope]`, then rewind 2 cells and emit
  `(?0branch) <slot>`. ~15 lines.
- **`mark_body` update**: one new op (2 cells: handler + offset).
- **LOC**: ~30 lines.
- **Expected gain**: 2-3%.

---

## Verification

`make test` passes after each step. `./logicforth < bench/synth.l4`
numbers improve monotonically (or stay flat for phases that don't
exercise the affected path).

## Total remaining

~125 lines of C across 7 additions. Combined expected gain over the
current state: ~15-20% on `bench/synth.l4` phases 1, 2, 4, 5, 6.
Phase 3 (DGEMM kernel) unaffected.
