# crash audit — static analysis results

Findings from a crash-scenario sweep of `src/c/` (six parallel agents, one per
crash class; reproductions run `printf 'CODE\nbye\n' | ./logicforth`, rc 139 =
SIGSEGV, rc 138/133 = abort/heap-overflow). Tiers ordered by severity × reach.
Not yet fixed — this is the work list.

General picture: common paths are well-guarded (array/stack bounds, tag checks,
and the display/copy paths `print`/`copy`/`reify`/JSON all have depth+cycle
guards). Crashes cluster in the spots below.

## Tier 1 — reachable from ordinary stdin, reproduced

### 1.1 No depth guard in the three core traversals → C-stack SIGSEGV
- `unify` (logic.c:76, head recursion), `val_cmp` (core.c:203, behind `=` `lt`
  `gt` sort set-membership), GC `mark_value` (core.c:1602). No `MAX_NESTING_DEPTH`
  cap (which `print`/`copy` have).
- Trigger: `null [: |> i | null cons :] 100000 i-times dup =` (also `~`, `gc`). rc=139.
- GC one is insidious: merely holding a deep structure live crashes at a later
  implicit collection.
- Fix: count only **stack-growing** (non-tail) recursion — `unify` already
  `musttail`s its tail, so increment depth on heads only (else a long proper
  list false-trips). For `val_cmp`/`mark_value`, iterate the spine in a loop
  (long lists cost no C-stack) and depth-guard head nesting — same shape as the
  printer. Runtime cost of the check is negligible (predicted branch).

### 1.2 Cyclic terms hang `unify`
- logic.c (no occurs check, by design). A var bound through itself
  (`X [( 1 X )] ~`) makes `unify` recurse forever (hang/timeout).
- `val_cmp`/`mark_value` survive cycles (handle-compare / mark bits) but not depth.
- Fix: depth cap in `unify` turns the hang into a clean "too deep" failure; a
  real occurs check is the heavier alternative.

### 1.3 Integer overflow / unvalidated counts in size arithmetic
- `concat` (collections.c:437): `a->len + b->len` in `int` wraps negative →
  undersized alloc + huge write. `0 1100000000 array-of dup concat` → rc=138.
- `range` (collections.c:455): `(to-from)*step+1` overflows. `0 2147483647 range`
  → rc=138.
- `array-of` (collections.c:350): no count check → negative-length / huge object
  (corrupt, NULL-deref under real OOM).
- `reshape` (matrix.c:391): `int` products + negative dims pass the consistency
  check → corrupt matrix; bypasses `create_matrix`'s guards.
- Fix: compute sizes in `size_t`, reject negative, check `INT_MAX/cols`-style
  overflow — mirror `array` (collections.c:172) and `create_matrix` (matrix.c:333).

### 1.4 Unbounded C recursion via `execute` / `load`
- `p_execute` (words.c:512) → `execute_cfa` → `run_inner`; `load_file`
  (core.c:1507) → `run_outer`. Each nesting stacks a native frame; the
  `return_stack` guard doesn't catch it. `: r ' r execute ; r` → rc=139.
  (Plain tail recursion IS caught gracefully.)
- Fix: a recursion-depth counter on `execute_cfa`/`run_inner` (and `load_depth`
  already exists for `load` — just bound it) → graceful error instead of SIGSEGV.

## Tier 2 — real defect, fires only when the object table is full mid-op

### 2.1 GC use-after-free in matrix & set arithmetic
- `+ - * /` on matrices (matrix.c:13 `matrix_add` etc.) and sets (collections.c:26
  `set_union` etc.), and matrix unary ops `transpose`/`@i`/`row-sums`/`reshape`:
  both operands are `POP`'d (unrooted), then the result allocation can trigger
  `gc()` and free them, then the freed operands are read. ASan-confirmed under
  forced GC; rarely fires naturally because the 4M-slot table seldom collects
  mid-op. (String `+`, and map/filter/reduce/JSON/copy are correctly rooted.)
- Fix: keep operands rooted (PEEK or `gc_root_push`) across the `object_new_*`
  call; re-fetching the handle is NOT enough (the object is freed, not moved).

## Tier 3 — `load-image` trusts file contents (corrupt/crafted image)

`p_load_image` (core.c ~2319-2623) validates top-level counts but not per-object
fields. Confirmed with crafted images:
- object `len`/`cap` unvalidated (`len > cap`, negative cap) → heap overflow, rc=133.
- matrix `rows`/`cols` negative/huge → `size_t` overflow → rc=139.
- restored Val tags/handles trusted (`r_val`, core.c:2138) → out-of-range /
  NULL-slot object deref → rc=139.
- continuation `local_base_offset` unvalidated (resume_ip next to it IS checked).
- negative string `len` → 1-byte underflow write (UB, didn't crash here).
- Fix: validate every field after read — `len>=0`, `cap>=0`, `len<=cap`, matrix
  dims non-negative + overflow-checked, every object-tagged handle in
  `[0,n_objects)` with a non-NULL slot whose kind matches the tag, bound
  `local_base_offset`. Trust-boundary caveat: images are normally self-produced.

## Tier 4 — systemic, low severity

~25 `malloc`/`realloc`/`calloc` sites (incl. `GROW_IF_FULL`, most `object_new_*`,
pair/lvar/bind growth, JSON/string buffers) don't NULL-check the result — crashes
only under genuine OOM. `object_new_matrix` (core.c:124) and the `slice!` temp are
the only ones that check.

## Verified safe (audited, no reproducible crash) — don't re-audit

- Fixed-array bounds: data/return/side stacks, gc_roots, input/token buffers,
  name/symbol/source pools, dict, locals, objects[] — all guarded.
- Type confusion: every `objects[]`/`pairs[]`/`lvar_stack[]` index is tag-checked
  (via POP_*/PEEK_* macros or switch-on-tag). The `make_pair`→`T_ARRAY` class is fixed.
- `@i`/`!i`/`@i,j`/slice/`substring` bounds-check the index.
- `print`/`copy`/`reify`/JSON parser+writer: depth- and cycle-guarded.
- map/filter/reduce, JSON build, copy/reify, `array>cons`/`cons>array`, the
  close words (`]`/`}`/`>`/`)]`), string ops: GC-rooted correctly.
- `f+`/`f-`/`f*`/`f/` on non-floats: garbage result, no crash (documented fast path).

## Suggested fix order (highest value first)
1.1 depth guards, 1.3 size-arithmetic validation, 1.4 execute/load recursion cap,
1.2 unify cycle cap, 2.1 root operands in matrix/set ops, then 3 image validation.
