# logicforth

A Forth-flavored language for numeric and matrix work, statistics and
regression, set/array/frame manipulation, string/regex processing, logic
programming, and multi-core data parallelism — with embedded SQLite and a
runtime C FFI. A compact C interpreter built with `clang -O3`.

## Building and running

```
make           # builds ./logicforth
make test      # runs the golden-output test suite
make bench     # runs the benchmark suite (logicforth vs CPython)
./logicforth   # REPL
```

Self-contained: its vendored dependencies — PCRE2 (regex), isocline (REPL line
editing), and SQLite (embedded SQL) — live under `external/` and are built from
source into the binary, so `make` needs only a C compiler and the system
`libffi`. Refresh them with `make vendor-pcre2`, `sh tools/vendor-isocline.sh`,
and `sh tools/vendor-sqlite.sh` (see each directory's `PROVENANCE`).

On macOS, `make` also builds `liblapacke_accel.dylib`, a thin LAPACK-over-
Accelerate shared library that the statistics library `dlopen`s through the FFI
for SVD and least-squares; only the linear-algebra fits depend on it. Re-vendor
it with `make vendor-lapacke`.

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

\ Path queries — * (any child), // (any depth), [pred] filters
{ :a { :n 1 } :b { :n 2 } } /*/n select-values .   \ [ 1 2 ]

\ JSON: parse to frames/arrays, serialize back
"[1, 2, 3]" json>frame frame>json .     \ [1, 2, 3]

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

\ Logic: unify binds variables; amb is a committed choice
[ 1 2 3 ] [ X Y Z ] ~ drop  X $ . Y $ . Z $ . cr   \ 1 2 3
[: fail :] [: "fallback" :] amb .                  \ fallback

\ Multi-core: run a quotation across the array on every core
[ 1 2 3 4 5 6 7 8 ] [: dup * :] pmap .  \ [ 1 4 9 16 25 36 49 64 ]

\ Statistics over a matrix column: mean and the median (0.5 quantile)
[ 2 4 4 4 5 5 7 9 ] 8 1 matrix dup mean . 0.5 quantile .  \ 5  4.5

\ SQLite, in-memory: create, insert a bound param, query back
":memory:" db-open
dup "create table t(x)" [ ] db-exec drop
dup "insert into t values (?)" [ 42 ] db-exec drop
"select x from t" [ ] db-query :rows @ 0 @i :x @ .   \ 42
```

## Features

### Core language

- **Tagged Vals** — floats, strings, symbols, sets, arrays, cons pairs, frames, matrices, execution tokens, dictionary addresses, continuations, logic variables, process streams, internal marks. A single 8-byte NaN-boxed representation; the tag determines interpretation.
- **Direct-threaded inner interpreter** — each dictionary cell is a handler function pointer, dispatched by an indirect tail call (`musttail`); a colon call, literal, or branch carries its operand in the cell(s) right after the handler. The dictionary *is* the threaded code — no separate bytecode.
- **Compile-time instruction fusion** — adjacent variable-reads and float ops collapse into single instructions (`var var f+` → one op; `… var f+!` fuses the store), `f*+` / `f*-` are fused multiply-add/subtract, and a comparison immediately before a branch (`= if`, `gt while`, `0= until`) fuses into a single compare-and-branch op, and an array read-modify-write (`arr i arr i @i f1- !i` or a `… delta f+ !i` step) collapses to one in-place element update. Variable-fused float words (`vf+`/`vf*`/… on one named variable, `vvf+`/`vvf*`/… on two) collapse the variable load into the float op.
- **Program and execution state separated** — the dictionary, symbol pool, and object heap live in global structures (`Vocabulary`, `Compiler`, `Arena`) that are read-only during a run; the per-run mutable state — the three stacks, instruction pointer, locals, and GC roots — lives in an `Interpreter`, so one program can be shared across multiple execution contexts.
- **Three stacks** — data, return, and a side stack for stashing values that mustn't sit on the other two.
- **Colon definitions** — `: name body ;`. The body is captured as source text for `see` and the text-form `save`.
- **Anonymous quotations** — `[: ... :]` pushes a fresh xt. Works at top level and inside colon defs.
- **Control flow** — `if`/`else`/`then`, the `begin`/`until`/`again` and `begin`/`while`/`repeat` loops, counted `times` / `i-times`, `exit`, and `>r`/`r>`/`r@` for return-stack access.
- **Tick and execute** — `' word execute` for first-class invocation by name.
- **`forget`** — truncate the dictionary back to a named word; symbol identities survive.
- **Variables and symbols** — `variable foo` declares a global; read it by bare name, assign with `42 to foo` (`to` also auto-creates a global on first assignment at the REPL). `symbol bar` defines a symbol; `:foo` is a symbol literal interned on use; `string>symbol` interns a computed string.
- **Word-local variables** — `| x y |` at the head of a colon definition or quotation declares scoped slots (initialized to `0.0`); read by bare name, assign with `to name`. `++ name` / `-- name` increment/decrement a local in place (`f++` / `f--` the unsafe float-only forms). Locals nest through quotations and survive continuation capture.
- **Mark-and-sweep GC** — walks data/return/side stacks, dictionary, and a small `gc_roots` array for in-flight C-level temporaries. It triggers on object-table pressure and on live-byte pressure, and runs at a safepoint between words so popped C-level operands stay live.

### Numeric / matrix

- **Polymorphic arithmetic** — `+`/`-`/`*`/`/` dispatch on operand tags: floats compute, strings concatenate (`+`), sets union/difference/intersection, matrices element-wise, a scalar broadcasts over a matrix, and arrays concatenate (`+`).
- **Integer division** — `%` ( a b -- rem quot ) truncating divmod on floats (errors on a zero divisor); `mod` (remainder, sign follows the dividend) and `quotient` (toward zero) build on it.
- **In-place matrix ops** — `+!`/`-!`/`*!`/`/!` mutate the left matrix in place (explicit; the programmer decides). Float-only fast paths (`f+`, `f-`, `f*`, `f/`, `f^`, …) skip the type dispatch when both operands are known floats.
- **Matrix construction** — `R C 0-matrix` (zeros), `[ ... ] R C matrix`, `V N diagonal-matrix` (N×N with V on the diagonal), `N identity-matrix`, `start end step matrix-range` (a 1×N row over a stepped range).
- **DGEMM** — `dgemm-nn`/`tn`/`nt`/`tt` (`αAB + βC`) for all four transpose variants. The non-transposed `nn` path is ikj-ordered with `restrict` pointers for cache-friendly access; the transposed variants use a straightforward triple loop.
- **Indexing** — `@i`/`@j`/`@i,j` to read rows, columns, or single cells.
- **Shape** — `dim`, `reshape`, `flatten`, `transpose`, `diagonal`.
- **Selection** — `augment` (concatenate two matrices column-wise), `submatrix` (copy a half-open row×column block), `select-rows` (gather rows named by a float index array).
- **Reductions** — `sum`, `row-sums`, `column-sums`, `max`, `min`, `argmax`, `argmin` (flat row-major index of the extreme element), `row-maxes`, `row-mins`, `column-maxes`, `column-mins`. Library `mean`, `row-means`, `column-means` on top.
- **Descriptive statistics** — `var` (sample variance) and `quantile` (linearly interpolated at p ∈ [0,1]) over all elements; the loadable statistics library layers `std`, `se`, `median`, `percentile`, `iqr`, and `ci` on these.
- **Element-wise math** — `abs`, `sqrt`, `exp`, `log`, `ln`, `sin`, `cos`, `tan`, `tanh`, `asin`, `acos`, `atan`, `round`, `truncate`, `round-up`, `round-down`. Polymorphic over floats and matrices.
- **Total ordering** — `=`/`lt`/`gt` compare matrices by shape then row-major contents, so matrices work as set members.

### Bitwise

Integer bitwise operators over the float representation: a value is read as a two's-complement integer (exact within the double's 53-bit range), the operation runs, and the result is pushed back as a float. Enough for byte- and bit-level work — block ciphers, codecs, bit-stream packing.

- **`bit-and`** / **`bit-or`** / **`bit-xor`** / **`bit-not`** — bitwise logic, named apart from the truthiness words `and`/`or`/`not`.
- **`lshift`** / **`rshift`** — left shift and arithmetic right shift (`= floor(a / 2ⁿ)`); **`lowest-bit`** — 0-indexed position of the lowest set bit (−1 when zero).

### Segments

Flat, fixed-length typed numeric buffers stored off the arena (one allocation, freed by GC), for dense numeric data without per-element boxing and as FFI scratch.

- **`int-segment`** / **`double-segment`** — `( n -- seg )` an n-element zero-filled buffer; both store doubles internally, so `@i` reads and `!i` writes a float, sharing the array indexing words.
- **`segment>pointer`** — intern the backing buffer as a `T_PTR` for an FFI `:ptr` argument, no copy.

### Sets, arrays, higher-order

- **Set literals** — `< 1 2 3 >`, set operations, `member?`, `size`, in-place `set-add!`/`set-remove!`, and `array>set` (sort-and-dedup an array into a set in one pass).
- **`group-by`** — `array :col group-by` groups frames by a symbol field into a frame from each value to a set of rows (the engine behind fast indexing and aggregation).
- **Array literals** — `[ 1 2 3 ]`, the `array` constructor (gather N from the stack), `array-of` (fill), `range` ( from to -- arr ) for an ascending or descending integer sequence, `iota` ( n -- [0..n-1] ), indexed access via `@i`, in-place store via `!i`.
- **Array operations** — `sort` (a sorted copy in `val_cmp` order), `reverse`, `take`, `concat`, `flatten-array` (flatten one level), and `sample` ( arr count repl -- arr ) drawing elements with or without replacement.
- **Map, fold, zip-map, filter** — `map` for a single source, `reduce` for a left fold with an accumulator, `mapn` for N-ary zip, `filter` to select by predicate, with anonymous quotations as the higher-order argument.
- **Destructuring** — `destruct` spreads a set/array/frame's elements onto the stack (a frame as alternating symbol/value). `destruct-to` ( values names -- ) takes two equal-length arrays and assigns each value to the global variable named by the corresponding symbol, creating it if absent.
- **In-place slicing** — `slice!` copies a strided run from one array into another, `reverse-slice!` reverses a run in place, `to-slice!` stores values from the stack into a range.

### Random

A thread-local xoshiro256\*\* stream. Each worker thread derives its own stream from the shared base seed, so parallel draws are deterministic per worker.

- **`seed`** — `( n -- )` set the global base seed and reset the stream.
- **`random`** — `( -- f )` a uniform float in `[0, 1)`; **`random-int`** — `( bound -- f )` a uniform integer in `[0, bound)`.
- `sample` (arrays) and `resample-indices` (datasets) draw on this stream.

### Multi-core parallelism

Worker threads over one shared object heap (not `fork`): a quotation runs across the collection on several cores, results joining back by handle with no copy. Allocation in a region is per-worker; a region whose results don't escape is rewound wholesale, and live results are retained by handle.

- **`pmap`** — `( arr xt -- arr )` parallel `map`; **`pfilter`** — `( arr pred -- arr )` parallel `filter`, order preserved; **`pmap-reduce`** — `( arr id map-xt combine-xt -- val )` fused parallel map+fold, with `combine-xt` associative and `id` its neutral element.
- **`-ext` forms** — `pmap-ext` / `pfilter-ext` / `pmap-reduce-ext` take an explicit worker count and items-per-claim; the bare forms default to `num-cores` workers.
- **`num-cores`** — online CPU count.

### Frames

Symbol-keyed nested maps — the associative type, and the compound term the logic layer builds on. The three bracket families are distinct: `[ ]` arrays, `{ }` frames, `< >` sets.

- **Literals** — `{ :a 1 :b 2 }`; values may be any Val, including nested frames, arrays, and sets.
- **Builders** — `frame` ( keys values -- frame ) from two parallel collections, `array>frame` ( kv-array -- frame ) from an alternating key/value array, and `frame>array` ( frame -- kv-array ) the inverse, flattening to a key-sorted alternating array.
- **Path literals** — `/a/b/c` is a symbol array `[ :a :b :c ]`, built once at compile time, used to address into the tree — and usable as a key when constructing a frame (`{ /a/b/c v }` / `array>frame`), where it vivifies nested frames. A path may also be a *search* pattern: `*` matches any child at that level, `//` matches at any depth (descendant-or-self), and `[…]` filters by predicate (`[city=:NYC]`, `[age>30]`, `[.>0]` on the node itself, `[addr/zip]` on a sub-path).
- **Access** — `@` ( frame key/path -- value ) get, `!` ( frame key/path value -- frame ) set with auto-vivified intermediates, `has?` existence test, `delete-at` remove, `update-at` apply a quotation to a leaf, `merge` combine two frames (right wins), plus `keys` / `values` / `size`. The single-location words (`@`, `!`, `delete-at`, `update-at`) take a `:symbol` key or a plain `/a/b/c` locator and reject a search pattern; `has?` accepts either, answering whether any node matches.
- **Path queries** — `select-values` ( frame pattern -- array ) returns every value matched by a `*`/`//`/predicate search pattern, in document order; `select-keys` returns the full root-to-match path for each match (each round-trips back through `@`). Convert the result with `array>set` for distinct values or `array>cons` to feed matches to `choose`.
- **Representation** — sorted parallel key/value arrays with binary-search lookup; mutable in place, reference semantics. Structurally comparable, so frames work as set members and round-trip through their `{ }` literal.

### Strings and regex

- **String literals** are raw (newlines allowed; `""` is the one escape → a literal `"`); **`format`** fills `{n}` placeholders from the stack — `"got {0} of {1}" format`; **polymorphic concatenation** via `+`.
- **Regex** on PCRE2 (Perl-compatible, JIT-compiled): `match` (first match as a flat `[ whole cap… ]`), `match-all` (all matches, nested), `replace` (replace-all, with `&` / `\1`–`\9` backrefs), and the `has?` string overload (does the pattern match?). Patterns are plain `"..."` literals — PCRE2 reads `\d`, `\w`, `\n`, lookaround, `\p{...}`.
- **Slicing / building** — `substring` (half-open codepoint range), `char-at` (the one-character string at a codepoint index), `split` (split at each non-overlapping match of a pattern, empty fields kept), `join` (concatenate an array of strings with a separator).
- **Unicode** — strings are UTF-8 and the bare words work in *codepoints*: `size`/`substring`/`char-at`/`codepoint-at` count and index by codepoint, with byte-level forms (`byte-size`, `byte-substring`) for the raw layer and to pair with regex byte offsets. `string>chars`/`string>codepoints` decompose a string, `codepoint>char`/`codepoints>string` rebuild one, and `emit` UTF-8-encodes a codepoint. Regex runs in UTF + UCP mode: `.` matches a codepoint, `\w`/`\d`/`\b` are Unicode-aware, and invalid byte sequences are tolerated rather than erroring.

### JSON

- **`json>frame`** — parse a JSON string into native values: objects → frames (keys interned as symbols), arrays → arrays, strings → strings (escapes and `\uXXXX` decoded to UTF-8), numbers → floats, `true`/`false` → the reserved `:1`/`:0` boolean symbols, `null` → `null` (the none value). Recursive-descent, GC-safe, rejects trailing garbage.
- **`frame>json`** — serialize a value back to a JSON string: floats use a shortest round-trip representation, strings are escaped, `:1`/`:0` → `true`/`false`, none → `null`.

### I/O and persistence

- **Interactive REPL** with full isocline line editing: theme-adaptive **syntax highlighting**, **matching-brace** highlighting, **inline hints** and **Tab completion** (word names from the live dictionary, filenames inside string literals), persistent history (`.logicforth_history`), and **multi-line editing** — `Ctrl+J` inserts a line, `Enter` submits the whole buffer. A `count|top` prompt shows stack depth and the top value, green on a terminal, red on error. `.` pretty-prints a nested array across lines with the opening brackets aligned; strings print quoted inside a collection and in `.s`, raw when printed bare.
- **`load`** runs a source file as if typed.
- **`save`** writes the user's vocabulary as a re-loadable `.l4` source file.
- **`save-image`** / **`load-image`** — binary image with full state preservation (dictionary, objects, stacks, continuations).
- **`reload`** truncates user state and re-runs every file `load`ed this session, in order.
- **`read-file`** / **`write-file`** / **`append-file`** — read a whole file as one (byte-safe) string; write or append a string's bytes to a path.
- **`env`** / **`env!`** — read an environment variable as a string (the none value if unset) and set one (process-wide, so `start-process` children inherit it).

### Subprocesses and pipes

Drive external programs over pipes (`fork`/`execvp`/`pipe`/`waitpid`; binary-safe, no shell):

- **`argv start-process`** — launch from an argv array; returns a frame `{ :pid :in :out :err }` with the child's pid and its stdin/stdout/stderr as `T_STREAM` values.
- **`write`** / **`read`** / **`close`** — write a string to a stream, read a stream to EOF, close one (closing `:in` sends EOF).
- **`running?`** / **`wait`** / **`stop`** — non-blocking liveness check, block-until-exit, signal-and-reap.
- `lib.l4` conveniences: **`run`** (split a command line and start it), **`read-out`** / **`read-err`** / **`write-in`**.
- **`commands width parallel-run`** — run a batch of argv arrays concurrently, at most `width` at a time, collecting `{ :out :err :status }` per command in input order (refills a slot as each child finishes). Process-level parallelism — e.g. firing off many `curl` requests at once.

### SQLite

Embedded relational storage via the vendored SQLite amalgamation — built into the binary, no external dependency. A database is a `T_DB` handle.

- **`db-open`** / **`db-close`** — open (creating if absent, or `":memory:"` for an in-memory DB) and push a handle; close frees the connection and is idempotent.
- **`db-exec`** — `( db statement params -- n )` — run an INSERT/UPDATE/DELETE/CREATE with `params` bound to its `?` placeholders; returns the affected-row count (0 for DDL).
- **`db-query`** — `( db query params -- rel )` — run a query; returns a fact-database relation `{ :rows <bag of row frames> :index { } }`, each row keyed by column-name symbols (INTEGER/REAL → float, TEXT → string, NULL → `null`, BLOB → raw bytes). Duplicates are kept, in result order; the result drops straight into `query` / `inner-join`.
- **Bound parameters** — `params` is an array bound positionally to the `?` placeholders (`[ ]` for none); floats, strings, symbols, and `null` bind, so string values need no hand-escaping.
- **`create-index`** — `( rel cols -- rel )`, `lib.l4` — index a query result on `cols`, interning those columns to symbols so the fact-db index and `query` can use them.

### Data: TSV, datasets, and statistics

TSV is the one tabular file format (convert other formats to TSV before loading).

- **`read-tsv`** / **`write-tsv`** — read a file into an array of row-arrays (a numeric cell becomes a float, an empty cell `none`, everything else a string) and write one back.
- **`rows>dataset`** — `( table header? -- frame )` a column-oriented frame, one array per column; **`rows>relation`** — `( table index-cols header? -- relation )` a deduped, indexed fact-database relation; **`dataset>matrix`** — `( dataset cols -- m )` an observations×columns numeric matrix from named columns.
- **`resample-indices`** — `( n -- arr )` n indices drawn from `[0,n)` with replacement, for bootstrap resampling.

The statistics library (`lib/statistics.l4`, loaded on demand) builds on the matrix and FFI layers:

- **Descriptive** — `std`, `se`, `median`, `percentile`, `iqr`, `ci` (percentile confidence interval).
- **Resampling** — `bootstrap` / `pbootstrap` (parallel) over a fit quotation.
- **Linear algebra** — `svd` and `fit-linear` (least-squares) call LAPACK's `dgesvd` / `dgelsd` through the FFI (the Accelerate-backed dylib on macOS).
- **Regression** — `linear-regression` and `logistic-regression` (IRLS with Firth correction), each returning per-coefficient estimate, standard error, bias, and confidence interval from a bootstrap.

### Foreign function interface

Call C functions in any shared library at runtime via `libffi` — no per-library glue. An opaque C pointer is a `T_PTR` handle (a registry index, since a 64-bit pointer doesn't fit a Val).

- **`ffi-open`** — `( path -- lib )` — `dlopen` a library and push a handle; `""` opens the running process for already-linked symbols.
- **`ffi-function`** — `( lib symbol arg-types ret-type -- ) <name>` — resolve a symbol and define the following word `<name>` to call it. Types are symbols: `:void :int :long :double :ptr :string`. Floats marshal to/from C `int`/`long`/`double`, strings pass as `const char*` (a returned `char*` is copied back into a string), `:ptr` is an opaque handle. The call interface is prepared once; calls are ~30–100 ns.
- **`ffi-variadic`** — `( lib symbol arg-types ret-type n-fixed -- ) <name>` — the same for a variadic C function (`ffi_prep_cif_var`); `n-fixed` leading args are fixed, the rest variadic, with the variadic types fixed per binding. Enough to drive `printf`, `curl_easy_setopt`, etc.
- **`ffi-free`** — `( ptr -- )` — `free` a C buffer held as a `T_PTR`.
- **`matrix>pointer`** / **`segment>pointer`** — intern a matrix's or segment's element buffer as a `T_PTR` (no copy, aliasing the live buffer) to pass dense numeric data to a `:ptr` parameter.
- Examples: `"/usr/lib/libcurl.4.dylib" ffi-open` plus a few declarations drives a real libcurl HTTPS request in-process, no subprocess; the statistics library drives LAPACK's `dgesvd`/`dgelsd` the same way (matrices in via `matrix>pointer`). FFI is unsafe — a wrong signature corrupts or crashes; arg *count* is checked, types are the caller's responsibility.

### Delimited continuations

A four-primitive substrate the rest of the control story is built on. See `docs/continuations.md` for the full treatment.

- **`reset`** — installs a delimiter (a uniquely-tagged mark on the return stack).
- **`shift`** — captures the slice up to the nearest reset, removes the mark and captured frames, pushes the continuation as a `T_CONT` Val. Used for coroutines and generators.
- **`shift-with`** — same capture, but runs a handler xt in the outer context after the unwind. Used for exceptions and restarts.
- **`resume`** — re-enters a captured continuation. Multi-shot.

### Generators

Coroutines on the continuation primitives, in `lib.l4`:

- **`yield`** — emit a value to the driver and suspend until resumed.
- **`start-generator`** — run a producer to its first `yield`, leaving the yielded value and a resumable continuation.
- **`gen-take`** — collect the first N values a producer yields into an array; **`gen-each`** — run a consumer on each yielded value until the producer falls off.

### Side stack

A third stack for stashing arbitrary Vals without disturbing the data or return stack: **`>side`**, **`side>`**, **`side-drop`**, **`side-depth`**.

### Exceptions (library)

Built in `lib.l4` on top of the continuation primitives:

- **`throw`** — non-local exit with a value.
- **`catch`** — wraps an xt; returns `(result 0)` on success, `(exc 1)` on a throw. It also intercepts **interpreter errors** — division by zero, out-of-bounds, type mismatch, and the like — returning the error message as the exception value, so a runtime fault is recoverable, not just a user `throw`.
- **`try-catch`** — wraps an xt with a recovery handler that runs on either kind of failure. Arity-agnostic.

An uncaught `throw` or interpreter error still surfaces at the REPL. The `shift-with` handler can also resume the captured continuation, giving the Common Lisp restart pattern — exceptions can recover rather than just abort.

### Logic

Unification and committed choice, on the trail and the continuation machinery:

- **Logic variables** — `lvar` makes a fresh one; a **capitalized identifier** is a logic-var literal: a persistent global at the REPL, or a fresh per-call variable when declared in `| X |` inside a definition or quotation.
- **`unify`** (`~`) — unifies two terms, binding logic vars through a trail so they match: atoms by value, arrays element-wise, frames as open records (shared keys must unify, extras allowed); on a mismatch it fails. **`deref`** (`$`) follows a variable's binding chain.
- **`amb`** / **`fail`** — committed choice: run the first branch; if it fails (a `unify` mismatch or an explicit `fail`), roll its bindings back through the trail and run the second, committing to whichever succeeds. **`choose`** generalizes it to a cons list, running a continuation with each element until one succeeds.
- **`_`** — the anonymous wildcard: unifies with anything, binds nothing, and allocates nothing.
- **`matches?`** — a non-destructive `unify` test: marks the trail, unifies, rolls back, and pushes whether the two unified — so it composes in straight-line code.
- **Cons lists** — `[( a b c )]` builds cons pairs and `[( H T )]` is the `[H|T]` head/tail pattern under `unify`; with `cons`, `head-tail`, and `array`↔`cons` conversions.
- **Fact database** — `relation` / `assert` / `query` / `retract` / `count-matches` / `inner-join`. A relation is a frame of a row-set plus per-column indexes (declared symbol columns); rows are column-keyed frames that dedup; `query` matches a pattern by unification, narrowing through the index (and returning the bucket directly when the index covers the whole pattern). `inner-join` merges two relations on a shared column via index probing; `bulk-load` builds a whole relation in one sorted pass (`array>set` for the rows, `group-by` per index) instead of row-by-row. The same row-frame shape is what a SQLite query would return.

### Other

- **`dup`**, **`drop`**, **`swap`**, **`over`**, **`rot`**, **`depth`**, **`roll`**, **`clear`** — stack-manipulation primitives.
- **`copy`** / **`reify`** — deep copy of a value (strings, arrays, sets, frames, matrices); `reify` additionally renames unbound logic vars to canonical `:_0`/`:_1`/… for a ground, storable, comparable snapshot.
- **`now`** — current Unix time as a float (seconds since epoch).
- **`see`** — prints a word's source definition; **`see-compiled`** disassembles its threaded body.
- **`man`** — `( xt -- fr )`, returns a frame of a word's reference entry (stack effect, one-line summary, cost notes). **`help name`** prints it for the named word.
- **`words`**, **`forget`**, **`bye`**, **`gc`**, **`clear`**, **`.s`**, **`.a`** — interpreter utilities.

## Future work

See `PLAN.md`.

## Project layout

```
src/c/logicforth.h     — types, global program structs (Vocabulary/Arena/Compiler), per-run Interpreter, prototypes
src/c/core.c           — engine: interpreter, dictionary, GC, printing, image, REPL
src/c/words.c          — arithmetic, stack, I/O, control flow, defining words, continuations
src/c/collections.c    — sets, arrays, and frames
src/c/matrix.c         — matrix words and numeric kernels
src/c/functional.c     — higher-order operations (map, mapn, …)
src/c/superwords.c     — compile-time instruction fusion (superwords)
src/c/strings.c        — string and PCRE2 regex operations
src/c/logic.c          — logic variables, unification, amb, fact database
src/c/database.c       — SQLite integration
src/c/foreign.c        — FFI (libffi), pointer registry, matrix/segment bridges
src/c/help_table.c     — generated help/man text (from docs/reference.md)
src/forth/lib.l4       — standard library (embedded, auto-loaded at startup)
lib/                   — loadable libraries: statistics.l4, files.l4, claude.l4
external/              — vendored deps: pcre2, sqlite, isocline, lapacke
tests/                 — golden-output test files
bench/                 — benchmark suite (logicforth vs CPython) and inventory
docs/                  — design documents and the word reference
examples/              — sample programs
PLAN.md                — future work
```

## License

See `LICENSE`.
