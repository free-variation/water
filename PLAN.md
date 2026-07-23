# Water — future work

A TODO list of pending work, highest priority first.

---

## xgboost: residuals

- **Model persistence.** Bind `XGBoosterSaveModel`/`XGBoosterLoadModel` as
  `xgb-save ( booster path -- )` and `xgb-load-model ( path -- booster )`
  (create an empty booster, then load into it). Acceptance: `xgb-save` →
  `xgb-load-model` → `xgb-predict` reproduces the original predictions exactly.
- **Multiclass / multi-output.** Read `out_dim`/`out_shape` in `xgb-predict`
  and `xgb-importance` so a multiclass model returns the full `[n, n_classes]`
  (predict) / `[k, n_classes]` (importance) matrix.

---

## Statistics: a minimal spanning set

Build applied statistics from a few kernels, each reused across many methods,
with inference resampling-first (index loops over asymptotic tables). Compose
everything in library code from four kernels — SVD, weighted least squares (the
`fit-linear` sqrt-w idiom), the resampling loop (`bootstrap`), and a
pairwise-distance primitive (§4, small C unless the dgemm identity suffices).

Add a built-in `svd` so the embedded (wasm-capable) library can decompose
without the FFI: a one-sided Jacobi kernel in C, masked by the dgesvd binding.
Pin its goldens on S and the U·S·Vᵀ reconstruction, never raw U/V entries
(column signs are not canonical and the two implementations disagree on them).

Where C work is and is not needed: IRLS needs none — each iteration is dgemm +
element-wise link/variance words + a LAPACK solve, and a new family (§2) swaps
only the element-wise words. The small C beyond the kernels:
`erf`/normal-CDF (§2 probit, BCa intervals — a few lines alongside
`exp`/`tanh`; `qnorm` covers the quantile inverse), the
empirical-distribution statistics
(§5 — `ks`, `cvm`, `ad`, `wasserstein`, each a small
one-pass kernel), and `row-argmins` with its argmax and column twins
(index-returning kin of `row-mins`, one pass — k-means' assignment step
needs the per-row index the flat `argmin` does not return). Gaussian
mixtures (§1) add no C, only a `dpotrf`/`dpotrs` export to the lapacke
vendoring. Rank transforms (§3) compose `iota` + `sort-by`; k-means,
spectral methods, and the tensor step of LCA (§1) otherwise ride
`dgemm`/`svd` at library level.

### 1. SVD exploitation (library code on `svd` / `dgemm`)

- **PCA** — center columns, SVD: loadings V, scores U·S, component variances
  S²/(n−1). Whitening, low-rank approximation (Eckart–Young), and
  principal-component regression on top.
- **k-means** — Lloyd iterations over dgemm distances, each row assigned
  by `row-argmins` (the new small C word above); needed by spectral
  clustering, useful alone. k-means++ initialization draws on the RNG.
- **Gaussian mixtures** — EM: per-cluster Mahalanobis distances and
  log-determinants from Cholesky (`dpotrf`/`dpotrs` — one more exported
  symbol pair in the lapacke vendoring), responsibilities and the M-step
  as dgemm + element-wise ops. k-means seeds it; LCA's
  spectral-init-then-EM pattern applies.
- **Spectral clustering** — affinity → normalized Laplacian → top-k embedding
  (PSD, so dgesvd *is* its eigendecomposition) → k-means on the embedded rows.
- **Classical MDS / kernel PCA** — double-centered squared distances /
  centered Gram matrix, decompose. dgesvd loses eigenvalue signs on
  indefinite matrices; Euclidean distances and PSD kernels are safe.
  Indefinite inputs need a dsyevr binding (re-vendor lapacke with one more
  exported symbol) — defer until wanted.
- **Correspondence analysis / MCA** — SVD of the standardized residuals of a
  contingency (or indicator) table; the categorical companion to PCA and the
  descriptive cousin of LCA, sharing its one-hot plumbing.
- **Spectral LCA** — the method-of-moments estimator (Anandkumar et al.,
  JMLR 2014), built as spectral initialization + EM polish, not
  spectral-only:
  - Partition items into three groups; build cross-moment matrices between
    groups from one-hot indicator matrices (dgemm); symmetrize the views.
  - Whiten with the top-k SVD of M₂ (W = UₖΣₖ^(−1/2)); k read from the
    singular-value gap, which is also the exposed diagnostic.
  - Accumulate the whitened k×k×k third moment directly from whitened rows —
    never materialize the p³ tensor.
  - Tensor power iteration with deflation and random restarts (the
    positive-random-slice PD shortcut as the cheap first cut) for the
    orthogonal factors.
  - Reconstruct wᵢ = 1/λᵢ² and aᵢ = λᵢ(Wᵀ)⁺ãᵢ; project onto the simplex.
  - Finish with a few EM iterations (elementwise ops only): the spectral
    step removes local optima and label switching, EM restores efficiency.
    Small-n accuracy degrades with conditioning (1/w_min, σₖ(M₂)); the M₂
    spectrum diagnostic says when to distrust the seed.
- **Total least squares / orthogonal regression** — smallest right singular
  vector of [X | y].

To settle: whether spectral clustering's affinity construction (RBF
bandwidth, kNN graph) waits on the distance primitive (§4); how much of
CA/MCA folds into the LCA indicator plumbing; whether clustering and LCA
warrant their own files rather than statistics.h2o.

### 2. Weighted least squares and the GLM family

- **fit-weighted** — name the sqrt-w row-scaling idiom inside `fit-logistic`
  as ( X y w -- beta ).
- **More GLM families** — probit (needs an element-wise `erf`/normal-CDF C
  word; `qnorm` covers the quantile side), negative binomial (estimate the
  dispersion), and multinomial/ordinal logistic (a stacked design, not a
  single reweighting).
- **Ridge** — singular-value filtering σ/(σ²+λ) on the design; λ chosen by
  cross-validation (§3).
- **LDA** — within-class whitening + SVD of the class means.
- **Random Fourier features** — approximate kernel classification and
  regression through the same linear fits; RNG + dgemm, no QP solver, which
  is why a separate SVM earns no spot.
- **Local regression and KDE** — kernel weights by distance to each
  evaluation point: LOESS is fit-weighted in a loop, KDE is the weights
  alone.

To settle: bandwidth selection (CV vs plug-in rules); whether multinomial
logistic reuses the Firth machinery or defers it to the binary case.

### 2b. Correlations: residuals

- Give `ranks` midranks for ties — average each tied run's ranks — so
  `correlation-spearman` stops drifting on heavily tied data and returns
  null (not 1) for a constant vector, matching pearson's degenerate cases.

### 3. The resampling loop as the inference engine

Generalize bootstrap's shape — index sets → refit → collect — into the
library's whole inference story; parametric standard errors and asymptotic
tables stay out.

- **permutation-test** — shuffle one column's indices for the null;
  replaces the t-test, ANOVA, and correlation tests.
- **jackknife** — leave-one-out index partitions; reuse `cross-validate`'s
  units/fit-xt/score-xt shape.
- **Model metrics** — the losses CV selects on: squared/absolute error,
  accuracy, and confusion counts as element-wise ops; AUC as a rank
  statistic (argsort — Mann–Whitney's twin); ROC and calibration curves
  as cumulative counts over the argsort order; isotonic calibration
  (PAVA) as a library pass.
- **Rank statistics** — build Wilcoxon / Mann–Whitney and Kruskal–Wallis as
  rank statistics with permutation nulls, over the `ranks` word spearman
  uses. Midranks for ties (§2b) feed directly into these.
- Ride pmap for the parallel variants as pbootstrap does; reuse the
  per-replicate seeding pattern (`resample-indices-ext` at run-seed + i,
  as `bootstrap-with` does) for the permutation and jackknife index
  generators.

To settle: one generic word ( data index-gen-xt statistic-xt n -- dist )
with bootstrap/permutation/jackknife as instances, or a word per method.

### 4. Pairwise distances (small, last)

( X Y -- D ) via the dgemm identity ‖x‖² + ‖y‖² − 2XYᵀ, plus a direct kernel
for other metrics. Unlocks kNN classification/regression, MDS input, RBF
affinities for spectral clustering, hierarchical/agglomerative clustering
(linkage passes over the distance matrix), DBSCAN (neighborhood sets via
`where`), and the distance-based tests — distance correlation, PERMANOVA,
MMD — whose nulls come from the permutation loop.

To settle: C word vs dgemm composition in library code (the identity is
three ops but loses precision on near-duplicate rows).

### 5. Empirical distributions

Distances between empirical distributions, each one pass over sorted
samples in the `ks-distance` mold:

- **one-sample ks** — against a reference CDF as a quotation ( x -- p );
  a normal reference is another consumer of the §2 `erf` word.
- **cvm / ad** — C: Cramér–von Mises and Anderson–Darling, the ks pass
  with accumulation; AD is CvM with 1/(F(1−F)) tail weights.
- **wasserstein** — C: 1-D W₁; equal-n samples pair sorted elements
  directly (mean |gap|), unequal n integrates the quantile difference over
  the merged grid.
- Significance by permutation of pooled labels (§3); energy distance joins
  once §4's pairwise distances land.

To settle: whether the permutation replicate loop wants a C null driver
(in-place byte-label shuffle + statistic in one loop, skipping the
per-replicate sort) once profiling shows the sort dominating.

### Test data

Three layers, adopted in this order:

- **Seeded synthetic, generated in-test** — the existing 104–107 pattern:
  fixed `seed`, planted parameters recovered (plant β, fit, compare),
  edge cases, resampling determinism.
- **Canonical sets in `data/`** — external ground truth: every fitting or
  inference word gets at least one test whose expected value was
  cross-checked against R or scikit once, the reference noted in the test
  comment, before the golden freezes it. `iris.tsv` is vendored; add
  `mtcars` (regression), `faithful` (KDE, mixtures), and `anscombe`
  (graphing) — a few KB each, freely redistributable.
- **Large synthetic for benchmarks** — generated by script as needed (the
  `logistic-sim.tsv` route), never vendored; bench material, not goldens.

---

## Reference coverage for the loadable libraries

Add a reference.md section for lib/statistics.h2o (`svd`, `fit-linear`,
`bootstrap`, the regressions, the xgboost binding, the dgemm rebinding) so
`help` answers after a load. Consider a `words` group distinguishing lib/-loaded words
from session definitions, so the undocumented canary reaches them.

---

## Basic graphing: residuals

- **Chart set** — step (the ecdf as drawn) and bar charts.
- **Stats consumers** — QQ plots (over `sort` + `qnorm`; bring back
  plot-side `fit-line` over `abline`), ROC and calibration
  curves (the model-metrics bullet), residual and fit plots for the
  regressions.
- **Log axes** — transform at the domain with power-of-ten tick labels,
  instead of transforming the data and labeling in log units.
- **Torn frames on overwrite** — `write-file` truncates in place; either
  a `rename-file` word for temp-and-rename atomicity, or accept that
  viewers re-read fast.
- Reference rows for the plot words (the loadable-library coverage item
  above).

---

## Executable documentation

Every example in the docs runs, and is machine-verified to run. The
motivation is genAI as much as readers: a new language lives in no model's
weights, so its documentation *is* its corpus — read in-context today,
fine-tuning material tomorrow — and one broken example poisons generation
the way a wrong golden would poison the suite. (The README's logic example
was broken for an unknown span until a session happened to run it; a
harness would have caught it at `make test`.)

- **README taste block as a golden** — extract the `## A taste` fence,
  run it, pin the output. The block is the single most-read (by humans
  and models) Water program in existence; it must never regress.
- **reference.md snippets** — the extractable inline examples join the
  same harness; a marker separates runnable from illustrative-only.
- **One extraction harness** — a tools/ script in the gen-help/gen-editors
  family renders marked snippets into generated .h2o/.expected pairs that
  tests/run.sh picks up like any other test.

To settle: the runnable-vs-illustrative marker; pin outputs verbatim or
only assert error-free execution; nondeterministic examples (`wall-now`,
unseeded draws) — skip-mark them or normalize their output.

---

## Language pack

One concatenated file sized for a model's context window: the whole
language, learnable in a single read. Water cannot be in the weights; it
can be in every prompt. The measure of success is direct — how good is
Water code written by a strong model given only this file?

- **Contents** — the reference (word tables are the core), the README
  taste block, the tokenizer's self-delimiting rules, the idiom notes a
  generator can't derive (locals are uninitialized by design; matrices
  for numbers, arrays for structure; resampling patterns), and a few
  verified programs from examples/.
- **Generated, never written** — tools/gen-pack.py beside gen-help and
  gen-editors, so the pack cannot drift from its sources; `make pack`,
  output `water-pack.md` (or `llms.txt`, per the emerging convention).
- **Budgeted** — the generator counts tokens (chars/4 is close enough)
  against a target (~50k) and names what to trim when it overflows.
- Complements lib/claude.h2o, which is the other direction: that file is
  Water calling a model; the pack is a model writing Water.

To settle: one pack or two tiers (lean core + full); whole example
programs or excerpts; whether the pack embeds the executable-docs goldens
as input/output pairs (few-shot format) once that section lands.

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
interned pool) and therefore how a slot is retired and reused; how `save-image`
serializes a collectible symbol (by name, re-interned on load, since its index
is not stable); whether pinned-vs-collectible is decided at the intern call site
or inferred from whether interning happens during compilation.

---

## FastCGI service

Run Water as a long-lived FastCGI application behind an off-the-shelf web
server (nginx, Caddy, lighttpd, Apache). The web server owns everything HTTP —
TLS termination, HTTP/1.1–3, request parsing, static files, timeouts, rate
limiting, access logs, load balancing — and forwards each request over a Unix or
TCP socket as FastCGI records. Water never sees a raw HTTP byte: it decodes
the records, runs a handler, writes the response.

Depends on symbol collection: a long-lived worker parsing request JSON mints
symbols from unbounded request keys, so without collection the worker leaks
until restart. Untrusted request bodies also make fuzzing `json>frame` (and
`load-image`, if images ever travel) a prerequisite — mutate a seed corpus
into the ASan build and fix what falls out.

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
  `END_REQUEST`. The framing is simple: embedded forth over `read-n`/`write` plus byte
  arithmetic, with maybe a tiny C helper for the 2/4-byte length fields.

**Serve loop.** A plain sequential `accept → decode → handle → respond` loop in
library forth, each handler wrapped in `try-catch` so a bad request can't kill the
worker; per-request allocations are reclaimed by GC. No threads.

**Worker processes.** Run N worker processes all accepting on the same socket
(the kernel load-balances) under a process manager (e.g. systemd template units)
that respawns on crash. One request per worker isolates failures; the web server
retries elsewhere.

**SQLite.** Each worker opens its own connection; enable WAL once
(`PRAGMA journal_mode=WAL`) plus a `busy_timeout`, so concurrent reads across
workers don't block and writes serialize safely (single host).

**Cost:** `accept` + `read-n` are small C; the FastCGI codec is library forth (plus an
optional tiny C codec for the integer fields); the serve loop and response
builders are library forth.

---

## Re-readable repr

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
the locals-frame and trail rewind the unwind carries; whether
`before`/`after` observe the region's data stack or run isolated.

---

## Foreign function interface

- **Callbacks** — C → Water function pointers (`qsort` comparators,
  `CURLOPT_WRITEFUNCTION` to capture a response body into a string). Needs
  re-entry plumbing: a Water xt invoked from within a C call.
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
- **Case folding**: `upcase`/`downcase`. Unicode-correct folding needs
  tables (ICU or a generated table), so even an ASCII-only first cut should
  name the boundary.

---

## Loader dictionary lookup

Token resolution in the outer interpreter is a linear dictionary walk with a
string compare per candidate, which dominates the load time of large generated
source files. A name-hash index over the dictionary (or reuse of the symbol
hash table) makes resolution O(1) and leaves large-file loads I/O-bound.

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

## Coroutines, generators, lazy sequences

Building on the generator primitives:

- Lazy `map` / `filter` / `take` / `zip` as generators.h2o wrappers that resume the
  source on demand, with `lazy>array` to force a finite prefix.
- A cooperative scheduler (`spawn` / `run-scheduler`, a queue of `T_CONT`s) for
  producer/consumer pipelines.
- **Kanren-style interleaving streams.** A captured continuation is the
  suspension a miniKanren stream needs — force it with `resume` and it yields an
  answer or suspends again. Fair interleaving: `mplus` (merge two streams so an
  infinite branch can't starve the other) and `bind` (flatMap with interleaving)
  — a *complete* search, distinct from the depth-first `amb` / `fail`. Generators
  are the substrate; the interleaving combinators are the work.

All library forth on the existing primitives — no new C.

---

## Source invariants

The C sources carry no comments; constraints a future change must honor
live here instead. File and function name each invariant's home.

- `execute_xt` pushes a return frame aimed at the immortal stop cell
  before running a body, so continuations captured inside see the same
  return-stack shape as a trampoline call. Changing either call path
  changes captured-continuation layout (core.c, `execute_xt`).
- The `WORD_LINK` chain from `latest_cfa` is strictly descending: every
  `create_header` appends, so each new cfa exceeds the previous. `gc`
  relies on this — it walks the chain (descending) and reverses in place
  to get the ascending cfa order its body-range scan needs, instead of
  sorting. A change that lets cfas be created out of order must restore a
  sort there (core.c, `gc`).
- `forget_user` frees only objects above `object_space.init`; below it
  sit literals baked into the compiled-in vocabulary (e.g. `run`'s
  `" +"`), which must survive every reset (core.c, `forget_user`).
- Images save only above the `init_*` watermarks: the embedded library is rebuilt
  every process and is not user state. Anything that grows the watermark
  set must grow all of it — here, latest_cfa, names, sources, symbols,
  objects, pairs, dimensions, quotation spans (core.c,
  `construct_vocabulary`; image.c).
- Image op translation: dovar/dosym call cells carry a trailing
  target-cfa operand that `op_cell_count` does not include;
  `image_op_cells` accounts for it (image.c).
- A `docol` cell is one cell as a quotation header, two as a colon-word
  call; the only platform-independent discriminator is
  `quotation_starts_at` (wasm function pointers are small table indices,
  so "the next cell looks like a handler/cfa" heuristics fail there).
  Every body walker — `running_op_name`, `see_compiled_body`,
  `see_tree_body`, `inline_word_body`, `mark_body`, the image saver —
  classifies through it; new walkers must too. This makes span coverage
  a correctness invariant: every quotation header must have a recorded
  span, so `record_quotation_span` fails loudly at the table cap instead
  of dropping, the image format persists spans, and `inline_word_body`
  declines to splice a quotation-bearing body (emits a plain call)
  rather than copy headers to span-less addresses (core.c, compiler.c,
  image.c).
- `op_cell_count` must list every op that carries operand cells; the
  body walkers step by it, so an op missing from the list desyncs them
  on both platforms — a skipped literal in `mark_body` means premature
  collection. A new primitive that emits operands after its handler
  cell gets a matching entry in the same change (core.c,
  `op_cell_count`; image.c, `image_op_cells` for dovar/dosym/dounit).
- `save-image`'s per-word cfa array is `static` to keep ~4MB off the
  call stack (image.c, `p_save_image`).
- The overall matrix reductions unroll into four accumulators so
  non-associative float addition still vectorizes; associative ops
  tolerate it. Collapsing to one accumulator kills the vectorization
  (matrix.c, `MATRIX_REDUCE_OVERALL_OP`).
- `matrix_sum_dense` must never compile under `float_control(precise,
  off)`: that pragma marks its instructions no-NaN, the optimizer then
  folds `matrix_sum_overall`'s `isnan` to false, and the NaN-skipping
  retry is deleted. `clang fp reassociate contract` gives the vectorizer
  what it needs without the no-NaN license (matrix.c).
- The superword fuser rewrites `<arr> <idx> <arr> <idx> @i [<delta>]
  <op> !i drop` into the single `(<op>!i)` ops by matching the compiled
  dict shape; changes to how those idioms compile must update the
  matcher (superwords.c).
- A pmap worker that finds its result chain too deep (possible cycle)
  conservatively keeps the whole region rather than rewinding it
  (functional.c).
- In-progress cons chains are gc-rooted during multi-pair allocation so
  a collection triggered mid-build cannot reap the spine (collections.c,
  `array>cons`).
- Bind BLAS and LAPACKE from the statistics shared library's single
  handle; never add a second `ffi-open`. Ports keep BLAS reachable from
  that handle (lib/statistics.h2o; Makefile `-reexport_framework` on
  Darwin, the DT_NEEDED OpenBLAS dependency on Linux).
- Keep statistics.h2o native-only; wasm excludes the FFI and skips its
  tests (wasm-skip.txt).
- Element-wise matrix ops broadcast any dimension of size 1 (n×1 and
  1×k against n×k), not only scalars; the reference documents only the
  scalar case — a doc gap to close (matrix.c,
  `MATRIX_ELEMENTWISE_OP`).
