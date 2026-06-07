# logicforth

A Forth-flavored language for matrix work, set/array manipulation,
string/regex processing, and (eventually) logic programming. A compact C
interpreter built with `clang -O3`.

## Building and running

```
make           # builds ./logicforth
make test      # runs the golden-output test suite
./logicforth   # REPL
```

Links PCRE2 (`libpcre2-8.a`, statically) for the regex engine — install it
first (`brew install pcre2`). Vendoring PCRE2 to restore the self-contained,
zero-dependency build is planned.

## A taste

```forth
\ Arithmetic
3 4 + .                                 \ 7

\ Matrices: * is element-wise; matrix multiply is dgemm (αAB + βC)
[ 1 2 3 4 ] 2 2 matrix dup transpose *  \ element-wise product of M and Mᵀ

\ Sets and set algebra
< 1 2 3 > < 2 3 4 > + .                 \ < 1 2 3 4 >  (union via polymorphic +)

\ Set-builder { x² | x ∈ 1..10, even x } — literal + filter/map + destruct
< 1 10 range [: 2 mod 0= :] filter [: fsq :] map destruct > .   \ < 4 16 36 64 100 >

\ Frames — symbol-keyed nested maps
{ :a 1 :b { :c 2 } } /b/c @ .           \ 2

\ Higher-order operations
[ 1 2 3 4 5 ] [: dup * :] map .         \ [ 1 4 9 16 25 ]

\ Strings and regex (PCRE2)
"x=42" "(\w+)=(\d+)" match .            \ [ "x=42" "x" "42" ]
"hello world" "o" "0" replace .         \ hell0 w0rld

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

- **Tagged Vals** — floats, strings, symbols, sets, arrays, frames, matrices, execution tokens, dictionary addresses, continuations, internal marks. A single 8-byte NaN-boxed representation; the tag determines interpretation.
- **Direct-threaded inner interpreter** — each dictionary cell is a handler function pointer, dispatched by an indirect tail call (`musttail`); a colon call, literal, or branch carries its operand in the cell(s) right after the handler. The dictionary *is* the threaded code — no separate bytecode.
- **Compile-time instruction fusion (superwords)** — adjacent variable-reads and float ops collapse into single instructions (`var var f+` → one op; `… var f+!` fuses the store), and `f*+` / `f*-` are fused multiply-add/subtract.
- **Per-interpreter state** — all mutable state lives in an `Interpreter`, which owns its `Vocabulary` (a growable dictionary plus name/source/symbol pools). Multiple independent instances can coexist in one process; the engine is embeddable.
- **Three stacks** — data, return, and a side stack for stashing values that mustn't sit on the other two.
- **Colon definitions** — `: name body ;`. The body is captured as source text for `see` and the text-form `save`.
- **Anonymous quotations** — `[: ... :]` pushes a fresh xt. Works at top level and inside colon defs.
- **Control flow** — `if`/`else`/`then`, the `begin`/`until`/`again` and `begin`/`while`/`repeat` loops, counted `times` / `i-times`, `exit`, and `>r`/`r>`/`r@` for return-stack access.
- **Tick and execute** — `' word execute` for first-class invocation by name.
- **`forget`** — truncate the dictionary back to a named word; symbol identities survive.
- **Variables and symbols** — `variable foo` declares a global; read it by bare name, assign with `42 to foo` (`to` also auto-creates a global on first assignment at the REPL). `symbol bar` defines a symbol; `:foo` is a symbol literal interned on use; `string>symbol` interns a computed string.
- **Word-local variables** — `| x y |` at the head of a colon definition or quotation declares scoped slots (initialized to `0.0`); read by bare name, assign with `to name`. Locals nest through quotations and survive continuation capture.
- **Mark-and-sweep GC** — walks data/return/side stacks, dictionary, and a small `gc_roots` array for in-flight C-level temporaries.

### Numeric / matrix

- **Polymorphic arithmetic** — `+`/`-`/`*`/`/` dispatch on operand tags: floats compute, strings concatenate (`+`), sets union/difference/intersection, matrices element-wise, a scalar broadcasts over a matrix, and arrays concatenate (`+`).
- **In-place matrix ops** — `+!`/`-!`/`*!`/`/!` mutate the left matrix in place (explicit; the programmer decides). Float-only fast paths (`f+`, `f-`, `f*`, `f/`, `f^`, …) skip the type dispatch when both operands are known floats.
- **Matrix construction** — `R C 0-matrix` (zeros), `[ ... ] R C matrix`, `V N diagonal-matrix` (N×N with V on the diagonal), `N identity-matrix`.
- **DGEMM** — `dgemm-nn`/`tn`/`nt`/`tt` (`αAB + βC`) for all four transpose variants. The non-transposed `nn` path is ikj-ordered with `restrict` pointers for cache-friendly access; the transposed variants use a straightforward triple loop.
- **Indexing** — `@i`/`@j`/`@i,j` to read rows, columns, or single cells.
- **Shape** — `dim`, `reshape`, `flatten`, `transpose`, `diagonal`.
- **Reductions** — `sum`, `row-sums`, `column-sums`, `max`, `min`, `row-maxes`, `row-mins`, `column-maxes`, `column-mins`. Library `mean`, `row-means`, `column-means` on top.
- **Element-wise math** — `abs`, `sqrt`, `exp`, `log`, `sin`, `cos`, `tan`, `tanh`. Polymorphic over floats and matrices.
- **Total ordering** — `=`/`lt`/`gt` compare matrices by shape then row-major contents, so matrices work as set members.

### Sets, arrays, higher-order

- **Set literals** — `< 1 2 3 >`, set operations, `member?`, `size`.
- **Array literals** — `[ 1 2 3 ]`, the `array` constructor (gather N from the stack), `array-of` (fill), indexed access via `@i`.
- **Map, fold, zip-map, filter** — `map` for a single source, `reduce` for a left fold with an accumulator, `mapn` for N-ary zip, `filter` to select by predicate, with anonymous quotations as the higher-order argument.

### Frames

Symbol-keyed nested maps — the associative type, and the compound term the planned logic layer builds on. The three bracket families are distinct: `[ ]` arrays, `{ }` frames, `< >` sets.

- **Literals** — `{ :a 1 :b 2 }`; values may be any Val, including nested frames, arrays, and sets.
- **Builders** — `frame` ( keys values -- frame ) from two parallel collections, `>frame` ( kv-array -- frame ) from an alternating key/value array.
- **Path literals** — `/a/b/c` is a symbol array `[ :a :b :c ]`, built once at compile time, used to address into the tree.
- **Access** — `@` ( frame key/path -- value ) get, `!` ( frame key/path value -- frame ) set with auto-vivified intermediates, `has?` existence test, `delete-at` remove, `update-at` apply a quotation to a leaf, `merge` combine two frames (right wins), plus `keys` / `values` / `size`. Each accessor takes a single `:symbol` key or a `/a/b/c` path.
- **Representation** — sorted parallel key/value arrays with binary-search lookup; mutable in place, reference semantics. Structurally comparable, so frames work as set members and round-trip through their `{ }` literal.

### Strings and regex

- **String literals** are raw (newlines allowed, no escapes/substitution); **`format`** fills `{n}` placeholders from the stack — `"got {0} of {1}" format`; **polymorphic concatenation** via `+`.
- **Regex** on PCRE2 (Perl-compatible, JIT-compiled): `match` (first match as a flat `[ whole cap… ]`), `match-all` (all matches, nested), `replace` (replace-all, with `&` / `\1`–`\9` backrefs), and the `has?` string overload (does the pattern match?). Patterns are plain `"..."` literals — PCRE2 reads `\d`, `\w`, `\n`, lookaround, `\p{...}`.
- **Slicing / building** — `substring` (half-open byte range), `join` (concatenate an array of strings with a separator).

### I/O and persistence

- **Stdin REPL**, rlwrap-friendly, with a `count|top` prompt showing stack depth and the top value — green on a terminal, red on error. `.` pretty-prints a nested array across lines with the opening brackets aligned; strings print quoted inside a collection and in `.s`, raw when printed bare.
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
- **`copy`** — deep copy of a value (strings, arrays, sets, frames, matrices).
- **`now`** — current Unix time as a float (seconds since epoch).
- **`see`** — prints a word's source definition; **`see-compiled`** disassembles its threaded body.
- **`words`**, **`forget`**, **`bye`**, **`gc`**, **`clear`**, **`.s`**, **`.a`** — interpreter utilities.

## What's planned

Tracked in `PLAN.md`, with design notes for each.

### Data types

- **Time / dates** — strftime/strptime formatting and parsing (`time-format`, `time-parse`) over the `now` float timestamps that already exist.
- **Random numbers** — xoshiro256++ PRNG: `random-float`, `random-int`, `seed!`, `shuffle`.

### Strings

- **`lib.l4` wrappers** over the regex layer — `split`, `index-of`, `starts-with`, `ends-with`, `trim`, `lines`.
- **UTF-8 / codepoint indexing** — string ops are byte-indexed today; codepoint-indexed at the user level is planned.
- **Vendor PCRE2** — bundle the PCRE2 sources to restore the self-contained build.

### External I/O

- **TSV file I/O** — the sole tabular format; other formats convert outside logicforth.
- **SQLite integration** — embedded relational storage via the vendored amalgamation. Queries return arrays of rows, or matrices when all columns are numeric.

### Language ergonomics

- **Sort** — `sort`, `sort-with`, `sort-by`.
- **stdin / env** — `stdin`/`stdout`/`stderr` as streams (read/written with the subprocess `read`/`write`), environment variable access.
- **Functional primitives** — `range` remains in C; `find`, `any?`, `all?`, `flat-map`, `sort-by` in `lib.l4`. (`map`/`mapn`/`filter`/`reduce`/`take`/`reverse`/`concat` done in C; `skip`/`last` in `lib.l4`.)
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
- **Optional BLAS/LAPACK build** — swap the hand-rolled kernels for BLAS/LAPACK behind a build switch; default stays zero-dependency.

## Project layout

```
src/c/logicforth.h     — types, Interpreter/Vocabulary structs, prototypes
src/c/core.c           — engine: interpreter, dictionary, GC, printing, image, REPL
src/c/words.c          — arithmetic, stack, I/O, control flow, defining words, continuations
src/c/collections.c    — sets, arrays, and frames
src/c/matrix.c         — matrix words and numeric kernels
src/c/functional.c     — higher-order operations (map, mapn, …)
src/c/superwords.c     — compile-time instruction fusion (superwords)
src/c/strings.c        — string and PCRE2 regex operations
src/forth/lib.l4       — standard library (auto-loaded at startup)
tests/                 — golden-output test files
docs/                  — design documents
examples/              — sample programs
PLAN.md                — deferred work and design notes
```

## License

See `LICENSE`.
