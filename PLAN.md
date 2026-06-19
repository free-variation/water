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

### Still deferred

- **Wrappers** over the match/replace layer: `index-of`, `starts-with`,
  `ends-with`, `trim`, `lines`.

### Unicode model

UTF-8 throughout, codepoint-indexed at the user level:

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

Remaining:

- **Wildcard mutation** — `*` / `//` in `!` / `delete-at` / `update-at` for
  broadcast writes.
- **Quotation predicates** — an arbitrary `[: … :]` evaluated per node, built as an
  explicit element array.
- **Axes beyond child and descendant.**

### Time / dates

Unix timestamps as `T_FLOAT` (seconds since epoch, fractional allowed). No
separate date type; durations are floats in seconds, arithmetic is `+` /
`-`.

- `"%Y-%m-%d %H:%M:%S" time-format` — strftime-style, UTC by default.
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

### Standard streams

- `stdin` / `stdout` / `stderr` — the three standard streams as `T_STREAM`
  values (fds 0/1/2). Reading and writing reuse the subprocess stream
  words: `stdin read` slurps all of stdin, `stdin read "\n" split` its
  lines, `s stdout write` emits.

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
small printf-style mini-language on top of the positional `{n}` fill.

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
the records, runs a handler, writes the response.

**Instrumentation needed** — less than an in-process server, since the web server
keeps the HTTP work:

- `accept ( listen-stream -- conn-stream )` — accept a forwarded connection as a
  `T_STREAM`. By convention the web server passes the listen socket on fd 0
  (`FCGI_LISTENSOCK_FILENO`), so `bind`/`listen` may be unnecessary.
- `read-n ( stream n -- s )` — read exactly `n` bytes. A slurp-to-EOF read never
  terminates on a persistent FastCGI connection; records are length-framed, so a
  bounded read is required.
- A FastCGI record codec — decode `BEGIN_REQUEST` / `PARAMS` (the CGI environment
  → a request frame) / `STDIN` (body → a string), and encode `STDOUT` +
  `END_REQUEST`. The framing is simple: `lib.l4` over `read-n`/`write` plus byte
  arithmetic, with maybe a tiny C helper for the 2/4-byte length fields.

**Serve loop.** A plain sequential `accept → decode → handle → respond` loop in
`lib.l4`, each handler wrapped in `try-catch` so a bad request can't kill the
worker; per-request allocations are reclaimed by GC. No threads.

**Worker processes.** Run N worker processes all accepting on the same socket
(the kernel load-balances) under a process manager (e.g. systemd template units)
that respawns on crash. One request per worker isolates failures; the web server
retries elsewhere.

**SQLite.** Each worker opens its own connection; enable WAL once
(`PRAGMA journal_mode=WAL`) plus a `busy_timeout`, so concurrent reads across
workers don't block and writes serialize safely (single host).

**Cost:** `accept` + `read-n` are small C; the FastCGI codec is `lib.l4` (plus an
optional tiny C codec for the integer fields); the serve loop and response
builders are `lib.l4`.

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

Building on the generator primitives:

- Lazy `map` / `filter` / `take` / `zip` as `lib.l4` wrappers that resume the
  source on demand, with `lazy>array` to force a finite prefix.
- A cooperative scheduler (`spawn` / `run-scheduler`, a queue of `T_CONT`s) for
  producer/consumer pipelines.
- **Kanren-style interleaving streams.** A captured continuation is the
  suspension a miniKanren stream needs — force it with `resume` and it yields an
  answer or suspends again. Missing is fair interleaving: `mplus` (merge two
  streams so an infinite branch can't starve the other) and `bind` (flatMap with
  interleaving) — a *complete* search, distinct from the depth-first `amb` /
  `fail`. Generators are the substrate; the interleaving combinators are the work.

All `lib.l4` on the existing primitives — no new C.

Scope: lazy data flow, not async I/O.

---

## Multi-core parallelism: threads over the shared heap

Remaining work, in rough priority:

- **Persistent worker-thread pool.** `parallel_for` `pthread_create`s and joins
  the workers per region. For one big region that amortizes to nothing (a single
  `pmap` over a huge domain saturates the cores); for many small regions the
  spawn/join dominates — system time, not compute. A pool that parks threads and
  dispatches per call fixes it. Must be co-designed with the rewind: pooled
  threads keep their `AllocContext` across regions, so teardown has to reset
  every worker's context, not just the caller's.

- **Reclamation for live-heap-result regions.** The rewind covers regions whose
  results are transient. A region returning live heap objects can't rewind; its
  dropped output is ordinary garbage, but GC triggers on slot count and is blind
  to byte pressure, so a big-payload churn can exhaust the arena before GC fires.
  Fix is a byte-pressure GC trigger (rare, not a cadence). Not parallel-specific
  — a sequential big-payload loop has the same gap.

- **De-fragilize the region rewind.** `abort_parallel_region` restores three
  counters and resets the calling thread's context by hand; correctness depends
  on invariants maintained across files (the `in_parallel` gating, the
  per-region thread lifecycle). Folding the region's mutated state into one
  begin/commit/abort owner removes that coupling — and is a prerequisite for the
  thread pool.

- **Numeric disjoint-write buffer / work-stealing.** Lower priority: a shared
  unboxed-`double` output buffer threaded under the matrix kernels, and
  work-stealing for skewed workloads.

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
  by a computed key, under a distinct name from the existing column form.
- **`partition`** — `arr [: pred :] partition` → matches and non-matches.

Composable in one line, so not added: `count` (`[: pred :] filter size`),
`min-by` / `max-by` (`reduce` with comparison), `sum` / `product`
(`0 [: + :] reduce`).

**Cost:** `find`, `any?`, `all?`, `flat-map`, `sort-by`, `each` → ~60 lines
of `lib.l4`; `group-by` / `partition` build on frames.
