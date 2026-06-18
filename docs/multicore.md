# Multicore in logicforth

This document is a primer on how logicforth runs work across CPU cores — what `pmap` does, and the machinery underneath it. By the end you should understand:

- The three ways a runtime can use multiple cores, and why logicforth threads over one shared heap rather than forking or copying results back
- What state is shared between threads and what each thread keeps private
- How a parallel loop is dispatched: a dynamic work cursor instead of a fixed partition, and why that load-balances
- How threads allocate from the shared heap without a global lock — per-thread slabs and object/pair bands, refilled by atomic claims
- Why the object and pair tables are pre-sized before a parallel region and never grown inside one
- How a region reclaims its allocations on teardown by rewinding the bump high-waters, and how a worker fault is surfaced as a clean error
- How symbol interning, the trampoline, and garbage collection behave under threads
- The `pmap` / `pfilter` / `pmap-reduce` words (and their `-ext` forms and `num-cores`), and the discipline a worker quotation must follow

The core machinery is in `src/c/core.c` (`parallel_for`, `worker_entry`, the `in_parallel` branches of `object_alloc_slot` and `object_new_pair`, `arena_bump`, `abort_parallel_region`, `intern_symbol`, `interp_init` / `main_init` / `worker_init`) and `src/c/functional.c` (`p_pmap`, `p_pfilter`, `p_pmap_reduce`, their kernels, `references_region`, `claim_worker`, `p_num_cores`). The types are in `src/c/logicforth.h` (`AllocContext`, `ParallelTask`, the `in_parallel` flag, and the `MAX_WORKER_THREADS` / `SLAB_BYTES` / `SLOTS_PER_CLAIM` constants), and the `pmap` shortcut is in `src/forth/lib.l4`. The aim is to make the parallel path feel like a small extension of the sequential one — a few atomic claims and a per-thread allocation cursor — rather than a separate runtime.

---

## Part 1: Three ways to use cores, and the approach logicforth uses

A single-threaded interpreter leaves a multicore machine mostly idle. There are three established ways to spread an interpreter's work across cores, and they differ mainly in what they do with *memory*.

- **Fork processes.** Each worker is a separate OS process with its own address space. Isolation is total — no shared state, no races — but the workers can't see each other's heaps, so any result has to be serialized, sent through a pipe or shared-memory segment, and rebuilt in the parent. For a language whose values are graphs of heap objects (arrays of frames of strings), that copy-back is most of the cost.

- **Threads with private heaps.** Workers share an address space but each allocates from its own heap. No allocator contention, but a value built by a worker lives in that worker's heap; to hand it back to the coordinator you must still copy it across, or freeze it and translate its internal pointers. The copy-back problem survives.

- **Threads over one shared heap.** Workers share the address space *and* the heap. A value a worker builds is, immediately, a value the coordinator can name — because both refer to it the same way. This is what logicforth does.

The shared-heap choice pays off because of how logicforth already represents heap values. A value on the stack is a `Val` whose payload, for a heap type, is a *handle* — an index into a global table, not a raw pointer (see `docs/gc.md`). The object table `arena.objects` and the cons-pair table `pairs.table` are file-scope globals, one per process. So when a worker builds an array and writes its handle into a result slot, that handle resolves to the same object in the coordinator after the threads join. No copy-back, no serialization, no pointer translation — the join is *zero-copy*. The handle a worker produces is already meaningful everywhere.

The cost of the shared heap is that allocation and interning now touch shared structures from several threads at once. The rest of this document is how that is made safe without putting a lock on the allocation hot path.

---

## Part 2: What is shared, and what is per-thread

The split follows one rule: **the program is shared; a thread of execution is private.**

The program lives in file-scope globals, shared by every thread:

- `Vocabulary vocab` — the dictionary, and the symbol pool and its hash index.
- `Arena arena` — the reserved bump region and the object handle table.
- `PairPool pairs` — the cons-cell table.
- `Compiler compiler` — the compile-time state and handler registry.

A thread of execution is an `Interpreter`, one per thread, holding everything that makes a computation *in progress*:

```c
typedef struct Interpreter {
    Val data_stack[DATA_STACK_DEPTH];   int dsp;
    Val return_stack[RETURN_STACK_DEPTH]; int rsp;
    Val side_stack[SIDESTACK_DEPTH];    int side_dsp;
    int local_base;
    int *bind_trail;  int bind_trail_top, bind_trail_cap;   // unification trail
    Val *lvar_stack;  int lvar_top, lvar_cap;               // logic variables
    int ip;
    int trampoline_base;                                    // this thread's trampoline trio
    int running, error_flag, gc_disabled;
    Val gc_roots[MAX_GC_ROOTS]; int n_gc_roots;
    /* regex cache, open databases, unwinding state, error buffer ... */
} Interpreter;
```

The coordinator runs on the main `Interpreter`; each worker thread runs on its own. Stacks, instruction pointer, the trampoline base (Part 6), GC roots, and the regex/database side-tables are all private, so two threads never trample each other's execution state.

One consequence worth stating: the logic-programming substrate — `lvar_stack` and `bind_trail` — is per-`Interpreter`. Unification bindings a worker makes are private to that worker and undone on its own backtrack. So parallel work shares the heap but not the unification store; logic programming across workers is not something `pmap` provides.

---

## Part 3: The harness — a dynamic work cursor

A parallel loop is described by a `ParallelTask` and run by `parallel_for`:

```c
typedef struct {
    int n_items;
    int items_per_claim;
    _Atomic int next_index;     // the shared cursor
    void (*kernel)(int start_index, int end_index, void *context);
    void *context;
} ParallelTask;
```

Each worker runs the same loop, `worker_entry`, which repeatedly claims a contiguous chunk of indices and runs the kernel over it:

```c
static void *worker_entry(void *parallel_task) {
    ParallelTask *task = parallel_task;
    for (;;) {
        int start_index = atomic_fetch_add(&task->next_index, task->items_per_claim);
        if (start_index >= task->n_items) break;
        int end_index = MIN(start_index + task->items_per_claim, task->n_items);
        task->kernel(start_index, end_index, task->context);
    }
    return NULL;
}
```

This is *dynamic* dispatch, not a static partition. A static scheme would slice `[0, n)` into one fixed range per worker up front; that is simplest but loses badly when items take unequal time — the worker handed the slow items runs long after the others have finished and gone idle. The dynamic cursor instead lets any worker that finishes its chunk grab the next one, with `atomic_fetch_add` on `next_index` handing out non-overlapping `[start, end)` ranges. Fast workers keep claiming; the load balances itself. `items_per_claim` is the granularity: large amortizes the atomic across many items, small balances finer (Part 7 returns to the tradeoff).

`parallel_for` builds the task, clamps the worker count, and spawns the threads:

```c
void parallel_for(int n_items, int n_threads, int items_per_claim,
        void (*kernel)(int, int, void *), void *context) {
    CLAMP(n_threads, 1, MAX_WORKER_THREADS);
    if (n_threads > n_items)
        n_threads = n_items > 0 ? n_items : 1;

    ParallelTask task = { .n_items = n_items, .items_per_claim = items_per_claim,
                          .next_index = 0, .kernel = kernel, .context = context };

    pthread_t threads[MAX_WORKER_THREADS];
    for (int worker = 1; worker < n_threads; worker++)
        pthread_create(&threads[worker], NULL, worker_entry, &task);
    worker_entry(&task);                          // worker 0 runs on the calling thread
    for (int worker = 1; worker < n_threads; worker++)
        pthread_join(threads[worker], NULL);
}
```

Worker 0 is the calling thread itself — it runs the kernel loop rather than blocking on a join, so an N-thread run spawns N−1 pthreads. After the joins, every claimed chunk has been processed and all the workers' heap writes are complete.

---

## Part 4: Allocating from a shared heap without a lock

If every `arena_alloc` and every object-slot grab took a mutex, the allocation hot path would serialize and the parallelism would evaporate. Instead each thread allocates from its own **`AllocContext`**, and only reaches for the shared arena occasionally, in bulk, with an atomic.

```c
typedef struct {
    char *slab_next, *slab_end;   // byte slab for payloads
    int slot_next, slot_end;      // object-table band
    int pair_next, pair_end;      // pair-table band
} AllocContext;
```

There are two contexts: a `main_alloc` for the coordinator and a `_Thread_local thread_alloc` for whatever thread is running. A global flag, `in_parallel`, selects between them, so the sequential path never touches thread-local storage:

```c
static inline void *arena_alloc(size_t bytes) {
    size_t advance = (bytes + (ARENA_ALIGNMENT - 1)) & ~(size_t)(ARENA_ALIGNMENT - 1);
    return arena_bump(in_parallel ? &thread_alloc : &main_alloc, advance);
}
```

**Byte payloads** (string bytes, array/frame storage) come from the context's slab window. When the window runs dry, `arena_bump` claims a fresh `SLAB_BYTES` (64 KiB) slab from the shared region with a single `atomic_fetch_add` on `arena.used` — the only shared write, paid once per slab rather than once per allocation (`docs/arena.md` covers the slab mechanism in full).

**Object slots** work the same way, one level up. `object_alloc_slot`, in a parallel region, hands out slots from the context's band, refilling it by claiming `SLOTS_PER_CLAIM` (1024) slots at once off `arena.n_objects`:

```c
int object_alloc_slot(Interpreter *interp) {
    if (in_parallel) {
        if (thread_alloc.slot_next >= thread_alloc.slot_end) {
            int claimed = atomic_fetch_add(&arena.n_objects, SLOTS_PER_CLAIM);
            if (claimed + SLOTS_PER_CLAIM > arena.objects_cap) {
                fail(interp, "object table full in parallel region");
                return -1;
            }
            thread_alloc.slot_next = claimed;
            thread_alloc.slot_end  = claimed + SLOTS_PER_CLAIM;
        }
        return thread_alloc.slot_next++;
    }
    /* ... sequential path: bands off arena, free-slot reuse, GC on exhaustion ... */
}
```

**Cons pairs** take the identical shape in `object_new_pair`: a per-thread pair band refilled by an `atomic_fetch_add` on `pairs.n_pairs`. Each thread thus writes only into slots it has claimed exclusively, so concurrent allocations never collide.

Notice what the parallel branches do *not* do: they never grow a table and never run GC. Both are forbidden inside a region, for the same reason — growth is a `realloc`, and a `realloc` can move `arena.objects` or `pairs.table` to a new address while another thread is reading through the old one. So the tables are **pre-sized once, before the region**, with enough headroom for every worker's bands, and the in-region claim *fails* rather than grows if it somehow runs past the cap:

```c
int object_headroom = arena.n_objects + domain->len + worker_count * SLOTS_PER_CLAIM;
object_headroom = MIN(object_headroom, arena.max_objects);
if (object_headroom > arena.objects_cap)
    GROW_OBJECT_TABLE(object_headroom);

int pair_headroom = pairs.n_pairs + domain->len + worker_count * SLOTS_PER_CLAIM;
if (pair_headroom > pairs.pairs_cap)
    GROW_PAIR_TABLE(pair_headroom);
```

These run on the single coordinating thread, before any worker starts, so the `realloc` inside the `GROW_*` macros is safe. Worker interpreters also run with `gc_disabled` set, so no collection fires mid-region; the per-thread bands leave gaps (slots claimed but unused), which a later sequential GC reclaims into the free lists. `docs/gc.md` describes the collector that the region suspends.

The whole scheme costs the sequential path one predicted branch — `if (in_parallel)`, almost always false — and no atomics, since the single-threaded program allocates entirely through `main_alloc`.

---

## Part 5: Interning under concurrency

Symbol interning writes to a shared table (the symbol pool and its hash index), so it needs its own care when several workers intern at once. The index slots are `_Atomic`: a lookup reads them lock-free with acquire-loads, and the rare insert of a genuinely new name takes a mutex, re-probes (in case another thread inserted the same name in the meantime), appends to the pool, and publishes the slot with a release-store. The lock is taken only when `in_parallel` is set, so ordinary single-threaded interning never touches it. `docs/symbol-hash.md` covers the protocol in detail; the point here is that a worker quotation may freely intern symbols (`:keys`, `string>symbol`, JSON keys) and the index stays consistent.

---

## Part 6: The per-thread trampoline

Calling a word from C — which the `pmap` kernel does once per element — goes through a small trampoline: three dictionary cells the interpreter writes a call into, points `ip` at, and runs (`docs/threading.md`, Part 9). Those cells are shared dictionary storage, so each `Interpreter` owns a disjoint trio, named by `interp->trampoline_base`: the main interpreter at 0, worker *k* at `3 * k`. `DICT_RESERVED` (`3 * (MAX_WORKER_THREADS + 1)`) holds back enough cells at the bottom of the dictionary for the coordinator and every worker, so two threads invoking quotations at the same time never write each other's trampoline.

---

## Part 7: pmap, pfilter, pmap-reduce

The three parallel words mirror `map`, `filter`, and an associative `reduce`. Each has a full `-ext` form exposing the two tuning knobs and a shortcut that defaults them (`num-cores` workers, claim 1):

```
pmap-ext        ( array worker_count items_per_claim xt -- image )
pmap            ( array xt -- image )
pfilter-ext     ( array worker_count items_per_claim pred -- image )
pfilter         ( array pred -- image )
pmap-reduce-ext ( array worker_count items_per_claim identity map-xt combine-xt -- result )
pmap-reduce     ( array identity map-xt combine-xt -- result )
num-cores       ( -- n )    \ sysconf(_SC_NPROCESSORS_ONLN)
```

`pmap-reduce` is a fused parallel map+fold: each worker folds its chunk into a running accumulator starting from `identity`, and the coordinator combines the per-worker partials — so `combine-xt` must be associative with `identity` as its neutral element. `pfilter` keeps the elements for which `pred` is truthy, order preserved.

`p_pmap` is the coordinator, and the other two have the same shape. It allocates the result array, runs the region through `parallel_apply` (which pre-sizes the tables (Part 4), snapshots the bump high-waters, and runs `parallel_for`), then either surfaces a worker fault or rewinds the region and returns the result:

```c
void p_pmap(Interpreter *interp) {
    POP_XT(function, "pmap-ext");
    POP_INT(items_per_claim, "pmap-ext", "items per claim");
    POP_INT(worker_count, "pmap", "worker count");
    PEEK_SEQUENCE_AT(domain_val, 0, "pmap-ext");
    Object *domain = OBJECT_AT(VAL_DATA(domain_val));
    int domain_index = interp->dsp - 1;

    NEW_ARRAY(image_handle, image, domain->len);          // the result, rooted for GC
    memset(image->items, 0, sizeof(Val) * (size_t)MAX(domain->len, 1));
    gc_root_push(interp, make_array(image_handle));

    PmapContext mapping = { .function = function, .domain = domain, .image = image };
    RegionSnapshot region;
    int failed = parallel_apply(domain, worker_count, items_per_claim, pmap_kernel, &mapping, &region);
    gc_root_pop(interp);

    if (failed) {                                         // a worker faulted
        abort_parallel_region(region.used, region.n_objects, region.n_pairs);
        fail(interp, "pmap: a worker quotation failed");
        return;
    }

    int rewindable = 1;                                   // results all transient?
    for (int i = 0; i < domain->len; i++)
        if (references_region(image->items[i], &region)) { rewindable = 0; break; }
    if (rewindable)
        abort_parallel_region(region.used, region.n_objects, region.n_pairs);

    interp->dsp = domain_index;
    push(interp, make_array(image_handle));
    DISPATCH(interp);
}
```

The kernel maps the elements of its claimed chunk, each through its own worker interpreter:

```c
static void pmap_kernel(int start_index, int end_index, void *context) {
    PmapContext *mapping = context;
    if (!worker_interp)
        worker_interp = claim_worker();
    for (int i = start_index; i < end_index; i++) {
        push(worker_interp, mapping->domain->items[i]);
        execute_cfa(worker_interp, mapping->function);
        if (worker_interp->error_flag) {                  // fault → signal and stop
            parallel_error = 1;
            return;
        }
        mapping->image->items[i] = pop(worker_interp);
    }
}
```

Two pieces make this work:

- **Worker interpreters are pooled, claimed thread-locally.** A worker `Interpreter` carries big stacks, so building one per `pmap` would be wasteful. Instead they live in a pool, reused across calls. The first time a thread enters the kernel, its `_Thread_local worker_interp` is null, so it `claim_worker()`s one — an `atomic_fetch_add` on `worker_claim` hands out a distinct pool index, and the interpreter there is created lazily by `worker_init` (which sets its `trampoline_base` and `gc_disabled`). `p_pmap` resets `worker_claim` to 0 and `worker_interp` to null before each region, so the coordinator's own thread (which persists between calls) re-claims a fresh slot rather than reusing a stale one.

- **The image write is zero-copy and contention-free.** Worker *t* writes `image->items[i]` for the indices it claimed — disjoint from every other worker's, so the writes need no synchronization. And `image` is an ordinary global-table array, so once the threads join, the coordinator reads the finished handles directly. There is no merge step.

### Teardown: faults and rewind

When a worker's quotation faults — throws, has the wrong stack effect, or exhausts the pre-sized headroom — the allocator sets that interpreter's `error_flag`, the kernel sees it after `execute_cfa`, sets the shared `parallel_error`, and returns. After the join, the coordinator finds `parallel_error` set, `abort_parallel_region`s, and raises a clean error on the main interpreter — so a worker fault surfaces as an error, never silent garbage in the result.

On success the coordinator does one more thing: it reclaims the region's allocations when nothing live points into them. It snapshotted the bump high-waters (`arena.used`, `n_objects`, `n_pairs`) before the workers ran; if no result references region-allocated memory — checked by a shallow `references_region` scan of the output — `abort_parallel_region` restores those high-waters, dropping the entire region's allocations at once:

```c
void abort_parallel_region(size_t used, int n_objects, int n_pairs) {
    arena.used = used;
    arena.n_objects = n_objects;
    pairs.n_pairs = n_pairs;
    memset(&thread_alloc, 0, sizeof thread_alloc);   // calling thread re-claims fresh
}
```

This is the same wholesale rewind the failure path uses, and it's valid for the same reason: a worker-allocated object always has a handle at or above the snapshot, so if the output references none of them, everything above the line is garbage. It costs an O(n) tag scan (O(1) for the single result of `pmap-reduce`), no GC, no lock. The cases it covers: `pfilter` always (results are kept input elements, below the line), and scalar-result `pmap` / `pmap-reduce` (numeric reductions, sizes, predicates). A region returning live heap objects fails the scan and commits, growing the heap as ordinary live data. The one assumption is the worker contract below — that a quotation doesn't bury a region reference inside a pre-existing object, where the output scan wouldn't see it.

### Choosing worker_count and items_per_claim

The two knobs exist because the right values depend on what each element costs.

- **CPU-bound work** (arithmetic, parsing, transforms). Set `worker_count` near the core count, and `items_per_claim` large — hundreds or more — so the per-chunk atomic is amortized across many cheap items. This is what `pmap` defaults to with `num-cores` workers, though its default claim of 1 favors balance over amortization; `pmap-ext` lets you raise it for very cheap elements.

- **Latency-bound work** (each element a network call, an LLM request, a subprocess). The limit is no longer cores but how many requests you may have outstanding — a rate limit or connection budget — so `worker_count` is set to *that*, often far below or above the core count. And `items_per_claim` should be 1: the per-item cost dwarfs the atomic, item latencies vary wildly, and a claim of 1 keeps any one worker from being handed a run of slow items while others idle.

`pmap`'s `(num-cores, 1)` default is the reasonable middle: one worker per core, finest-grained balancing.

---

## Part 8: What a worker quotation may do

A `pmap` quotation runs on a worker thread over the shared heap, so it works within a contract:

- **It produces new values; it does not mutate shared inputs.** Reading the domain is fine — every worker reads it. Writing into a shared array or frame from several workers is a race the runtime does not guard. The natural style, building a fresh result per element, stays inside the contract; the kernel writes only the disjoint image slot.

- **It allocates freely — objects, pairs, interned symbols.** Strings, arrays, sets, frames, matrices (object slots + arena payloads), cons cells (the pair band), and interned symbols are all on the per-thread allocation path and safe to create concurrently (Parts 4–5).

- **It should be net `+1` on the data stack.** The kernel pushes one element and pops one result per item, so the quotation is expected to leave exactly one value, like `map`. A quotation that faults — throws, has the wrong arity, or out-allocates the region headroom — is not undefined behavior: the kernel catches the `error_flag`, the region is abandoned and rewound, and the coordinator raises a clean error (Part 7). You get an error, not a partial or corrupt result.

- **It must not print.** Worker threads share one stdout; concurrent writes interleave. Compute in the workers, print from the coordinator after the join.

- **Its logic-variable state is private.** Each worker has its own `lvar_stack` and trail (Part 2), so unification inside a quotation is local to that worker and does not compose into a shared search.

---

## Part 9: Where to look in the source

In `src/c/logicforth.h`:

- **`AllocContext`** — the per-thread slab window and object/pair bands.
- **`ParallelTask`** — the work description with the atomic `next_index` cursor.
- **`in_parallel`**, **`MAX_WORKER_THREADS`**, **`SLAB_BYTES`**, **`SLOTS_PER_CLAIM`**, **`DICT_RESERVED`** — the flag and the sizing constants.
- **`GROW_OBJECT_TABLE` / `GROW_PAIR_TABLE`** — the pre-region table growth.

In `src/c/core.c`:

- **`parallel_for` / `worker_entry`** — the harness and the dynamic work cursor.
- **`arena_bump`** — slab claim shared by both contexts.
- **`object_alloc_slot` / `object_new_pair`** — the `in_parallel` allocation bands.
- **`abort_parallel_region`** — the wholesale rewind: restore the bump high-waters, reset the calling thread's context.
- **`intern_symbol`** — the lock-free-read / locked-insert interner (see `docs/symbol-hash.md`).
- **`interp_init` / `main_init` / `worker_init`** — per-thread vs whole-program setup; `worker_init` sets `trampoline_base` and `gc_disabled`.

In `src/c/functional.c`:

- **`parallel_apply`** — pre-size the tables, snapshot the high-waters, run `parallel_for`, return whether a worker faulted.
- **`references_region`** — the shallow test of whether a result Val points into region-allocated memory (drives the success rewind).
- **`p_pmap` / `p_pfilter` / `p_pmap_reduce`** and their kernels — the three coordinators and per-element loops; the kernels check `error_flag` and signal `parallel_error` on a fault.
- **`claim_worker`** — the thread-local worker-pool claim.
- **`p_num_cores`** — `sysconf(_SC_NPROCESSORS_ONLN)`.

In `src/forth/lib.l4`:

- **`pmap` / `pfilter` / `pmap-reduce`** — the `num-cores`/claim-1 shortcuts over the `-ext` forms.

For broader context:

- **`docs/arena.md`** — the slab allocator the per-thread contexts claim from.
- **`docs/gc.md`** — the collector suspended inside a parallel region, and the object/pair tables the workers allocate into.
- **`docs/symbol-hash.md`** — the concurrent interning protocol.
- **`docs/threading.md`** — the dispatch model and the per-interpreter trampoline base.
