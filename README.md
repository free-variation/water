# logicforth

A Forth-flavored language for doing matrix work, set/array manipulation, and (eventually) logic programming, implemented as a single-file C interpreter.

The starting point was a small, readable Forth in the spirit of jonesforth — index-threaded inner interpreter, a flat dictionary, stack-based computation, colon definitions with anonymous quotations. From there the language has grown a numeric matrix type, polymorphic arithmetic, first-class sets and arrays, delimited continuations, and a working exception system built on top of them.

The aesthetic constraints are deliberate. One file of hand-written C (~4000 lines), no dependencies, no build system beyond `clang -O3`. The language reaches for matrix-lab and symbolic-programming territory while staying recognizable as Forth: the data stack is the working memory, words are short and compose by juxtaposition, the file reads top-to-bottom from primitives to the REPL loop.

The name hints at the eventual destination: unification, logic variables, and microKanren-flavored search on top of the delimited continuations that are already in place. Whether the language gets there depends on whether the surface stays small enough to be worth using. So far it does.

## Building and running

```
make           # builds ./logicforth
make test      # runs the test suite (48 tests as of this writing)
./logicforth   # REPL
```

No external dependencies. The interpreter and standard library together are around 4400 lines.

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
- **Three stacks** — data, return, and a side stack for stashing values that mustn't sit on the other two.
- **Colon definitions** — `: name body ;`. The body is captured as source text for `see` and the text-form `save`.
- **Anonymous quotations** — `[: ... :]` pushes a fresh xt. Works at top level and inside colon defs.
- **Control flow** — `if`/`else`/`then`, `begin`/`until`/`again`, `>r`/`r>`/`r@` for return-stack access.
- **Tick and execute** — `' word execute` for first-class invocation by name.
- **`forget`** — truncate the dictionary back to a named word; symbol identities survive.
- **Variables and symbols** — `variable foo`, `symbol bar`. Variables hold one Val; symbols are interned.
- **Mark-and-sweep GC** — walks data/return/side stacks, dictionary, and a small `gc_roots` array for in-flight C-level temporaries.

### Numeric / matrix

- **Polymorphic arithmetic** — `+`/`-`/`*`/`/` dispatch on operand tags. Floats arithmetic, strings concatenate, sets union/difference/intersect, matrices element-wise.
- **Matrix construction** — `2 3 0-matrix`, `[ ... ] R C matrix`, `N diagonal-matrix`, `N identity-matrix`.
- **DGEMM** — `dgemm-nn`/`tn`/`nt`/`tt` for all four transpose variants; ikj-ordered fast path with restrict pointers (~5× over naive).
- **Indexing** — `@i`/`@j`/`@i,j` to read rows, columns, or single cells.
- **Shape** — `dim`, `reshape`, `flatten`, `transpose`, `diagonal`.
- **Reductions** — `sum`, `row-sums`, `column-sums`, `max`, `min`, `row-maxes`, `row-mins`, `column-maxes`, `column-mins`. Library `mean`, `row-means`, `column-means` on top.
- **Element-wise math** — `abs`, `sqrt`, `exp`, `log`, `sin`, `cos`, `tan`, `tanh`. Polymorphic over floats and matrices.

### Sets, arrays, higher-order

- **Set literals** — `{ 1 2 3 }`, set operations, `member?`, `cardinality`.
- **Array literals** — `[ 1 2 3 ]`, indexed access via `@i`.
- **Map and zip-map** — `map` for a single source, `mapn` for N-ary zip.
- **Anonymous quotations as the higher-order argument**.

### Strings

- **String literals** with newlines allowed inside.
- **Interpolation** — `"hello {0}"` substitutes from the data stack.
- **Polymorphic concatenation** via `+`.

### I/O and persistence

- **Stdin REPL** with rlwrap-friendly behavior.
- **`load`** runs a source file as if typed.
- **`save`** writes the user's vocabulary as a re-loadable `.l4` source file.
- **`save-image`** / **`load-image`** binary image with full state preservation (dictionary, objects, stacks, continuations).
- **`reload`** truncates user state and re-runs every file that was `load`ed this session, in order. Useful during iterative development.

### Delimited continuations

A four-primitive substrate that the rest of the control story is built on. See `docs/continuations.md` for the full treatment.

- **`reset`** — installs a delimiter (a uniquely-tagged mark on the return stack).
- **`shift`** — captures the slice up to the nearest reset, removes the mark and the captured frames, pushes the continuation as a `T_CONT` Val. Used for coroutines and generators.
- **`shift-with`** — same capture, but runs a handler xt in the outer context after the unwind. Used for exceptions and restarts.
- **`resume`** — re-enters a captured continuation. Multi-shot (the same `k` can be resumed many times).

### Side stack

A third stack for stashing arbitrary Vals without disturbing the data stack or the return stack.

- **`>side`**, **`side>`**, **`side-drop`**, **`side-depth`**.

### Exceptions (library)

Three lines in `lib.l4` on top of the continuation primitives:

- **`throw`** — non-local exit with a value.
- **`catch`** — wraps an xt; returns `(result 0)` on success, `(exc 1)` on throw.
- **`try-catch`** — wraps an xt with a recovery handler. Arity-agnostic.

The handler in `shift-with` can also choose to resume the captured continuation, providing the Common Lisp restart pattern — exceptions can recover rather than just abort.

### Other

- **`depth`**, **`roll`** — standard Forth stack-manipulation primitives.
- **`see`** — prints a word's definition.
- **`words`**, **`forget`**, **`bye`**, **`gc`**, **`clear`**, **`.s`**, **`.a`** — usual interpreter utilities.

### Test coverage

48 golden-output tests in `tests/`. Run `make test`.

## What's planned

These are tracked in `PLAN.md`, with design notes for each.

### Data types

- **Dictionaries / hash maps** — first-class key→value mapping with literal syntax `{ "a": 1, "b": 2 }`. Open-addressing hash table.
- **Time / dates** — Unix timestamps as floats, `now`, `time-format`, `time-parse`.
- **Random numbers** — xoshiro256++ PRNG with `random-float`, `random-int`, `seed!`, `shuffle`.

### Strings

- **POSIX regex + UTF-8** — `match` as the primitive, with higher-level wrappers (`split`, `replace`, `index-of`, `starts-with`, `ends-with`, `trim`, `lines`, `substring`). Codepoint-indexed at the user level.

### External I/O

- **TSV file I/O** — the sole tabular format; everything else (CSV, JSON) gets converted outside logicforth.
- **SQLite integration** — embedded relational storage via vendored sqlite amalgamation. Queries return arrays of rows or matrices when columns are all numeric.

### Language ergonomics

- **Word-local variables** — `{| name |}` syntax at the head of a colon definition; `TO` for mutation.
- **Sort** — `sort`, `sort-with`, `sort-by`.
- **stdin / env** — `read-line`, `read-all`, environment variable access.
- **Functional primitives** — `filter`, `reduce` (in C); `find`, `any?`, `all?`, `range`, `take`, `drop`, `reverse`, `concat`, `flat-map`, `distinct` (in `lib.l4`).
- **REPL prompt** — show stack depth + compressed top-of-stack representation.
- **Help system** — `help word` showing a one-line doc string.

### Logic layer

- **Unification + nondeterminism** — `T_LOGIC_VAR`, trail-based binding, `unify` primitive. `amb`, `fail`, `once`, `fresh`, `run` as library words on top of the continuation machinery. microKanren-flavored, closer to Prolog than to the faithful Scheme port.

### Concurrency

Three coherent paths, independent enough to be built in stages:

- **Cooperative green threads** — `spawn`, `yield`, `run-scheduler` on top of continuations. No parallelism, but cheap interleaving for I/O-bound work.
- **Interpreter refactor** — bundle global state into `Interpreter *` and thread it through every primitive. Independent of concurrency but a prerequisite for the next item, and good architecture regardless (embeddability, sandboxing).
- **OS-thread parallelism + mailboxes** — Erlang-style isolated interpreters communicating via per-thread mailboxes. Real multi-core parallelism, no shared mutable state.

### Matrix follow-ups

- **`val_cmp` for matrices** — currently falls through to "always equal."
- **Slicing**, **`hstack`**/`vstack`, **norms**, **element-wise comparison**.
- **SVD** — one-sided Jacobi, ~150 lines.

## Project layout

```
src/c/logicforth.c     — the entire interpreter
src/forth/lib.l4       — standard library (auto-loaded at startup)
tests/                 — golden-output test files
docs/                  — design documents
examples/              — sample programs
bench/                 — benchmarks (matrix multiply vs Accelerate)
PLAN.md                — deferred work and design notes
```

## License

See `LICENSE`.
