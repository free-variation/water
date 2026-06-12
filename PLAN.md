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

### Vendor PCRE2

The build links Homebrew's `libpcre2-8.a` through a hardcoded path. Restore
self-containment by vendoring the PCRE2 sources (amalgamation-style: ~40
`.c` files plus a generated `config.h` / `pcre2.h` / `pcre2_chartables.c`,
and sljit for the JIT).

---

## Core language additions

### Path keys in frame literals

A path (`/a/b/c`) in key position when building a frame is construction
sugar for nested frames: `{ /addr/city C }` builds `{ :addr { :city C } }`;
the stored frame has only symbol keys and nested frames. This reuses the
auto-vivify walk `!` performs (`frame_walk` in `WALK_VIVIFY` mode): the
literal applies walk-and-set per pair. Shared prefixes merge —
`{ /addr/city C /addr/zip Z }` builds `{ :addr { :city C :zip Z } }`.
Applies to `{ }` and `>frame` alike.

Because a path key expands to a nested pattern frame at construction time,
the open-records `unify` (below) matches deep values with no path-specific
machinery: `person { /addr/city C } unify` binds `C` to the deep value.

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

### File I/O — whole-file, no handles

- `read-file ( path -- string )` — read an entire file as one string
  (byte-safe; errors if missing).
- `write-file ( string path -- )` — create or truncate, then write.
- `append-file ( string path -- )` — open in append mode, write, close.

No file-handle type and no open / close / seek. The only fd-shaped things
in the language are the pipe and socket streams from `start-process` /
`serve` (`T_STREAM`). For line-oriented access, pipe through a command:
`[ "cat" path ] start-process read-out "\n" split`. ~15 lines of C each.

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

## SQLite integration

Embedded relational storage, built into the binary via the amalgamation
(`sqlite3.c` + `sqlite3.h` — one extra source file, public-domain). A small
Forth-side API opens databases, runs queries, and materializes results as
nested arrays or as matrices.

Three storage roles, one tool each: SQLite is the durable bulk store that
survives restarts and coordinates across processes (WAL); the fact database
is in-memory relations queried by unification, loaded from SQLite into a
working set; frames are the in-flight object shape and JSON wire mapping.

**API:**

- `"path.db" db-open` — open (create if needed), push a database handle.
- `db-handle db-close` — close the handle. Idempotent.
- `db-handle "SELECT ..." sql` — run, return a nested array (rows of
  cells). INTEGER / REAL → `T_FLOAT`, TEXT → `T_STRING`, NULL → sentinel,
  BLOB → `T_STRING` of raw bytes.
- `db-handle "SELECT ..." sql-matrix` — materialize directly into a
  `T_MATRIX`; errors if any cell isn't numeric.
- `db-handle "INSERT/UPDATE/DELETE ..." exec` — run a statement with no
  result set; return the affected row count as a float.

**Query construction:** build queries with `"...{0}..."` interpolation; no
separate bindings mechanism.

```
42 "SELECT * FROM users WHERE id = {0}" sql
```

String parameters are unescaped concatenation, so callers escape strings
before interpolating or use numeric values; a `sql-quote` helper for
quoted SQL literals can come later.

**Type:** new tag `T_DB` carrying the database handle, so `val_cmp` /
`print_val` and type errors stay specific.

**Storage:** a `databases[]` registry of `sqlite3 *` indexed by handle;
closed databases free the slot. A dropped handle leaks the connection until
process exit.

Not included initially: language-level prepared-statement caching, cursor
streaming, async, schema-introspection words (`db-tables`, `db-columns`).

---

## HTTP server

A `serve` word that stands up an HTTP/1.1 API server from a route table,
calling a user handler per matched route. Zero-dependency: the BSD socket
accept loop from libc (`socket` / `bind` / `listen` / `accept` / `recv` /
`send` / `close`), with request parsing by **picohttpparser** (one
public-domain `.c`/`.h`, vendored).

**Surface:**

```forth
[ :GET "/health" [: drop "ok" text :]
  :GET "/users/:id" [: :params :id @ lookup-user json :]
  :POST "/users" [: :body @ create-user json :] ]
8080 serve
```

The route table is an array of `[ method-symbol path-string handler-xt ]`
triples scanned in order; a trailing `:default` handles no-match (else a
built-in 404). Path segments written `:name` are captured into `:params`.

**Request** — the handler receives a frame:

```
{ :method :path :query :headers :params :body }
```

**Response** — the handler returns a frame:

```
{ :status :headers :body }
```

with `lib.l4` builders for the common cases — `text`, `json`, `ok`,
`not-found`, `status`.

**C primitives:**

- `port listen-on ( -- listen-stream )` — socket, `SO_REUSEADDR`, bind,
  listen.
- `listen-stream accept ( -- conn-stream )` — block for the next
  connection.
- `conn-stream recv-request ( -- request )` — recv, parse the request line
  and headers with picohttpparser, read the `Content-Length` body, build
  the request frame.
- `conn-stream response send-response ( -- )` — serialize the response
  frame to HTTP/1.1 bytes and send.
- `stream close ( -- )` — close a socket.

A socket is a `T_STREAM` (shared with the subprocess work). `serve` is a
`lib.l4` word: `listen-on`, then a loop of `accept` → handle → close.

**Concurrency — fork-per-connection.** Each accepted connection is handled
in a `fork()`ed child with a copy-on-write interpreter image; it services
the one request and exits. No shared mutable state, no locks; per-request
allocation dies with the process. Each child opens or inherits its own
SQLite connection, coordinated by WAL.

Two refinements on the same `serve`:

- **Prefork** — spawn N children all `accept`ing on the same socket; the
  kernel load-balances. Worker count is a `serve` parameter.
- **Worker pool (multi-core)** — a pool of OS threads, each running its own
  accept/handle loop on the shared listen socket; the kernel load-balances.
  Prefork's model inside one process. (The single-thread async server from
  the non-blocking I/O layer is the other scale-up.)

**Running from the REPL.** `serve` `fork()`s the server and returns
immediately; the REPL parent continues while the child runs its own accept
loop. The server runs a snapshot of fork-time definitions. Live
redefinition and shared in-process state need the async build (non-blocking
listen socket + stdin under a `kqueue`/`epoll` readiness wait), which arrives
with the non-blocking I/O layer.

To settle at implementation: request size cap and overflow behavior (413 vs
drop); whether `recv-request` returns a structured error or a sentinel for
a malformed request; repeated-header representation (array vs last-wins);
whether routing is C or a `lib.l4` scan.

HTTP/1.1 with `Content-Length` bodies, `Connection: close`. TLS terminates
at a reverse proxy. `:name` captures cover routing.

**Cost:** sockets + picohttpparser wrapper + request/response framing ~150
lines of C; `serve`, router, and response builders ~60 lines of `lib.l4`.

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

## Logic layer — remaining work

- **Fact database** — a relational store over arrays of rows. `query` collects
  *all* matches, so it's a deterministic loop with trail rollback (committed
  `amb` yields only one solution): for each row, mark the bind trail, `unify` the
  pattern against the row, collect the reified match on success, roll the trail
  back, continue. Per-column indices (a frame from a symbol value to a set of row
  ids) narrow the candidate rows before the scan. `assert` appends a row and
  updates each index; `retract` drops matching rows. Builds on an in-place set
  insert (`set-add!`).

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

- **`group-by`** — `arr [: ( elt -- key ) :] group-by` → frame from key to
  array of elements (keys must be symbols).
- **`partition`** — `arr [: pred :] partition` → matches and non-matches.

Composable in one line, so not added: `count` (`[: pred :] filter size`),
`min-by` / `max-by` (`reduce` with comparison), `sum` / `product`
(`0 [: + :] reduce`).

**Cost:** `find`, `any?`, `all?`, `flat-map`, `sort-by`, `each` → ~60 lines
of `lib.l4`; `group-by` / `partition` build on frames.
