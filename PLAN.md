# logicforth — deferred work

Planned and pending work. Completed items are deleted; git history holds
what's been built.

---

## Matrix — remaining work

### argmax / argmin

Index of the maximum / minimum element (or an `(i, j)` pair). Deferred
until there's a concrete use case.

### Beyond core

Deferred until there's a specific use case:

- **Slicing** — submatrix by `(row-start, row-end, col-start, col-end)`.
- **Concatenation** — `hstack` (side by side), `vstack` (stacked).
- **Element-wise comparison** — `<` etc. returning a matrix of `1`/`0`, as
  a polymorphic extension of the comparison words.
- **Norms** — `norm` (L2), `frobenius-norm`.
- **SVD** — one-sided Jacobi. ~50–100 lines.

### Optional BLAS/LAPACK build

An opt-in build switch that swaps the matrix kernels for BLAS/LAPACK,
leaving the `Matrix` representation, the word surface, and the test corpus
unchanged.

- **BLAS** for the compute kernels: DGEMM (all four transpose variants) →
  `cblas_dgemm`, and the reduction / element-wise loops.
- **LAPACK** for SVD, solve / least-squares, inverse, eigen.

To settle at implementation: interface (reference CBLAS/LAPACKE vs
OpenBLAS / MKL / Accelerate); how the switch is wired (a `Makefile` target
and/or `#ifdef`); row-major vs column-major handling at the boundary.

---

## String operations

A single pattern-matching primitive subsumes the string-handling zoo
(split / index-of / starts-with / ends-with / trim / lines / replace), with
named operations as `lib.l4` wrappers. The engine is **PCRE2**
(Perl-compatible, JIT-compiled, statically linked); patterns are a superset
of POSIX ERE — `\d`, `\w`, `\n` / `\t`, lookaround, `\p{...}` — so plain
`"..."` patterns express newlines directly.

### Still deferred

- **Wrappers** over the match/replace layer: `index-of`, `starts-with`,
  `ends-with`, `trim`, `lines`.

### Unicode model

UTF-8 throughout, codepoint-indexed at the user level. Today every string
operation is byte-indexed (exact for ASCII). When done:

- `size` (on a string), `substring`, `index-of`, and match positions
  expressed in *codepoints* via a small UTF-8 codec (encode / decode /
  count / advance-by-n); translate PCRE2's byte offsets at the boundary.
  `size` stays the single count word.
- **ASCII fast path**: a cached per-string all-ASCII flag collapses the
  codec to no-ops, keeping byte-oriented speed for the common case.
- `setlocale(LC_CTYPE, "")` plus PCRE2's UCP mode for codepoint-aware
  classes; `\p{...}` property classes available under UCP.

Not covered: normalization (NFC vs NFD stay distinct), grapheme clusters,
locale case-folding outside ASCII.

---

## Core language additions

### Path queries — follow-ups

The read-side query layer is implemented (see `docs/reference.md`): the `*` and `//`
axes and `[…]` predicates in path literals, `select-values` / `select-keys`, and
search-path rejection on `@` / `!` / `delete-at` / `update-at` (with `has?`
accepting a search path). Remaining:

- **Wildcard mutation** — `*` / `//` in `!` / `delete-at` / `update-at` for
  broadcast writes. The single-location words reject a search path today; that is
  the syntax this claims.
- **Quotation predicates** — an arbitrary `[: … :]` evaluated per node, built as an
  explicit element array. The fixed predicate set (existence, `=`, `<`, `>`, the
  self `.`, a sub-path subject) is the current scope.
- **Axes beyond child and descendant.**

### Time / dates

Unix timestamps as `T_FLOAT` (seconds since epoch, fractional allowed). No
separate date type; durations are floats in seconds, arithmetic is `+` /
`-`.

- `"%Y-%m-%d %H:%M:%S" time-format` — strftime-style, UTC by default
  (`now` for the current time as a float is already built).
- `"2026-05-25" time-parse` — strptime-style parsing.
- `"%Y-%m-%dT%H:%M:%SZ"` is the recommended ISO 8601 format string.

### Random numbers

Small high-quality PRNG (xoshiro256++ or PCG, ~30 lines).

- `random-float` — uniform `[0, 1)`.
- `min max random-int` — uniform integer in `[min, max]`.
- `seed seed!` — set the RNG seed for reproducibility.
- `array shuffle` — new array, elements randomly permuted (Fisher-Yates).
- `array sample` — a uniformly random element (`lib.l4` over `random-int`).

### Sort

- `array sort` — new array sorted by the existing `val_cmp` ordering; input
  untouched.
- `array [ x y -- cmp ] sort-with` — same, with a user comparator quotation
  that pops two Vals and pushes `-1` / `0` / `1`.
- Algorithm: introsort or libc `qsort` with a comparator thunk. ~30 lines.

### stdin / env

- `stdin` / `stdout` / `stderr` — the three standard streams as `T_STREAM`
  values (fds 0/1/2). Reading and writing reuse the subprocess stream
  words: `stdin read` slurps all of stdin, `stdin read "\n" split` its
  lines, `s stdout write` emits.
- `"VAR" env` — environment variable value as a string; empty if unset.

`argv` is not included; invocation is `logicforth file.l4`, and argument
handling lives in the shell-script wrapper layer.

### Error handling — `catch` intercepts `error_flag`

Interpreter-level errors (stack underflow, type mismatch, division by zero,
bad pattern, out-of-bounds) are catchable, not only user `throw`s. `catch` /
`try-catch` run a wrapped xt; if `error_flag` is set afterward, `catch`
clears it and returns the `error_message` as a string with the failure
flag, exactly as a `throw` would. Uncaught errors still surface at the REPL.
~10 lines: a check in the `catch` path that converts a set `error_flag`
into the same `(exc 1)` result a `throw` produces.

### Format specs

Extend `format`'s placeholders with optional format specifiers after a
colon: `{0:.2f}` (precision), `{0:8}` (field width), `{0:x}` (hex) — a
small printf-style mini-language on top of the existing positional `{n}`
fill, which keeps its current rendering.

---

## TSV file I/O

Read and write tab-separated-value files, numeric and non-numeric. TSV is
the only tabular format logicforth supports — no CSV, JSON, Parquet, Arrow,
or HDF5 reader; convert other formats to TSV before loading.

- Reader: `"file.tsv" read-tsv` → array of arrays, one per row. Numeric-
  looking cells become `T_FLOAT`; everything else stays a string.
- Writer: `arr-of-arrs "file.tsv" write-tsv`. Vals are printed with the
  `print_val` logic; a tab or newline inside a cell errors out.
- Header row is pass-through (just another row); a named-column table is a
  user-level word on top.

To settle at implementation: representation of Vals with no clean TSV form
(sets, arrays, xt's, matrices) — sentinel or error, not lossy.

---

## REPL line editing

The interactive REPL is driven by vendored **isocline** (MIT, single-source build
under `external/isocline`, refreshed by `tools/vendor-isocline.sh`): persistent
history (`.logicforth_history`), emacs/vi editing with reverse-search, dictionary
word completion, and filename completion inside string literals. Batch mode
(`-b`) reads stdin raw and is unaffected. Remaining:

- **Inline hints and syntax highlighting** via isocline's highlight callback.
- **Native multiline editing** — continuation on an open delimiter works today
  through the reader's need-more accumulation; isocline's in-place multiline
  editor is not yet wired.

---

## Foreign function interface

Load any `.so` / `.dylib`, look up symbols by name, declare a C signature
at the Forth level, and call — nothing about the target known at
logicforth's compile time. Makes LAPACK, full PCRE, libcurl, libgit2
bindable without per-library C.

Mechanism: link `libdl` (`dlopen` / `dlsym`) and `libffi` (runtime-described
calls); user code declares each function's signature, libffi handles
calling-convention details at call time. ~30–100 ns per call overhead.

Implementation: ~250–400 lines of C glue; build adds `-ldl -lffi`.

To settle at implementation: word-level API surface; signature declaration
syntax; the marshalled C type set; representation of opaque C pointers in
the Val tag space; ownership of C-allocated buffers; callbacks from C back
into logicforth; struct-by-value arguments.

---

## Cooperative green threads (single OS thread)

Lightweight tasks within one OS thread, scheduled cooperatively on the
continuation machinery. On their own they are coroutines: producer/consumer
pipelines, suspendable simulations, anything that interleaves straight-line
tasks without parallelism.

**API:**

```
xt spawn          ( -- )           \ schedule xt as a green task
yield             ( -- )           \ pause this task; scheduler picks next
run-scheduler     ( -- )           \ drive the queue until empty
```

**Implementation:**

- A scheduler queue: a per-process array of `T_CONT` Vals, each a paused
  task.
- `spawn` captures a continuation that runs the given xt and pushes it.
- `yield` is `shift`: capture the current continuation, push it, resume the
  next task.
- `run-scheduler` dequeues, resumes, repeats until empty.

**Cost:** ~50 lines on `reset` / `shift` / `resume`.

Scheduling alone runs on one core and does not overlap blocking syscalls — a
blocking `read` stalls every task. The I/O payoff arrives only with the
non-blocking I/O layer below, which is built on top of this scheduler.

---

## Non-blocking I/O

Turns the green-thread scheduler into an async runtime: a task that would
block on a socket yields instead, and the scheduler resumes it once the
descriptor is ready.

**Pieces:**

- Non-blocking sockets and streams (`O_NONBLOCK`); `read` / `write` yield on
  `EAGAIN` rather than blocking.
- A readiness wait in the scheduler (`kqueue` on macOS, `epoll` on Linux):
  when no task is runnable, wait on the descriptors that parked tasks are
  blocked on, then wake the ready ones.

This is what makes green threads worth having: a single-thread server
multiplexing thousands of connections, written in straight-line blocking
style with the suspension hidden underneath. It is a larger build than the
scheduler itself.

**Build order:** green threads first (the scheduler, on continuations), then
non-blocking I/O on top.

---

## Multi-core parallelism: fork-join over a shared immutable heap

Multiple OS threads, each with its own `Interpreter` and a private GC'd heap
for scratch. Work is structured as fork-join, not message passing: a
coordinator splits the work, hands each worker a slice, and gathers results
when they join. Shared data is immutable; mutable scratch stays per-worker;
no locks on the hot path. Three layers, built in order, each broadening
coverage.

**1. Shared immutable heap.** A global object store every interpreter can
read, alongside its private heap.

- `x freeze ( x -- gx )` deep-copies `x` once into the global store and
  returns a global handle; reads after that are zero-copy from every thread.
- A `Val` distinguishes a global handle from a local one with a single tag
  bit, so dereference is `is_global(h) ? global[h] : interp->objects[h]`.
- The symbol pool moves to the shared store (intern is a synchronized append,
  reads lock-free), so frame keys resolve identically in every worker.
- Frozen objects are immutable: the in-place mutators check the frozen flag
  and refuse, or copy back into the local heap.
- Append-only to start (bounded broadcast data); reclamation by epoch or
  explicit free is a later refinement.

This removes the broadcast cost — handing a large read-only input (a corpus,
a matrix) to every worker is free.

**2. Fork-join workers.** A pool of OS threads, each running the same xt over
a disjoint slice of the work.

- `items width xt parallel-map ( -- results )`: partition `items` into
  `width` ranges, each worker runs `xt` over its range in its own heap,
  results are copied back once at the join barrier and gathered in input
  order.
- A worker reads frozen input zero-copy and builds its result in its private
  heap. The single result-copy at join is unavoidable for boxed results —
  intermediates have to be GC'd locally — and is dwarfed by per-chunk compute
  for the work this targets (text processing, parsing, per-record transforms).
- `spawn-thread` clones the vocabulary *tight* from the parent's compiled
  state (used `dict` region and pools, capacity = `here`) so the worker
  inherits every word defined so far and the xt's dict index resolves.

The broad workhorse: any chunked text / object / numeric work where
per-element compute is non-trivial.

**3. Disjoint-write shared buffer (numeric).** For tight numeric kernels whose
result is unboxed doubles, where a copy-back would cost as much as the compute.

- A preallocated shared mutable matrix or float buffer in the global store;
  each worker gets a disjoint index/row range and writes only there, so the
  writes need no lock.
- Sound only for unboxed `double` output — a boxed result would put a
  worker-local handle into a shared slot. The buffer is rooted by the
  coordinator and exempt from worker GC for the parallel region.
- Highest value as the threading *under* the built-in matrix kernels (matmul,
  element-wise, reductions): the user writes ordinary matrix code and it
  saturates the cores, with no per-element interpreter dispatch.

**Cost (rough):**

- Shared immutable heap + `freeze` + global-handle dereference + shared symbol
  pool: ~250 lines, touching the object-deref hot path.
- Fork-join `parallel-map` + tight vocabulary clone + join/gather: ~200 lines.
- Disjoint-write buffer + threaded matrix kernels: ~150 lines.

Constraints: shared data is immutable (mutating a frozen object errors or
copies back); workers communicate only through frozen inputs and joined
results — never live shared mutable state, except the disjoint numeric
buffer; output coordination via a shared stdout mutex.

**Build order within this section:** shared immutable heap, then fork-join
`parallel-map`, then the disjoint-write numeric buffer.

---

## Functional primitives

A word that builds a new array of data-dependent length must be a C
primitive, not `lib.l4`: Forth-side construction gathers off the data stack
(capped at `DATA_STACK_DEPTH`, 256), so `map` / `mapn` / `filter` allocate
and fill in C. Words returning a scalar, element, or boolean belong in
`lib.l4`.

**`lib.l4` (scalar/element result, or compose C builders):**

- **`find`** — `arr [: pred :] find` → first matching element, or `T_NONE`.
  Short-circuits via `shift`.
- **`any?`** — `arr [: pred :] any?` → boolean float.
- **`all?`** — `arr [: pred :] all?` → boolean float.
- **`flat-map`** — `arr [: ( elt -- arr ) :] flat-map` → `map` then a
  `concat` fold (both C, so not stack-bounded).
- **`sort-by`** — `arr [: ( elt -- key ) :] sort-by` → sorted by extracted
  key, atop `sort-with`.
- **`each`** — `arr [: ( elt -- ) :] each` → apply xt for side effects, no
  result.

**On frames:**

- **`group-by` (quotation-keyed variant)** — `arr [: ( elt -- key ) :]` grouping
  by a computed key. The column form — `rows :col group-by`, grouping an array of
  frames by a symbol field into a frame of row-sets — is already implemented as a
  C primitive (see docs/reference.md); this would be the computed-key variant,
  under a distinct name.
- **`partition`** — `arr [: pred :] partition` → matches and non-matches.

Composable in one line, so not added: `count` (`[: pred :] filter size`),
`min-by` / `max-by` (`reduce` with comparison), `sum` / `product`
(`0 [: + :] reduce`).

**Cost:** `find`, `any?`, `all?`, `flat-map`, `sort-by`, `each` → ~60 lines
of `lib.l4`; `group-by` / `partition` build on frames.
