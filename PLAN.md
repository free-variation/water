# Water — future work

A TODO list of pending work, highest priority first.

---

## Matrix vector vocabulary

Vectors are n×1 matrices — the unboxed representation the C kernels are
typed to — but the generic container vocabulary is array-only, so numeric
pipelines that lead with a sort detour through boxed arrays. Five words
close the gap; the existing names go polymorphic in the house style
(`+`/`=`/`has?` already dispatch on tags):

- `sort` — on a matrix: sorted copy, elements in row-major order, shape
  preserved; a specialized double sort (inlined-comparison introsort, or
  bit-pattern radix at large n — not libc `qsort`, whose per-comparison
  function call costs 2–5×), no `val_cmp`, no boxing. Front of the ecdf
  and rank-transform pipelines below.
- `reverse` — on a matrix: element order reversed, shape preserved;
  descending views after `sort`.
- `argsort` — ( m -- m ) the sorting permutation as a matrix; ranks are
  argsort applied twice, making the statistics plan's rank transform pure
  C passes.
- `stack` — ( a b -- m ) row-wise concatenation, the missing twin of
  `augment`; joins vectors and samples.
- `where` — ( mask -- indices ) indices of the nonzero elements of a 0/1
  matrix (what `lt`/`gt` return) as an index vector; `select-rows` learns
  to accept an index matrix alongside its index array, closing the
  mask → select loop.

To settle: NaN ordering (a total order needs them placed deliberately —
last, matching `val_cmp`'s spirit; note the radix bit-transform scatters
NaNs to both ends, so "last" costs a separate pass there, vs a comparator
tweak under introsort); argsort tie handling (compare indices on equal
values for deterministic, effectively stable ranks); whether `sort` on a
non-vector shape is worth defining at all or should error until a
per-axis story exists.

---

## Statistics: a minimal spanning set

Build applied statistics from a few kernels, each reused across many methods,
with inference resampling-first (index loops over asymptotic tables). The
kernels: SVD (present), weighted least squares (present as `fit-linear` plus
the sqrt-w idiom), the resampling loop (present as `bootstrap`), one tree
grower (§4 — the one substantial new C), and a pairwise-distance primitive
(§5 — small C, unless the dgemm identity suffices). Everything else is library
code composing them.

Where C work is and is not needed: IRLS needs none — each iteration is dgemm +
element-wise link/variance words + a LAPACK solve, all C already; `fit-logistic`
runs the full Firth loop as library code today, and generalized IRLS (§2) only
swaps the element-wise words in the loop. The small C beyond the kernels:
`erf`/normal-CDF and its inverse Φ⁻¹ (§2 probit, BCa intervals, QQ plots — a
few lines alongside `exp`/`tanh`), the empirical-distribution statistics
(§6 — `ks`, `cvm`, `ad`, `wasserstein`: used constantly, each a small
one-pass kernel), and `row-argmins` / its argmax and column twins
(index-returning kin of `row-mins`, one pass — k-means' assignment step
needs the per-row index, and only the flat `argmin` exists). Gaussian
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
bandwidth, kNN graph) waits on the distance primitive (§5); how much of
CA/MCA folds into the LCA indicator plumbing; where the library splits
(statistics.h2o is already the stats home — clustering and LCA may warrant
their own files).

### 2. Weighted least squares and the GLM family

- **fit-weighted** — name the sqrt-w row-scaling idiom `fit-logistic`
  already uses ( X y w -- beta ).
- **Generalized IRLS** — parameterize link and variance in one loop:
  Poisson (log link), probit, negative binomial, multinomial/ordinal
  logistic. One reweighting loop, one linear solver. Library code
  throughout; probit is the one link needing a new C word (element-wise
  `erf`/normal-CDF, with its inverse Φ⁻¹ alongside — BCa bootstrap
  intervals and QQ plots want the normal quantile function).
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

### 3. The resampling loop as the inference engine

Generalize bootstrap's shape — index sets → refit → collect — into the
library's whole inference story; parametric standard errors and asymptotic
tables stay out.

- **permutation-test** — shuffle one column's indices for the null;
  replaces the t-test, ANOVA, and correlation tests.
- **jackknife**, **cross-validate** — leave-one-out and k-fold index
  partitions; CV is the model-selection tool the rest of this plan cites.
- **Model metrics** — the losses CV selects on: squared/absolute error,
  accuracy, and confusion counts as element-wise ops; AUC as a rank
  statistic (argsort — Mann–Whitney's twin); ROC and calibration curves
  as cumulative counts over the argsort order; isotonic calibration
  (PAVA) as a library pass.
- **Rank transform** — sort + inverse permutation; Spearman, Wilcoxon /
  Mann–Whitney, Kruskal–Wallis become rank statistics with permutation
  nulls.
- Parallel variants ride pmap as pbootstrap does.

To settle: one generic word ( data index-gen-xt statistic-xt n -- dist )
with bootstrap/permutation/jackknife as instances, or a word per method;
seeding discipline so parallel resampling stays reproducible per worker.

### 4. One tree grower, three predictors

The recursive partitioner is the one substantial new C kernel: split search
over presorted columns on gradient/hessian statistics, with depth and
min-leaf limits. Growing on grad/hess pairs serves all three products;
squared error and Gini/entropy are special cases of the statistics fed in.

- **Single tree** — readable rules; cost-complexity pruning by CV. Represent
  the tree as a frame tree so it prints, stores, and save-images like any
  value; prediction is a C traversal.
- **Random forest** — bootstrap rows + per-split feature subsampling around
  the grower; out-of-bag error and permutation importance fall out of §3.
  The resampling-native ensemble.
- **Gradient boosting** — fit-to-gradients with shrinkage and early stopping
  (CV); any loss with grad/hess: logistic, multiclass, quantile, survival.
  The tabular-accuracy workhorse.

To settle: exact split search only, or histogram binning from the start
(histograms are what make boosting fast at scale); categorical features —
one-hot at the library level vs native category splits in the grower;
missing values (surrogate splits vs a learned default direction); how much
of the ensemble loop is C vs library code over a
( X y grad hess params -- tree ) word.

### 5. Pairwise distances (small, last)

( X Y -- D ) via the dgemm identity ‖x‖² + ‖y‖² − 2XYᵀ, plus a direct kernel
for other metrics. Unlocks kNN classification/regression, MDS input, RBF
affinities for spectral clustering, hierarchical/agglomerative clustering
(linkage passes over the distance matrix), DBSCAN (neighborhood sets via
`where`), and the distance-based tests — distance correlation, PERMANOVA,
MMD — whose nulls come from the permutation loop.

To settle: C word vs dgemm composition in library code (the identity is
three ops but loses precision on near-duplicate rows).

### 6. Empirical distributions

The ecdf and distances between empirical distributions. These will be used
constantly, so the statistics are small C kernels, not library algebra:
each is one allocation-free pass over the once-sorted pooled sample plus a
0/1 label vector, so a permutation replicate is shuffle + one C word.
Library code owns the ecdf representation and the §3 permutation plumbing;
nulls come from §3, not asymptotic tables.

- **ecdf** — ( sample -- ecdf ) library: a sorted copy as the
  representation; **ecdf-at** ( ecdf x -- p ) evaluates by binary search;
  mapping it over a grid gives the step function.
- **ks** — C: two-sample Kolmogorov–Smirnov, sup |F₁ − F₂| in one pass.
  The one-sample form takes a reference CDF as a quotation ( x -- p ); a
  normal reference is another consumer of the §2 `erf` word.
- **cvm / ad** — C: Cramér–von Mises and Anderson–Darling, the same pass
  with accumulation; AD is CvM with 1/(F(1−F)) tail weights.
- **wasserstein** — C: 1-D W₁; equal-n samples pair sorted elements
  directly (mean |gap|), unequal n integrates the quantile difference over
  the merged grid.
- Significance by permutation of pooled labels (§3); energy distance joins
  once §5's pairwise distances land.

To settle: the ecdf representation (bare sorted matrix vs a frame carrying
n and the sort); tie handling in ks (step both functions at a shared point
before comparing); whether the replicate loop eventually wants a C null
driver (in-place byte-label shuffle + statistic in one loop) once profiling
shows the shuffle dominating — the statistics words stay useful either way.

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

## Basic graphing

Charts rendered to an SVG file; a browser tab does the display (an
auto-refresh wrapper page or extension re-reads it). Water writes, the
browser refreshes — no windowing, no display dependency, no rasterizer.

- **SVG output** — text generation over `format` + `write-file`; no
  rasterizer, no new C. Each plot word draws the whole chart and
  overwrites the target file, so the viewer always shows the latest state.
- **Chart set** — scatter, line, step (the ecdf as drawn), histogram
  (binning in library code), bar; axes, ticks, and labels computed in
  library code from the data range.
- **Stats consumers** — QQ plots (sort + Φ⁻¹), ROC and calibration curves
  (the model-metrics bullet), residual and fit plots for the regressions,
  ecdf step functions.

To settle: nice-number tick heuristics; overlaying several series in one
chart (an array of series per plot word, probably); torn frames on
overwrite (`write-file` truncates in place — either a `rename-file` word
for temp-and-rename atomicity, or accept that viewers re-read fast);
whether one fixed, readable style is enough (it should be).

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

## Sort with a rule

- `array [ x y -- cmp ] sort-with` — sorted copy under a user comparator
  quotation that pops two Vals and pushes `-1` / `0` / `1`; input untouched.
  Algorithm: introsort or libc `qsort` with a comparator thunk. With `sort-by`
  covering key extraction, this is for orderings that aren't key extractions.

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
the locals-frame and trail rewind the unwind already carries; whether
`before`/`after` observe the region's data stack or run isolated.

---

## Foreign function interface

- **Callbacks** — C → Water function pointers (`qsort` comparators,
  `CURLOPT_WRITEFUNCTION` to capture a response body into a string). The same
  re-entry plumbing `sort-with` needs.
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
