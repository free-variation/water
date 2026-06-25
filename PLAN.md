# logicforth — future work

A TODO list of pending work.

---

## Matrix

- **Vertical stacking** — `vstack`, concatenating two matrices row-wise (the
  row counts of the result sum; column counts must match).
- **Element-wise comparison** — `<` etc. returning a matrix of `1`/`0`, as
  a polymorphic extension of the comparison words.
- **Norms** — `norm` (L2), `frobenius-norm`.

---

## String operations

### Wrappers

Over the match/replace layer: `index-of`, `starts-with`, `ends-with`,
`lines`.

### Unicode

- **ASCII fast path**: a cached per-string all-ASCII flag to collapse the
  codepoint walk in `size`/`substring`/`char-at`/`codepoint-at` back to byte
  speed for the common case.
- **Case folding**: `upcase`/`downcase`. Unicode-correct folding needs
  tables (ICU or a generated table), so even an ASCII-only first cut should
  name the boundary.

---

## Core language additions

### Path queries — follow-ups

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

### Sort and shuffle with a rule

- `array [ x y -- cmp ] sort-with` — sorted copy under a user comparator
  quotation that pops two Vals and pushes `-1` / `0` / `1`; input untouched.
  Algorithm: introsort or libc `qsort` with a comparator thunk.
- `array shuffle` — new array, elements randomly permuted (Fisher-Yates over
  the PRNG stream); input untouched.

### Standard streams

- `stdin` / `stdout` / `stderr` — the three standard streams as `T_STREAM`
  values (fds 0/1/2). Reading and writing reuse the subprocess stream
  words: `stdin read` slurps all of stdin, `stdin read "\n" split` its
  lines, `s stdout write` emits.

### Format specs

Extend `format`'s placeholders with optional format specifiers after a
colon: `{0:.2f}` (precision), `{0:8}` (field width), `{0:x}` (hex) — a
small printf-style mini-language on top of the positional `{n}` fill.

### Named interpolation

A named form `{name}` referencing an in-scope local or global reads better
than the positional `{0}` — `"ls {dir}" …`. `format` runs at runtime where
local names are gone, so this needs either a compile-time f-string (scan a
string literal, resolve each `{name}` in scope, rewrite to the positional
`{n}` form) under an explicit opt-in marker so raw strings and regex like
`\d{3}` stay literal, or a runtime frame-keyed `format-with`
(`{ :dir d } "{dir}" format-with`).

### Number parsing and stringify

Reading a number out of a string, and capturing a value as the text `.` would
print, are both indirect today: numeric literals are parsed only at read time,
and a value renders to a string only through `format`'s placeholders (scalars)
or `frame>json` (structured values).

- `string>number` ( s -- n ) — parse a decimal/float string to a float.
- `>string` ( v -- s ) — the rendering `.` produces, returned as a string rather
  than printed: floats in shortest round-trip form, strings raw, symbols by
  name, arrays/frames/matrices/sets in the same laid-out form.

To settle: how `string>number` signals a non-numeric input (an error, a trailing
success flag, or the none value); whether `>string` mirrors `.`'s display
(truncation and all) or always emits the full value; whether a separate
re-readable `repr` (quoted strings, `[ ]`/`{ }`/`< >` literals) earns its place
given `frame>json` already round-trips structured data.

### Loop ergonomics

Counted iteration is `times` / `i-times` (with or without a pushed index) and the
`begin … until` / `begin … while … repeat` / `again` family; leaving a loop early
is hand-rolled by threading a flag through the condition. Missing is structured
early exit and a counted index loop.

- `leave` — exit the innermost counted loop immediately; `?leave` ( flag -- ) the
  conditional form; a skip-to-next-iteration (`continue`).
- `do … loop` ( limit start -- ) with the index available as `i` (and `j` one
  level out), and `+loop` for a custom step.

To settle: whether to add `do … loop` as a second counted form or instead give
`i-times` / `begin` a `leave` / `continue`; the index model (`do…loop`'s `i`/`j`
read the return stack, where `i-times` pushes its index to the data stack — two
conventions to reconcile or keep apart); how `leave` / `continue` compile
(forward/back branch patching) and unwind cleanly past loop-local frames.

---

## Symbol collection

Interned symbols are never reclaimed: `:foo` literals, `string>symbol`, and
`json>frame` object keys all add to the symbol table for the life of the
process. For a bounded, static set of names — source identifiers, fixed-schema
keys — that is correct and cheap. But symbols minted at run time from dynamic or
user-supplied strings (parsing JSON whose keys are unbounded, interning
arbitrary input) grow the table without limit, because the everyday associative
type — the frame — is symbol-keyed.

Make runtime-minted symbols collectible by reachability, the contract strings
and arrays already follow: a symbol keeps its identity (and its O(1) index
equality) for as long as something live refers to it, and is reclaimed once
nothing does. A string re-interned after its symbol was collected gets a fresh
identity, which is sound because no live value held the old one.

Two classes:

- **Pinned** — any symbol a compiled cell can name (`:foo` literals, `symbol`
  definitions, source identifiers). Interned at read/compile time; never
  collected. Bounded, so it does not grow.
- **Collectible** — symbols created at run time from computed strings. Reachable
  only from live values, never embedded in compiled code. The collector marks
  them while walking its existing roots and retires the unmarked ones, freeing
  the name and reusing the slot.

The partition keeps it cheap and safe: the collector never scans compiled code
for symbol references, and a baked-in literal can never dangle. When a computed
string matches a name already pinned, the pinned symbol wins, so a collectible
symbol never shares a name with a pinned one.

To settle: how symbols are represented (dictionary entries vs a separate
interned pool) and therefore how a slot is retired and reused; how `save-image`
serializes a collectible symbol (by name, re-interned on load, since its index
is not stable); whether pinned-vs-collectible is decided at the intern call site
or inferred from whether interning happens during compilation.

---

## Guaranteed cleanup

`catch`/`try-catch` recover the throw path — user `throw`s and interpreter
errors both unwind to the enclosing `reset`. What's absent is a cleanup hook
guaranteed to run however the protected region is left: normally, by throw, by
backtrack, or by a captured continuation. Resource handles make this concrete —
a `db`/stream/FFI handle is a registry slot with no GC finalization, so any
non-local exit past its close leaks it until the process ends.

**Tier 1 — `ensure` over throw and normal exit.** Cleanup that runs on both the
normal and the throw/interpreter-error path, expressible today on `catch`:

```
: ensure ( body-xt cleanup-xt -- … )
    >side  catch  side> execute  if throw then ;
```

plus resource `with-` helpers — `with-db`, `with-stream`, `with-file` — that
open, run a body, and close on either exit. Provide these as standard words
rather than leaving each program to hand-roll the pattern. This tier covers the
common case (open, use, release-even-on-error in straight-line code).

**Tier 2 — `dynamic-wind` across every exit.** A `before body after` whose
`after` also runs on a `fail` backtrack and a `shift` capture, and whose
`before` re-runs on `resume` re-entry. A `catch` wrapper can't reach these:
`fail` unwinds to the nearest *choice* prompt, past `catch`'s *exception*
prompt, and a region re-entered by `resume` needs setup per entry, not a
once-only handler. The mechanism is a *wind mark* — a return-stack mark, kin to
`reset`'s, carrying the before/after thunks, recognized by both unwind cascades
in the inner loop (exception and choice prompts) and by `resume`'s splice so
re-entry re-runs `before`.

To settle: whether `after` firing once per failed alternative of a multi-shot
region is the wanted semantics or a footgun; how a wind mark interleaves with
the locals-frame and trail rewind the unwind already carries; whether
`before`/`after` observe the region's data stack or run isolated.

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

- **Callbacks** — C → logicforth function pointers (`qsort` comparators,
  `CURLOPT_WRITEFUNCTION` to capture a response body into a string).
- **Struct-by-value** arguments and returns.
- **Per-call varargs** — variadic arg types chosen at the call site rather
  than fixed per declared word.
- **Finer numeric types** — `float`, unsigned variants, explicit widths.
- **`dlclose`** for library handles.

---

## Coroutines, generators, lazy sequences

Building on the generator primitives:

- Lazy `map` / `filter` / `take` / `zip` as `lib.l4` wrappers that resume the
  source on demand, with `lazy>array` to force a finite prefix.
- A cooperative scheduler (`spawn` / `run-scheduler`, a queue of `T_CONT`s) for
  producer/consumer pipelines.
- **Kanren-style interleaving streams.** A captured continuation is the
  suspension a miniKanren stream needs — force it with `resume` and it yields an
  answer or suspends again. Fair interleaving: `mplus` (merge two streams so an
  infinite branch can't starve the other) and `bind` (flatMap with interleaving)
  — a *complete* search, distinct from the depth-first `amb` / `fail`. Generators
  are the substrate; the interleaving combinators are the work.

All `lib.l4` on the existing primitives — no new C.

---

## Multi-core parallelism: threads over the shared heap

In rough priority:

- **Persistent worker-thread pool.** Spawning and joining OS threads per region
  amortizes to nothing for one big region (a single `pmap` over a huge domain
  saturates the cores), but the spawn/join dominates for many small regions —
  system time, not compute. A pool that parks threads and dispatches per call
  fixes it. Co-design with the rewind: pooled threads keep their `AllocContext`
  across regions, so teardown has to reset every worker's context, not just the
  caller's.

- **De-fragilize the region rewind.** The rewind restores several counters and
  resets the calling thread's context by hand; correctness depends on invariants
  maintained across files (the `in_parallel` gating, the per-region thread
  lifecycle). Fold the region's mutated state into one begin/commit/abort owner
  to remove that coupling — a prerequisite for the thread pool.

- **Numeric disjoint-write buffer / work-stealing.** Lower priority: a shared
  unboxed-`double` output buffer threaded under the matrix kernels, and
  work-stealing for skewed workloads.

---

## Dynamic vector

Arrays are fixed-length and O(1)-indexed; cons lists grow by O(1) prepend but
read sequentially. Neither grows at the end *and* indexes cheaply — the shape
incremental, natural-order construction wants. Today that means accumulating
onto a cons list and freezing with `cons>array`, or pre-sizing with `array-of`
and filling by `!i`: a phase boundary, not one structure that is cheap to both
grow and index.

A fill-pointer vector closes it — a mutable, contiguous buffer with
amortized-doubling append/remove at the end and O(1) index, update, and length.

- `vector` / `vector-of` — empty, or pre-sized with a fill value.
- `push` ( vec v -- vec ) append, doubling the backing buffer when full;
  `pop` ( vec -- v ) remove and return the last element.
- `@i` / `!i` / `size` extend to the live region (slots past the fill pointer
  stay invisible), so indexing and update stay O(1).
- `vector>array` freezes a copy of the live region; an array converts in.

The pieces to hand-roll it already exist (`array-of`, `!i`, `size`, `slice!` /
`to-slice!`, manual doubling); the value is a standard wordset, so every program
isn't re-implementing the doubling buffer.

To settle: a distinct type vs a growable mode of the array object (a fill-pointer
field, so `@i`/`size` work unchanged); whether `push`/`pop` mutate and return the
vector (like `set-add!`) or are value-returning; the word names.

---

## Functional primitives

Forth-side construction gathers off the data stack, so any word that builds a
new array of data-dependent length must allocate and fill in C; words returning
a scalar, element, or boolean belong in `lib.l4`.

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

Composable in one line: `count` (`[: pred :] filter size`), `min-by` / `max-by`
(`reduce` with comparison).
