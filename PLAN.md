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

## FastCGI service

Run logicforth as a long-lived FastCGI application behind an off-the-shelf web
server (nginx, Caddy, lighttpd, Apache). The web server owns everything HTTP —
TLS termination, HTTP/1.1–3, request parsing, static files, timeouts, rate
limiting, access logs, load balancing — and forwards each request over a Unix or
TCP socket as FastCGI records. logicforth never sees a raw HTTP byte: it decodes
the records, runs a handler, writes the response. This is the web story instead
of an in-process HTTP server — the language stays small (no HTTP stack, no TLS,
no concurrency layer needed just to serve), and scaling, supervision, and crash
isolation come from the deployment.

**Instrumentation needed** — less than an in-process server, since the web server
keeps the HTTP work:

- `accept ( listen-stream -- conn-stream )` — accept a forwarded connection as a
  `T_STREAM`. By convention the web server passes the listen socket on fd 0
  (`FCGI_LISTENSOCK_FILENO`), so `bind`/`listen` may be unnecessary.
- `read-n ( stream n -- s )` — read exactly `n` bytes. The current `read` slurps
  to EOF, which never comes on a persistent FastCGI connection; records are
  length-framed, so a bounded read is required.
- A FastCGI record codec — decode `BEGIN_REQUEST` / `PARAMS` (the CGI environment
  → a request frame) / `STDIN` (body → a string), and encode `STDOUT` +
  `END_REQUEST`. The framing is simple: `lib.l4` over `read-n`/`write` plus byte
  arithmetic, with maybe a tiny C helper for the 2/4-byte length fields.

**Serve loop.** A plain sequential `accept → decode → handle → respond` loop in
`lib.l4`, each handler wrapped in `try-catch` so a bad request can't kill the
worker; per-request allocations are reclaimed by GC. No threads.

**Scaling and robustness from the deployment, not the language.** Run N worker
processes all accepting on the same socket (the kernel load-balances) under a
process manager (e.g. systemd template units) that respawns on crash. One request
per worker isolates failures; the web server retries elsewhere — the robustness
the fork-per-connection idea was reaching for, provided by the OS.

**SQLite.** Each worker opens its own connection; enable WAL once
(`PRAGMA journal_mode=WAL`) plus a `busy_timeout`, so concurrent reads across
workers don't block and writes serialize safely (single host).

**Cost:** `accept` + `read-n` are small C; the FastCGI codec is `lib.l4` (plus an
optional tiny C codec for the integer fields); the serve loop and response
builders are `lib.l4`.

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

## Coroutines, generators, lazy sequences

Suspendable computations on the delimited-continuation machinery (`reset` /
`shift` / `resume`, already built). A coroutine yields control and resumes where
it left off; a generator is a coroutine that yields a stream of values pulled on
demand. The aim is **lazy sequences** — produce values only as a consumer asks,
so `map` / `filter` / `take` / `zip` compose over large or unbounded sequences
without materializing the whole thing.

**API:**

```
xt generator      ( -- gen )       \ a paused producer; resume to pull values
gen next          ( gen -- value gen' | done )  \ pull one value, or signal exhaustion
v yield           ( v -- )         \ inside a producer: emit v, then suspend
```

with lazy `map` / `filter` / `take` / `zip` as `lib.l4` wrappers that resume the
source on demand, and `lazy>array` to force a finite prefix. Cooperative tasks
are the same machinery used differently — `spawn` / `yield` / `run-scheduler`
round-robin several coroutines for producer/consumer pipelines and suspendable
simulations.

**Implementation:** `yield` is `shift`; `next` re-enters the captured slice; a
generator is a `T_CONT` plus an exhaustion sentinel; the scheduler is a queue of
`T_CONT`s. ~50 lines of `lib.l4` on the existing primitives — no new C.

No async-I/O layer sits above this: under the FastCGI model the web server
multiplexes connections, so coroutines here are for lazy data flow, not for
overlapping blocking syscalls.

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
