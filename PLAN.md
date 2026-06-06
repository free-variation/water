# logicforth — deferred work

Tracks work that's planned or pending. Completed items are deleted; the
git history is the place to look for what's been built.

---

## Matrix — remaining work

### argmax / argmin

Index of the maximum / minimum element (or an `(i, j)` pair). Deferred
until there's a concrete use case; the additive and max/min reductions
already cover aggregation.

### Beyond core

Deferred until there's a specific use case:

- **Slicing** — submatrix by `(row-start, row-end, col-start, col-end)`.
- **Concatenation** — `hstack` (side by side), `vstack` (stacked).
- **Element-wise comparison** — `<` etc. returning a matrix of
  `1`/`0` booleans. Cleanest as polymorphic extension of the existing
  comparison words.
- **Norms** — `norm` (L2), `frobenius-norm`. Composable from existing
  reductions but common enough to deserve named words.
- **SVD** — one-sided Jacobi as the starting point. ~50–100 lines.
  No LAPACK in the default build, so hand-rolled — but see the optional
  BLAS/LAPACK build below, which would supply it directly.

### Optional BLAS/LAPACK build

An optional build that replaces the autochthonous matrix kernels with
BLAS/LAPACK. The default build stays zero-dependency and self-contained (the
project ethos); this is opt-in behind a build switch, leaving the `Matrix`
representation and the word surface unchanged — only the implementations
behind the primitives swap, so behaviour and the test corpus are unaffected.

- **BLAS** for the compute kernels: DGEMM (all four transpose variants) →
  `cblas_dgemm`, and the reduction / element-wise loops where a BLAS routine
  fits.
- **LAPACK** unlocks the linear-algebra ops the zero-dep build can't
  reasonably match — SVD (above), solve / least-squares, inverse, eigen.

Open questions to settle before implementing: which interface (reference
CBLAS/LAPACKE vs a vendor lib — OpenBLAS / MKL / Accelerate); how the switch
is wired (a `Makefile` target and/or `#ifdef`); and row-major vs column-major
handling at the boundary.

---

## String operations

A single pattern-matching primitive subsumes the usual string-handling
zoo (split / index-of / starts-with / ends-with / trim / lines / replace),
with named operations as `lib.l4` wrappers. The engine is **PCRE2**
(Perl-compatible, JIT-compiled, statically linked).

POSIX ERE was the original plan, but `bm_regex_dna` measurement settled it:
PCRE2 + JIT runs that benchmark's match phase ~2.9× faster than CPython,
where POSIX `regexec` was ~2× slower. PCRE2 is also a superset — `\d`, `\w`,
`\n` / `\t` in patterns, lookaround, `\p{...}` — so plain `"..."` patterns
express newlines directly and the harder regex benchmarks (v8 / effbot)
become portable.

**Built** (specs in `docs/reference.md`): `match` (first match as a flat
`[ whole cap1 … ]`, `0` on no match), `match-all`, `replace` (with the
`&` / `\0` / `\1`–`\9` / `\&` / `\\` substitution forms), the `has?` string
overload, `substring`, `join`; counting is `match-all size`. Matching is
single-pass over the whole subject with a `startoffset`; `compiled_pattern`
caches compiled-and-JITted patterns (64-entry round-robin); non-overlapping
leftmost enumeration with the zero-width +1 advance, matching Python
`findall` / awk `gsub` counts.

### Still deferred

- **Wrappers** over `match-all` / `replace`, in `lib.l4`: `split`,
  `index-of`, `starts-with`, `ends-with`, `trim`, `lines`.

### Unicode model — deferred

UTF-8 throughout, codepoint-indexed at the user level — *future work*.
Today every string operation is byte-indexed, which is exact for ASCII
(the common case: English, identifiers, CSV, DNA) and what the built words
do. When done:

- `size` (on a string), `substring`, `index-of`, and match positions
  expressed in *codepoints*, not bytes, via a small UTF-8 codec (encode /
  decode / count / advance-by-n). `pcre2_match` returns byte offsets;
  translate at the boundary. `size` stays the single count word — no
  separate `length`.
- **ASCII fast path**: when a string is all-ASCII (a cached per-string
  flag), codepoint offset equals byte offset, so the codec collapses to
  no-ops; the common case keeps byte-oriented speed.
- `setlocale(LC_CTYPE, "")` plus PCRE2's UCP mode for codepoint-aware
  classes.

Not covered, by design (document): normalization (NFC vs NFD stay
distinct), grapheme clusters (`.` matches one codepoint, so flag emoji /
ZWJ break the intuitive `size`), locale case-folding outside ASCII.
Unicode property classes (`\p{Letter}`) *are* available — PCRE2 supports
them under UCP. For full i18n use ICU; this model suffices for scientific
/ scripting work.

### Vendor PCRE2 — TODO

The build currently links Homebrew's `libpcre2-8.a` through a hardcoded
path, so it is no longer the zero-dependency `clang -O3` build it was.
Restore self-containment by vendoring the PCRE2 sources (the
SQLite-amalgamation approach, heavier: ~40 `.c` files plus a generated
`config.h` / `pcre2.h` / `pcre2_chartables.c`, and sljit for the JIT).
Until then, building requires PCRE2 installed.

---

## Core language additions

### Path keys in frame literals

Allow a path (`/a/b/c`) in key position when building a frame, as
construction sugar for nested frames. `{ /addr/city C }` builds
`{ :addr { :city C } }`; the stored frame still has only symbol keys and
nested frames — the path never persists as a key. This reuses the
auto-vivify walk `!` already performs (`frame_walk` in `WALK_VIVIFY`
mode): the literal applies walk-and-set per pair instead of the
symbol-keyed `frame_put`. Shared prefixes merge rather than clobber,
because vivify reuses an existing intermediate frame: `{ /addr/city C
/addr/zip Z }` builds `{ :addr { :city C :zip Z } }` from the two pairs
left to right. Applies to `{ }` and `>frame` alike, not only to patterns.

The payoff is path-based destructuring for free: because a path key
expands to a nested pattern frame at construction time, the open-records
`unify` (below) matches deep values with no path-specific machinery —
`person { /addr/city C } unify` binds C to the deep value, and unify
never sees a path, only the nested frame it expanded to.

### Time / dates

Unix timestamps as `T_FLOAT` (seconds since epoch, fractional allowed).
No separate date type — durations are just floats in seconds, arithmetic
is the existing `+` / `-`.

- `now` — current time as float.
- `"%Y-%m-%d %H:%M:%S" time-format` — strftime-style format, UTC by
  default.
- `"2026-05-25" time-parse` — strptime-style parsing.
- `"%Y-%m-%dT%H:%M:%SZ"` is the recommended ISO 8601 format string.

### Random numbers

Small high-quality PRNG (xoshiro256++ or PCG, ~30 lines, far better
than `rand()`).

- `random-float` — uniform `[0, 1)`.
- `min max random-int` — uniform integer in `[min, max]`.
- `seed seed!` — set the RNG seed for reproducibility.
- `array shuffle` — return a new array with the elements randomly
  permuted (Fisher-Yates).
- `array sample` — return a uniformly random element. Likely a `lib.l4`
  word over `random-int` + indexed access, not a C primitive.

### Sort

- `array sort` — return a new array sorted by the existing `val_cmp`
  ordering; the input is untouched (consistent with `map` / `filter` /
  `take` / `reverse` / `concat`, which all return fresh arrays).
- `array [ x y -- cmp ] sort-with` — same, with a user comparator
  quotation that pops two Vals and pushes `-1` / `0` / `1`.
- Algorithm: introsort or just `qsort` from libc with a comparator
  thunk. ~30 lines either way.

### stdin / env

- `read-line` — one line from stdin as a string; empty string at EOF.
- `read-all` — all of stdin as one string.
- `"VAR" env` — environment variable value as a string; empty string
  if unset.

(`argv` deliberately not included — invocation is `logicforth file.l4`
and command-line argument handling lives at the shell-script wrapper
layer, not in the core language.)

### Error handling — `catch` intercepts `error_flag`

Interpreter-level errors (the `error_flag` path: stack underflow, type
mismatch, division by zero, bad pattern, out-of-bounds) must be catchable,
not only user `throw`s. `catch` / `try-catch` intercept `error_flag`:
after running the wrapped xt, if `error_flag` is set, `catch` clears it and
returns the error (the `error_message` as a string) with the failure flag,
exactly as if the primitive had `throw`n. This is what lets a long-running
service recover from a faulty handler — catch the error, return a 500, keep
serving — rather than aborting to the REPL. Uncaught errors still surface at
the REPL as before. ~10 lines: a check in the `catch` path that converts a
set `error_flag` into the same `(exc 1)` result a `throw` produces.

### File I/O — whole-file, no handles

- `read-file ( path -- string )` — read an entire file as one string
  (byte-safe; errors if the file is missing).
- `write-file ( string path -- )` — create or truncate, then write.
- `append-file ( string path -- )` — open in append mode, write, close.

No file-handle type and no open / close / seek: files are whole values.
The only fd-shaped things in the language are the pipe and socket fds from
`start-process` / `serve` (`T_FD`); you never `open` a file. The cases
whole-file reads don't fit — a file too large for memory, or line-by-line
streaming — go through a pipe instead (`[ "cat" path ] start-process` then
`read-line`), the
same streaming model subprocesses already use. ~15 lines of C each.

### Interpolation format specs

Extend the existing `"... {0} ..."` interpolation with optional format
specifiers after a colon in the placeholder: `{0:.2f}` (fixed precision),
`{0:8}` (field width / padding), `{0:x}` (hex), and so on — a small
printf-style mini-language. Reuses the interpolation already in the
interpreter; no new word. Covers report output, HTTP / JSON response
formatting, and TSV cells. The bare `{0}` keeps today's default rendering
(integer-valued floats as integers, else `%g`; strings and symbols as
their text).

---

## TSV file I/O

Read and write tab-separated-value files. Both numeric and non-numeric content.

**TSV is the only tabular I/O format logicforth will ever support.** Not
a starting point, not a default — the entire story. No CSV reader. No
JSON. No Parquet, Arrow, HDF5, anything. If a user has data in another
format, they convert it to TSV outside logicforth before loading.

TSV is just "split on tab, split on newline." A reader fits in 40
lines. A writer fits in 20. Whatever cleanup the data needs to fit
that model (escaping tabs, fixing newlines in fields) is the user's
problem before the file gets to logicforth, not the interpreter's.

**Sketch:**

- Reader: `"file.tsv" read-tsv` → array of arrays. Each row is an array
  of cells. Numeric-looking cells become `T_FLOAT`; everything else stays
  a string. Caller can post-process if they want stricter typing.
- Writer: `arr-of-arrs "file.tsv" write-tsv`. Vals are printed using the
  same logic as `print_val`, except tabs and newlines inside a cell would
  break the format — initial version errors out if encountered.
- Header row handling: pass-through (the first row is just another row).
  Higher-level "give me a named-column table" can be a user-level word
  built on top.

**Status:** not started.

**Open questions for later:**

- What about Vals that don't have a clean TSV representation — sets,
  arrays, xt's, matrices? Probably emit a sentinel or error; don't
  silently lossy-encode. Matrix could naturally serialize as its
  flattened elements followed by `rows cols matrix` — same form `save`
  will use.
- Should there be a streaming reader for large files, or always load
  whole? Whole-file is fine for the sizes we're likely to hit.

---

## SQLite integration

Embedded relational storage for logicforth. Build SQLite into the
binary via the amalgamation (`sqlite3.c` + `sqlite3.h` — one extra
source file, public-domain, zero external dependencies). Expose a
small Forth-side API for opening databases, running queries, and
materializing results either as nested arrays (heterogeneous types)
or as matrices (when all columns are numeric).

**Division of labor with the fact DB and frames** (they are not
redundant): SQLite is the durable bulk store that survives restarts and
coordinates across processes; the fact database (logic layer) is in-memory
relations queried by deterministic logical lookup / unification — loaded
from SQLite into a working set; frames are the in-flight object shape and
the JSON wire mapping. Durable storage, logical retrieval, and object
representation are three jobs, one tool each.

**Why this:**

- Pure C, drops cleanly into the existing build with one extra
  compile step.
- Real SQL, real transactions, real concurrency via WAL — handles
  multi-process access correctly, so the hypothetical fork-per-request
  server model works without extra coordination.
- Per-call overhead is microseconds for indexed lookups. Open the DB
  once per worker, prepare statements once, reuse. The interpreter
  doesn't need any new threading or event-loop machinery — fork() at
  the network layer if concurrent users are wanted.

**API sketch:**

- `"path.db" db-open` — opens (creates if needed), pushes a
  database handle.
- `db-handle db-close` — closes the handle. Idempotent.
- `db-handle "SELECT ..." sql` — runs the query, returns a nested
  array: outer array is rows, each row is an array of cells with
  appropriate Val types. SQLite INTEGER and REAL become `T_FLOAT`,
  TEXT becomes `T_STRING`, NULL becomes a sentinel (`T_NONE` or a
  reserved symbol), BLOB becomes a `T_STRING` of raw bytes.
- `db-handle "SELECT ..." sql-matrix` — same, but materializes
  directly into a `T_MATRIX` (no intermediate array allocation).
  Errors if any cell isn't numeric. Faster path for analytics.
- `db-handle "INSERT/UPDATE/DELETE ..." exec` — runs a statement
  with no result set. Returns the affected row count as a float.

**Query construction:**

Use the existing `"...{0}..."` string interpolation to build queries.
No separate bindings mechanism — the user formats the SQL string
themselves before passing it to `sql`:

```
42 "SELECT * FROM users WHERE id = {0}" sql
```

Keeps the API surface tiny and reuses the interpolation that already
exists in the interpreter. SQLite's parser then handles the assembled
string normally.

**SQL injection caveat.** Because there's no parameterized binding,
any string-typed input that flows into a query interpolation is
unescaped concatenation. For single-user / trusted-input use that's
fine. For the hypothetical multi-user server case, the user has to
manually quote-escape any string parameter before interpolating —
or use only numeric values. A small `sql-quote` helper that produces
properly-quoted SQL literals would be a useful library word later.

**Type representation:**

New tag `T_DB` carrying the database handle — keeps type errors specific
("`sql` requires a database, got a string") and lets `val_cmp` /
`print_val` handle it.

**Storage:**

Separate registry similar to `objects[]` — `databases[]` array of
`sqlite3 *` pointers, indexed by handle. Closed databases free the
slot. GC doesn't need to do anything special; if the user drops a
handle from the stack without calling `db-close`, the connection
leaks until process exit. Acceptable.

**Out of scope** (at least initially):

- Prepared statement caching at the language level. SQLite's parser is
  fast enough that re-preparing every query is fine for low-rate
  workloads; can revisit if profiling shows it matters.
- Cursor-style streaming reads. The current API materializes the whole
  result set. For huge tables that fits poorly; defer until needed.
- Async / non-blocking. Single-threaded interpreter, SQLite is
  synchronous, that's fine.
- Schema introspection words (`db-tables`, `db-columns`). Easy to add
  later; they're just specific SELECTs against `sqlite_master`.

---

## HTTP server

A `serve` word that stands up an HTTP/1.1 API server from a route table,
calling a user handler per matched route. Zero-dependency: the server is the
BSD socket accept loop from libc (`socket` / `bind` / `listen` / `accept` /
`recv` / `send` / `close`), and request parsing uses **picohttpparser** — one
public-domain `.c`/`.h`, vendored like the SQLite amalgamation. No third-party
HTTP library: an embedded server framework would bring its own event loop and
threading model that fight logicforth's.

**Surface:**

```forth
[ :GET "/health" [: drop "ok" text :]
  :GET "/users/:id" [: :params :id @ lookup-user json :]
  :POST "/users" [: :body @ create-user json :] ]
8080 serve
```

The route table is an array of `[ method-symbol path-string handler-xt ]`
triples, scanned in order; a trailing `:default` entry handles no-match
(otherwise a built-in 404). Path segments written `:name` are captured into the
request's `:params`.

**Request** — the handler receives a frame:

```
{ :method :path :query :headers :params :body }
```

**Response** — the handler returns a frame:

```
{ :status :headers :body }
```

with `lib.l4` builders for the common cases — `text` (200 text/plain), `json`
(200 application/json), `ok`, `not-found`, `status` — so a handler body stays
one line.

**C primitives:**

- `port listen-on ( -- listen-fd )` — socket, `SO_REUSEADDR`, bind, listen.
- `listen-fd accept ( -- conn-fd )` — block for the next connection.
- `conn-fd recv-request ( -- request )` — recv, parse the request line and
  headers with picohttpparser, read the `Content-Length` body, build the
  request frame.
- `conn-fd response send-response ( -- )` — serialize the response frame to
  HTTP/1.1 bytes and send.
- `fd fd-close ( -- )` — close a socket.

A file descriptor is a dedicated `T_FD` tag, so type errors stay specific and
`print_val` can render it. `serve` itself is a `lib.l4` word over these
primitives: `listen-on`, then a loop of `accept` → handle → close.

**Concurrency — fork-per-connection.** Each accepted connection is handled in a
`fork()`ed child that owns a copy-on-write image of the interpreter, services
the one request, and exits. No shared mutable state, no locks, and a crashing
handler takes down only its own child. Per-request allocation never accumulates
— the process dies after the response. This fits the SQLite-with-WAL story
directly: each child opens or inherits its own connection and the WAL
coordinates writers across processes.

Two refinements layer onto the same `serve`:

- **Prefork.** Spawn N children that all `accept` on the same listening socket;
  the kernel load-balances among them, removing the per-request `fork()` cost.
  The worker count is a `serve` parameter.
- **Worker pool (multi-core).** Once OS-thread actors exist (see the OS-thread
  parallelism section), the accept loop hands each connection's fd to a worker
  thread through a mailbox, sharing one process across cores. The socket /
  parse / router / handler core is unchanged; only the dispatch strategy swaps.

**Running from the REPL.** The accept loop's `accept()` is a blocking syscall,
and so is the REPL's own stdin read, so a server started inline would freeze the
REPL inside the kernel. Two ways keep the prompt interactive:

- **Background process.** `serve` `fork()`s the server and returns immediately;
  the REPL parent keeps going while the child runs its own blocking accept loop
  (forking again per connection). Zero extra machinery. The trade is that the
  server runs a snapshot of fork-time definitions — redefining a route at the
  REPL does not reach the running server.
- **Co-scheduled green task.** Run the accept loop as a green task in the REPL
  process. This requires the listening socket *and* stdin to be non-blocking and
  a `select`/`kqueue`/`epoll` readiness wait covering both, so the scheduler
  resumes the accept task only when a connection is pending and the REPL only
  when stdin is ready. This is the event-loop build noted under the worker-pool
  scale-up; in exchange, handlers can be redefined live and share in-process
  state with the REPL.

**Out of scope:**

- **TLS.** Terminate at a reverse proxy (nginx, Caddy). In-process TLS would
  pull in OpenSSL — a large dependency against the zero-dep ethos.
- **Keep-alive.** The first cut closes the connection after each response
  (`Connection: close`), which fork-per-connection makes natural. Persistent
  connections are a later refinement.
- **Wildcard / regex routes.** `:name` captures cover the common case; richer
  patterns can use the regex string layer (`match`).
- **HTTP/2, chunked transfer, websockets.** HTTP/1.1 with `Content-Length`
  bodies only.

**Open questions:**

- Request size cap and the behavior on overflow (413 vs. connection drop), and
  a sane default ceiling.
- Whether `recv-request` returns a structured error for a malformed request or
  a sentinel the loop turns into a 400.
- Header representation when a header repeats (array of values vs. last-wins).
- Whether routing lives in C (for large tables) or stays a `lib.l4` scan
  (simpler; fine for modest route counts).

**Cost:** sockets + picohttpparser wrapper + request/response framing ~150
lines of C; `serve`, the router, and the response builders ~60 lines of
`lib.l4`. picohttpparser vendored as-is.

---

## Subprocesses and pipes

Start an external program and talk to it over pipes — the basis for doing
filesystem, network, and system work by driving standard binaries (`cat`,
`tee`, `ls`, `curl`, …) instead of growing a native primitive for each. Zero
dependency: `fork` / `execvp` / `pipe` / `waitpid` from libc. Pipes are raw byte
streams, so this is binary-safe (logicforth strings are length-counted byte
buffers, not NUL-terminated).

A process is launched from an argv array — no shell, so no quoting or injection
surface:

```forth
[ "cat" "/etc/hosts" ] start-process   ( -- proc )
```

`start-process` returns a frame `{ :pid :in :out :err }`: `:pid` the child's process id,
`:in` a writable fd for its stdin, `:out` / `:err` readable fds for its stdout
and stderr. File descriptors carry a `T_FD` tag (shared with the HTTP-server
work).

**Words:**

- `argv start-process ( argv -- proc )` — fork/exec, return the process frame.
- `string fd write ( -- )` — write the string's bytes to a child's `:in`.
- `fd read-line ( -- string )` — one line from `:out`/`:err`; empty at EOF.
- `fd read-all ( -- string )` — everything until EOF.
- `fd close ( -- )` — close an fd; closing `:in` sends the child EOF.
- `pid wait ( -- status )` — block until the child exits, return its status.
- `pid stop ( -- status )` — signal the child, then reap it; for aborting a
  process that won't finish on its own.

**Concurrent outbound calls.** `start-process` is non-blocking — it forks
and returns the process frame immediately; only `read-all` blocks. So
fan-out is start-all-then-drain-all: `start-process` N children (they run
concurrently as OS processes), then `read-all` each. Wall time is the
slowest call, not the sum. This is how concurrent LLM calls are done — no
green threads or non-blocking pipe I/O required (the deferred non-blocking
work is only for reacting to whichever child finishes *first*, which
fan-out-and-collect doesn't need). Cap concurrency to vendor rate limits
with a `lib.l4` loop that starts processes in batches of N; cap inbound
concurrency with the HTTP server's prefork worker count.

The normal lifecycle is start-process → `write` input → `close` `:in` → `read-all` →
`wait`. Use `wait`, not `stop`, after draining output: EOF on stdout doesn't
mean the child is finished (e.g. `cat > file` produces no stdout but is still
writing to disk, and killing it there truncates the file), and `wait` returns
the program's real exit code where a signal kill would not.

**Built on top, in `lib.l4`:** `llm-call` (`curl`), and so on — the C
surface stays small and the conveniences are Forth definitions over it.
(Plain file read/write is *not* here — `read-file` / `write-file` /
`append-file` are native whole-file primitives, not `cat`/`tee` wrappers;
see "File I/O".)

**Out of scope:**

- **Shell interpretation.** argv only; no `/bin/sh -c`, so no `|`, glob, or
  redirect inside a command string.
- **Multi-stage pipelines** in the primitive. Wiring one child's `:out` into
  another's `:in` is a later concern (and buffers through logicforth); the
  driving cases are single binaries.
- **Concurrent bidirectional streaming.** Interleaving `write` and `read` on a
  live child can deadlock when a pipe's fixed buffer fills; the supported
  pattern is write-then-drain. Full duplex would need `select`/threads. (A
  child that block-buffers stdout when it's a pipe rather than a tty will also
  stall a line-by-line loop — the `stdbuf` gotcha.)
- Windows. POSIX only.

**Cost:** ~150 lines of C (start-process, the fd read/write/close words,
wait/stop) plus the `T_FD` tag; the LLM convenience words are `lib.l4`.

---

## JSON ↔ frame

Two words, with JSON carried as a string: `json>frame ( string -- value )`
parses, `frame>json ( value -- string )` serializes. JSON objects map to frames
(symbol keys), arrays to arrays, strings to strings, numbers to floats,
`true`/`false` to the boolean floats, `null` to a sentinel. Needed to consume
HTTP/LLM API responses (`curl` output) and build request bodies.

The mapping isn't quite 1:1 — object keys are strings in JSON but symbols in a
frame (intern on parse), and `null` needs a representation (`T_NONE` or a
reserved symbol). Settle those at implementation time.

**Decision: hand-roll, no vendored library.** The JSON grammar is small enough
(RFC 8259 fits on a page) that a one-pass recursive-descent parser building Vals
directly is ~200-400 lines of C with no dependency. That keeps the type mapping
above (numbers → `T_FLOAT`, objects → frames, `null` → sentinel) under our exact
control, which a generic library's tree would obscure. `frame>json` is a
`print_val`-style recursive walk.

---

## Foreign function interface

Once in, user code can load any `.so` / `.dylib` on the system, look
up symbols by name, declare a C signature at the Forth level, and call
— with nothing about the target library known at logicforth's compile
time. Targets like LAPACK, full PCRE, libcurl, libgit2 become bindable
without writing per-library C code.

Mechanism: link against `libdl` (for `dlopen` / `dlsym`) and `libffi`
(for runtime-described calls). User code declares each function's
signature; libffi handles per-architecture calling-convention details
at call time.

Performance: `libffi` adds ~30-100 ns per call vs ~1 ns for a static
native call. Negligible for chunky-operation libraries (matmul, regex
compile, DB query); meaningful only for tight loops calling trivial C
functions.

Implementation cost: ~250-400 lines of C glue. Build adds `-ldl -lffi`.

Open questions to settle at implementation time: word-level API
surface; signature declaration syntax; the C type set the marshalling
supports; how to represent opaque C pointers in the Val tag space;
ownership of C-allocated buffers; whether to support callbacks from C
back into logicforth; whether to support struct-by-value arguments.

---

## Unification + nondeterminism (microKanren-flavored, on continuations)

Now that delimited continuations are in, a logic-programming layer is
tractable: logic variables, unification, `amb` / `fail` for choice and
backtracking. The flavor is closer to Prolog than to the faithful
microKanren stream-of-states model — the substitution is implicit
state (logic-var bindings + a trail), and search is driven by
continuations rather than by mapping goals over streams. The name
"logicforth" finally earns its second half.

Built on logicforth's own substrate — the trail, delimited continuations,
tagged values, and frames. A Java microKanren (free-variation/archelogic
`MicroKanren.java`) is a behavioral reference for the relations (`conso`,
`appendo`, `membero`, `conde`) and the fact-database design; the control
structure is logicforth's own.

**New machinery in C:**

- `T_LOGIC_VAR` tag, `OBJECT_LOGIC_VAR` kind carrying a name (for
  display) and a current binding (Val, or `T_NONE` if unbound).
- `make_logic_var()` / `object_new_logic_var()`.
- A trail stack of `(var, prior_binding)` pairs. Every binding made by
  `unify` is recorded. Marks on the trail let `fail` undo to a known
  point without disturbing earlier bindings.

**Primitives:**

- `lvar ( -- v )` — fresh logic variable.
- `deref ( v -- val )` — follow binding chain; returns `v` itself if
  unbound, else the bound value (recursively dereffed).
- `unify ( a b -- bool )` — try to unify; returns truthy on success
  (with any new bindings trailed), falsy on failure. Atomic equality
  via existing `val_cmp`. Arrays unify structurally (same length, then
  element-wise) and also against a `[ H | T ]` cons pattern: an array of
  length ≥ 1 unifies with H bound to the head and T to a fresh tail array
  of the remaining elements. This works both directions — `arr [ H | T ]
  unify` decomposes (binds H and T), and unifying a free variable with
  `[ H | T ]` where H and T are bound constructs the array. This cons
  pattern is the sole head/tail mechanism; there are no `>head` / `head>`
  primitives (see "Array head/tail decomposition"). Sets, matrices, xt's,
  continuations only unify by identity.
- **Frames unify as open records**, and this is the logic-var
  destructuring mechanism for frames: a pattern frame constrains only
  the keys it names. `{ :name N :age A } { :name "Ann" :age 30 }
  unify` binds N and A; extra keys on either side are permitted. Shared
  keys' values must unify (recursively, so nested patterns reach deep
  values). Path keys in the pattern literal (`{ /addr/city C }`, see
  "Path keys in frame literals") expand to a nested pattern frame at
  construction time, so deep destructuring needs no path-specific
  machinery in `unify` — it only ever sees the nested frame. A key named
  in one frame but absent in the other makes the unification **fail** — a
  var only binds where its key exists. This
  makes frame `unify` deliberately distinct from frame `=` (`val_cmp`),
  which stays exact structural equality; matching is not equality.
  `destruct` / `destruct-to` stay as the faster non-logic-var path for
  pulling a frame or array apart without introducing logic variables.
- `trail-mark ( -- m )` and `trail-undo ( m -- )` — for managing the
  trail across choice points. `fail` ultimately calls `trail-undo`.

**Library words (lib.l4, built on `reset` / `shift` / `resume`):**

- `amb ( xt1 xt2 -- ... )` — try xt1; if it `fail`s, try xt2. Captures
  a continuation at the choice point; `fail` resumes it.
- `fail ( -- )` — undo bindings back to the last `amb`, invoke its
  saved continuation to try the next branch. If there's no enclosing
  `amb`, surfaces as an interpreter error.
- `once ( xt -- )` — run xt; if it succeeds, commit (no backtracking
  through it). Just sugar over `reset` + early exit.
- `fresh ( xt -- ... )` — introduces a fresh logic variable and passes
  it to xt. Sugar; `lvar swap execute` works without it.
- `run ( xt -- result )` — convenience for "execute a goal and collect
  the first successful state's bindings."

**Sample:**

```forth
\ Sample query: pattern-match a list.
lvar lvar lvar                       \ X Y Z
[ 1 2 3 ] [ X Y Z ] unify  drop      \ success
X deref . Y deref . Z deref .        \ 1 2 3

\ Choice point:
lvar  [: 1 over unify drop :]
      [: 2 over unify drop :] amb
      deref .                         \ 1 (first branch wins)
\ later, fail to get 2
```

**Cost:** ~140 lines in C (logic var, trail, the `unify` primitive with
the `[H|T]` cons pattern and open-record frames) plus ~30 lines of
`lib.l4` for `amb` / `fail` / `once` / `fresh` / `run`. The fact database
adds the `set-add!` primitive plus its `assert` / `retract` / `query`
words on top. Assumes continuations are working (they are).

**Subtleties:**

- **Occur check skipped** — `X = [X]` makes a cyclic term and may
  loop on later use. Match Prolog's default; document the gotcha.
- **Variable keys in frames not allowed** — same restriction Prolog
  has for compound functors. Only values can be logic variables.
- **Trail interaction with `forget`** — logic vars are objects and
  survive `forget` like any other heap value, but their names (kept
  as `namepool` offsets) might be invalidated. Either copy the name
  into the object's own storage, or stop displaying names after
  `forget` runs.
- **Image save/load** — logic-var objects serialize like any other;
  the trail is session state and doesn't need to persist.

**Fact database:**

A relational fact store is part of the logic layer, built on frames and
sets rather than new C structures:

- **Rows** — each fact is an array of values; a relation is an array of
  those rows.
- **Indices** — per indexed column, a frame mapping a column value to the
  set of row ids holding it. Frame keys are symbols, so indexed column
  values must be symbols (or interned with `string>symbol`). This matches
  the Datalog convention of relations over atoms; columns holding numbers
  or compound terms fall back to a scan.
- **Query** — `query` is a nondeterministic goal, not an eager fetch.
  The indices first narrow the candidates: intersect the per-column
  index sets for the bound terms (smallest first, via `intersection`).
  The goal then unifies the pattern against each candidate row in turn,
  succeeding once per match and backtracking to the next on `fail`, so it
  composes with `amb` / `fail` like any other goal. Logic-var terms in the
  pattern bind to the row's values on each success and unbind on backtrack.

**Words:** `assert` adds a fact to a relation (appending the row and
updating each column index); `retract` removes facts matching a pattern;
`query` is the backtracking goal above — it succeeds once per matching
row, binding the pattern's logic vars, and resumes the search on `fail`.
Exact stack shapes (how a relation is named and its columns declared) are
settled at implementation time.

The one primitive this needs that doesn't exist yet is an in-place set
insert (`set-add!`) for incremental `assert`, since today's set words all
allocate a new set. `set_add` already exists internally; exposing a
mutating word is a small addition.

**Parallelism is deferred, and the trail forces how:**

A mutable trail cannot be shared across parallel search branches — two
branches binding the same variable would race. So parallel logic search is
not branch-level concurrency over one substitution; it is the isolated-
interpreter + mailbox model (see "OS-thread parallelism" below), one trail
per worker, subproblems farmed out and solutions returned by message.
Continuations and the trail give backtracking *within* a single worker;
threads give independent workers. The entire logic layer — unify, trail,
`amb`/`fail`, the fact database — is built and validated single-threaded
first; the worker/mailbox layer is added underneath later.

**Out of scope for the first cut:**

- Constraint logic programming (CLP) — finite domains, intervals.
- Tabling / memoization of goals.
- Negation as failure (`\+`). Easy to add as a library word; defer.
- Cut (`!`). Sugar over committing-once patterns; defer.

---

## Cooperative green threads (single OS thread)

Lightweight tasks within a single OS thread, scheduled cooperatively
via the existing continuation machinery. Useful for interleaved I/O-
bound work — multiple network requests, multiple SQLite queries,
REPL-driven simulations — without OS-thread overhead and without any
synchronization concerns (the single thread serializes everything).

**API:**

```
xt spawn          ( -- )           \ schedule xt as a green task
yield             ( -- )           \ pause this task; scheduler picks next
run-scheduler     ( -- )           \ drive the queue until empty
```

**Implementation:**

- A scheduler queue: a per-process array of `Val`s of tag `T_CONT`,
  each representing a paused task.
- `spawn` captures a continuation that will execute the given xt and
  pushes it onto the queue.
- `yield` is `shift` under the hood: captures the current task's
  continuation, pushes it onto the queue, then `resume`s the next
  task in the queue.
- `run-scheduler` drives the queue: dequeue, resume, repeat until
  empty.

**Cost:** ~50 lines on top of `reset` / `shift` / `resume`. Composes
naturally with the existing continuation machinery.

**Limitations:**

- No parallelism. CPU-bound tasks starve siblings until they `yield`.
- Blocking syscalls (read, sleep) block the entire scheduler.
- Useful for I/O-bound concurrency, useless for compute-bound.

**Composes with the OS-thread story below.** When path B exists, each
OS thread has its own green-thread scheduler. Mailboxes work the same
way whether the sender and receiver are green tasks in the same
thread (local mailbox access, no mutex needed) or in different OS
threads (mutex + deep-copy across).

---

## OS-thread parallelism via isolated interpreters + mailboxes (path B)

Real multi-core parallelism. Multiple OS threads, each owning its own
`Interpreter` instance. No shared mutable state. Communication strictly
via per-thread mailboxes; sending deep-copies the value across the
boundary.

The Erlang actor model.

The per-interpreter `Interpreter`/`Vocabulary` foundation is already in
place, so this is a layered-on follow-on rather than a rewrite: each OS
thread runs its own owned interpreter, with no shared mutable state.

**New primitives:**

- `xt spawn-thread ( -- thread-id )` — fork a fresh `Interpreter`
  whose `Vocabulary` is cloned *tight* from the parent's compiled
  state (copy the used `dict` region and pools, capacity = `here`), so
  the worker inherits every word defined so far — not just the
  primitives + lib.l4 — and the body `xt`'s dict index resolves in the
  clone. Run xt as the body; return a handle (`T_THREAD`). After the
  fork the two vocabularies evolve independently.
- `thread-id join ( -- )` — wait for the thread to finish.
- `thread-id message send ( -- )` — enqueue the message in the
  target thread's mailbox. Non-blocking (mailbox is unbounded).
- `receive ( -- message )` — pull the oldest message from this
  thread's mailbox; block if the mailbox is empty.
- `xt receive-match ( -- message )` — pull the first message
  satisfying `xt` (an `( msg -- bool )` predicate); leaves non-
  matching messages in the mailbox for later. Erlang's "selective
  receive."
- `self ( -- thread-id )` — this thread's own ID. Pass it around so
  others can reach back.

**Cross-thread value semantics:**

Values that travel through mailboxes get deep-copied into the
receiver's `objects[]`:

- `T_FLOAT`, `T_THREAD`: bit-for-bit.
- `T_SYMBOL`: travels as its byte name and is re-interned in the
  receiver's symbol pool (a pool offset is interpreter-specific).
- `T_STRING`, `T_ARRAY`, `T_SET`, `T_FRAME`, `T_MATRIX`: deep copy.
- `T_XT`, `T_ADDR`, `T_CONT`: *not transmissible* — they reference
  interpreter-specific dict positions / rstack contents. Sending one
  is a type error. Same restriction Erlang has on local PIDs and
  function references.

**Mailbox storage.** Each `Interpreter` has its own mailbox: a queue
of Vals plus a mutex + condvar pair. No separate registry — addresses
are thread IDs (handles from `spawn-thread`), which directly index
into the live-thread table.

**Cost:**

- Mailbox + send + receive + receive-match: ~80 lines (mutex/condvar
  dance, queue management, predicate matching for the selective
  variant).
- `spawn-thread` / `join` / `self`: ~80 lines (pthread wrappers,
  tight vocabulary clone, ID assignment).
- Deep-copy on send: ~100 lines, one case per object kind.
- Symbol-pool: per-thread automatically — each interpreter owns its
  `Vocabulary`, and the symbol pool lives in it. On send, symbols
  travel as their byte names and get re-interned in the receiver.
  Nothing shared, no lock.
- Output coordination: wrap stdout writes in a single shared mutex,
  or have a dedicated "output thread" that serializes. Easy either
  way.

Total: ~350 lines, assuming the refactor is done.

**Composes with green threads (path A):** each OS thread has its own
green-thread scheduler. Sends from one green task to another in the
same OS thread go through the local mailbox (the mailbox is just
this thread's `Interpreter`); sends across OS threads take the
mutex + deep-copy path. The send/receive surface is the same either
way.

**What you get:**

- True multi-core parallelism.
- Zero locks in user code; data races impossible by construction.
- GC stays simple — per-thread.
- One thread crashing doesn't take down the others.

**What you don't get:**

- Shared mutable state. Build a "service thread" that owns the state
  and others reach it by `self` + `send` + `receive`. Exactly the
  actor pattern.
- Preemption within a thread (path A's limitation carries over).
- Sharing large objects by reference. Sending a 1GB matrix copies it.
- Bounded flow control. Mailboxes are unbounded by default. If a
  producer outpaces a consumer, the consumer's mailbox grows
  unboundedly. Erlang has the same characteristic and has lived with
  it; a `receive` that backs off when the mailbox is large is the
  user-level workaround.

**Build order:**

Continuations and the per-interpreter refactor are both in place, so
what remains:

1. Path A — green threads (~50 lines), on the existing continuations.
2. Path B — spawn-thread + mailboxes (~350 lines), on the existing
   per-interpreter interpreters.

Path A stands alone; Path B lights up parallelism.

---

## Array head/tail decomposition — via the `[H|T]` cons pattern

Prolog-style head/tail decomposition is provided by the `[ H | T ]` cons
pattern on `unify` (see "Unification + nondeterminism"), not by dedicated
primitives. `arr [ H | T ] unify` binds `H` to the head and `T` to a fresh
tail array; unifying a free variable with `[ H | T ]` where both are bound
builds the array. One declarative mechanism, both directions.

There are no `>head` / `head>` primitives; the cons pattern is the only
head/tail mechanism. For hot imperative iteration use the C-side `reduce` /
`map` family rather than per-step array splitting.

**Cost note that still applies.** Arrays are contiguous, not linked, so a
recursive walk that splits a head off each step — whether via the cons
pattern or otherwise — allocates tail arrays of sizes N-1, N-2, …, 1:
O(N²) Val storage churned. Fine for shallow, clause-shaped decomposition;
for anything large, iterate with `reduce` / `map`, which stays C-side.

**Back-end (snoc-style) decomposition** is likewise not a primitive. The
`[H|T]` pattern doesn't need it, and `last` already covers last-N in
`lib.l4` (`arr n -- arr'`).

---

## Functional primitives

**The dividing line is whether a word builds a new array.** Forth-side
array construction has exactly one path: push the elements and gather
them with `[ … ]` / `array`. That gather reads off the data stack,
which is fixed at `DATA_STACK_DEPTH` (256), and there is no in-place
element store (`@i` only reads; `array-of` only fills a constant). So
a word that produces an array of data-dependent length cannot be a
`lib.l4` definition — it would cap out at ~250 elements. Anything that
allocates and fills a result array must do it in C, the way `map` /
`mapn` / `filter` already do. Words that return a scalar, an element,
or a boolean have no such constraint and belong in `lib.l4`.

**lib.l4 definitions (return a scalar/element, or compose C builders):**

- **`find`** — `arr [: pred :] find` → first matching element, or a
  sentinel (`T_NONE`). Short-circuits via `shift`.
- **`any?`** — `arr [: pred :] any?` → boolean float (1 / 0).
- **`all?`** — `arr [: pred :] all?` → boolean float.
- **`flat-map`** — `arr [: ( elt -- arr ) :] flat-map` → map then
  concatenate. `map` then a `concat` fold; both pieces are C, so the
  result isn't stack-bounded. Monadic bind for arrays.
- **`sort-by`** — `arr [: ( elt -- key ) :] sort-by` → sorted by
  extracted key. Atop `sort-with` (see the Sort section); sorting
  reorders in place rather than building, so it stays in `lib.l4`.
- **`each`** — `arr [: ( elt -- ) :] each` → apply xt to each element
  for its side effects, no result. `i-times` + `@i` underneath; this is
  for-each.

**Build on frames (now available):**

- **`group-by`** — `arr [: ( elt -- key ) :] group-by` → frame from
  key to array of elements with that key. Keys must be symbols, per the
  frame key rule.
- **`partition`** — `arr [: pred :] partition` → two arrays, matches
  and non-matches.

**Deliberately not adding** (composable in one line of user code):

- `count` — `[: pred :] filter size`.
- `min-by` / `max-by` — `reduce` with comparison.
- `sum` / `product` — `0 [: + :] reduce` etc.

**Cost:**

- `lib.l4`: `find`, `any?`, `all?`, `flat-map`, `sort-by`, `each` → ~60 lines.
- `group-by` / `partition` build on frames (now available); `group-by`'s
  result is a frame, so its keys must be symbols.

---

## Help system

A `help` word that shows a one-line description of any word — colon
definition, variable, symbol, or primitive.

**Status:** not started.

**Design:**

- **Storage**: reuse the existing `SRCIDX` header field, no new
  header cell. Primitives' SRCIDX points to a doc string. Colon
  defs' SRCIDX still points to body source; `help` extracts the
  first `( ... )` paren-comment as the doc.
- **Entry — primitives**: extend `define_primitive` to take a doc
  string parameter. The ~170 user-facing primitives get short
  stack-effect-style docs at registration time (e.g. `( n -- n*n )
  square the top`); internal words (parenthesized, flagged internal)
  can pass an empty doc.
- **Entry — colon defs**: convention is the first `( ... )` after
  the name, Forth-style: `: square ( n -- n*n ) dup * ;`. No new
  syntax. Words without a paren-comment have no help text.
- **Entry — variables, symbols**: no doc by default. `help` shows
  just the kind and name.
- **Lookup**: `' foo help`. xt-style, consistent with `see`, `execute`,
  `map`. `help` dispatches on the handler: `docol` → extract from
  body source; `dovar`/`dosym` → kind + name; otherwise (primitive)
  → print the stored doc string directly.

**Why this shape:**

- One field handles both kinds with minimal machinery.
- Colon-def docs need zero extra work from the user beyond writing
  the conventional stack-effect comment.
- `help` complements `see`: `see` shows the full definition, `help`
  shows the short doc.

**Cost:**

- One signature change to `define_primitive` (touches every call site —
  ~230 — but mechanically: add a doc string, empty for internal words).
- Static doc strings: ~600–1000 bytes.
- About a dozen lines for `p_help`.

**Open questions:**

- Should `help` with no argument print a list of all words with a
  short doc each, sorted? Could be a separate `apropos` word later.
- Variable/symbol docs are uncovered. If they need them, we'd add
  `( comment )` parsing in `p_variable` / `p_symbol` to capture a
  doc string from the input stream after the name.
