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
- **Worker pool (multi-core)** — once OS-thread actors exist (below), the
  accept loop hands each connection to a worker thread through a mailbox.

**Running from the REPL.** `serve` `fork()`s the server and returns
immediately; the REPL parent continues while the child runs its own accept
loop. The server runs a snapshot of fork-time definitions. Live
redefinition and shared in-process state need the co-scheduled green-task
build (non-blocking listen socket + stdin under a `select`/`kqueue`/`epoll`
readiness wait), which arrives with the worker-pool scale-up.

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

## Unification + nondeterminism (microKanren-flavored, on continuations)

A logic-programming layer on the existing continuation machinery: logic
variables, unification, `amb` / `fail` for choice and backtracking. The
substitution is implicit state (logic-var bindings + a trail) and search is
driven by continuations. Built on logicforth's own substrate — the trail,
delimited continuations, tagged values, and frames. A Java microKanren
(free-variation/archelogic `MicroKanren.java`) is the behavioral reference
for the relations (`conso`, `appendo`, `membero`, `conde`) and the
fact-database design.

**New machinery in C:**

- `T_LOGIC_VAR` tag, `OBJECT_LOGIC_VAR` kind carrying a name (for display)
  and a current binding (Val, or `T_NONE` if unbound).
- `make_logic_var()` / `object_new_logic_var()`.
- A trail stack of `(var, prior_binding)` pairs; every binding `unify`
  makes is recorded. Marks let `fail` undo to a known point.

**Primitives:**

- `lvar ( -- v )` — fresh logic variable.
- `deref ( v -- val )` — follow the binding chain; returns `v` if unbound,
  else the (recursively dereffed) bound value.
- `unify ( a b -- bool )` — try to unify; truthy on success (new bindings
  trailed), falsy on failure. Atoms by `val_cmp`. Arrays unify structurally
  and against a `[ H | T ]` cons pattern: an array of length ≥ 1 unifies
  with `H` bound to the head and `T` to a fresh tail array. Works both
  directions — decomposes a bound array, constructs from bound `H`/`T`.
  This is the sole head/tail mechanism. Sets, matrices, xt's, continuations
  unify by identity.
- **Frames unify as open records** — the logic-var destructuring mechanism
  for frames. A pattern frame constrains only the keys it names:
  `{ :name N :age A } { :name "Ann" :age 30 } unify` binds `N` and `A`;
  extra keys on either side are permitted; shared keys' values must unify
  (recursively, so nested patterns reach deep values). Path keys expand to
  nested pattern frames at construction time. A key named in one frame but
  absent in the other fails the unification. Frame `unify` is distinct from
  frame `=` (`val_cmp`, exact structural equality). `destruct` /
  `destruct-to` stay as the non-logic-var path.
- `trail-mark ( -- m )` and `trail-undo ( m -- )` — manage the trail across
  choice points; `fail` calls `trail-undo`.

**Library words (`lib.l4`, on `reset` / `shift` / `resume`):**

- `amb ( xt1 xt2 -- ... )` — try xt1; on `fail`, try xt2.
- `fail ( -- )` — undo bindings to the last `amb`, resume its continuation
  for the next branch; with no enclosing `amb`, an interpreter error.
- `once ( xt -- )` — run xt; commit on success (no backtracking through it).
- `fresh ( xt -- ... )` — introduce a fresh logic variable and pass it to
  xt.
- `run ( xt -- result )` — execute a goal, collect the first successful
  state's bindings.

**Sample:**

```forth
lvar lvar lvar                       \ X Y Z
[ 1 2 3 ] [ X Y Z ] unify  drop      \ success
X deref . Y deref . Z deref .        \ 1 2 3

lvar  [: 1 over unify drop :]
      [: 2 over unify drop :] amb
      deref .                         \ 1 (first branch wins)
```

**Cost:** ~140 lines of C (logic var, trail, `unify` with the `[H|T]` cons
pattern and open-record frames) plus ~30 lines of `lib.l4` for `amb` /
`fail` / `once` / `fresh` / `run`.

**Handle at implementation:**

- Occur check skipped (`X = [X]` makes a cyclic term) — match Prolog's
  default; document it.
- Variable keys in frames not allowed; only values can be logic variables.
- `forget` interaction: logic vars survive `forget`, but their namepool
  offsets can be invalidated — copy the name into the object's storage, or
  stop displaying names after `forget`.
- Image save/load: logic-var objects serialize like any other; the trail is
  session state and doesn't persist.

**Fact database:**

A relational fact store on frames and sets:

- **Rows** — a fact is an array of values; a relation is an array of rows.
- **Indices** — per indexed column, a frame mapping a column value to the
  set of row ids holding it. Indexed column values must be symbols (or
  interned with `string>symbol`); other columns fall back to a scan.
- **Query** — `query` is a nondeterministic goal. Indices narrow the
  candidates (intersect per-column index sets, smallest first); the goal
  unifies the pattern against each candidate row, succeeding once per match
  and backtracking on `fail`, composing with `amb` / `fail`.

**Words:** `assert` adds a fact (append the row, update each column index);
`retract` removes facts matching a pattern; `query` is the backtracking
goal. Exact stack shapes settled at implementation.

This needs one new primitive: an in-place set insert (`set-add!`) for
incremental `assert` (`set_add` already exists internally).

**Parallelism deferred.** A mutable trail can't be shared across parallel
branches, so parallel logic search is the isolated-interpreter + mailbox
model (below): one trail per worker, subproblems farmed out, solutions
returned by message. The whole logic layer is built and validated
single-threaded first.

Not in the first cut: constraint logic programming (finite domains,
intervals); tabling / memoization; negation as failure (`\+`); cut (`!`).

---

## Cooperative green threads (single OS thread)

Lightweight tasks within one OS thread, scheduled cooperatively via the
continuation machinery — for interleaved I/O-bound work (multiple network
requests, multiple SQLite queries, REPL-driven simulations).

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

Single-threaded: no parallelism, and a blocking syscall blocks the whole
scheduler. Each OS thread (below) has its own green-thread scheduler;
mailboxes work the same whether sender and receiver are green tasks in one
thread or in different OS threads.

---

## OS-thread parallelism via isolated interpreters + mailboxes

Multi-core parallelism on the Erlang actor model: multiple OS threads, each
owning its own `Interpreter`, no shared mutable state, communication via
per-thread mailboxes that deep-copy across the boundary. The
per-interpreter `Interpreter`/`Vocabulary` foundation is already in place,
so this layers on.

**New primitives:**

- `xt spawn-thread ( -- thread-id )` — fork a fresh `Interpreter` whose
  `Vocabulary` is cloned *tight* from the parent's compiled state (copy the
  used `dict` region and pools, capacity = `here`), so the worker inherits
  every word defined so far and the body xt's dict index resolves in the
  clone. The two vocabularies then evolve independently. Returns a
  `T_THREAD` handle.
- `thread-id join ( -- )` — wait for the thread to finish.
- `thread-id message send ( -- )` — enqueue the message in the target's
  mailbox. Non-blocking (mailbox unbounded).
- `receive ( -- message )` — pull the oldest message; block if empty.
- `xt receive-match ( -- message )` — pull the first message satisfying
  `xt` (an `( msg -- bool )` predicate), leaving others queued (selective
  receive).
- `self ( -- thread-id )` — this thread's ID.

**Cross-thread value semantics** (deep-copied into the receiver's
`objects[]`):

- `T_FLOAT`, `T_THREAD`: bit-for-bit.
- `T_SYMBOL`: travels as its byte name, re-interned in the receiver.
- `T_STRING`, `T_ARRAY`, `T_SET`, `T_FRAME`, `T_MATRIX`: deep copy.
- `T_XT`, `T_ADDR`, `T_CONT`, `T_STREAM`: not transmissible (they reference
  interpreter- or process-specific state); sending one is a type error.

**Mailbox storage.** Each `Interpreter` has its own mailbox: a queue of Vals
plus a mutex + condvar. Addresses are thread IDs indexing the live-thread
table.

**Cost:**

- Mailbox + send + receive + receive-match: ~80 lines.
- `spawn-thread` / `join` / `self`: ~80 lines (pthread wrappers, tight
  vocabulary clone, ID assignment).
- Deep-copy on send: ~100 lines, one case per object kind.
- Output coordination: a shared stdout mutex or a dedicated output thread.

Total ~350 lines.

Constraints: no shared mutable state (use a service thread that owns state,
reached by `self` + `send` + `receive`); no preemption within a thread;
sending a large object copies it; mailboxes are unbounded (a `receive` that
backs off when the mailbox is large is the user-level flow control).

**Build order:**

1. Green threads (~50 lines), on the existing continuations.
2. spawn-thread + mailboxes (~350 lines), on the existing per-interpreter
   interpreters.

---

## Array head/tail decomposition

Head/tail decomposition is the `[ H | T ]` cons pattern on `unify` (see
"Unification + nondeterminism"): `arr [ H | T ] unify` binds `H` to the head
and `T` to a fresh tail array; unifying a free variable with `[ H | T ]`
where both are bound builds the array. One declarative mechanism, both
directions. There are no `>head` / `head>` primitives, and no snoc-style
back-end primitive (`last` covers last-N in `lib.l4`).

Arrays are contiguous, so a recursive head-split walk allocates tail arrays
of sizes N-1, N-2, …, 1 — O(N²) churn. Fine for shallow, clause-shaped
decomposition; for large data iterate with the C-side `reduce` / `map`.

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
