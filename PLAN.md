# logicforth — deferred work

Tracks work that's planned or pending. Completed items are deleted; the
git history is the place to look for what's been built.

---

## Matrix — remaining work

The matrix type is functionally complete for value-semantic numeric
work: construction, element-wise arithmetic, scalar broadcasting,
transpose, DGEMM in all four transpose variants, indexing (`@i`, `@j`,
`@i,j`), `reshape`, `flatten`, `diagonal-matrix` / `identity-matrix` /
`diagonal`, and the reduction family (`sum`, `row-sums`,
`column-sums`, `mean`, `row-means`, `column-means`). The reductions
on max/min and the element-wise math primitives are the next
substantive additions.

### Element-wise math primitives

Apply a scalar function to every element of a matrix, producing a new
same-shape matrix. Each is ~10 lines of straight `MAT(...)` loop.
Dispatched the same way as `+` and friends so the user-facing names
read naturally (`m abs`, `m sqrt`, etc.).

Words to add:

- **`abs`** — absolute value.
- **`sqrt`** — square root. Domain check (negative input → NaN or
  error? Decide at implementation.)
- **`exp`** — `e^x`. Needed for softmax, sigmoid.
- **`log`** — natural log. Needed for cross-entropy. Domain check
  (non-positive → NaN or error?).
- **`negate`** already exists for floats and could extend to matrices
  trivially.

These should ALSO dispatch on `T_FLOAT` for the scalar case, so
`5 sqrt` and `m sqrt` both work. Polymorphic via the same
type-dispatch the arithmetic primitives use.

### Max/min reductions

The sum/mean family currently covers additive aggregation. The
matching shape for max/min:

- **`max`** — overall maximum of a matrix (returns a float).
- **`min`** — overall minimum (returns a float).
- **`row-maxes`, `column-maxes`** — N×1 and 1×N matrices of per-row
  and per-column max.
- **`row-mins`, `column-mins`** — same for min.
- **`argmax`, `argmin`** — possibly. Index of the maximum element
  (or `(i, j)` pair). Defer until there's a use case.

Six straightforward primitives mirroring `sum`/`row-sums`/`column-sums`.

### Cleanup items (still pending)

(none currently — `val_cmp` for matrices is done.)

### Beyond core

Deferred until there's a specific use case:

- **Slicing** — submatrix by `(row-start, row-end, col-start, col-end)`.
- **Concatenation** — `hstack` (side by side), `vstack` (stacked).
- **Element-wise comparison** — `<` etc. returning a matrix of
  `-1`/`0` booleans. Cleanest as polymorphic extension of the existing
  comparison words.
- **Norms** — `norm` (L2), `frobenius-norm`. Composable from existing
  reductions but common enough to deserve named words.
- **SVD** — one-sided Jacobi as the starting point. ~50–100 lines.
  No LAPACK available, so hand-rolled is the only option.

---

## String operations via POSIX regex

A single pattern-matching primitive subsumes the usual string-handling
zoo (split / substring / index-of / starts-with / ends-with / trim /
lines / replace). The engine is POSIX ERE via `<regex.h>` — already
in libc on every Unix, zero dependency, well-known syntax.

**Why pattern-first instead of seven named primitives:**

- One concept to learn, not seven.
- Anything we didn't pre-anticipate (split on whitespace *or* comma,
  match a number anywhere in a line) is expressible without adding a
  new primitive.
- Named primitives stay possible as library words in `lib.l4` for the
  common cases.

**Why POSIX ERE, not PCRE or Lua patterns:**

- Zero dependency. `regcomp` / `regexec` / `regfree` / `regerror` are
  in libc. No vendoring, no Makefile change, no library install.
- Couple hundred bytes of binary growth from the linker, not megabytes.
- Syntax users already know. `^`, `$`, `*`, `+`, `?`, `{n,m}`, `|`,
  `()`, character classes, captures.
- Covers every operation on the destructuring list.

The features POSIX ERE lacks vs PCRE — shorthand classes (`\d`,
`\w`), non-greedy quantifiers, lookahead/lookbehind, named captures
— are convenience, not capability. If something later forces the
upgrade, the user-facing API stays the same and only the engine
swaps underneath.

**API sketch:**

Core word:

```
"hello world" "w[a-z]+" match
```

→ either an array `[ start end capture1 capture2 ... ]` or a
sentinel on no match. Captures are 1-indexed groups from `()`.

Higher-level words built on top (in C or `lib.l4`):

- `split` — `"a,b,c" "," split` → array of three strings.
- `replace` — `"hello" "l" "L" replace` → `"heLLo"`. Replaces all
  matches.
- `index-of` — `"hello world" "world" index-of` → 6, or `-1` on miss.
- `starts-with`, `ends-with` — anchored match returning a boolean float.
- `trim` — wrapper around `^[[:space:]]+|[[:space:]]+$` replace.
- `lines` — wrapper around `"\n" split`.
- `substring` — positional, no regex. `"hello" 1 4 substring` →
  `"ell"`, half-open `[start, end)`. Lives in the same area but
  doesn't use the regex engine.

**Unicode model:**

UTF-8 throughout, codepoint-indexed at the user level.

- `setlocale(LC_CTYPE, "")` at startup, so POSIX regex picks up the
  process locale and interprets `.` / `[[:alpha:]]` / `[[:digit:]]`
  as codepoint-aware rather than byte-aware.
- `length`, `substring`, `index-of`, regex match positions — all
  expressed in *codepoints*, not bytes. A small UTF-8 codec (~50
  lines: encode, decode, count, advance-by-n) sits underneath every
  string operation. `regexec` returns byte offsets in `regmatch_t`;
  we translate to codepoint offsets at the boundary.
- Strings are stored as UTF-8 bytes in `objects[]`. The length field
  stores byte count for storage purposes; codepoint count is
  recomputed on demand (cheap — a single linear scan per call, and
  most strings are short).

**What's covered:**

- All ASCII operations behave identically to a byte-oriented design.
- Non-ASCII characters in patterns and inputs match correctly.
- `length "café"` → 4 (codepoints), not 5 (bytes).
- `substring` never splits a multi-byte sequence.

**What's not covered, called out explicitly:**

- **Normalization.** `é` as U+00E9 vs `e` + U+0301 are distinct. We
  don't normalize. A user pasting from sources that disagree on
  normalization can hit this; document it.
- **Grapheme clusters.** `.` matches one codepoint, not one
  user-perceived character. Flag emoji, zalgo text, ZWJ sequences
  all break the intuitive `length`.
- **Unicode property classes** (`\p{Letter}` etc.). POSIX doesn't
  define them.
- **Locale-aware case folding for non-ASCII.** `REG_ICASE` is
  implementation-dependent and weak outside ASCII.

For real Unicode work that needs the above (i18n applications), the
right tool is ICU. For matrix lab / scientific computing /
general-purpose scripting with non-English labels and identifiers,
this model is enough.

**Implementation notes:**

- Compile patterns lazily on first use; cache the compiled `regex_t`
  keyed by the pattern string. Strings are immutable once interned,
  so the cache key is stable. Bound the cache (LRU-ish) at e.g. 64
  entries to avoid unbounded growth.
- Pass `REG_EXTENDED` to `regcomp` to get ERE syntax (not the older
  BRE).
- Errors (bad pattern) surface through the existing `error_flag`
  path, with `regerror` providing the diagnostic.

**Out of scope:**

- Non-greedy quantifiers (not in POSIX).
- Lookahead / lookbehind (not in POSIX).
- Streaming / incremental match against large inputs. Whole-string
  matching only.

---

## Core language additions

Features identified as load-bearing for a complete core language.
Each kept short here; expand into its own section once we start
implementing.

### Dictionaries / hash maps

Key→value mapping. Keys are strings (or symbols, treated equivalently).
Values are any `Val`.

- New tag `T_DICT`, new `OBJECT_DICT` kind.
- Literal syntax: `{ "a": 1, "b": 2 }`. Bracketed by `{` `}` like sets,
  disambiguated by the `:` separator.
- Operations: `at` (key → value, polymorphic with array/matrix indexing),
  `set` or `!` (insert/update), `keys`, `values`, `size` (polymorphic
  with array length), `contains?`, `delete`.
- Internals: open-addressing hash table, linear probing, ~150 lines of C.
  Mutable in place — same semantics as sets and arrays.

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
- `array shuffle` — permute in place (Fisher-Yates).

### Sort

- `array sort` — sort using the existing `val_cmp` ordering. Mutates
  in place (consistent with how arrays work today).
- `array [ x y -- cmp ] sort-with` — sort with a user comparator
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

### Word-local variables

Currently every `variable` is global to the dictionary. This forces
top-level state for things that should be scoped to a single colon
definition (intermediate matrices, loop accumulators, named popped
arguments), and risks collisions on `reload`. Standard Forth's
answer is locals declared at the head of a definition.

**Syntax:** `{| name1 name2 ... |}` at the start of a colon def. Pops
N values off the data stack into named slots; names are valid only
inside that definition.

```
: predict-row {| u |}
    U @ u row
    S @ diagonal-matrix *
    Vt @ * ;
```

- A bare local name compiles to a "fetch local n" op.
- `TO localname` compiles a store (`5 TO u`).
- Frames live on the return stack: entry pushes N slots, exit pops them.

**Implementation:**

- Compile-time table of declared locals for the in-progress definition.
- Body parser checks each token against that table; if matched,
  compile `local@ n` instead of a dictionary lookup.
- `TO` becomes an immediate word that consumes the next token and
  compiles either `local! n` (if local) or a normal variable store.
- Two new primitives: `local@` and `local!`. ~100 lines total.

**Quotations and locals:** A quotation defined inside a word does
*not* implicitly capture the enclosing word's locals — that would
require closures and heap-allocated frames. Instead, quotations
declare their own locals the same way:

```
[: {| x |} x x * :]
```

Cleaner than closures, fits Forth's mental model, no lifetime
tracking. The cost is a small amount of repetition where a loop body
wants the index it's iterating over.

**What locals don't replace:** long-lived state (open DB handles,
factor matrices, lookup dicts) that outlives any single word call
still wants either a `variable` or a bundled state dict passed
explicitly. Locals fix intra-word stack juggling; they're not a
substitute for program-level state.

---

## TSV file I/O

Read and write tab-separated-value files. Both numeric and non-numeric content.

**TSV is the only tabular I/O format logicforth will ever support.** Not
a starting point, not a default — the entire story. No CSV reader. No
JSON. No Parquet, Arrow, HDF5, anything. If a user has data in another
format, they convert it to TSV outside logicforth before loading.

**Why:** Every other format adds disproportionate complexity for
disproportionate benefit:

- **CSV** looks similar but isn't — quoted fields, escaped quotes inside
  quoted fields, locale-dependent decimal separators, BOM handling.
  A spec-correct CSV parser is hundreds of lines of state machine.
- **JSON** would need its own parser plus a mapping into logicforth's
  tagged-value model (which is *almost* a JSON value but not quite,
  since we have sets and symbols and execution tokens).
- **Binary formats** would require schema definitions, endianness
  handling, version negotiation.

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

New tag `T_DB` carrying the database handle. Could alternatively
reuse `T_ADDR` or just a `T_FLOAT` index, but a dedicated tag keeps
type errors specific ("`sql` requires a database, got a string") and
lets `val_cmp` / `print_val` handle it without confusing it with
dictionary addresses.

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

## Unification + nondeterminism (microKanren-flavored, on continuations)

Once delimited continuations are in, a logic-programming layer becomes
tractable: logic variables, unification, `amb` / `fail` for choice and
backtracking. The flavor is closer to Prolog than to the faithful
microKanren stream-of-states model — the substitution is implicit
state (logic-var bindings + a trail), and search is driven by
continuations rather than by mapping goals over streams. The name
"logicforth" finally earns its second half.

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
  via existing `val_cmp`. Arrays and hashmaps unify structurally
  (same length / same key set, then element-wise). Sets, matrices,
  xt's, continuations only unify by identity.
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

**Cost:** ~140 lines in C (logic var, trail, unify primitive) plus
~30 lines of `lib.l4` for `amb` / `fail` / `once` / `fresh` / `run`.
Assumes continuations are working.

**Subtleties:**

- **Occur check skipped** — `X = [X]` makes a cyclic term and may
  loop on later use. Match Prolog's default; document the gotcha.
- **Variable keys in hashmaps not allowed** — same restriction Prolog
  has for compound functors. Only values can be logic variables.
- **Trail interaction with `forget`** — logic vars are objects and
  survive `forget` like any other heap value, but their names (kept
  as `namepool` offsets) might be invalidated. Either copy the name
  into the object's own storage, or stop displaying names after
  `forget` runs.
- **Image save/load** — logic-var objects serialize like any other;
  the trail is session state and doesn't need to persist.

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

- `T_FLOAT`, `T_SYM` (bytes; re-interned in receiver), `T_THREAD`:
  bit-for-bit.
- `T_STRING`, `T_ARRAY`, `T_SET`, `T_MATRIX`, `T_DICT`: deep copy.
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

## Functional primitives

`map` and `mapn` are already in. Adding the rest of the standard
higher-order toolkit. Most are short library definitions in `lib.l4`;
a couple of hot ones get C primitives for speed on large arrays.

**C primitives (perf-critical):**

- **`filter`** — `arr [: pred :] filter` → array of elements matching
  the predicate. Done in C so it walks the source array once and
  allocates the result with the right size after a counting pass.
  ~30 lines.
- **`reduce`** — `arr initial [: ( acc elt -- acc ) :] reduce` → folded
  result. Left fold. Done in C to avoid xt-call overhead on every
  element. ~25 lines.

**lib.l4 definitions (built atop map / filter / reduce / each):**

- **`find`** — `arr [: pred :] find` → first matching element, or a
  sentinel (probably `T_NONE`). Short-circuits via `shift` once we
  have continuations.
- **`any?`** — `arr [: pred :] any?` → boolean float (-1 / 0).
- **`all?`** — `arr [: pred :] all?` → boolean float.
- **`range`** — `n range` → `[ 0 1 … n-1 ]`. Two-arg form
  `start end range` → `[ start … end-1 ]`. Sequence constructor;
  head of most pipelines.
- **`take`** — `arr n take` → first n elements.
- **`drop`** — `arr n drop` → skip first n.
- **`reverse`** — reverse an array.
- **`concat`** — `a b concat` → joined array. (Sets and strings
  already concatenate via `+`.)
- **`sort-by`** — `arr [: ( elt -- key ) :] sort-by` → sorted by
  extracted key. Cleaner than `sort-with` for the common case of
  "sort by some field."
- **`flat-map`** — `arr [: ( elt -- arr ) :] flat-map` → map then
  concatenate. Monadic bind for arrays.
- **`distinct`** — remove duplicates while preserving order. (Sets
  already dedupe but lose order.)

**Predicated on dicts being in:**

- **`group-by`** — `arr [: ( elt -- key ) :] group-by` → hashmap from
  key to array of elements with that key.
- **`partition`** — `arr [: pred :] partition` → two arrays, matches
  and non-matches.

**Deliberately not adding** (composable in one line of user code):

- `count` — `[: pred :] filter length`.
- `min-by` / `max-by` — `reduce` with comparison.
- `sum` / `product` — `0 [: + :] reduce` etc.
- `for-each` — already covered by `each`.

**Cost:**

- C: `filter` + `reduce` → ~55 lines.
- `lib.l4`: ~10 short definitions, ~80 lines total.
- `group-by` and `partition` wait on dicts.

Net: small. Most of the value is in committing to consistent
stack-effect conventions across the family.

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
  string parameter. All ~70 primitives get short stack-effect-style
  docs at registration time (e.g. `( n -- n*n )  square the top`).
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

- One signature change to `define_primitive` (touches ~70 call sites
  but mechanically — add a doc string each).
- Static doc strings: ~600–1000 bytes.
- About a dozen lines for `p_help`.

**Open questions:**

- Should `help` with no argument print a list of all words with a
  short doc each, sorted? Could be a separate `apropos` word later.
- Variable/symbol docs are uncovered. If they need them, we'd add
  `( comment )` parsing in `p_variable` / `p_symbol` to capture a
  doc string from the input stream after the name.
