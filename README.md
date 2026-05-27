# logicforth

A Forth-flavored language for matrix work, set/array manipulation, and
(eventually) logic programming. A dependency-free C interpreter built with
`clang -O3`.

## Building and running

```
make           # builds ./logicforth
make test      # runs the golden-output test suite
./logicforth   # REPL
```

No external dependencies.

## A taste

```forth
\ Arithmetic
3 4 + .                                 \ 7

\ Matrices and matrix algebra
[ 1 2 3 4 ] 2 2 matrix dup transpose *  \ matrix multiplication

\ Sets and set algebra
{ 1 2 3 } { 2 3 4 } + .                 \ { 1 2 3 4 }  (union via polymorphic +)

\ Higher-order operations
[ 1 2 3 4 5 ] [: dup * :] map .         \ [ 1 4 9 16 25 ]

\ Exceptions
[: "missing" throw :]
[: "got " . . cr :] try-catch           \ prints "got missing"

\ Coroutines via delimited continuations
: yield shift ;
: producer 1 yield 2 yield 3 ;
reset producer                          \ leaves (1, k) — next value via resume
```

## What's currently implemented

### Core language

- **Tagged Vals** — floats, strings, symbols, sets, arrays, matrices, execution tokens, dictionary addresses, continuations, internal marks. Single 16-byte representation; tag determines interpretation.
- **Index-threaded inner interpreter** — dispatches one CFA per iteration, no separate bytecode.
- **Per-interpreter state** — all mutable state lives in an `Interpreter`, which owns its `Vocabulary` (a growable dictionary plus name/source/symbol pools). Multiple independent instances can coexist in one process; the engine is embeddable.
- **Three stacks** — data, return, and a side stack for stashing values that mustn't sit on the other two.
- **Colon definitions** — `: name body ;`. The body is captured as source text for `see` and the text-form `save`.
- **Anonymous quotations** — `[: ... :]` pushes a fresh xt. Works at top level and inside colon defs.
- **Control flow** — `if`/`else`/`then`, `begin`/`until`/`again`, `>r`/`r>`/`r@` for return-stack access.
- **Tick and execute** — `' word execute` for first-class invocation by name.
- **`forget`** — truncate the dictionary back to a named word; symbol identities survive.
- **Variables and symbols** — `variable foo` holds one Val. `symbol bar` defines a symbol; `:foo` is a symbol literal interned on use; `string>symbol` interns a computed string.
- **Mark-and-sweep GC** — walks data/return/side stacks, dictionary, and a small `gc_roots` array for in-flight C-level temporaries.

### Numeric / matrix

- **Polymorphic arithmetic** — `+`/`-`/`*`/`/` dispatch on operand tags. Floats arithmetic, strings concatenate, sets union/difference/intersect, matrices element-wise.
- **Matrix construction** — `2 3 0-matrix`, `[ ... ] R C matrix`, `N diagonal-matrix`, `N identity-matrix`.
- **DGEMM** — `dgemm-nn`/`tn`/`nt`/`tt` for all four transpose variants; ikj-ordered fast path with restrict pointers (~5× over naive).
- **Indexing** — `@i`/`@j`/`@i,j` to read rows, columns, or single cells.
- **Shape** — `dim`, `reshape`, `flatten`, `transpose`, `diagonal`.
- **Reductions** — `sum`, `row-sums`, `column-sums`, `max`, `min`, `row-maxes`, `row-mins`, `column-maxes`, `column-mins`. Library `mean`, `row-means`, `column-means` on top.
- **Element-wise math** — `abs`, `sqrt`, `exp`, `log`, `sin`, `cos`, `tan`, `tanh`. Polymorphic over floats and matrices.
- **Total ordering** — `=`/`<`/`>` compare matrices by shape then row-major contents, so matrices work as set members.

### Sets, arrays, higher-order

- **Set literals** — `{ 1 2 3 }`, set operations, `member?`, `cardinality`.
- **Array literals** — `[ 1 2 3 ]`, the `array` constructor (gather N from the stack), `array-of` (fill), indexed access via `@i`.
- **Map and zip-map** — `map` for a single source, `mapn` for N-ary zip, with anonymous quotations as the higher-order argument.

### Strings

- **String literals** with newlines allowed inside.
- **Interpolation** — `"hello {0}"` substitutes from the data stack.
- **Polymorphic concatenation** via `+`.

### I/O and persistence

- **Stdin REPL** with rlwrap-friendly behavior and a stack-state prompt.
- **`load`** runs a source file as if typed.
- **`save`** writes the user's vocabulary as a re-loadable `.l4` source file.
- **`save-image`** / **`load-image`** — binary image with full state preservation (dictionary, objects, stacks, continuations).
- **`reload`** truncates user state and re-runs every file `load`ed this session, in order.

### Delimited continuations

A four-primitive substrate the rest of the control story is built on. See `docs/continuations.md` for the full treatment.

- **`reset`** — installs a delimiter (a uniquely-tagged mark on the return stack).
- **`shift`** — captures the slice up to the nearest reset, removes the mark and captured frames, pushes the continuation as a `T_CONT` Val. Used for coroutines and generators.
- **`shift-with`** — same capture, but runs a handler xt in the outer context after the unwind. Used for exceptions and restarts.
- **`resume`** — re-enters a captured continuation. Multi-shot.

### Side stack

A third stack for stashing arbitrary Vals without disturbing the data or return stack: **`>side`**, **`side>`**, **`side-drop`**, **`side-depth`**.

### Exceptions (library)

Built in `lib.l4` on top of the continuation primitives:

- **`throw`** — non-local exit with a value.
- **`catch`** — wraps an xt; returns `(result 0)` on success, `(exc 1)` on throw.
- **`try-catch`** — wraps an xt with a recovery handler. Arity-agnostic.

The `shift-with` handler can also resume the captured continuation, giving the Common Lisp restart pattern — exceptions can recover rather than just abort.

### Other

- **`depth`**, **`roll`** — stack-manipulation primitives.
- **`see`** — prints a word's definition.
- **`words`**, **`forget`**, **`bye`**, **`gc`**, **`clear`**, **`.s`**, **`.a`** — interpreter utilities.

## What's planned

Tracked in `PLAN.md`, with design notes for each.

### Data types

- **Dictionaries / hash maps** — first-class key→value mapping with literal syntax `{ "a": 1, "b": 2 }`. Open-addressing hash table.
- **Time / dates** — Unix timestamps as floats: `now`, `time-format`, `time-parse`.
- **Random numbers** — xoshiro256++ PRNG: `random-float`, `random-int`, `seed!`, `shuffle`.

### Strings

- **POSIX regex + UTF-8** — `match` as the primitive, with wrappers (`split`, `replace`, `index-of`, `starts-with`, `ends-with`, `trim`, `lines`, `substring`). Codepoint-indexed at the user level.

### External I/O

- **TSV file I/O** — the sole tabular format; other formats convert outside logicforth.
- **SQLite integration** — embedded relational storage via the vendored amalgamation. Queries return arrays of rows, or matrices when all columns are numeric.

### Language ergonomics

- **Word-local variables** — `{| name |}` at the head of a colon definition; `TO` for mutation.
- **Sort** — `sort`, `sort-with`, `sort-by`.
- **stdin / env** — `read-line`, `read-all`, environment variable access.
- **Functional primitives** — `filter`, `reduce` in C; `find`, `any?`, `all?`, `range`, `take`, `drop`, `reverse`, `concat`, `flat-map`, `distinct` in `lib.l4`.
- **Help system** — `help word` showing a one-line doc string.

### Logic layer

- **Unification + nondeterminism** — `T_LOGIC_VAR`, trail-based binding, a `unify` primitive, with `amb`, `fail`, `once`, `fresh`, `run` as library words over the continuation machinery. microKanren-flavored.

### Concurrency

Built in stages on the per-interpreter foundation already in place:

- **Cooperative green threads** — `spawn`, `yield`, `run-scheduler` on top of continuations. Cheap interleaving for I/O-bound work, no parallelism.
- **OS-thread parallelism + mailboxes** — Erlang-style isolated interpreters communicating via per-thread mailboxes. Real multi-core parallelism, no shared mutable state.

### Matrix follow-ups

- **Slicing**, **`hstack`**/`vstack`, **norms**, **element-wise comparison**.
- **SVD** — one-sided Jacobi.

## Project layout

```
src/c/logicforth.h     — types, Interpreter/Vocabulary structs, prototypes
src/c/core.c           — engine: interpreter, dictionary, GC, printing, image, REPL
src/c/words.c          — arithmetic, stack, I/O, control flow, defining words, continuations
src/c/collections.c    — sets, arrays, higher-order operations
src/c/matrix.c         — matrix words and numeric kernels
src/forth/lib.l4       — standard library (auto-loaded at startup)
tests/                 — golden-output test files
docs/                  — design documents
examples/              — sample programs
PLAN.md                — deferred work and design notes
```

## License

See `LICENSE`.
