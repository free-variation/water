# logicforth — deferred work

Items that have been discussed and shelved for later, not implementation tasks
in flight. Each entry: what to build, why, rough scope.

## Numeric matrix type (2D)

A first-class fixed-size 2D matrix of `double`s, stored row-major in one
contiguous block. Separate from nested arrays, which stay as the way to
express type-free / ragged / heterogeneous compound data.

**Why:** Nested arrays work for structural use but are wrong for numeric
workloads — too many heap objects, two indirections per cell, no contiguous
storage for vectorization. A `double *cells` matrix is half the memory per
element, cache-friendly, and pairs cleanly with DGEMM-style algorithms
(which are strictly 2D anyway).

**Sketch:**
- New tag `T_MATRIX`, new `ObjectKind` `OBJECT_MATRIX`.
- `Object` gains a third arm in the anonymous union (or a small struct of
  `int rows; int cols; double *cells;`).
- Constructor: `rows cols matrix` for a zero-filled matrix. Possibly also
  a from-array builder.
- Index/update: `m i j m@`, `value m i j m!`.
- 2D-aware printer.
- GC mark function (touch the Object only; no Vals inside).
- Core operations (required, not optional). All dispatched on operand
  types via the same mechanism `+`/`-`/`*` already use to switch
  between float, string, and set behavior — no new word names:
  - **Element-wise arithmetic**: `+`, `-`, `*`, `/` extended so that
    matrix-matrix operands produce an element-wise matrix result. Shape
    constraint: same-shape operands; mismatch is a type error.
  - **Transpose**: a new word, e.g. `transpose`. Out-of-place copy
    returning a new matrix. (Lazy/view-based transpose is a possible
    future optimization but not needed for first cut.)
  - **`*` for matrices means DGEMM**, not element-wise: the standard
    convention in `numpy`-style libraries is to keep `*` element-wise
    and use a separate `@` (or `matmul`) for general multiply. The
    cleanest call here is probably to reserve `*` for element-wise (so
    the scalar/matrix overload story stays simple) and give DGEMM its
    own word — `dot` or `@` or `matmul`. Decide at implementation
    time; the dispatch mechanism is the same either way.
  - **Diag**: a single word `diag` that dispatches on operand shape.
    On a 1×N or N×1 matrix (vector), builds a square matrix with the
    operand on the main diagonal and zeros elsewhere. On an N×N matrix,
    extracts the main diagonal as a vector. On other shapes, type error.
  - **SVD**: singular value decomposition. ( M -- U S Vt ) — an m×n
    matrix decomposes into U (m×m orthogonal), S (vector of length
    min(m,n) with singular values in descending order), and Vt (n×n
    orthogonal), satisfying M = U · diag(S) · Vt. Convention: return
    S as a 1D vector, not a matrix; the caller can `diag` it if they
    want a full Σ.

    **No LAPACK available.** This has to be hand-rolled. Realistic
    options, roughly ordered by code volume vs. quality:

    - **One-sided Jacobi** on `M`: rotate column pairs until off-
      diagonals shrink below a tolerance. Code is short (~50–100 lines),
      O(m·n²) per sweep with several sweeps to converge, numerically
      robust. Best starting point — clear, correct, slow on large
      matrices but plenty fast for the sizes a Forth toy will actually
      see.
    - **Two-sided Jacobi**: similar idea, rotates both sides. Marginally
      more code, same complexity. No real reason to prefer it for our
      use case.
    - **Golub–Reinsch** (Householder bidiagonalization → implicit-QR on
      bidiagonal). The textbook fast SVD. Standard but lengthy —
      probably 300–500 lines done well, with subtle numerical edge cases
      (shift selection, deflation). Worth it only when one-sided Jacobi
      becomes a bottleneck on real workloads.

    Start with one-sided Jacobi. Upgrade later if needed.

No automatic conversion between matrices and nested arrays — if you want
to do math on a nested array, you write a word to copy it.

For higher-rank tensors (3D+), the standard pattern is "reshape/permute,
call DGEMM, reshape back." That belongs in a separate future tool, not in
the matrix type itself.

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

**Open questions for later:**
- What about Vals that don't have a clean TSV representation — sets, arrays,
  xt's? Probably emit a sentinel or error; don't silently lossy-encode.
- Should there be a streaming reader for large files, or always load whole?
  Whole-file is fine for the sizes we're likely to hit.

## File organization (when to split logicforth.c)

The source is currently one file (~2200 lines). Adding the matrix type
will push it toward 3000+. Resist the urge to pre-split.

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

The matrix *primitives* (`p_matrix_add`, `p_transpose`, `p_diag`, etc.)
are tightly tied to the interpreter — pop, type-check, error, push.
They belong with the other `p_*` functions. The thing that's actually
self-contained is the *pure numeric kernels*: a `dgemm()` over plain
`double *` arrays, a `transpose()` that takes (`double *src, double
*dst, int rows, int cols`), an `svd_jacobi()` — none of which touch
Val, the stack, or `error_flag`.

When matrix code exists and its shape is clear, factor out a
**`linalg.c`** containing those pure kernels. The `p_*` wrappers stay
in `logicforth.c`. This split:

- Keeps the interpreter's narrative intact.
- Cleanly separates "language" from "math."
- Makes the eventual BLAS/LAPACK substitution local to one file.
- Doesn't require exposing language internals.

Until the matrix code is written and its shape is visible, splitting
is premature optimization based on speculation. File size by itself
isn't a forcing function.

## HIGHLY SPECULATIVE — convolutional nets on greyscale images

Notes on what it would take to train a CNN in logicforth, if we ever
went there. Recording the gap honestly so the matrix-type plan above
doesn't get read as "we're 90% of the way there."

The linalg ops above (arithmetic, transpose, DGEMM, diag, SVD) are
maybe 30% of what's needed. The other 70% breaks into four piles:

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
3. `im2col` and `col2im` as pure-numeric kernels in linalg.c.
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

## Help system

A `help` word that shows a one-line description of any word — colon
definition, variable, symbol, or primitive.

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
