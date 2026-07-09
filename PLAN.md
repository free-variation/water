# Water — future work

A TODO list of pending work.

---

## String operations

### Unicode

- **ASCII fast path**: a per-string all-ASCII flag to collapse the byte-offset
  walk in `substring`/`char-at`/`codepoint-at` to direct byte indexing.
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

### Time / dates — follow-ups

- **Named-timezone conversion** — decompose/compose/format in an explicit
  zone (`epoch>date-in ( instant zone -- date )`-style, tzdata-backed).
- **Sub-second rendering** — a fractional-seconds directive in the format
  words (strftime has none).

### Sort and shuffle with a rule

- `array [ x y -- cmp ] sort-with` — sorted copy under a user comparator
  quotation that pops two Vals and pushes `-1` / `0` / `1`; input untouched.
  Algorithm: introsort or libc `qsort` with a comparator thunk.
- `array shuffle` — new array, elements randomly permuted (Fisher-Yates over
  the PRNG stream); input untouched.

### Re-readable repr

`render` produces a value's display form, which is not always re-readable —
strings print raw, a matrix prints as a grid. `frame>json` round-trips, but only
the JSON-expressible subset (frames, arrays, strings, numbers, booleans).
Missing is a representation that reads back through the Water reader for
*any* value.

- `repr` ( v -- s ) — a string of Water source that, read back, reconstructs
  an equal value: quoted strings (with `""` escaping), `[ ]` arrays, `{ :k v }`
  frames, `< >` sets, `[( )]` cons lists, `:name` symbols, floats in shortest
  round-trip form, a matrix as its `[ … ] R C matrix` constructor.

To settle: how a value with no source form (an unbound logic var, continuation,
stream, db, or ptr) reprs — an error, or a `reify`-style canonical placeholder;
whether `repr` then `load`-style evaluation is the intended round-trip path or a
dedicated `read` ( s -- v ) word is wanted.

### Loop ergonomics

Structured early exit from a loop — today it's hand-rolled by threading a flag
through the condition.

- `leave` — exit the innermost loop immediately; `?leave` ( flag -- ) the
  conditional form; `continue` — skip to the next iteration.

To settle: how `leave` / `continue` compile (forward/back branch patching) and
unwind cleanly past loop-local frames.

---

## Loader dictionary lookup

Token resolution in the outer interpreter is a linear dictionary walk with a
string compare per candidate, which dominates the load time of large generated
source files. A name-hash index over the dictionary (or reuse of the symbol
hash table) makes resolution O(1) and leaves large-file loads I/O-bound.

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

## Guaranteed cleanup across every exit

**`dynamic-wind`** — a `before body after` whose `after` runs on every exit
from the region — normal, throw/interpreter error, `fail` backtrack, `shift`
capture — and whose `before` re-runs on `resume` re-entry. No `catch`-style
wrapper can provide this: a `fail` unwinds to the nearest *choice* prompt,
past `catch`'s *exception* prompt, and a region re-entered by `resume` needs
setup per entry, not a once-only handler. Without it, a `db`/stream/FFI
handle — a registry slot with no GC finalization — leaks on a backtrack past
its close until the process ends.

The mechanism: a *wind mark* — a return-stack mark, kin to `reset`'s, carrying
the before/after thunks, recognized by both unwind cascades in the inner loop
(exception and choice prompts) and by `resume`'s splice so re-entry re-runs
`before`.

To settle: whether `after` firing once per failed alternative of a multi-shot
region is the wanted semantics or a footgun; how a wind mark interleaves with
the locals-frame and trail rewind the unwind already carries; whether
`before`/`after` observe the region's data stack or run isolated.

---

## FastCGI service

Run Water as a long-lived FastCGI application behind an off-the-shelf web
server (nginx, Caddy, lighttpd, Apache). The web server owns everything HTTP —
TLS termination, HTTP/1.1–3, request parsing, static files, timeouts, rate
limiting, access logs, load balancing — and forwards each request over a Unix or
TCP socket as FastCGI records. Water never sees a raw HTTP byte: it decodes
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
  `END_REQUEST`. The framing is simple: `lib.h2o` over `read-n`/`write` plus byte
  arithmetic, with maybe a tiny C helper for the 2/4-byte length fields.

**Serve loop.** A plain sequential `accept → decode → handle → respond` loop in
`lib.h2o`, each handler wrapped in `try-catch` so a bad request can't kill the
worker; per-request allocations are reclaimed by GC. No threads.

**Worker processes.** Run N worker processes all accepting on the same socket
(the kernel load-balances) under a process manager (e.g. systemd template units)
that respawns on crash. One request per worker isolates failures; the web server
retries elsewhere.

**SQLite.** Each worker opens its own connection; enable WAL once
(`PRAGMA journal_mode=WAL`) plus a `busy_timeout`, so concurrent reads across
workers don't block and writes serialize safely (single host).

**Cost:** `accept` + `read-n` are small C; the FastCGI codec is `lib.h2o` (plus an
optional tiny C codec for the integer fields); the serve loop and response
builders are `lib.h2o`.

---

## Foreign function interface

- **Callbacks** — C → Water function pointers (`qsort` comparators,
  `CURLOPT_WRITEFUNCTION` to capture a response body into a string).
- **Struct-by-value** arguments and returns.
- **Per-call varargs** — variadic arg types chosen at the call site rather
  than fixed per declared word.
- **Finer numeric types** — `float`, unsigned variants, explicit widths.
- **`dlclose`** for library handles.

---

## Coroutines, generators, lazy sequences

Building on the generator primitives:

- Lazy `map` / `filter` / `take` / `zip` as `lib.h2o` wrappers that resume the
  source on demand, with `lazy>array` to force a finite prefix.
- A cooperative scheduler (`spawn` / `run-scheduler`, a queue of `T_CONT`s) for
  producer/consumer pipelines.
- **Kanren-style interleaving streams.** A captured continuation is the
  suspension a miniKanren stream needs — force it with `resume` and it yields an
  answer or suspends again. Fair interleaving: `mplus` (merge two streams so an
  infinite branch can't starve the other) and `bind` (flatMap with interleaving)
  — a *complete* search, distinct from the depth-first `amb` / `fail`. Generators
  are the substrate; the interleaving combinators are the work.

All `lib.h2o` on the existing primitives — no new C.

---

## Multi-core parallelism: threads over the shared heap

In rough priority:

- **Persistent worker-thread pool.** Spawning and joining OS threads per region
  amortizes to nothing for one big region (a single `pmap` over a huge domain
  saturates the cores), but the spawn/join dominates for many small regions —
  system time, not compute. A pool that parks threads and dispatches per call
  fixes it. Pooled threads keep their per-worker allocation context across
  regions, so region teardown must reset every worker's context, not just the
  caller's.

- **Numeric disjoint-write buffer / work-stealing.** Lower priority: a shared
  unboxed-`double` output buffer threaded under the matrix kernels, and
  work-stealing for skewed workloads.

---

## Functional primitives

Forth-side construction gathers off the data stack, so any word that builds a
new array of data-dependent length must allocate and fill in C; words returning
a scalar, element, or boolean belong in `lib.h2o`.

**`lib.h2o` (scalar/element result, or compose C builders):**

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
