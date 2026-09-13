# Telic — future work

A TODO list of pending work, highest priority first.

---

## 1.0-alpha gate

The gate for the tag, in priority order; an entry vanishes as its item
completes.

### Serialization depth

`bytes>value` roots each in-progress container on the GC root stack, so a
value nested past 62 levels writes but cannot be read, reporting `gc roots
exhausted`. `value>bytes` recurses per level with no guard and takes SIGSEGV
above about 50000 levels of array or frame nesting.

Implementation:

1. Park the in-progress container on the data stack, which the collector
   scans and which holds a million slots, instead of `gc_root_push`.
2. Guard the writer with the depth cap printing and `val_cmp` already use,
   erroring `structure too deeply nested` rather than overflowing.

Acceptance:

1. A 10000-level nested frame round-trips through `save-value`/`load-value`.
2. A synthesized 200000-level file errors from both words rather than
   crashing; both cases join `tests/102_serialize.telic`.

### Release mechanics

Implementation:

1. Add a `make install` (PREFIX-parameterized) copying the installed
   set: the `telic` binary, `lib/`, and `liblapacke_telic.so`, with
   `data/` only for running the README examples verbatim.
2. Add `make pack` output and `telic-pack.md` to the released set.
3. Run `make acceptance` and read the failures: fix what is a pack gap,
   record what is not.
4. Set VERSION (src/c/telic.h) to the release version and tag the
   commit.
5. Release notes: the benchmark table's provenance line, the platform
   pair (native, wasm) the suites passed on, and the acceptance pass@1
   with its model and date — a description of that run, not a
   threshold, since the tasks and the pack text were developed against
   each other. Name `lib/mcp.telic` as experimental: it has passed no run
   against a real MCP client, and no in-process test can stand in for
   one.

Acceptance:

1. From a clean checkout: `make && make test && make test-wasm &&
   make bench && make pack` all succeed, and `make test-libs` on a host
   with LAPACK and libxgboost.
2. Copy the installed set to a directory outside the repo; `telic`
   starts from any cwd, `"statistics" load-library` and
   `"plot" load-library` load, and `help` answers.
3. The tagged commit's README benchmark table matches a full run on the
   release host.

### Decisions to take before the tag

1. Symbol order is interning order, so a new symbol in the embedded library
   reshuffles frame key order and every golden that prints one. Either accept
   it and keep the embedded library free of short generic symbol names, or
   order symbols by name and pay a `strcmp` on each frame lookup.
2. `exact_to_double`'s method, `EXACT_POWER_BIT_CAP`'s value, and citations
   like Acklam's `qnorm` have no home under the no-comments rule: reference
   rows carry behavior and PLAN.md carries constraints, neither carries
   algorithm provenance. Decide where it goes, or decide it goes nowhere.

---

## xgboost — follow-ups

- **Multiclass / multi-output.** Read `out_dim`/`out_shape` in `xgb-predict`
  and `xgb-importance` so a multiclass model returns the full `[n, n_classes]`
  (predict) / `[k, n_classes]` (importance) matrix.
- **Buffer-form persistence.** The buffer form of `XGBoosterSaveModel`, so a
  booster travels inside a serialized frame rather than only through its own
  file (`xgb-save`/`xgb-load`).

---

## Statistics

### 1. SVD / dgemm methods (library)

- **PCA** — center columns, SVD; loadings V, scores U·S, variances S²/(n−1); add
  whitening, low-rank (Eckart–Young), and principal-component regression.
- **k-means** — Lloyd iterations over dgemm distances, rows assigned by
  `row-argmins`; k-means++ init on the RNG.
- **Gaussian mixtures** — EM with Cholesky (`dpotrf`/`dpotrs`) Mahalanobis
  distances and log-determinants; M-step as dgemm + element-wise ops; seed from
  k-means.
- **Spectral clustering** — affinity → normalized Laplacian → top-k embedding →
  k-means on the embedded rows.

### 2. Weighted least squares and GLM families

- `fit-weighted ( X y w -- beta )` — name the sqrt-w row-scaling idiom.
- `group-demean` — the within (fixed-effects) transform over `group-indices`.
- **More GLM families** — negative binomial, ordinal logistic (cumulative-logit).
- **KDE** — kernel density estimation, distance weights alone.

### 3. Resampling inference

Generalize the bootstrap shape — index sets → refit → collect:

- `permutation-test` — shuffle one column's indices for the null.
- `jackknife` — leave-one-out partitions, over `cross-validate`'s units shape.
- **Model metrics** — squared/absolute error, accuracy, confusion counts,
  ROC and calibration curves, isotonic calibration (PAVA), MASE.
- **Cluster resampling** — index-array units and the wild-cluster
  weight-multiplier (Rademacher) variant; cluster jackknife.
- Parallel variants via `pmap` (as `pbootstrap` does).

### 4. Pairwise distances

- `( X Y -- D )` via the dgemm identity ‖x‖² + ‖y‖² − 2XYᵀ, plus a direct kernel
  for other metrics. Unlocks kNN, RBF affinities, hierarchical /
  DBSCAN clustering, and distance-based tests (distance correlation, PERMANOVA,
  MMD).

### 5. Empirical-distribution distances

Each one pass over sorted samples, in the `ks-distance` mold:

- `ks` — one-sample against a reference CDF quotation ( x -- p ).
- `wasserstein` — 1-D W₁.
- Significance by permutation of pooled labels; energy distance once §4 is in place.

### Test data

- Seeded synthetic generated in-test: plant parameters, fit, recover; edge
  cases; resampling determinism.
- Canonical sets in `data/`: add `mtcars`, `faithful`, `anscombe`, each with a
  golden cross-checked once against R/scikit and the reference noted in-test.
- Large synthetic generated by script for benchmarks, never vendored.

---

## Basic graphing — follow-ups

- **Chart set** — step (the ecdf as drawn) and bar charts.
- **Stats consumers** — QQ plots (over `sort` + `qnorm`; bring back
  plot-side `fit-line` over `abline`), ROC and calibration
  curves (the model-metrics bullet), residual and fit plots for the
  regressions.
- **Log axes** — transform at the domain with power-of-ten tick labels,
  instead of transforming the data and labeling in log units.
- **Torn frames on overwrite** — `write-file` truncates in place; have
  `save-figure` write to a temp name and `rename-file` into place.

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
and arrays follow: a symbol keeps its identity (and its O(1) index
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
interned pool) and therefore how a slot is retired and reused; whether
pinned-vs-collectible is decided at the intern call site or inferred from
whether interning happens during compilation.

---

## MCP server — follow-ups

`lib/mcp.telic` serves revision 2026-07-28 with two tools, sessions as child
interpreters, and a poll loop over stdin and every busy child.

- Progress — a call carrying `_meta.progressToken` gets no
  `notifications/progress`. The child's output already arrives line by line, so
  emit each line as progress and keep the collected text as the result.
- Cancellation — confirm what this revision defines for cancelling an in-flight
  request, then route it to the kill path `mcp-expire` already uses, answering
  the cancelled call rather than leaving the client waiting.
- `tools/list` pagination and caching — the result may carry `nextCursor`,
  `ttlMs` and `cacheScope`; a two-tool server needs none of it, a host that
  registers many tools does.
- Structured results — a tool may declare `outputSchema` and answer
  `structuredContent` beside its text block, which suits a tool answering a
  dataset or a fit rather than printed output.
- Legacy clients — a client that opens with `initialize` is refused. Serving
  both eras means answering `initialize` with the negotiated older revision and
  keeping per-request `_meta` for modern ones; decide from what real clients
  send, not in advance.
- Writing back to a client that has stopped reading blocks the server: once the
  response pipe fills, `write` waits, and the loop stops polling children while
  it does. A client sending without reading can therefore stall the whole
  server. Answering it needs the same treatment the read side already has —
  poll for writability and hold unsent responses per session — so the server
  never waits on one descriptor.

---

## FastCGI service

Run Telic as a long-lived FastCGI application behind a web server, decoding
records off a Unix or TCP socket, running a handler, writing the response.

Blocked on symbol collection: a long-lived worker mints symbols from unbounded
request keys. Fuzz `json>frame` against a mutated seed corpus in the ASan build
before accepting untrusted bodies.

- `accept ( listen-stream -- conn-stream )` — accept a forwarded connection as a
  `T_STREAM`; small C. The listen socket arrives on fd 0
  (`FCGI_LISTENSOCK_FILENO`), so `bind`/`listen` may be unnecessary.
- `read-n ( stream n -- s )` — read exactly `n` bytes; small C. Records are
  length-framed, so a slurp-to-EOF read never terminates. `read-line` holds no
  buffer and `read-available` answers only what is already waiting, so this word
  accumulates across reads itself, waiting on `wait-readable` between them.
- FastCGI record codec — decode `BEGIN_REQUEST` / `PARAMS` (CGI environment → a
  request frame) / `STDIN` (body → a string); encode `STDOUT` + `END_REQUEST`.
  Library forth over `read-n`/`write`, with an optional C helper for the 2- and
  4-byte length fields.
- Serve loop — sequential `accept → decode → handle → respond` in library forth,
  each handler in `try-catch`.
- Worker processes — N workers accepting on the same socket under a process
  manager that respawns on crash.
- SQLite — one connection per worker, `PRAGMA journal_mode=WAL` and a
  `busy_timeout`.

---

## Re-readable repr

`render` produces a value's display form, which is not always re-readable —
strings print raw, a matrix prints as a grid. `frame>json` round-trips, but only
the JSON-expressible subset (frames, arrays, strings, numbers, booleans).
Missing is a representation that reads back through the Telic reader for
*any* value.

- `repr` ( v -- s ) — a string of Telic source that, read back, reconstructs
  an equal value: quoted strings (with `""` escaping), `[ ]` arrays, `{ :k v }`
  frames, `< >` sets, `:name` symbols, floats in shortest
  round-trip form, a matrix as its `[ … ] R C matrix` constructor.

To settle: how a value with no source form (an unbound logic var, continuation,
stream, db, or ptr) reprs — an error, or a `reify`-style canonical placeholder;
whether `repr` then `evaluate` is the intended round-trip path, which leaves the
value on the stack, or a dedicated `read` ( s -- v ) word is wanted.

---

## Handle release on collection

`T_DB`, `T_STREAM` and `T_PTR` are tagged immediates with no `ObjectKind`, so a
dropped handle holds its slot and its OS resource until the process exits.

- Mark registry slots from those three tags in the mark phase; close each open
  slot the sweep leaves unmarked.
- Record interpreter-opened descriptors at creation, so the stream sweep skips
  `stdin`/`stdout`/`stderr` and descriptors it did not open.
- Fix `ensure`: a throw crossing an outer `catch` leaves its cleanup xt on the
  data stack.

---

## Foreign function interface

- **Callbacks** — C → Telic function pointers (`qsort` comparators,
  `CURLOPT_WRITEFUNCTION` to capture a response body into a string). Needs
  re-entry plumbing: a Telic xt invoked from within a C call.
- **Struct-by-value** arguments and returns.
- **Per-call varargs** — variadic arg types chosen at the call site rather
  than fixed per declared word.
- **Finer numeric types** — `float`, unsigned variants, explicit widths.
- **`dlclose`** for library handles.

---

## Time / dates — follow-ups

- **Named-timezone conversion** — decompose/compose/format in an explicit
  zone (`epoch>date-in ( instant zone -- date )`-style, tzdata-backed).
- **Sub-second rendering** — a fractional-seconds directive in the format
  words (strftime has none).

---

## Path queries — follow-ups

- **Wildcard mutation** — `*` / `//` in `!` / `delete-at` / `update-at` for
  broadcast writes.
- **Quotation predicates** — an arbitrary `[: … :]` evaluated per node, built as an
  explicit element array.
- **Axes beyond child and descendant.**

---

## String operations

### Unicode

- **ASCII fast path**: a per-string all-ASCII flag to collapse the byte-offset
  walk in `substring`/`char-at`/`codepoint-at` to direct byte indexing.
- **Unicode case folding** — `upper-case`/`lower-case` fold ASCII only;
  folding the rest needs tables (ICU or a generated table).

---

## Error trace for a primitive invoked as an xt

- Carry the calling word through nested `execute_cfa` into the error
  trace: the caller's ip is a C local (`execute_cfa`, `saved_ip`),
  invisible to `capture_error_trace`, so `' transpose execute` inside a
  colon word names the primitive but not the caller.

---

## Loader dictionary lookup

Token resolution in the outer interpreter is a linear dictionary walk with a
string compare per candidate, which dominates the load time of large generated
source files. A name-hash index over the dictionary (or reuse of the symbol
hash table) makes resolution O(1) and leaves large-file loads I/O-bound.

---

## Multi-core parallelism: threads over the shared heap

In rough priority:

- **Numeric disjoint-write buffer / work-stealing.** Lower priority: a shared
  unboxed-`double` output buffer threaded under the matrix kernels, and
  work-stealing for skewed workloads.

---

## Coroutines, generators, lazy sequences

Building on the generator primitives:

- Lazy `map` / `filter` / `take` / `zip` as generators.telic wrappers that resume the
  source on demand, with `lazy>array` to force a finite prefix.
- A cooperative scheduler (`spawn` / `run-scheduler`, a queue of `T_CONT`s) for
  producer/consumer pipelines.
- **Kanren-style interleaving streams.** A captured continuation is the
  suspension a miniKanren stream needs — force it with `resume` and it yields an
  answer or suspends again. Fair interleaving: `mplus` (merge two streams so an
  infinite branch can't starve the other) and `bind` (flatMap with interleaving)
  — a *complete* search, distinct from the depth-first `amb` / `fail`. Generators
  are the substrate; the interleaving combinators are the work.
- **Occurs check** — `unify` builds cyclic terms today (docs/logic.md names the
  omission); add the check, or a guarded `unify-safe`, and measure the cost.
- **`yield` inside a combinator body.** `map`/`each`/`times` run their bodies
  in a nested `run_inner` with the loop state (`loop_n`, `loop_body_start`,
  the `CallContext`) on the C side, so a `shift` inside the body unwinds out
  correctly but a `resume` cannot re-enter the loop: the combinator's arity
  check fires instead. Move the loop state into return-stack frames the
  slice can carry, or run combinator bodies through ordinary return frames
  as `execute` now does, so a producer may yield from inside `each`.

All library forth on the existing primitives — no new C, except the combinator
item above.

---

## Template JIT

Close the remaining 3–4× on dispatch-bound code — fused float loops,
array/segment index loops, locals-heavy bodies (~3–6 ns/op interpreted).
C-bound words (regex, SQLite, dgemm, JSON) gain nothing; the interpreter
stays permanently as the wasm implementation, the deopt target, and the
semantics of record.

- **Shape: compile bodies, not control.** The governing invariant is that
  control state stays serializable data — continuations capture virtual
  return-stack slices of dict indices, never C-stack state; that is what
  makes resume multi-shot, images able to serialize live continuations,
  and the GC walk uniform. Straight-line native within a word body;
  branches native; calls stay virtual (trampolined first cut); return
  stack, locals frames, marks, and unwinding untouched, so a continuation
  captured under JIT is byte-identical to one captured interpreted.
- **Mechanism: templates / copy-and-patch.** One pre-built native fragment
  per op, concatenated per body, operand cells patched as immediates;
  template variants with register-pinned inputs/outputs keep stack values
  in registers. The dictionary already provides the front end: a linear
  resolved operand-inline stream, superwords one-template-per-fused-op,
  quickening's guards becoming native tests that deopt.
- **Per compiled word, a side table** mapping cell index → native offset;
  resume re-enters through the interpreter in the first cut (side-table
  jump later if generator-heavy profiles justify it); deopt to the
  interpreter is possible at every op boundary because virtual state is
  complete there.
- **Ruled out permanently:** subroutine threading with native calls,
  native-stack copying for continuations, a tracing JIT — each moves
  control state into native form and breaks capture, images, and the GC
  walk at once.
- **Do not hand-build first** (the JIT subsumes them): TOS-in-a-register,
  the loop-back patch for combinator drivers, bounds-check hoisting.
- **Requirements:** W^X executable memory (macOS `MAP_JIT` +
  `pthread_jit_write_protect_np`); arm64 templates first; invalidation on
  `forget`/redefinition (dictionary truncation defines the boundary) and
  on quickening retargets; emission fenced outside parallel regions.
- **Staging:** executable-memory plumbing with everything still
  interpreted; then templates for the fused-loop op families with a
  compile-on-Nth-execution trigger; the gate at every step is the full
  suite run JIT-on and JIT-off with nothing observable changed except
  time.

---

## Source invariants

The C sources carry no comments; constraints a future change must honor
live here instead. File and function name each invariant's home.

- The reader has one input buffer, so every nested run of source text goes
  through `run_input_text`: it saves the buffer, its length, its position and
  `need_more`, runs, and restores them. `load_file` and `p_evaluate` both call
  it — a second path that swaps the buffer itself would truncate whatever
  input the caller had left (core.c, `run_input_text`).
- `compiler.loop_begin` is negative while a `do` compiles (the negated loop
  top marks the loop as a `do`); code testing it must handle both signs
  (compiler.c, `p_do`).
- A `do` loop's counter and delta are ordinary body locals whose names embed
  a space ("do counter"), so no token can resolve to them; the `(do)`/`(loop)`
  operand cells are depth-0 slot indices (compiler.c, `p_do`).
- Locals frames are none-filled at creation (the three enter ops and
  `call_open`'s reuse frame); the per-iteration refill path deliberately does
  not re-fill (core.c, `p_enter_locals`).
- An exact-magnitude quantity rescales only through the long-long ratio APIs,
  never a double factor (dimension.c, `unit_conversion_ratio`).
- `exact_to_double` must stay correctly rounded; cross-type comparison and
  `rationalize` round-trips depend on it (exact.c, `exact_to_double`).
- A complex part is never NaN — `make_float` would canonicalize it into the
  null tag; the constructor guards (words.c, `complex_from_parts`).
- `compiler.case_chain` is 0 outside a `case`, -1 inside one with no `endof`
  yet, else the endof-branch chain head; quotations save and zero it
  (compiler.c, `p_case`).
- `word_locations` is ordered by cfa because `create_header` appends, so
  `word_location` binary-searches it; every path that lowers `vocab.here`
  (`forget`, `rollback_partial_definition`, `forget_user`) must call
  `truncate_word_locations` right after, or a later word at a reused cfa
  inherits a stale file and line. A path under the binary's directory is
  recorded relative to it, so a library word's location reads
  `lib/plot.telic:412` on every install and goldens stay portable. The same
  record carries the definition's `( a b -- c ) \ summary` comment as two
  source-pool offsets, 0 when absent; `man` and `apropos` read them only when
  the help table has no row for the name, so a reference row always wins
  (core.c, `record_word_location`; compiler.c, `definition_comment`).
- `cell_lines` maps every handler cell `emit_call` lays down while compiling a
  loaded file to its source line, ordered by address; a trace takes a frame's
  line from the greatest recorded cell below its return address, guarded by
  the owning word's cfa so a word with no recorded cells falls back to its
  definition line. Every path that lowers `vocab.here` truncates it with the
  location table (core.c, `record_cell_line`, `cell_line_at`).
- `gc_pending` is a bit set: `GC_PENDING` from the allocators, `TRACE_PENDING`
  from `trace`. Setters use `|=` and the loop clears only its own bit, so a
  collection requested while tracing does not end the trace and a trace does
  not swallow a collection. The dispatch macros test the whole word, which is
  what makes tracing free when off (core.c, `run_inner`, `trace_step`).
- A serial tag's meaning never changes; a new type takes the next tag and
  bumps SERIAL_VERSION (serialize.c, `write_value`).
- Serialized numbers are little-endian whatever the host; every new numeric
  field converts through `little_endian_32`/`little_endian_64`. Build with
  -DSERIAL_PRETEND_BIG_ENDIAN to exercise the swap path (serialize.c).
- Loading a value never redefines a word (dimension.c, `unit_declare`).
- Sets and frames are rebuilt by insertion on load, never from stored order
  (serialize.c, `read_collection`).
- Printing never allocates GC objects; an exact magnitude under an unnamed
  scaled unit folds the scale in arena temporaries (core.c,
  `print_exact_magnitude_scaled`).
- `hoist_assigned_locals` refills across input chunks so name resolution never
  depends on how source arrives; `refill_input` is fenced by `load_depth` and
  `nested_input_depth`, so a `load` or `evaluate` never reads stdin
  (compiler.c, core.c `refill_input`).
- An exact is always canonical — gcd-reduced, denominator ≥ 1, zero as
  sign 0 with numerator {0} and denominator {1}; every constructor goes
  through `exact_normalized` (exact.c).
- Exact kernels compute into raw `arena_malloc` temps and create the result
  object last, so no GC-visible allocation happens while limb pointers into
  operand objects are live (exact.c).
- An integer literal (reader, JSON, SQLite) promotes to exact only when the
  value does not round-trip through a double — `decimal_lossless_as_double`
  is the single rule (exact.c).
- A new heap-referencing tag must join both walkers: `mark_value_at`'s tag
  filter and `references_region_depth`'s switch — a tag missing from the
  second has its region objects rewound under a pmap result and reused
  (the quantity case was latent until exacts crashed there) (core.c,
  functional.c).
- Handle-shaped tags compare by payload, not by tag alone: `T_STREAM`,
  `T_DB`, `T_PTR` and `T_CONT` sit with `T_SYMBOL`/`T_XT` in the
  payload-comparison branch. A new handle tag left to the `default` case
  compares equal to every other value carrying that tag, which makes `=`
  useless for it and collapses a set of them to one element (core.c,
  `val_cmp_depth`).
- `read-available` polls with a zero timeout before it reads, so it answers
  `""` rather than blocking, and `wait-readable` counts any `revents` — end of
  input included — so a stream whose writer has exited comes back ready and the
  read that follows answers `none` instead of waiting forever (io.c,
  `p_read_available`, `p_wait_readable`).
- `execute_xt` pushes a return frame aimed at the immortal stop cell
  before running a body, so continuations captured inside see the same
  return-stack shape as a trampoline call. Changing either call path
  changes captured-continuation layout (core.c, `execute_xt`).
- `execute` on a colon body must push an ordinary return frame and dispatch
  inline, never through `execute_xt`: a nested loop's stop frame inside a
  captured slice ends a resumed run early (words.c, `p_execute`).
- A combinator's fast path calls a primitive xt's handler once per element and
  takes its return as the end of the element, so it may run only primitives
  whose dispatch chain completes before returning. `execute` dispatches inline
  into a body whose ops may return to the loop (`sum`, the reductions), so
  `call_open` routes it through `execute_cfa` like `dovar`; a new primitive
  that jumps into compiled code must join that exclusion (core.c, `call_open`;
  telic.h, `call_step`).
- While `TRACE_PENDING` is set, `call_open` disables the fast path so every
  combinator element runs under a `run_inner` whose loop prints each op,
  first op included; the fast path itself carries no trace check, which is
  what keeps `map` at its measured per-element cost (core.c, `call_open`).
- `shift` must raise the unwinding flag as `shift-with` does; nothing after
  `shift` in the shifting word runs at capture time (words.c, `p_shift`).
- The `WORD_LINK` chain from `latest_cfa` is strictly descending: every
  `create_header` appends, so each new cfa exceeds the previous. `gc`
  relies on this — it walks the chain (descending) and reverses in place
  to get the ascending cfa order its body-range scan needs, instead of
  sorting. A change that lets cfas be created out of order must restore a
  sort there (core.c, `gc`).
- A curried token holds its target xt in `items[0]` and its bound values in
  `items[1..]`, so its object kind stays `OBJECT_ARRAY` and the sweep, the
  image format and `references_region` need no case of their own (core.c,
  `curried_new`).
- `call_open_callable` roots the token for the whole combinator call: the
  invocation slice holds only its handle, which GC does not scan (core.c,
  `call_open_callable`).
- No op receives a frame depth above 0. A quotation reaches an enclosing
  local only as a capture: a trailing received slot of its own frame, copied
  from the immediately enclosing scope at `:]`. The capture pre-scan must
  declare every outside name the body reads, nested quotations included, and
  must treat `to`/`do` targets and nested heads as shadowing, exactly as
  token resolution does (compiler.c, `scan_body_captures`,
  `reject_outer_local`).
- `recurse` inside a capturing quotation must re-push the captured slots
  before the call; the head expects them (compiler.c, `p_recurse`).
- `forget_user` frees only objects above `object_space.init`; below it
  sit literals baked into the compiled-in vocabulary (e.g. `run`'s
  `" +"`), which must survive every reset (core.c, `forget_user`).
- A `docol` cell is one cell as a quotation header, two as a colon-word
  call; the only platform-independent discriminator is
  `quotation_starts_at` (wasm function pointers are small table indices,
  so "the next cell looks like a handler/cfa" heuristics fail there).
  Every body walker — `running_op_name`, `see_compiled_body`,
  `see_tree_body`, `inline_word_body`, `mark_body` — classifies through
  it; new walkers must too. This makes span coverage a correctness
  invariant: every quotation header must have a recorded span, so
  `record_quotation_span` fails loudly at the table cap instead of
  dropping, and `inline_word_body` declines to splice a
  quotation-bearing body (emits a plain call) rather than copy headers
  to span-less addresses (core.c, compiler.c).
- `op_cell_count` must list every op that carries operand cells; the
  body walkers step by it, so an op missing from the list desyncs them
  on both platforms — a skipped literal in `mark_body` means premature
  collection. A new primitive that emits operands after its handler
  cell gets a matching entry in the same change (core.c,
  `op_cell_count`).
- The overall matrix reductions unroll into four accumulators so
  non-associative float addition still vectorizes; associative ops
  tolerate it. Collapsing to one accumulator kills the vectorization
  (matrix.c, `MATRIX_REDUCE_OVERALL_OP`).
- `matrix_sum_dense` must never compile under `float_control(precise,
  off)`: that pragma marks its instructions no-NaN, the optimizer then
  folds `matrix_sum_overall`'s `isnan` to false, and the NaN-skipping
  retry is deleted. `clang fp reassociate contract` gives the vectorizer
  what it needs without the no-NaN license (matrix.c).
- The superword fuser rewrites `<arr> <arr> <idx> @i [<delta>] <op>
  <idx> !i drop` into the single `(<op>!i) <idx-slot>` ops by matching
  the compiled dict shape; changes to how those idioms compile must
  update the matcher (superwords.c).
- A pmap worker that finds its result chain too deep (possible cycle)
  conservatively keeps the whole region rather than rewinding it
  (functional.c).
- A failed handle claim must not leave `space->n` above `cap`; readers walk
  to `n` (core.c, `local_claim_handle`).
- `HANDLE_PRESSURE_SLOTS` must exceed one claim per worker, or only the
  worker that trips it collects (telic.h).
- The byte trigger counts malloc'd payloads only — matrix, segment and
  continuation storage through `heap_bytes_add`/`heap_bytes_sub` — and is
  tested once per object, in `object_alloc_slot`, which stays out of line so
  the test adds no inline cost to the constructors (clang inlines
  `object_new_frame`/`object_new_string`/`object_new_array` into hot callers
  such as `copy_value_inner` only while their cost stays under its threshold;
  a few dozen instructions added to `object_new` flip that and cost 5% on
  frame-copy loops). Arena objects (arrays, strings, frames, sets,
  exacts) never request a collection by design: their churn grows the heap
  until the handle ceiling, an explicit `gc`, or a payload-triggered
  collection. `GC_PENDING` is set even while `gc_disabled` and stays pending
  until the collector is enabled again, so a `copy` that crosses the
  threshold collects at the first instruction after it (core.c,
  `object_new`, `run_inner`).
- An Object whose `items` equals its `inline_items` owns no arena block:
  `object_new_array` keeps up to `INLINE_ITEMS_CAPACITY` elements inside the
  struct, every growth of `items` (array or set, including an array turned
  set by `group-by`) goes through `items_reserve`/`ITEMS_GROW_IF_FULL`, and
  `free_one_object` skips the free. A site that assigns or reallocs `items`
  directly frees or grows inline storage as if it were a block (core.c,
  telic.h).
- `object_alloc_slot` reuses free-listed handles before claiming fresh ones,
  so the handle table, and every sweep over it, tracks the live set plus one
  collection's churn rather than the run's total allocation (core.c).
- The handle-pressure test must stay in the claim branch; per allocation it
  contends on `space->n` (core.c, `local_claim_handle`).
- Bind BLAS and LAPACKE from the statistics shared library's single
  handle; never add a second `ffi-open`. Ports keep BLAS reachable from
  that handle (lib/statistics.telic; Makefile `-reexport_framework` on
  Darwin, the DT_NEEDED OpenBLAS dependency on Linux).
- Keep statistics.telic native-only; wasm excludes the FFI and skips its
  tests (wasm-skip.txt).
- Element-wise matrix ops broadcast any dimension of size 1 (n×1 and
  1×k against n×k), not only scalars; the reference documents only the
  scalar case — a doc gap to close (matrix.c,
  `MATRIX_ELEMENTWISE_OP`).
- `dodefer` is a two-cell op of the `dovar` family (body walkers advance by two),
  and `defer` reserves four cells with zeroed pads so `embodies!` overwrites the
  word in place as a `docol` forwarder (core.c, compiler.c).
- `(tailcall)` is a two-cell op (target cfa operand, `op_cell_count` returns 2);
  `rewrite_tail_calls` at `;`/`:]` converts only `docol` tail calls, never when
  `body_has_tail_hazard` holds (`>r`/`r>`/`r@`/`reset`/`shift`/`shift-with`/`fail`,
  or locals plus a quotation), and `inline_word_body` demotes a copied
  `(tailcall)` back to a call (compiler.c, core.c `p_tailcall`/`inline_word_body`).
  `amb` needs no exclusion: it removes its own choice mark before returning.
