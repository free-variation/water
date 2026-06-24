# Static analysis: allocation, GC, and image save/load

Crash and corruption issues found by static reading of `src/c/core.c`,
`src/c/functional.c`, and `src/c/logicforth.h`. Ordered by severity. Each entry
cites file:line as of this analysis.

## 1. `pmap`/`pmap-reduce` worker GC frees live results (deterministic SIGSEGV)

The parallel-map output array is allocated *before* the parallel region, so it is
an "old" object, but each worker stores its results into it during the region.
When a worker runs `worker_local_gc`, that array is not a GC root, so
already-stored results are collected and their handles left dangling.

Mechanism:
- `p_pmap` allocates `image` before the region (`functional.c:471`); handle is
  `< parallel_region_object_base`.
- Workers write results into it: `mapping->image->items[i] = ...`
  (`functional.c:260`) — an old->young pointer.
- `worker_local_gc` marks only the worker's own stacks + `gc_roots`
  (`core.c:2981-2988`); it never marks `image`. Results for items below the
  current one are unreachable and freed: `arena.objects[handle] = NULL`
  (`core.c:2999-3004`).
- The dangling handle in `image->items[i]` is later dereferenced — e.g.
  `references_region_depth` does `obj->len` on a NULL `OBJECT_AT(handle)`
  (`functional.c:277`), or the result array's consumer faults. Freed handles are
  also recycled within the same region, aliasing later results.

Aggravating factors:
- Fires early, not just at scale. Worker threads are recreated per
  `parallel_for` (`core.c:462-464`), so each worker's `_Thread_local
  thread_alloc` starts zeroed including `heap_gc_threshold == 0`. The first
  matrix/segment allocation makes `heap_bytes_live > 0 > 0` true
  (`core.c:353`, `core.c:379`) and sets `gc_pending`.
- `worker_init` sets `gc_disabled = 1` (`core.c:3507`), but the `in_parallel`
  branch at `core.c:1121` calls `worker_local_gc` regardless of `gc_disabled`.
- The `GC_DEBUG` guard `debug_check_no_old_to_young` (`functional.c:426`,
  asserted `functional.c:489`) forbids exactly the old->young edge that pmap
  depends on.

`p_pmap_reduce` is the same shape: partials are written to the old `partials`
array (`functional.c:535`) with no rooting in `worker_local_gc`.

Direction for a fix: `worker_local_gc` must treat the region's output array(s)
as roots, or workers must root their stored results. This is a design change.

## 2. `load_image` trusts unbounded counts -> unchecked realloc / NULL deref

`saved_n_pairs` and `saved_lvar_top` are validated only `>= 0`, never bounded
above (`core.c:3406`, `core.c:3423`):
- `while (pairs.space.cap < saved_n_pairs) GROW_PAIR_TABLE(pairs.space.cap * 2);`
  (`core.c:3410-3411`) — `cap * 2` can overflow `int`, and `GROW_PAIR_TABLE`
  calls `realloc` with no NULL check (`logicforth.h:360-366`). A crafted count
  forces an enormous request -> NULL -> write to `pairs.table[i]` faults.
- Same for the `lvar_stack` realloc loop (`core.c:3427-3430`), unchecked.

No `realloc`/`GROW_*` path checks for NULL anywhere (`GROW_OBJECT_TABLE`,
`GROW_PAIR_TABLE`, lvar loop). Object `capacity` is bounded only by `len <= cap`
with `cap` up to `INT_MAX` (`core.c:3280`), so a large `cap` with small `len`
forces a large `arena_malloc` (also unchecked). `validate_loaded`
(`core.c:3073`) runs only *after* all allocation, so it cannot prevent these
allocation-time crashes.

## 3. `load_image` continuation fields under-validated

`resume_ip` is range-checked (`core.c:3359-3360`), but `local_base_offset` is
read raw (`core.c:3366-3371`) and later used as `slice_base + local_base_offset`
to set `interp->local_base` in `p_resume` (`words.c:750-751`); a corrupt value
yields out-of-range local fetches. Also `capture_generation` is left 0 by the
`arena_alloc_object` memset rather than restored — it happens to match
`forget_generation == 0` post-load by coincidence, not by design.

## 4. Object handle table grows toward `MAX_OBJECTS` under allocate+GC load

Not a crash; unbounded handle-index growth. In `object_alloc_slot`
(`core.c:169`) the GC free list (`object_space.free`/`n_free`) is consulted only
after `arena.object_space.n < arena.object_space.max` fails (`core.c:182` vs
`core.c:202`). `gc()` rebuilds that free list (`core.c:2552-2563`) but never
lowers `object_space.n` and resets the local claim window to 0 (`core.c:2565`).
So a long allocate-then-GC workload keeps bumping `n` and reclaims the memory
(Object structs and payloads via the recycling lists) but not the handle
indices — the `arena.objects` pointer array marches toward `MAX_OBJECTS`
(~536MB at 1<<26 slots) before freed handles are reused.
