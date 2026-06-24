# Multicore in logicforth

This is a primer on how logicforth spreads work across CPU cores — what `pmap`
does and the machinery underneath it. By the end you should understand:

- The three ways a runtime can use multiple cores, and why logicforth threads
  over one shared heap rather than forking or copying results back
- What state is shared between threads and what each thread keeps private
- How a parallel loop load-balances itself with a dynamic work cursor
- How threads allocate from the shared heap without a global lock
- Why the heap tables are pre-sized before a region and never grown inside one
- How a region reclaims its memory on teardown, and how a worker collects its own
  garbage mid-region without a global pause
- How a worker fault becomes a clean error
- The `pmap` / `pfilter` / `pmap-reduce` words and the contract a worker
  quotation works within

It's a conceptual tour. The machinery is in `src/c/core.c` and
`src/c/functional.c`; this document explains the ideas, not their field names.

---

## Part 1: Three ways to use cores, and the one logicforth uses

A single-threaded interpreter leaves a multicore machine mostly idle. There are
three established ways to spread an interpreter's work across cores, differing
mainly in what they do with *memory*.

- **Fork processes.** Each worker is a separate process with its own address
  space. Isolation is total, but workers can't see each other's heaps, so any
  result has to be serialized, sent through a pipe, and rebuilt in the parent.
  For a language whose values are graphs of heap objects, that copy-back is most
  of the cost.

- **Threads with private heaps.** Workers share an address space but each
  allocates from its own heap. No allocator contention, but a value a worker
  builds lives in that worker's heap; handing it back still means copying or
  pointer-translating it. The copy-back problem survives.

- **Threads over one shared heap.** Workers share the address space *and* the
  heap. A value a worker builds is, immediately, a value the coordinator can
  name. This is what logicforth does.

The shared-heap choice pays off because of how logicforth already represents
values. A value on a stack is a tagged word whose payload, for a heap type, is a
*handle* — an index into a global table, not a raw pointer (see `gc.md`). The
object table and the cons-pair table are one-per-process globals. So when a
worker builds an array and writes its handle into a result slot, that same handle
resolves to the same object in the coordinator once the threads join. The join is
**zero-copy**: no serialization, no pointer translation. The cost is that
allocation and interning now touch shared structures from several threads at
once, and the rest of this document is how that's made safe without a lock on the
hot path.

---

## Part 2: What is shared, and what is private

One rule: **the program is shared; a thread of execution is private.**

The program lives in process-global structures — the dictionary and symbol pool,
the arena and object table, the pair table, the compile-time state — shared by
every thread because they're read-only (or, for the heap tables, append-only in
controlled ways) during a run.

A *thread of execution* is an interpreter: its data, return, and side stacks, its
instruction pointer, its GC roots, its regex and database side-tables, and its
logic-programming store — the binding trail and logic-variable stack. The
coordinator runs on the main interpreter; each worker on its own. Two threads
never trample each other's execution state.

One consequence is worth stating: because the logic store is per-interpreter, a
worker's unification bindings are private to it and undone on its own backtrack.
Parallel work shares the heap but not the unification store — logic programming
across workers is not something `pmap` provides.

---

## Part 3: The harness — a dynamic work cursor

A parallel loop is a shared description — how many items, how many to claim at a
time, and the per-chunk kernel — plus a set of worker threads that each run the
same loop: atomically claim the next contiguous chunk of indices, run the kernel
over it, repeat until the items run out.

```
loop:
    start = atomic_fetch_add(cursor, items_per_claim)   // grab the next chunk
    if start >= n_items: stop
    run kernel over [start, min(start + items_per_claim, n_items))
```

This is *dynamic* dispatch, not a static partition. Slicing the range into one
fixed piece per worker up front is simpler but loses badly when items take
unequal time — the worker handed the slow items runs long after the others have
gone idle. The shared cursor instead lets any worker that finishes its chunk grab
the next, so the load balances itself. `items_per_claim` is the granularity:
large amortizes the atomic across many cheap items; small balances finer when
item costs vary (Part 7 returns to the tradeoff).

The calling thread is itself worker 0 — it runs the kernel loop rather than
blocking — so an N-worker run spawns N−1 extra threads and joins them at the end.
After the joins, every chunk has been processed and every worker's heap writes
are complete and visible.

---

## Part 4: Allocating from a shared heap without a lock

If every allocation took a mutex, the hot path would serialize and the
parallelism would evaporate. Instead each thread allocates from its own
**allocation context** and reaches the shared heap only occasionally, in bulk,
with an atomic. A single global flag selects the running thread's context, so the
sequential program pays just one predicted branch and never touches thread-local
storage.

Two things get claimed in bulk:

- **Byte payloads** come from the context's slab window; when it's dry, the
  thread claims a fresh slab from the reserved region with one atomic (`arena.md`
  covers the slab mechanism).
- **Handles** — object slots and pair slots — come from a *band* the thread has
  claimed: a contiguous run of slot indices (`SLOTS_PER_CLAIM` of them) taken from
  the table's shared high-water mark with one atomic. The thread then hands out
  slots from its band with a plain increment, and records the band's start so it
  can later sweep its own slots (Part 8). When the band empties it claims another.

Each thread thus writes only into slots it has claimed exclusively, so concurrent
allocations never collide, and the only shared writes are the occasional bulk
claims.

What the parallel path must *never* do is **grow** a table. Growth is a
reallocation, and a reallocation can move the object or pair table to a new
address while another thread is reading through the old one. So the tables are
**pre-sized once, before the region**, with headroom for the domain plus every
worker's bands, and an in-region claim that would exceed the pre-sized capacity
*fails* rather than grows. The pre-sizing runs on the single coordinating thread
before any worker starts, where reallocation is safe.

A worker can still reclaim its own garbage in place — a per-worker collection that
never changes a table's shape (Part 8). The global collector, which would, stays
suspended for the region's duration.

---

## Part 5: Interning under concurrency

Symbol interning writes to a shared table, so it takes its own care when several
workers intern at once: a lookup reads the index lock-free, and only the rare
insert of a genuinely new name takes a brief lock, re-checks (another thread may
have inserted the same name meanwhile), and publishes. The lock is taken only
while a region is active, so ordinary single-threaded interning never touches it.
A worker quotation may freely intern symbols — `:keys`, `string>symbol`, JSON
object keys — and the table stays consistent. `symbol-hash.md` covers the
protocol.

---

## Part 6: The per-thread trampoline

Calling a word from C — which the `pmap` kernel does once per element — goes
through a small trampoline: a few dictionary cells the interpreter writes a call
into and runs (`threading.md`). Those cells are shared dictionary storage, so each
interpreter owns a disjoint set, and the dictionary holds back enough cells at the
bottom for the coordinator and every worker. Two threads invoking quotations at
the same time never write each other's trampoline.

---

## Part 7: pmap, pfilter, pmap-reduce

The three parallel words mirror `map`, `filter`, and an associative `reduce`.
Each has a full `-ext` form exposing the two tuning knobs and a shortcut that
defaults them (`num-cores` workers, claim 1):

```
pmap-ext        ( array worker_count items_per_claim xt -- image )
pmap            ( array xt -- image )
pfilter-ext     ( array worker_count items_per_claim pred -- image )
pfilter         ( array pred -- image )
pmap-reduce-ext ( array worker_count items_per_claim identity map-xt combine-xt -- result )
pmap-reduce     ( array identity map-xt combine-xt -- result )
num-cores       ( -- n )
```

`pmap-reduce` is a fused parallel map+fold: each worker folds its chunk into a
running accumulator starting from `identity`, and the coordinator combines the
per-worker partials — so `combine-xt` must be associative with `identity` as its
neutral element. `pfilter` keeps the elements for which `pred` is truthy, order
preserved.

The coordinator's shape is the same for all three: allocate the result array (and
root it, since the kernels allocate), pre-size the heap tables, snapshot the
tables' high-water marks, run the work cursor, then either surface a worker fault
or finish. The kernel maps each element of its claimed chunk through a worker
interpreter and writes the result into the matching output slot.

Two properties make the kernel safe:

- **The result writes are disjoint and zero-copy.** Worker *t* writes only the
  output slots for the indices it claimed — disjoint from every other worker — so
  the writes need no synchronization, and once the threads join the coordinator
  reads the finished handles directly. There is no merge step.

- **Worker interpreters are pooled.** A worker interpreter carries large stacks,
  so building one per call would be wasteful. They live in a pool, claimed
  thread-locally the first time a thread enters the kernel and reused across
  calls; the coordinator resets the pool claim before each region so its own
  persistent thread takes a fresh slot rather than a stale one.

### Teardown: faults and rewind

When a worker quotation faults — throws, leaves the wrong number of values, or
out-allocates the pre-sized headroom — the kernel sees the error after the call,
signals a shared fault flag, and stops. After the join the coordinator finds the
flag set, discards the region, and raises a clean error on the main interpreter.
A worker fault surfaces as an error, never as silent garbage in the result.

On success the coordinator does one more thing: it reclaims the region's
allocations when nothing live points into them. It snapshotted the tables'
high-water marks before the workers ran; if no output value references memory the
region allocated — checked by a shallow scan of the results — it **rewinds**
those high-water marks, dropping the whole region's allocations at once. This is
valid because a worker-allocated value always has a handle at or above the
snapshot, so if the output references none of them, everything above the line is
garbage. It costs an O(n) scan of the results, no collection, no lock. The cases
it covers: `pfilter` always (its results are kept *input* elements, below the
line), and scalar-result `pmap` / `pmap-reduce` (numeric reductions, counts,
predicates). A region returning freshly built heap values fails the scan and
commits, growing the heap as ordinary live data. The one assumption is the worker
contract below — a quotation must not bury a region reference inside a pre-
existing object, where the output scan wouldn't see it.

### Choosing worker_count and items_per_claim

The two knobs exist because the right values depend on what each element costs.

- **CPU-bound work** (arithmetic, parsing, transforms). Set `worker_count` near
  the core count and `items_per_claim` large, so the per-chunk atomic is
  amortized across many cheap items.

- **Latency-bound work** (each element a network call, an LLM request, a
  subprocess). The limit is no longer cores but how many requests may be
  outstanding — a rate limit or connection budget — so `worker_count` is set to
  *that*, often far from the core count, and `items_per_claim` should be 1: the
  per-item cost dwarfs the atomic, latencies vary wildly, and a claim of 1 keeps
  one worker from being handed a run of slow items while others idle.

`pmap`'s default — one worker per core, claim 1 — is the reasonable middle:
finest-grained balancing.

---

## Part 8: Per-worker garbage collection

The teardown rewind reclaims a region's memory only *after* it ends. That's fine
for a short region, but a long one — a `pmap` over a million rows, each building
and discarding matrices — would pile up garbage for the whole run and could
exhaust the heap before the join. The global collector can't run (it would move
the tables under a live reader). So a worker collects its *own* garbage, in place,
while the others keep going.

The trigger is **byte pressure**, not slot count — the same metered live-byte
total `gc.md` describes, kept per worker. When a worker's big allocations push it
past its threshold, a collection is requested and serviced at a safepoint between
words, so no popped-but-unrooted operand is freed under a running primitive. In a
region that request routes to the per-worker collector rather than the global one.

The per-worker collector is a *generational* mark-and-sweep scoped to one worker:

- **It marks from that worker's own roots** — its stacks and GC roots — using the
  same epoch-stamp marking as the global collector.
- **It stops at the region boundary.** Marking returns immediately on any handle
  below the region's start — every value allocated *before* the region (the
  shared inputs, all older data) is "old", never traversed and never swept by a
  worker. Only the *young* values the region created are in scope.
- **It sweeps only that worker's own slots.** A worker recorded the start of every
  band it claimed; the sweep walks exactly those, frees the unmarked, and pushes
  their handles onto a *local* free list that the worker's own allocator reuses
  first. One worker never reads or writes another's slots.

Because each worker touches only its own bands, handles, free list, and byte
counter, several can collect at once with no lock — the one shared write is the
atomic epoch bump. The generational scheme rests on a single invariant: **no old
value points to a young one.** A region's job is to *produce* new values from old
inputs, so the natural data flow only ever points young→old; nothing writes a
fresh handle back into a pre-existing object. Debug builds check it both ways — a
worker is asserted to mark only within its own bands, and after the region the
coordinator scans the surviving old set for any pointer into reclaimed young
space.

---

## Part 9: What a worker quotation may do

A `pmap` quotation runs on a worker thread over the shared heap, so it works
within a contract:

- **It produces new values; it does not mutate shared inputs.** Reading the domain
  is fine — every worker reads it. Writing into a shared array or frame from
  several workers is a race the runtime does not guard. The natural style —
  building a fresh result per element — stays inside the contract, and upholds the
  young→old invariant the per-worker collector depends on.

- **It allocates freely.** Strings, arrays, sets, frames, matrices, segments,
  cons cells, interned symbols — all are on the per-thread allocation path and
  safe to create concurrently.

- **It should leave exactly one value per element.** The kernel pushes one element
  and pops one result, like `map`. A quotation that faults — throws, has the
  wrong arity, or out-allocates the region headroom — is not undefined behavior:
  the kernel catches it, the region is discarded, and the coordinator raises a
  clean error. You get an error, not a partial or corrupt result.

- **It must not print.** Worker threads share one stdout; concurrent writes
  interleave. Compute in the workers, print from the coordinator after the join.

- **Its logic-variable state is private.** Each worker has its own binding store,
  so unification inside a quotation is local to that worker and does not compose
  into a shared search.

---

For broader context: `arena.md` is the slab allocator the per-thread contexts
claim from; `gc.md` is the global collector the region suspends and the tables the
workers allocate into; `symbol-hash.md` is the concurrent interning protocol; and
`threading.md` is the dispatch model and the per-interpreter trampoline.
