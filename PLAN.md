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

- **`val_cmp` for matrices** — currently falls through to `default:
  return 0`, so any two matrices compare as equal regardless of
  contents. Implement: compare shape first (rows, then cols), then
  element-wise via the `elements` array. Same pattern as the existing
  `T_STRING` and `T_SET`/`T_ARRAY` cases.

- **Binary image format** — replace the current text-based `save` /
  `load` entirely with a single binary file. The text form was nice
  for inspection, but it can't handle large matrices (the
  `[ e1 ... eN ] rows cols matrix` reload would exceed `DSTACK`
  almost immediately) and is roughly 2× larger than raw doubles for
  numeric content. REPL history is handled by `rlwrap` externally, so
  the "readable transcript" virtue of text save isn't load-bearing.

  **Format sketch.** One file, sectioned:

  - Magic + version header.
  - Sizes for each section: user-added bytes/cells in `dict[]`,
    `namepool`, `source_pool`, `symbol_pool`; `dsp`; object count.
  - Raw bytes of each pool (just the user-added slice — primitives
    get recreated by `main()` at startup, so we don't save them).
  - User-added `dict[]` slice, with code-field cells (CFAs) replaced
    by a 1-byte handler tag (`docol` / `dovar` / `dosym`) — the only
    handler kinds a user-area CFA can hold, since users can't define
    new C primitives.
  - Data stack: `dsp` Vals, raw bytes.
  - Object table: for each live object referenced from the dict or
    data stack, its `kind` + dimensions + raw element bytes. Matrices
    just dump their `double *elements` block verbatim.

  **Load sequence:**

  1. Verify magic/version.
  2. Append the user-area pool slices to the live state (the
     pre-existing primitives stay in place, then user state appends).
  3. Walk the user dict and fix up each handler tag back into the
     live function-pointer for `docol`/`dovar`/`dosym`.
  4. Read objects, register them in `objects[]`, return new handles.
     Rewrite Val payloads on the dict and data stack to use the new
     handles (since on-disk handles aren't valid in the running
     process). This is the one piece of relocation work needed.
  5. Restore `dsp`.

  **Why this works:**

  - Body cells already use index-threaded references (CFA indices,
    not pointers), so they round-trip as-is — no fixup needed beyond
    the handler tags.
  - Primitive CFAs are deterministic (same registration order in
    `main`), so absolute indices in the saved user dict stay valid.
  - Matrix elements survive byte-for-byte. No DSTACK pressure on
    reload. No parsing.

  **Trade-offs accepted:**

  - Not portable across architectures with different endianness.
    Same machine in / same machine out is the only supported case.
  - Not human-readable. `rlwrap` covers REPL transcript needs.
  - No graceful upgrade across logicforth versions — embed a version
    in the header and refuse to load mismatches. Matches what every
    binary image format does.

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
  No LAPACK available, so hand-rolled is the only option. (See the
  CNN-speculative section below for the broader linear-algebra arc.)

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

Six features identified as load-bearing for a complete core language.
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

### Error handling — `throw` / `catch`

Forth-style. Lets user code recover from errors instead of always
aborting back to the REPL.

- `[ ... ] catch` — runs the quotation; pushes `0` on clean completion,
  or the thrown error code on failure. Data-stack state is restored to
  the pre-`catch` height regardless.
- `code throw` — abort with the given integer code. Unwinds to the
  nearest enclosing `catch`, or to the REPL if none.
- The interpreter's existing `error_flag` becomes the throw mechanism;
  primitives that currently set the flag effectively throw a built-in
  code. User-supplied codes are integers; richer info can be encoded
  via symbols if needed later.

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

## File organization (when to split logicforth.c)

The source is around 3000 lines now — single file, growing. Resist
the urge to pre-split.

**Why not split speculatively:**

The literate-programming flow we've been protecting works *because* the
file reads top-to-bottom — Tag enum, then Val constructors, then storage,
then handlers, then primitives, then the outer interpreter. Splitting
fragments that narrative.

The coupling cost of splitting is also real. A separate `matrix.c`
would need visibility into `objects[]`, `Val`, `Object`, `Tag`,
`push`/`pop`, `error_flag`, the `make_*` constructors, and the
type-dispatch in `p_add` etc. All of that would have to become
`extern`, killing the compiler's static-inlining hints, requiring a
header file that duplicates the type definitions, and leaking
implementation details across what's supposed to be a boundary.

**Where the seam actually is:**

The matrix *primitives* (`p_add`'s matrix arm, `p_dim`, `p_matrix`,
`p_sum`, etc.) are tightly tied to the interpreter — pop, type-check,
error, push. They belong with the other `p_*` functions. The thing
that's actually self-contained is the *pure numeric kernels*: the
optimized `dgemm_kernel`, the row-major transpose, eventual
`svd_jacobi` — none of which touch Val, the stack, or `error_flag`.

When more pure kernels exist (SVD, element-wise math loops, etc.),
factor them out into a **`linalg.c`** that takes `double *` arrays.
The `p_*` wrappers stay in `logicforth.c`. This split:

- Keeps the interpreter's narrative intact.
- Cleanly separates "language" from "math."
- Doesn't require exposing language internals.

Until the kernel layer is substantively larger, splitting is premature
optimization. File size by itself isn't a forcing function.

---

## HIGHLY SPECULATIVE — convolutional nets on greyscale images

Notes on what it would take to train a CNN in logicforth, if we ever
went there. Recording the gap honestly so the matrix-type plan
doesn't get read as "we're 90% of the way there."

The linalg ops have come together — DGEMM, transpose, diag,
reductions are all in. They're maybe 40% of what a CNN needs. The
remaining 60%:

**More math primitives.** Already listed up under the matrix work —
`exp`, `log`, `sqrt`, `abs`. CNN-relevant uses: `exp` (softmax,
sigmoid), `log` (cross-entropy), `sqrt` (RMS-style optimizers,
normalization). All cheap to implement.

**Max/min reductions.** Already listed up under the matrix work.
Softmax needs `max`. Pooling needs windowed `max` and `mean`.

**The nasty bits: im2col / col2im.** Convolution itself isn't basic
linalg, but it can be reduced to DGEMM via *im2col*: unfold each
image patch into a column, stack the columns, multiply by the
flattened kernel. The forward pass is then one DGEMM. The backward
pass needs the inverse (*col2im*) — scatter-add the unfolded gradient
back onto the image's pixel positions, accumulating where patches
overlap. Tedious in practice — striding, padding, channel handling,
edge cases — and they're the main thing that makes "implement a
CNN" more than a weekend of work.

**Autograd or hand-derived gradients.** The real obstacle. CNN
training needs the gradient of every forward op composed via the
chain rule. Two paths:

- **Hand-derived backward per layer.** Tractable for a small fixed
  set of layer types (conv, pool, fc, relu, softmax). No autograd
  infrastructure, just a paired forward+backward word for each
  layer. Cheap to write, painful to extend.
- **Autograd tape.** Build a graph during forward, traverse it
  backward. This is a real subsystem — node types, gradient
  accumulation, possibly some form of garbage collection on the
  tape. Worth the effort only if many model variants get explored.

For a first CNN, hand-derived gradients per layer is the right move.

**Optimizer.** Tiny once gradients exist. SGD is one line per
parameter. Adam/RMSprop add a few moving averages.

**Approximate ordering if we ever pursue this:**

1. Element-wise math primitives (`exp`, `log`, `sqrt`, `abs`).
2. Max/min reductions.
3. `im2col` and `col2im` as pure-numeric kernels in `linalg.c`.
4. Layer words: `conv-forward`, `conv-backward`, `relu`, `relu-grad`,
   `maxpool-forward`, `maxpool-backward`, `softmax`, `cross-entropy`,
   `fc-forward`, `fc-backward`.
5. An SGD update word.
6. A training loop in user-level logicforth.

**Why greyscale only.** A greyscale image is 2D (H × W) and fits the
matrix type directly. A color image is 3D (H × W × C), and a batch
of color images is 4D (N × H × W × C). Those don't fit a 2D type —
they'd require either:

- Faking it by flattening (H × (W·C) or similar), which makes every
  per-channel operation an awkward indexing exercise; or
- Adding a real N-dimensional tensor type with shape + strides.

The latter is the right answer if CNNs become a serious goal, but
it's a different and bigger project than the 2D matrix. The PyTorch
/ NumPy / TensorFlow story is "tensors are the primary type,
matrices are just rank-2 tensors" — and they pay for that with
substantial machinery (broadcasting, strided views, advanced
indexing, einsum-style operations).

Recommended order: get a greyscale MNIST classifier working
end-to-end on the existing matrix type, *then* decide whether
color/tensor work is worth the investment.

---

## `export` — text dump of user-defined vocabulary

A complement to the binary image, not a replacement. Writes the user
portion of the dictionary out as a `.l4` source file that can be
re-loaded with the existing `load`. Round-trips definitions, not
runtime state.

**Use case:** experiment in the REPL, then promote the result into a
file under version control. Without an exporter, the user has to
reconstruct the file by hand from `see` calls — friction enough to
discourage exploration.

**API:**

```
"mywork.l4" export
```

**What gets emitted, in dictionary order:**

- Colon def: `: name <body source> ;` — body comes straight from
  `source_pool` via the header's `SRCIDX` field. Already there for
  `see`, just re-used.
- Variable: `variable name`. Current value is *not* exported — for
  scalars we could emit `42 name !`, but for matrices / arrays / sets
  the value can't round-trip through text in any sane way, so leave
  every variable un-initialized to keep the rule simple.
- Symbol: `symbol name`.

**What's deliberately excluded:**

- Variable values, the data stack, any runtime state. Those are what
  the binary image is for.
- `forget`-ten definitions. The exporter walks the live dict chain.

**Cost:** ~30 lines. No interpreter changes beyond the new primitive
— body source is already captured for `see`.

---

## `reload` — re-run loaded files

Track every `.l4` file loaded during a session and add a `reload` word
that re-loads them all in order. Useful during iterative development:
edit the file in another window, type `reload` in the REPL, see the
new definitions.

**Mechanism:**

- A small fixed-size array `loaded_files[]` of filename indices into
  `namepool` (or a dedicated string pool). Order = first-load order.
- `load` appends to `loaded_files[]` if the filename isn't already
  present; otherwise leaves the list unchanged (so `reload` order
  stays stable across re-loads of the same file).
- `lib.l4` is auto-loaded at startup the same as today and gets
  recorded first, so `reload` re-runs it before anything else.
- `reload`: iterate `loaded_files[]` in order, calling the existing
  load path on each.

**Open questions:**

- Should `reload` clear the dictionary back to its post-startup state
  first, then replay? Otherwise re-running a file just appends
  duplicate definitions and overrides via the most-recent-wins lookup.
  Cleanest is: `reload` calls something like `forget-user` to truncate
  back to the boot snapshot, then replays. Needs `initial_here` /
  `initial_names_here` / `initial_sources_here` / `initial_symbol_pool_here`
  snapshots taken at the end of primitive registration (same snapshots
  the binary image format would want).
- What about files loaded from inside other files? Recorded too, in
  their actual load order? Probably yes — record every distinct
  filename `load` sees, regardless of who called it.
- Path handling: store filenames as the user gave them (relative
  paths resolve against CWD at reload time, same as the original
  load). Don't try to canonicalize.

**Cost:** ~30 lines plus the boot-snapshot scaffold.

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
