# logicforth — deferred work

Tracks work that's planned or pending. Completed items are deleted; the
git history is the place to look for what's been built.

---

## Matrix — remaining work

The matrix type is functionally complete for value-semantic numeric
work: construction, element-wise arithmetic, scalar broadcasting,
transpose, DGEMM in all four transpose variants, indexing (`@i`, `@j`,
`@i,j`), `reshape`, `flatten`, `diagonal-matrix` / `identity-matrix` /
`diagonal`, the reduction family (`sum`, `row-sums`, `column-sums`,
`mean`, `row-means`, `column-means`, plus the `max`/`min` /
`row-maxes` / `column-maxes` / `row-mins` / `column-mins` set), and
the polymorphic element-wise math primitives (`abs`, `sqrt`, `exp`,
`log`, `sin`, `cos`, `tan`, `tanh`, `negate`) that dispatch on both
floats and matrices. `val_cmp` orders matrices by shape then contents,
so they work as set members. What remains is the "beyond core" list.

### argmax / argmin

Index of the maximum / minimum element (or an `(i, j)` pair). Deferred
until there's a concrete use case; the additive and max/min reductions
already cover aggregation.

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

## Tensors and DNNs (long-horizon)

This section captures a deliberately large body of future work — tensors
as the generalization of `T_MATRIX`, and the deep-neural-network use
case that surface drives. **None of this is near-term.** It lands after
the interpreter-performance items, the language-feature items above, and
probably after the logic layer. The intent here is to record the design
decisions already taken (so future work doesn't relitigate them) and to
size the scope (so it doesn't sneak up unannounced). Implementation
specifics are for whoever picks this up.

The decisions taken:

- Tensors are **fully N-dimensional**, not just rank-3 or rank-4.
- **One unified type `T_TENSOR`** replaces `T_MATRIX`. A matrix is a
  rank-2 tensor. Single vocabulary; no conversion words.
- The name stays **tensor**.
- **Tensor-op primitives**, not layer primitives. Users compose layers
  as colon defs over a small set of coarse-grained tensor operations.
- **BLAS becomes the default backend** for tensor work (the optional
  BLAS build above gets promoted to required-for-tensors).
- **Convolution via im2col + GEMM**, reusing the BLAS-accelerated
  matmul path.
- **Training is in scope**, which means autograd.

### Tensors

The Object union arm replaces the current `(int rows, int columns,
double *elements)` matrix struct with `(int rank, int *shape, double
*elements)`. Row-major / C-order layout (numpy default). A rank-2
tensor's flat-offset arithmetic reduces to today's `i·columns + j`, so
every existing matrix kernel reads the same bytes the same way after
the storage refactor; rank-2-specific kernels (DGEMM, identity,
diagonal-matrix) keep their fast paths and error on other ranks.

API surface to design:

- Element-wise `+`/`-`/`*`/`/` and polymorphic unary math (`abs`,
  `sqrt`, `exp`, `log`, `sin`, `cos`, `tanh`) extend to any rank with
  matching shape.
- **Broadcasting** for shape-mismatched element-wise ops: `(B, N)` plus
  `(N)` produces `(B, N)`. numpy semantics.
- **Axis reductions**: `sum-axis`, `mean-axis`, `max-axis`, `min-axis`.
  Today's `row-sums` / `column-sums` become the rank-2 cases of
  `sum-axis 1` / `sum-axis 0`.
- **Shape ops**: `reshape`, `transpose` (reverses dim order at any
  rank), `permute` (explicit axis permutation), `flatten`, `shape`,
  `rank`, `numel`.
- **Indexing**: `tensor i₀ … iₙ₋₁ @` for scalar access;
  `tensor k i @-axis` for slice along axis k.
- **Slicing**: ranges along each axis, returning fresh contiguous
  tensors. Views with shared storage are a later optimization.

Open questions to settle at implementation time: NCHW vs NHWC
convention (matters for conv); how the `@i` / `@j` shortcuts coexist
with the general `@-axis`; print format for rank ≥ 3 (suggested: shape
header plus a few 2D slices).

### BLAS as the default tensor backend

The "Optional BLAS/LAPACK build" item above moves from optional to
default-on for tensor-enabled builds. For DNN scale, BLAS isn't
optional — pure-C DGEMM peaks around 5-10 GFLOPS on modern CPUs;
vendor BLAS hits 50-500 GFLOPS via SIMD/AVX/NEON. That's the
difference between useful and toy.

The matrix-only zero-dep build remains as a smaller-footprint variant.
Vendor selection (Apple Accelerate / OpenBLAS / MKL) and build wiring
(Makefile target vs `#ifdef`) are open per the existing BLAS
subsection — those questions carry over.

### Convolution via im2col + GEMM

Standard implementation. Unfold the input window into a matrix
(im2col), GEMM it against the filter matrix, reshape the output back.
Reuses the BLAS-accelerated matmul path; minimal new compute code on
top.

- **`im2col`** primitive — tensor → 2D matrix with each row a
  filter-shaped window. ~80 lines of C.
- **`col2im`** for the backward pass — the same transform inverted.
- `conv2d`, `conv1d`, transposed convolutions, strided / dilated
  variants compose from `im2col` + GEMM + `reshape` in user-level
  colon defs. No per-variant C primitive needed.

Open questions at implementation time: stride / dilation / padding
parameter packaging (single packed Val vs multiple stack args);
padding mode set (`valid` and `same` only, or also `reflect` /
`replicate`); NCHW vs NHWC.

### Autograd for training

Training requires automatic differentiation. With no JIT and no AOT-to-C,
the path is a runtime-recorded computation graph: each tensor op records
itself onto a tape, the backward pass walks the tape in reverse and
accumulates gradients into each leaf.

This is the largest single piece of work in this section by line count,
because it requires every tensor op to grow a backward implementation.

Components:

- A flag on `T_TENSOR` (or a parallel `T_GRAD_TENSOR` tag) marking
  tensors that participate in the graph. Regular tensors flow through
  unchanged.
- A **tape** data structure recording each op's operands and the
  derivative info needed for the backward pass.
- A **`backward`** primitive that walks the tape from a scalar loss,
  accumulating gradients into the leaves.
- A **`requires-grad`** / **`no-grad`** context for opting tensors in
  or out of graph participation.
- Per-op **backward implementations** for every primitive that can
  appear in a graph: matmul, conv (im2col + GEMM in both directions),
  element-wise, axis reductions, activations, broadcasting. This is
  the bulk of the code.

Open questions: tape representation (linear array vs linked list);
gradient accumulation strategy (one tensor per leaf vs sparse map);
how `to`-mutation interacts with graph membership (probably:
in-place store on a grad tensor severs the graph node); whether to
support second-order gradients (probably not in the first cut).

### Suggested build order

When this work is eventually started, the rough dependency order:

1. **Tensor storage refactor** (`T_MATRIX` → `T_TENSOR`, rank/shape
   fields, existing matrix kernels still working).
2. **Tensor-op primitives** (axis reductions, reshape, permute,
   broadcast, indexing, slicing).
3. **BLAS integration as the default**. Matmul lights up.
4. **`im2col` / `col2im`**. Convolution composable.
5. **Autograd**. Last, because it requires every prior op to have a
   backward.

Each step is a substantial body of work; together easily several
thousand lines of C plus a much larger test corpus.

### Out of scope

- **GPU acceleration** (CUDA / Metal / OpenCL). CPU only.
- **Quantization** (INT8 / FP16 / BF16). Could come later if useful;
  not in the initial design.
- **Distributed / multi-node training.** Single-process only.
- **PyTorch / ONNX model loading.** Conversion outside logicforth.
- **Kernel-level micro-tuning beyond BLAS.** Not writing a BLAS
  competitor.

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

### Dictionaries / hash maps — done, as frames

Superseded by the **frame** type (`T_FRAME`): symbol-keyed nested maps with
`{ :a 1 :b 2 }` literals, `@` / `!` / `has?` / `delete-at` / `update-at` /
`keys` / `values`, `frame` / `>frame` builders, and `/a/b/c` path literals.
Sorted parallel key/value arrays rather than a hash table — chosen for
structural compare/unify (frames are the planned unification layer's compound
term) and for small record-sized maps where a flat ordered scan beats hashing.
Complete, including `merge`, deep `copy`, and image save/load.

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

## Array head decomposition

Two C primitives supporting Prolog-style `[H|T]` decomposition over arrays:

- **`>head`** ( elem arr -- arr' ) — prepend an element. Allocates a new
  array of length len(arr)+1; copies arr after the new first slot.
- **`head>`** ( arr -- first rest ) — split off the first element. Returns
  the head Val and a new array of length len(arr)-1 starting at index 1.
  Errors on empty array.

Names follow the existing direction convention (`>r` / `r>`, `>side` /
`side>`, `>frame`, `string>symbol`): `>head` puts a head on, `head>` takes
a head off.

**Why C and not `lib.l4`.** Both could be defined over `@i` + `take` +
`skip`, but `skip` is `reverse take reverse` — three traversals per call.
A C `head>` is a single `memcpy`, materially faster for the recursive
patterns this enables. ~15 lines each.

**Use case.** Recursive walks shaped like Prolog clauses:

```forth
: sum-arr ( arr -- sum )
  dup size 0 = if drop 0
  else head> swap sum-arr + then ;
```

**Cost note worth documenting.** Arrays are contiguous, not linked. A
recursive walk over N elements via `head>` allocates a sequence of arrays
of sizes N-1, N-2, …, 1 — O(N²) Val storage churned. Fine for shallow
decomposition; pathological for large arrays. The performant alternative
for those cases is the existing `reduce` / `map` / `each` family, which
keeps iteration C-side.

**Back-end (snoc-style) deferred.** No equivalent for the tail end of an
array — the `[H|T]` pattern doesn't need it, and `last` is already in
`lib.l4` with a different shape (`arr n -- arr'`, last-N elements). If
added later, name them by behavior, not by symmetry with the head pair.

**Connection to the planned logic layer.** When unification lands,
`[ H | T ]` becomes a literal pattern term that unifies with arrays of
length ≥ 1 — `H` binds to head, `T` to a fresh tail array (or a logic
variable thereof, depending on direction). The pattern syntax is the
declarative form; `>head` / `head>` remain the imperative fast path.

---

## Functional primitives

`map`, `mapn`, and `filter` are in (`src/c/functional.c`). Adding the
rest of the standard higher-order toolkit.

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

`map`, `mapn`, `filter`, `take`, `reverse`, `concat`, and `reduce` are
in C. `skip` (rename of the planned "drop", since `drop` is taken by the
stack primitive) and `last` are in `lib.l4` atop `take` + `reverse`.

**C primitives (build a result array):**

- **`range`** — `n range` → `[ 0 1 … n-1 ]`; two-arg `start end range`
  → `[ start … end-1 ]`. Builds an n-element array → C.

**lib.l4 definitions (return a scalar/element, or compose C builders):**

- **`find`** — `arr [: pred :] find` → first matching element, or a
  sentinel (`T_NONE`). Short-circuits via `shift`.
- **`any?`** — `arr [: pred :] any?` → boolean float (-1 / 0).
- **`all?`** — `arr [: pred :] all?` → boolean float.
- **`flat-map`** — `arr [: ( elt -- arr ) :] flat-map` → map then
  concatenate. `map` then a `concat` fold; both pieces are C, so the
  result isn't stack-bounded. Monadic bind for arrays.
- **`sort-by`** — `arr [: ( elt -- key ) :] sort-by` → sorted by
  extracted key. Atop `sort-with` (see the Sort section); sorting
  reorders in place rather than building, so it stays in `lib.l4`.

**Predicated on dicts being in:**

- **`group-by`** — `arr [: ( elt -- key ) :] group-by` → hashmap from
  key to array of elements with that key.
- **`partition`** — `arr [: pred :] partition` → two arrays, matches
  and non-matches.

**Deliberately not adding** (composable in one line of user code):

- `count` — `[: pred :] filter size`.
- `min-by` / `max-by` — `reduce` with comparison.
- `sum` / `product` — `0 [: + :] reduce` etc.
- `for-each` — already covered by `each`.

**Cost:**

- C: `range` → ~25 lines.
- `lib.l4`: `find`, `any?`, `all?`, `flat-map`, `sort-by` → ~50 lines.
- `group-by` and `partition` wait on dicts.

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
