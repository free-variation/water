# logicforth — deferred work

Tracks work that's planned, in progress, or recently shipped. Each entry
says what's done, what's left, and the design decisions made along the way.

---

## Numeric matrix type (2D)

A first-class fixed-size 2D matrix of `double`s, stored row-major in one
contiguous block. Separate from nested arrays, which stay as the way to
express type-free / ragged / heterogeneous compound data.

### Done

- `T_MATRIX` tag in the Tag enum (slotted between `T_ARRAY` and `T_XT`).
- `OBJECT_MATRIX` in `ObjectKind`.
- `Object` union grew a third arm: `struct { int rows; int columns; double *elements; }`.
- `object_new_matrix(rows, cols)` allocates a zero-filled matrix.
- `make_matrix(handle)` Val constructor.
- GC `mark_val` treats `T_MATRIX` as a heap-rooted value; sweep frees the
  `elements` block.
- 2D-aware printer: `<matrix RxC: first10 ... last3>` using the same
  truncation rule as arrays (via a new `print_corners` helper).
- Element-wise arithmetic via polymorphic dispatch in `p_add`, `p_sub`,
  `p_mul`, `p_div`. Shape mismatch is a type error. Helper:
  `matrix_scalar_op` walks both inputs and applies a `scalar_operator`
  function pointer (`scalar_add`, `scalar_subtract`, `scalar_multiply`,
  `scalar_divide`).
- Constructor primitives:
  - `rows cols 0-matrix` → zero-filled.
  - `array rows cols matrix` → matrix initialized from a flat array of
    floats (length must equal rows*cols).
  - `m dim` → pushes rows then columns (two floats).

### Decision points settled

- `*` is element-wise multiplication for matrices, **not** DGEMM. The
  numpy-style split holds: `*` is element-wise, a separate word will be
  used for DGEMM (`dot` / `@` / `matmul` — name still TBD when DGEMM is
  written).

### Open / not yet done

- **Transpose** — a new `transpose` word, out-of-place copy.
- **Indexing** — `m i j m@`, `value m i j m!`. Currently no element
  read/write.
- **DGEMM** — general matrix-matrix multiply, hand-rolled triple loop.
  Will be wired as a new word (not `*`).
- **`diag`** — bidirectional, dispatches on shape: vector → square
  diagonal matrix; matrix → diagonal vector.
- **SVD** — start with **one-sided Jacobi** (~50–100 lines). Upgrade to
  Golub–Reinsch later if one-sided Jacobi proves a bottleneck. No LAPACK
  available, so hand-rolling is the only option.
- **`val_cmp` for matrices** — currently falls through to `default:
  return 0`, so any two matrices compare as equal regardless of contents.
  Either implement proper element-wise comparison or make matrices a
  non-orderable type (rejected in comparison primitives).
- **`save` for matrices** — `write_val_literal` has no `T_MATRIX` case,
  so saving a stack that contains a matrix silently emits the unsupported
  fallback. Decide: serialize as `[ ... ] rows cols matrix` reconstruction,
  or error.
- **Tests** — no matrix tests yet. The arithmetic, constructor, and
  `dim` paths are all untested in the suite.

### Style nits noted in code

- The helper `matrix_scalar_op` is misnamed — it does element-wise
  matrix-matrix ops, not matrix-scalar. Rename when more matrix code
  lands.
- The matrix section uses tab indentation while the rest of the file uses
  4-space indentation. Normalize when convenient.

---

## Standard library mechanism (`src/forth/lib.l4`)

A user-facing logicforth source file is loaded at startup, after all C
primitives are registered. Lives at `src/forth/lib.l4`. The bootstrap
code in `main` pushes the path and calls `p_load` directly before
entering the REPL; any error halts startup.

**Current contents:** `2dup` (with stack-effect comment), plus a stub
sketch of `scalar+` for matrix scalar addition.

**Implications:**

- Anything defined in `lib.l4` is part of every session, no user action
  required.
- Library words appear in `words` and are dumped by `save` like any
  other user definition (see the regression below).

### Known regression: save+forget+load and lib.l4 interaction

`tests/13_save_load.l4` currently fails. The cause: `save` dumps **all**
user-defined words, including those from `lib.l4` (e.g. `2dup`). When the
test does `forget double` followed by a reload, the saved file
re-creates `2dup` first, shifting every subsequent CFA. The pre-forget
`red` symbol value on the data stack still carries its original CFA,
which now points at a *different* word in the rebuilt dictionary —
hence the printed output shows `+` where `red` was expected.

**Options to fix:**

1. **Mark library defs and skip them in `save`** — add a header flag
   bit so library words don't appear in dumps. Cleanest but adds a
   header field semantic.
2. **Re-resolve symbol/xt Vals through name on save**, then re-resolve
   by name on load. Requires symbols/xts to remember their name, not
   just their CFA — currently they remember CFA only.
3. **Document the limitation** and update the test expectation.

Option 2 is the right long-term answer because CFA values were never
portable across save/load anyway; only word *names* are. Option 1 is a
cheap patch.

---

## New primitives added (not previously in this plan)

- **`gc`** — manually trigger a garbage collection sweep. Useful for
  testing, profiling, and forcing reclamation before snapshotting.
- **`clear`** — empty the data stack in one call. Equivalent to repeated
  `drop` but constant-time.
- **`array`** — `init-value length array` builds an array of `length`
  filled with `init-value`. Complements the existing literal-collection
  `[ ... ]` syntax.

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

- What about Vals that don't have a clean TSV representation — sets, arrays,
  xt's, matrices? Probably emit a sentinel or error; don't silently lossy-encode.
- Should there be a streaming reader for large files, or always load whole?
  Whole-file is fine for the sizes we're likely to hit.

---

## File organization (when to split logicforth.c)

The source is currently 2697 lines — single file, growing. Resist the
urge to pre-split.

**Why not split speculatively:**

The literate-programming flow we've been protecting works *because* the
file reads top-to-bottom — Tag enum, then Val constructors, then storage,
then handlers, then primitives, then the outer interpreter. Splitting
fragments that narrative.

The coupling cost of splitting is also real. A `matrix.c` would need
visibility into `objects[]`, `Val`, `Object`, `Tag`, `push`/`pop`,
`error_flag`, the `make_*` constructors, and the type-dispatch in
`p_add` etc. All of that would have to become `extern`, killing the
compiler's static-inlining hints, requiring a header file that
duplicates the type definitions, and leaking implementation details
across what's supposed to be a boundary.

**Where the seam actually is:**

The matrix *primitives* (`p_add`'s matrix arm, `p_dim`, `p_matrix`, etc.)
are tightly tied to the interpreter — pop, type-check, error, push.
They belong with the other `p_*` functions. The thing that's actually
self-contained is the *pure numeric kernels*: a `dgemm()` over plain
`double *` arrays, a `transpose()` that takes (`double *src, double
*dst, int rows, int cols`), an `svd_jacobi()` — none of which touch
Val, the stack, or `error_flag`. The current `scalar_add`/`scalar_multiply`
helpers are a microscopic seed of this kernel layer.

When DGEMM / transpose / SVD exist and their shape is clear, factor
out a **`linalg.c`** containing those pure kernels. The `p_*` wrappers
stay in `logicforth.c`. This split:

- Keeps the interpreter's narrative intact.
- Cleanly separates "language" from "math."
- Doesn't require exposing language internals.

Until the bigger linear-algebra primitives exist and their shape is
visible, splitting is premature optimization. File size by itself
isn't a forcing function.

---

## HIGHLY SPECULATIVE — convolutional nets on greyscale images

Notes on what it would take to train a CNN in logicforth, if we ever
went there. Recording the gap honestly so the matrix-type plan above
doesn't get read as "we're 90% of the way there."

The linalg ops planned above (transpose, DGEMM, diag, SVD) are maybe
30% of what's needed. The other 70% breaks into four piles:

**More math primitives.** Element-wise nonlinear functions, applied
to whole matrices: `exp` (softmax, sigmoid), `log` (cross-entropy),
`sqrt` (RMS-style optimizers, normalization), and scalar variants of
`max`/`min` (ReLU). All cheap to implement once the matrix type
exists — a few lines each, dispatched the same way as `+`/`*`.

**Reductions.** Sum, max, mean along an axis (or across the whole
matrix). Softmax needs `max` and `sum`. Cross-entropy needs `sum`.
Batch norm needs `mean` and a variance via `sum`-of-squares. Pooling
needs windowed `max` and `mean`. Reductions are easy individually
but you need quite a few of them.

**The nasty bits: im2col / col2im.** Convolution itself isn't basic
linalg, but it can be reduced to DGEMM via *im2col*: unfold each
image patch into a column, stack the columns, multiply by the
flattened kernel. The forward pass is then one DGEMM. The backward
pass needs the inverse (*col2im*) — scatter-add the unfolded gradient
back onto the image's pixel positions, accumulating where patches
overlap. These two routines are short in description and tedious in
practice — striding, padding, channel handling, edge cases — and
they're the main thing that makes "implement a CNN" more than a
weekend of work.

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

1. Math primitives (`exp`, `log`, `sqrt`, etc.) on matrices.
2. Reductions (`sum`, `max`, `mean` with optional axis arg).
3. `im2col` and `col2im` as pure-numeric kernels in `linalg.c`.
4. Layer words: `conv-forward`, `conv-backward`, `relu`, `relu-grad`,
   `maxpool-forward`, `maxpool-backward`, `softmax`, `cross-entropy`,
   `fc-forward`, `fc-backward`.
5. An SGD update word.
6. A training loop in user-level logicforth.

**Why greyscale only.** A greyscale image is 2D (H × W) and fits the
matrix type directly. A color image is 3D (H × W × C), and a batch of
color images is 4D (N × H × W × C). Those don't fit a 2D type —
they'd require either:

- Faking it by flattening (H × (W·C) or similar), which makes
  every per-channel operation an awkward indexing exercise; or
- Adding a real N-dimensional tensor type with shape + strides.

The latter is the right answer if CNNs become a serious goal, but
it's a different and bigger project than the 2D matrix above. The
PyTorch / NumPy / TensorFlow story is "tensors are the primary
type, matrices are just rank-2 tensors" — and they pay for that with
substantial machinery (broadcasting, strided views, advanced
indexing, einsum-style operations).

So: greyscale is plausible on the matrix type. Color genuinely
requires the tensor type, and the tensor type is its own significant
piece of work (see the SVD/dgemm discussion above — DGEMM is 2D, so
all rank-3+ ops would reduce to "permute, reshape, DGEMM, reshape
back," which works but isn't free).

Recommended order: build the matrix type, get a greyscale MNIST
classifier working end-to-end, *then* decide whether color/tensor
work is worth the investment.

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
  string parameter. All ~60 primitives get short stack-effect-style
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

- One signature change to `define_primitive` (touches ~60 call sites
  but mechanically — add a doc string each).
- ~600 bytes of static doc strings.
- About a dozen lines for `p_help`.

**Open questions:**

- Should `help` with no argument print a list of all words with a
  short doc each, sorted? Could be a separate `apropos` word later.
- Variable/symbol docs are uncovered. If they need them, we'd add
  `( comment )` parsing in `p_variable` / `p_symbol` to capture a
  doc string from the input stream after the name.
