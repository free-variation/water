# logicforth static analysis — crash scenarios & performance bottlenecks

Static (no-run) review of `src/c/` (~11k lines). Vendored pcre2/sqlite/isocline/sljit
excluded. Re-verified against current `dev` (post `47dac82 fixed crash scenarios`,
`e1cee93 per-region rewinds`).

Data model: NaN-boxed `Val`; objects referenced by integer handle into an `Object**`
table (struct pointers stable for life); bump + per-size-class free-list arena over a
1<<34 mmap reservation; mark/sweep GC (`mark_epoch`); separate `PairPool`; per-thread
slab bands (`AllocContext`) for `pmap`/`pfilter`/`pmap-reduce` over one shared heap with
GC disabled in-region.

Status: **verified** = traced the exact code path this pass; **inferred** = reasoned from
the code, trigger not executed. **FIXED** = resolved; fix + red→green verification noted inline.

Resolved this pass: C1, H1, H2, H3, H4, H5, H6, H7 (all Critical + High). H3/H4/H5/H7 carry
golden regression tests (`tests/140`–`143`); C1/H1/H2 (corrupt-image fixtures) and H6
(realloc-failure injection) were red→green-verified out of tree but are not golden-guarded.

Load-bearing invariants (relied on throughout; would break silently if violated):
1. `arena_realloc` never triggers GC; relocates only the one payload passed.
2. GC frees payloads but never relocates them — only `frame_put`/`frame_reserve` move them.
3. The object table holds stable `Object*`; struct allocations live for the object's life.
4. Worker bands always begin at/above main's reserved high-water mark (no slot overlap).
5. `gc()` resets `main_alloc.slot_next/slot_end = 0` after sweep (the fix below depends on it).

---

## Critical — heap corruption

### C1. Image load: collection `len > cap` → heap OOB write
`core.c:2968-3006` — **verified**

`len` and `cap` are read straight from the file with no `>= 0` / `len <= cap` check
(2975-2976). SET/ARRAY allocates `sizeof(Val) * MAX(cap,1)` (2988) but the fill loop runs
`for j < len` (2989). FRAME is identical: `MAX(cap,1)` keys/values (2997-2998), loop to
`len` (2999). A crafted image with `cap=0, len=N` writes N `Val`s into a one-slot buffer.
Trigger: load a malformed `.lfi`. The STRING path is self-consistent (alloc `len+1`, read
`len`); the bug is specific to the `cap`/`len` split on collections.

**FIXED.** Image load rejects `len < 0 || cap < 0 || len > cap` before allocating/filling.
Red→green via a crafted corrupt `.lfi` fixture.

---

## High

### H1. Image load: loaded handles never validated → type confusion / OOB deref
`core.c:2573-2580` (`r_val`) + every `OBJECT_AT` consumer — **verified**

`r_val` rebuilds any tag+data via `make_tagged` with no validation. A stored `T_ARRAY`/etc.
value carries an object handle that is never checked against `[0, n_objects)` nor against the
target object's `kind`. `mark_value` self-protects (`core.c:2001-2002` bounds-checks the
handle), but ordinary word ops (`POP_ARRAY`/`POP_STRING`/`OBJECT_AT(...)->items`) do not: a
`T_ARRAY` Val pointing at a slot whose object is actually a string aliases `bytes`/`items`
through the union → type confusion; an out-of-range handle → OOB table read + deref.
Trigger: crafted `.lfi`.

**FIXED.** `loaded_handle_ok` + `validate_loaded` walk every loaded object (ARRAY/SET items,
FRAME keys + values, CONTINUATION slice), pairs, lvars, and the data stack, checking each
handle against its kind and bounds; load fails on any bad handle. Red→green via a crafted
fixture.

### H2. Image load: matrix `rows`/`cols` unvalidated
`core.c:3008-3022` — **verified**

`rows`,`cols` read with no sign/magnitude check (the continuation path checks `return_len`;
the matrix path does not). `(size_t)rows*(size_t)cols` sign-extends negatives to huge sizes
(`calloc` fails → graceful), but the size_t product can also *wrap small* (e.g. `rows=cols=-2`
→ product 4) yielding a tiny buffer with negative stored dims, and the dims propagate into
later `rows*cols` loop bounds and `MAT()` indexing. Trigger: crafted `.lfi`.

**FIXED.** Matrix load rejects `rows < 0 || cols < 0 || (int64_t)rows*cols > INT_MAX` before
allocating. Red→green via a crafted fixture.

### H3. `references_region` is shallow → committing `pmap`/`pmap-reduce` can free still-referenced region objects
`functional.c:262-278`, `394-401`, `492-493` — **inferred**

`references_region` checks only the *top-level* handle (`VAL_DATA(value) >= snapshot->n_objects`,
line 270; pair variant line 272). It does not recurse into array elements / frame values /
pair tails. If a worker quotation returns a result whose top handle is *below* the snapshot
(a pre-region object) but which it mutated to *contain* region-allocated children, the result
is judged rewindable; if every result is rewindable, `abort_parallel_region` rewinds
`arena.used`/`n_objects`/`n_pairs` (401), reclaiming the live children → dangling handles →
UAF on next use. The pair-pool variant additionally desyncs GC bookkeeping: after a rewind
that drops `n_pairs` below a live pair handle, `gc()` only memsets/sweeps `[0, n_pairs)`
(`core.c:2311`, `2378`) so the slot is never reclaimed correctly and can be reissued while
referenced. Requires a quotation that mutates pre-region shared state (itself a data race
across workers), so the trigger is narrow but the rewind logic is unsound for it. `pfilter`
always aborts and rebuilds from booleans, so it is unaffected.

**FIXED.** `references_region` is now recursive (depth-capped at `MAX_NESTING_DEPTH`,
conservatively returning "references region" on overflow so a cyclic pre-region object never
triggers a wrong rewind), descending into SET/ARRAY items, FRAME values, CONT slice, and pair
head/tail. Common cases stay O(1) (scalar results, region-allocated results). Red→green: with
the shallow check the region is rewound and a reused slot makes `A[0]` alias a later allocation
(`5678`); recursive keeps it (`1234`). Guard: `tests/141_pmap_region_keep`.

### H4. GC `mark_value` recursion is unbounded → native stack overflow
`core.c:1976-2019` (esp. 1995-1996) — **verified**

`mark_value` recurses on `pairs.table[slot].head`/`.tail` and on array/frame elements with no
depth or spine cap. A cons list of length N recurses N deep on the tail; a deeply nested
structure recurses by depth. The pair-mark guard (1992-1994) prevents *cyclic* infinite
recursion but not linear depth. `copy`/`reify` cap the spine (`COPY_SPINE_MAX`); GC marking
does not. Trigger: build a long cons list (constructible up to ~1<<24) and trigger a GC.

**FIXED.** `mark_value` rewritten as a behavior-preserving transform: the tail positions (pair
tail, container last element, logic-var deref) iterate via a `for(;;)` loop instead of
recursing; only heads/branches recurse. A worklist was rejected as over-engineered — it would
trade depth-overflow for width-memory blowup, and rewrite the GC core. Red→green: 1M-deep cons
list under `gc` — recursive marking SIGSEGVs, iterative survives. Guard: `tests/140_gc_deep_list`.
Residual: a deliberately left-/head-deep structure still recurses by structural nesting depth
(needs one allocated object per level — far narrower than the trivially-built cons list).

### H5. `resume` trusts a captured `resume_ip` that referenced words may have outlived
`words.c` `p_resume` (capture in `capture_continuation`) — **inferred**

`p_resume` sets `interp->ip = continuation.resume_ip` and runs. The load path validates
`resume_ip ∈ [DICT_RESERVED, vocab.here)` (`core.c:3040-3041`), but a live continuation
captured before a `forget` (which rewinds `vocab.here`) is not revalidated: `resume_ip` then
points into reclaimed/overwritten dict cells, executed as a `cfa_handler` → call through a
bogus function pointer. Trigger: capture a continuation, `forget` the enclosing word, `resume`.

**FIXED — but not via a `resume_ip` range check.** That subcase (forget, then resume) does not
even crash: `forget` only rewinds `vocab.here`, leaving the cells intact. The dangerous case is
forget **then redefine** — new words overwrite the reclaimed cells and `vocab.here` climbs back
*past* `resume_ip`, so a range check passes while the code underneath is overwritten (confirmed:
SIGSEGV). Real fix = forget-generation stamp: `vocab.forget_generation++` on `forget`, stamped
onto each continuation at capture, compared at `resume`; any forget after capture invalidates it.
No image-format change (loaded stamp memsets to 0, global resets to 0 on load). Red→green on the
redefine case (unguarded SIGSEGV → clean error). Guard: `tests/142_resume_after_forget`.

### H6. Unchecked `realloc` in regex match-all / replace → NULL deref + leak
`strings.c:113` (`p_match_all` spans), `strings.c:225` (`p_replace` append) — **verified**

Both do `buf = realloc(buf, ...)` with no NULL check; on failure the old pointer leaks and the
next `memcpy`/store writes through NULL+offset. Driven by match count on a long subject /
large replacement under memory pressure. (Same posture for several other raw `malloc`s —
regex-cache store `strings.c:31`, continuation slice `core.c:3032`, `string_concat`,
`interp_append` — listed under Low.)

**FIXED.** Both reallocs check the return: on NULL, free the old buffer and the PCRE2 match
data and `fail` cleanly. `p_replace`'s `append_bytes` is error-guarded (no-ops once failed) and
the function bails before building the result. Red→green via a scoped failing-`realloc` shim on
`strings.c` (unguarded SIGSEGV on both sites → clean OOM error). Not golden-guarded (needs
allocator injection).

### H7. REPL error recovery never clears `unwinding` → permanent interpreter wedge
`core.c:3586-3594` (REPL recovery) + `core.c:956-974` (`run_inner`) — **verified**

On `error_flag` the REPL resets `dsp`, `rsp`, and compiler state but not `unwinding`/
`unwind_target`. `run_inner` breaks at the floor with `unwinding` still set when no matching
`T_MARK` is found (`958-959`: `if (rsp <= floor) break;`). So an unbalanced `fail`/backtrack/
`shift` with no enclosing prompt returns to the REPL with `unwinding == 1`; the REPL clears
`error_flag` but not `unwinding`; thereafter every word entered hits `DISPATCH`'s
`if (unwinding || error_flag) return` / `run_inner`'s unwind branch and silently does nothing
while the prompt still prints `ok`. The interpreter is wedged until restart. Trigger: type
`fail` (or a `shift` with no `reset`) at the top level.

**FIXED — trigger above is wrong.** Top-level `fail`/`shift` fail cleanly: with no enclosing
prompt, `find_prompt` errors and never sets `unwinding`. The actual wedge is `shift-with` with
no resume (e.g. `reset [: 99 . cr :] shift-with`): it sets `unwinding=1` to unwind to the reset,
which escapes the top level — and critically sets **no** `error_flag`, so the error-recovery
block never runs. Hence the fix cannot live there: clear `unwinding` unconditionally per REPL
line, next to `error_flag = 0`. Red→green (unguarded: handler prints then every later line
no-ops; fixed: lines after the trigger run). Guard: `tests/143_repl_unwind_recovery`. Scope: only
`unwinding` is cleared — `n_gc_roots`/`lvar_top`/`bind_trail_top` (M9) are a separate error-path
rollback question, deliberately not blanket-reset per line.

---

## Medium

### M1. `pmap`/`pfilter` don't clamp `worker_count` before headroom math → int overflow / under-reservation
`functional.c:283,288` reached from `p_pmap`/`p_pfilter` (`:372`,`:333`) — **verified**

`p_pmap_reduce` does `CLAMP(worker_count,1,MAX_WORKER_THREADS)` (`:451`); `p_pmap`/`p_pfilter`
pass the raw popped int into `parallel_apply`, which computes
`arena.n_objects + domain->len + worker_count*SLOTS_PER_CLAIM` (`SLOTS_PER_CLAIM=1<<10`). A
huge/negative `worker_count` overflows the `int` headroom → wrong/under reservation. Not
memory-unsafe (the in-region band claim re-checks `objects_cap`, `core.c:150`, and fails),
but converts to a nondeterministic whole-region failure. Trigger: `[ … ] 999999999 pmap`.

**FIXED.** `parallel_apply` clamps `worker_count` to `[1, MAX_WORKER_THREADS]` before the
headroom math, covering `p_pmap`/`p_pfilter` (`p_pmap_reduce`'s pre-clamp stays — it sizes the
`partials` array first). Verified: `999999999`/`-5` clamp and give the correct map result
(`parallel_for` already clamped the thread count, so only the headroom overflowed). No
dedicated golden; `tests/100_pmap` covers pmap correctness.

### M2. `select`/`has?` predicate read without array-length check → OOB read
`collections.c:867-898`, reached from `select_walk:935-938` — **verified**

Any path element with `VAL_TAG == T_ARRAY` is treated as a predicate and passed to
`predicate_holds`, which reads `pred->items[0]`, `[1]`, and (EQ/LT/GT) `[2]` with no `len`
check. `do_select` validates only the outer path and frame, and predicate arrays are
user-constructable via `[...]` literals (not gated to `parse_path_predicate`). Trigger:
`f [ :a [ ] ] select-values` (empty step) or a 2-element step decoding to EQ/LT/GT.

**FIXED.** `predicate_holds` rejects `pred->len < 2` (needs op + key) and `< 3` for the
comparison ops that read `items[2]`; valid len-2 EXISTS and len-3 comparison predicates pass.
Guard: `tests/137_select_values` (empty-step + missing-value cases).

### M3. `range` / `matrix-range` element count overflows `int`
`collections.c:555-566`, `matrix.c:553` — **verified**

`n_values = (range_to - range_from) * step + 1` in `int`; a wide span (e.g.
`2000000000 -2000000000 range`) wraps negative → `object_new_array(negative)` stores a
negative `len` over a 1-slot buffer (`core.c:263-265`), corrupting downstream `len` use; a
near-`INT_MAX` positive attempts a ~16 GB alloc. `matrix-range` casts a possibly-huge
`double` step count to `int` (`matrix.c:553`). No upper bound enforced.

**FIXED.** `range` computes the span in `int64_t` and rejects `count > INT_MAX` (no more
negative-`len` wrap); `matrix-range` bounds the double step-count before the `int` cast.
Guards: `tests/61_range`, `tests/79_matrix_range`.

### M4. `reshape` does not validate `new_rows`/`new_cols` and checks element count in `int`
`matrix.c:396-397` — **verified**

`total = rows*cols` and the guard `new_rows*new_cols != total` are `int` products from
unguarded popped ints. Negative dims (e.g. `-2 -2`) can satisfy the equality and reach
`object_new_matrix` with negative dims (which mostly fail the `calloc`, but the size_t-wrap
edge yields a corrupt small matrix). `create_matrix` guards `rows*cols ≤ INT_MAX`; `reshape`
bypasses it.

**FIXED.** `reshape` now rejects negative dims and `new_rows > INT_MAX / new_cols` before the
element-count equality (mirroring `create_matrix`), so `-2 -2` no longer satisfies `-2*-2==4`.
Guard: `tests/37_reshape_flatten`.

### M5. `shift-with` / `amb` leave the return stack / unwind state imbalanced on the error path
`words.c` `p_shift_with`, `p_amb` — **inferred**

`p_shift_with` returns early on `handler` `error_flag` with `unwind_target` set but
`unwinding` still 0 and the continuation left pushed. `p_amb` resets `rsp` only on the
backtrack arm and the normal arm; a non-backtrack `error_flag` from `branch1` skips both,
leaving the `PROMPT_CHOICE` mark and spliced frames on the return stack. Corrupt state on the
next op / next backtrack rather than a clean error.

**NOT A CORRUPTION BUG — collapses into M9.** Tested by execution: `shift-with`-handler-error
and `amb`-branch1-error, each followed by a normal line / a fresh `amb`+`fail` / a later
`resume`, all run cleanly. Two reasons: `error_flag` halts execution until recovery, so no
"next op" ever runs on the imbalanced state; and per-line recovery resets `dsp`/`rsp` (and
`unwinding` via H7), discarding the pushed continuation and the leftover `PROMPT_CHOICE`/
exception mark and making `unwind_target` inert (read only when `unwinding`). The original
"inferred" write-up didn't account for that recovery. The one genuine residual — `p_amb`'s
error arm not restoring `lvar_top`/`bind_trail_top` — is a logic-state leak fixed under M9.

### M6. Control-flow patch words write `vocab.dict[slot]` from an unvalidated popped value
`words.c:888` (`then`), `899` (`else`), `915` (`until`), `924` (`again`), `945` (`repeat`) — **verified**

These IMMEDIATE words pop a value and use it as a dict index with no bounds/provenance check
(`vocab.dict[slot] = vocab.here - slot`). Unbalanced control flow (a stray `then`/`repeat`,
or `5 then`) writes an arbitrary float-as-index into the fixed `dict[1<<20]` array → OOB
write. Self-inflicted at the REPL (no compile-time control-flow stack discipline), but
memory-unsafe.

**FIXED.** A shared `valid_patch_slot` guard rejects `slot`/`back` outside `[DICT_RESERVED,
vocab.here]` (inclusive upper so empty `begin until` still works) in all five words. Note this
fires at *compile* time (these are IMMEDIATE words) — the bad index never reaches a write. The
bug was only reachable from hand-typed/generated malformed source, never from data. Verified:
`5 then` → clean compile error; valid `if/else/then`, `begin/until`, empty `begin until` all
compile and run.

### M7. `worker_init`/`claim_worker` reset omits continuation/mark state
`functional.c` `claim_worker` — **inferred**

Reset covers `dsp/rsp/side_dsp/local_base/run_floor/bind_trail_top/lvar_top/n_gc_roots/
unwinding/error_flag`. It does *not* reset `unwind_target`, `next_mark_id`, or
`error_message`. A worker quotation that uses `shift`/`reset`/`amb` and faults leaves stale
mark state for the next region reusing that pooled worker → wrong results.

**FIXED (not a demonstrated bug).** `claim_worker` now also resets `unwind_target` and
`next_mark_id`. But the omitted fields are inert: `unwind_target` is read only inside the
`if (unwinding)` branch (`unwinding` is reset and rewritten before each unwind) and
`error_message` only when `error_flag` is set (reset; rewritten by `fail`; workers never print
it). `next_mark_id` is a counter with no marks living across regions. Tested: worker reuse
after mark-using + faulting regions gives correct results — couldn't produce the claimed wrong
result. The reset completes the "pristine worker" contract and bounds `next_mark_id` growth;
`error_message` left alone (worker-irrelevant).

### M9. REPL error recovery leaks `n_gc_roots` / `lvar_top` / `bind_trail_top` / `side_dsp`
`core.c:3586-3594` — **verified**

The recovery block omits `n_gc_roots`, `lvar_top`, `bind_trail_top`, `side_dsp`, `local_base`,
`next_mark_id`. A word that `gc_root_push`es then `fail`s before its matching `gc_root_pop`
leaks a root; after `MAX_GC_ROOTS` (64) such faults, every subsequent rooted allocation hits
"gc roots exhausted" permanently. `lvar_top`/`bind_trail_top` grow monotonically across
errors (memory leak; stale logic-var bindings can also leak into later unification), and
`side_dsp` keeps residue. Bounded per-error but unbounded across a session.

**FIXED.** Recovery resets `n_gc_roots`/`side_dsp`/`local_base` to 0 (their clean-boundary
value) and rolls `lvar_top`/`bind_trail_top` back to a line-start snapshot via `trail_undo_to`
(exposed from logic.c) — so a failed line's bindings are *unbound*, not just abandoned (no
stale bindings leaking into later unification), while prior committed bindings (logic state
persists across REPL lines — verified) are preserved. `next_mark_id` deliberately left: a
benign counter, and rewinding it risks id reuse against a persisted continuation. Verified: 70
root-leaking faults then a normal rooted op still works (past the 64 threshold); a committed
binding survives an intervening failing line. Guard: `tests/115_unify`.

### M10. `read` (stream) capacity `*= 2` overflows `int` + unchecked `malloc`/`realloc`
`words.c:1948-1956` — **verified mechanism**

`capacity` starts at `1<<16` and doubles; past `1<<30` the next double overflows `int`
negative, making `(size_t)(capacity - length)` enormous → `read` writes past the buffer. The
`malloc`/`realloc` returns are unchecked. Trigger: read a stream emitting >2 GB (e.g. a
spawned `yes`/`cat /dev/zero`). The same `capacity *= 2` + unchecked-`realloc` pattern is in
`interp_append` (`words.c:1424-1432`, the string-interpolation buffer); overflows on a
template that renders >2 GB. (`p_read_file` is *not* affected — it sizes upfront with a
`size > INT_MAX` guard, `words.c:1793`.)

**FIXED.** Both paths cap growth at `capacity > INT_MAX/2` (clean `fail` before `*=2` wraps)
and check every `malloc`/`realloc`. `interp_append`/`interp_render_val` now thread `interp`
(error-guarded; `interpolate` bails before building the result). Not golden-able (needs a
>2 GB stream/template); verified green (no regression — `100_read`/`107_format` pass).

### M11. `stop`/`wait` pass an unvalidated user pid to `kill`/`waitpid`
`words.c:2020`, `1997`, `2025` — **verified**

`p_stop_process` does `kill((pid_t)pid, SIGKILL)` with `pid` straight from `POP_INT`. `0 stop`
→ `kill(0, …)` signals the interpreter's entire process group (self-terminate); `-1 stop` →
`kill(-1, …)` signals every process the user can reach. Not memory-unsafe, but a destructive
footgun with no guard that the pid is one this interpreter spawned.

**FIXED.** `wait`/`stop` reject `pid <= 0` — which is every broadcast/process-group case
(`kill(0)`, `kill(-1)`, `kill(-pgid)`); a spawned child is always `> 0`. Residual: a positive
pid you own but didn't spawn here is contained by the OS (`kill` → EPERM, `waitpid` non-child
→ ECHILD, both surfacing as a clean `fail`); full pid-table tracking would close that sliver
but isn't warranted for the named footgun. Guard: `tests/106_running`.

### M12. `start-process` child does not close inherited fds (no `FD_CLOEXEC`)
`words.c:1882-1894` — **verified**

The child `dup2`s and closes only its own six pipe fds; it inherits every other open
descriptor — REPL stdin/stdout, sqlite fds, and crucially *other live processes'* pipe ends.
With ≥2 concurrent processes a child holds a sibling's pipe write-end, so the parent's `read`
on that sibling's `out` never sees EOF → `read`/`wait` hangs. Also leaks fds into children.
Fix: `O_CLOEXEC` on the pipes (or close-from-`exec` in the child).

**FIXED.** `FD_CLOEXEC` is set on all six pipe fds right after `pipe()`, so inherited copies
(including siblings') close at the child's `execvp`; the `dup2`'d 0/1/2 survive (dup2 clears
CLOEXEC on the target). Red→green (timeout-bounded): `cat` + a `sleep 30` sibling — buggy
`read` stalls until the sibling exits (124/timeout), fixed completes immediately. The real
symptom is a stall proportional to the sibling's lifetime (a never-exiting sibling → hang).
Not golden-able (a stall, not divergent stdout).

### M8. Misc verified
- **`pthread_create` return unchecked** — `core.c:389`; under thread exhaustion (EAGAIN)
  `threads[w]` is uninitialized and later `pthread_join`ed → UB. **verified.** **FIXED:**
  pack only successfully-created handles and join those; the atomic dynamic dispatch means the
  main thread + survivors still drain all items, so failure degrades parallelism, not work.
- **`forget` does not rewind `symbol_pool_here` or rebuild the symbol hash** — `p_forget`
  rewinds `here`/`names_here`/`latest_cfa`/`source_here` only; `forget_user` does both.
  Stale symbol-hash entries can reference reclaimed CFAs. **verified.** **NOT A BUG (no
  change):** the premise is wrong — the symbol hash maps name→pool-offset, not CFAs, and
  `find()` walks the dict chain, never the hash. Re-interning a symbol after `forget` works
  (verified). A partial-`forget` symbol rewind would be *unsafe* anyway (no per-word watermark;
  symbols can be shared with surviving words / live data). Leftover symbols are a benign leak.
- **`db-exec`/`db-query` pass SQL with `-1` (NUL-terminated), ignoring `->len`** —
  `database.c:99,164`; a query containing an embedded NUL is silently truncated (correctness,
  not a crash). **verified.** **FIXED:** pass `->len` (matching the text-bind at `:74`).
- **`dict_ensure` overflow is `exit(1)`** — `core.c:1152`; and **worker-side arena exhaustion
  `exit(1)` from a worker thread** — `core.c:44`. Hard process kill instead of a recoverable
  `fail`. **verified.** **LEFT AS-IS:** `arena_bump` returns a pointer the caller immediately
  dereferences and `dict_ensure` precedes an unconditional write, so a bare `fail` would turn a
  clean process death into an OOB write. Recoverable OOM needs a longjmp path or pervasive
  caller checks — architectural, out of scope for a guard. The `exit(1)` is memory-safe.
- **Native-stack recursion via `execute_cfa`/`resume`/`amb`/self-`load`** has no call-depth
  guard (`MAX_NESTING_DEPTH` bounds only data-structure recursion); `docol` is musttail-safe.
  **inferred.** **FIXED:** `interp->call_depth` (balanced inc/dec around `run_inner`) with
  `MAX_CALL_DEPTH = 1<<12`; deep `execute`-recursion now fails cleanly below C-stack overflow
  (50000-deep → clean error, not SIGSEGV). Normal return-stack recursion is unaffected.
- **`p_join` sums lengths in `int`** — `strings.c:321-330`; overflow → bad alloc/OOB on a huge
  array. **inferred.** **FIXED:** sum in `int64_t`, reject `total > INT_MAX` (like M3).

---

## Low

- Unchecked `malloc`/`realloc`/`strdup` on OOM: regex-cache store (`strings.c:31`),
  continuation slice (`core.c:3032`), lvar-stack realloc on image load (`core.c:3089`),
  `record_loaded_file` strdup, `string_concat`/`interp_append`. NULL-deref rather than graceful.
- Image load: `saved_lvar_top`/`saved_n_pairs` have only a `< 0` check, no upper bound →
  attacker-driven huge `realloc` (`core.c:3070-3090`).
- Per-region `thread_alloc` free-list metadata is dropped on commit → minor permanent
  fragmentation (not a growing leak; arena bytes are recovered).
- `db_bind` return codes ignored; `POP_DB` handle index unbounds-checked (`database.c`).
- `array-of` / `object_new_array` accept negative length → corrupt `len` field (no immediate
  OOB; loop is a no-op).

---

## Performance bottlenecks

- **`find()`/`name_of()` are O(n) linked-list `strcmp` scans per token** — `core.c:890,900`.
  Symbols have a hash (`symbol_hash`); words do not. Dominant compile cost O(words × tokens).
  **verified.**
- **`gc()` rebuilds + insertion-sorts the whole vocabulary every collection** —
  `core.c:2323-2341`. The link chain is descending, the worst case for insertion sort →
  O(W²) per GC; it also re-marks *every* word body each collection (`2343-2361`), O(total
  dict). **verified.**
- **Set literal `<…>` builds via repeated `set_add`** — `collections.c:139-151`; each insert
  is binary-search + `memmove` → O(n²), vs `array>set`'s qsort+dedup O(n log n). **verified.**
- **Frame literal `{…}` / `array>frame` build via repeated `frame_put`** —
  `collections.c` (`p_frameclose`, `p_array_to_frame`, `p_merge`); each insert shifts the
  sorted keys/values tail → O(n²). **verified.**
- **`dgemm` transposed paths (TN/NT/TT) have no cache blocking** — `matrix.c:175-187`;
  column-strided `MAT(A,p,i)`/`MAT(B,j,p)` thrash cache on large transposed multiplies; the NN
  path streams rows but is also unblocked + single-threaded. **verified.**
- **Dynamic chunk dispatch contends on `task->next_index`** — `core.c:362`; with a small
  `items_per_claim` the atomic cache line bounces across cores, and it shares a line with
  read-mostly task fields. Tunable via the `-ext` knob. **inferred, Low.**
- **GC fires only at object-table exhaustion** — `object_alloc_slot:165-198`; the bump arena
  is reclaimed only when `n_objects` reaches `max_objects` (default `1<<26`) and the free list
  is empty. A churning workload that never reaches that many *live* slots never collects, so
  `arena.used` grows monotonically toward the `1<<34` reservation and then `exit(1)`s at the
  bump (`core.c:44`). No heap-pressure trigger. **verified.**

---

## Verified clean (suspected, then checked)

- **Symbol interning under parallelism** — `intern_symbol:1127-1148` takes `intern_lock` in a
  region and double-checks `probe_symbol` with a release/acquire store on `symbol_hash`. Sound.
- **SIGPIPE** — ignored in `main` (`core.c:3544`); `p_write` handles the `EPIPE` errno path.
  Writing to a dead child does not kill the interpreter.
- **`read_string_literal`** (`words.c:1398-1422`) — writes into `token_buffer` with no explicit
  bound, but it is sized `INPUT_BUFFER_SIZE` (= `input_buffer`) and the REPL caps
  `input_buffer_len < INPUT_BUFFER_SIZE-1` before every append (`3562`/`3573`), so `length`
  cannot reach the end. No overflow.
- **`p_read_file`** (`words.c:1781-1814`) — sizes via `ftell` with a `size > INT_MAX` guard and
  a single `fread`; no doubling loop. (Embedded-NUL in the path truncates at `fopen`; Low.)

## Not yet audited (coverage boundary of this pass)

`next_token` input-buffer bounds; `p_write_file`/`p_append_file` beyond the short-write check;
`copy`/`reify`/`deref` internals (the depth cap is confirmed; the traversal is not); the
sqlite connection lifecycle (double-close / use-after-close of `databases[]`); a full sweep of
`GROW_IF_FULL`'s `cap*2` int-overflow across all call sites. Findings here would be additive,
not corrections.

## Suggested priority

1. ~~**C1**, **H1**, **H2** — image-load heap corruption from a crafted `.lfi`.~~ **DONE.**
2. ~~**H3** — recursive `references_region`; the one remaining unsound rewind path.~~ **DONE.**
3. ~~**H4**, **H5**, **H7** — GC mark recursion; `resume_ip` after `forget`; REPL `unwinding`.~~
   **DONE** (H7 cleared `unwinding`; the `n_gc_roots`/`lvar_top`/`bind_trail_top` siblings are
   **M9**, now also done).
4. ~~**H6** — checked reallocs in regex match-all/replace.~~ **DONE.**
5. ~~**M1–M12**~~ **DONE** (M2/M5/M7 verified benign; M8's `exit(1)` OOM left as architectural,
   the rest fixed; M10–M12 fixed).
6. Perf: word-name hash (compile-time), incremental/pressure-triggered GC — **open** (the only
   remaining category; no correctness/safety findings left).
