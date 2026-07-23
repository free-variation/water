# <img src="water_logo.png" alt="" height="40" align="top"> Water

A Forth-flavored language for numeric and matrix work, statistics and
regression, dimensioned quantities and calendar arithmetic, set/array/frame
manipulation, string/regex processing, logic programming, and multi-core data
parallelism — with embedded SQLite and a runtime C FFI. A compact,
self-contained C interpreter.

The doc system is designed to fit into a single LLM prompt, making the language LLM-friendly from the start.

Dedicated to Chuck Peddle and Tony Wilkinson.

## Building and running

```
make           # builds ./water
make test      # runs the golden-output test suite
make bench     # runs the benchmark suite (Water vs CPython)
./water              # REPL
./water prog.h2o     # run program files and exit (repeatable, in order; -i to drop into the REPL after)
./water -e '3 4 + .' # run a code string and exit (repeatable, in argument order with files; implies -b)
```

Self-contained: its vendored dependencies — PCRE2 (regex), isocline (REPL line
editing), and SQLite (embedded SQL) — live under `external/` and are built from
source into the binary, so `make` needs only a C compiler and the system
`libffi`. Refresh them with `make vendor-pcre2`, `make vendor-sqlite`, and
`make vendor-isocline` (see each directory's `PROVENANCE`).

`make` also builds `liblapacke_water.so`, a thin shared library that wraps
the platform BLAS/LAPACK (Accelerate on macOS, OpenBLAS on Linux) behind
the LAPACKE C interface. The statistics library `dlopen`s it through the
FFI and requires it — the stats module is native-only; the wasm build
excludes the FFI. Re-vendor with `make vendor-lapacke`.

```
make wasm        # cross-builds water.wasm (needs wasi-sdk in ~/wasi-sdk, or set WASI_SDK)
make test-wasm   # runs the golden suite against water.wasm under wasmtime
```

The wasm build targets WASI (a-shell, standalone runtimes, the browser via a
WASI shim): PCRE2 compiles without JIT, SQLite single-threaded, and the
platform layer stubs what WASI lacks — no isocline line editing, FFI,
subprocesses, or threads; the loadable statistics library is native-only.
`make test-wasm` finds `wasmtime` on `PATH` or `~/.wasmtime/bin`, or set
`WASMTIME=<path>`; tests exercising the stubbed words are skipped via
`tests/wasm-skip.txt`.

## A taste

```forth
\ Arithmetic
3 4 + .                                 \ 7

\ Anaphora: it names the top of the stack as the line began — pinned, non-consuming
5
it it * .                               \ 25

\ Matrices: * is element-wise; matrix multiply is dgemm (αAB + βC)
[ 1 2 3 4 ] 2 2 matrix dup transpose *  \ element-wise product of M and Mᵀ

\ Dimensioned quantities: units propagate, combine, and collapse
10 m 2 s / .                            \ 5 m.s^-1
1 kg 1 m * 1 s / 1 s / .                \ 1 newton   (interns to the named unit)

\ Dates: instants are quantities in s, so units do the date arithmetic
wall-now 2 week + time>iso .            \ the ISO timestamp two weeks from now
"2026-01-31T09:00:00Z" iso>time { :months 1 } date-shift time>iso .   \ clamps to Feb 28

\ Sets and set algebra
< 1 2 3 > < 2 3 4 > + .                 \ < 1 2 3 4 >  (union via polymorphic +)

\ Set-builder { x² | x ∈ 1..10, even x } — literal + filter/map + destruct
< 1 10 range [: 2 mod 0= :] filter [: fsq :] map destruct > .   \ < 4 16 36 64 100 >

\ Frames — symbol-keyed nested maps
{ :a 1 :b { :c 2 } } /b/c @ .           \ 2

\ Path queries — * (any child), // (any depth), [pred] filters
{ :a { :n 1 } :b { :n 2 } } /*/n select-values .   \ [ 1 2 ]
{ :ann { :age 34 } :bo { :age 25 } } /*[age>30]/age select-values .   \ [ 34 ]

\ JSON: parse to frames/arrays, serialize back
"[1, 2, 3]" json>frame frame>json .     \ [1, 2, 3]

\ Higher-order operations
[ 1 2 3 4 5 ] [: dup * :] map .         \ [ 1 4 9 16 25 ]
[ "bb" "a" "ccc" ] [: size :] sort-by . \ [ "a" "bb" "ccc" ]

\ Strings and regex (PCRE2)
"x=42" "(\w+)=(\d+)" match .            \ [ "x=42" "x" "42" ]
"hello world" "o" "0" replace .         \ hell0 w0rld

\ Exceptions
[: "missing" throw :]
[: "got " . . cr :] try-catch           \ prints "got missing"

\ Generators — coroutines on the delimited-continuation primitives
: primes 2 yield 3 yield 5 yield 7 yield ;
' primes 4 gen-take .                   \ [ 2 3 5 7 ]

\ Subprocesses over pipes
"echo hi" run read-out .                \ hi

\ Logic: unify binds variables; amb is a committed choice
lvar to X  lvar to Y  lvar to Z
[ 1 2 3 ] [ X Y Z ] ~ drop  X ? . Y ? . Z ? . cr   \ 1 2 3
[: fail :] [: "fallback" :] amb .                  \ fallback

\ Multi-core: run a quotation across the array on every core
[ 1 2 3 4 5 6 7 8 ] [: dup * :] pmap .  \ [ 1 4 9 16 25 36 49 64 ]

\ Datasets: column-oriented tables with verbs
[ [ "name" "age" ] [ "ann" 34 ] [ "bo" 25 ] [ "cy" 61 ] ] true rows>dataset
dup :age @ mean .                       \ 40  (a numeric column is already a vector)
[: :age @ 30 gt :] filter :name @ .     \ [ "ann" "cy" ]

\ Count distinct values, most frequent first; masks alter as well as select
[ :b :a :b :c :b ] count first .        \ [ :b 3 ]
[ 1 2 999 4 ] vector dup 999 eq null mesh matrix>array .   \ [ 1 2 null 4 ]

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

- **Tagged Vals** — floats, strings, symbols, sets, arrays, cons pairs, frames, matrices, quantities, segments, execution tokens, dictionary addresses, continuations, logic variables, process streams, database handles, C pointers, internal marks. A single 8-byte NaN-boxed representation; the tag determines interpretation.
- **Direct-threaded inner interpreter** — each dictionary cell is a handler function pointer, dispatched by an indirect tail call (`musttail`); a colon call, literal, or branch carries its operand in the cell(s) right after the handler. The dictionary *is* the threaded code.
- **Compile-time instruction fusion** — adjacent variable-reads and float ops collapse into single instructions (`var var f+` → one op; `… var f+!` fuses the store), `f*+` / `f*-` are fused multiply-add/subtract, and a comparison immediately before a branch (`= if`, `gt while`, `0= until`) fuses into a single compare-and-branch op, and an array read-modify-write (`arr i arr i @i f1- !i` or a `… delta f+ !i` step) collapses to one in-place element update. Variable-fused float words (`vf+`/`vf*`/… on one named variable, `vvf+`/`vvf*`/… on two) collapse the variable load into the float op.
- **Program and execution state separated** — the dictionary, symbol pool, and object heap live in global structures (`Vocabulary`, `Compiler`, `Arena`) that are read-only during a run; the per-run mutable state — the three stacks, instruction pointer, locals, and GC roots — lives in an `Interpreter`, so one program can be shared across multiple execution contexts.
- **Three stacks** — data, return, and a side stack for stashing values that mustn't sit on the other two.
- **Colon definitions** — `: name body ;`. The body is captured as source text for `see` and the text-form `save`.
- **Anonymous quotations** — `[: ... :]` pushes a fresh xt. Works at top level and inside colon defs.
- **Partial application** — `curry` ( value xt -- xt' ) binds a value into a new anonymous word; the curried xt travels through other words' frames intact.
- **Control flow** — `if`/`else`/`then`, the `begin`/`until`/`again` and `begin`/`while`/`repeat` loops with `leave` / `continue` for early exit, counted `times` / `i-times`, `exit`, and `>r`/`r>`/`r@` for return-stack access.
- **Tick and execute** — `' word execute` for first-class invocation by name.
- **`forget`** — truncate the dictionary back to a named word; symbol identities survive.
- **Variables and symbols** — `variable foo` declares a global; read it by bare name, assign with `42 to foo` (`to` also auto-creates a global on first assignment at the REPL). `symbol bar` defines a symbol; `:foo` is a symbol literal interned on use; `string>symbol` interns a computed string.
- **Word-local variables** — `| x y |` at the head of a colon definition or quotation declares scoped slots (uninitialized — assign before reading; the compiler rejects a scope that reads a slot it stores nowhere, which also catches a local name shadowing a word the body meant to call); read by bare name, assign with `to name`. A `>` prefix receives a slot from the stack, a `?` prefix fills it with a fresh logic variable per call. `++ name` / `-- name` increment/decrement a local in place (`f++` / `f--` the unsafe float-only forms). Locals nest through quotations and survive continuation capture.
- **Mark-and-sweep GC** — walks data/return/side stacks, dictionary, and a small `gc_roots` array for in-flight C-level temporaries. It triggers on object-table pressure and on live-byte pressure, and runs at a safepoint between words so popped C-level operands stay live.

### Numeric / matrix

- **Polymorphic arithmetic** — `+`/`-`/`*`/`/` dispatch on operand tags: floats compute, strings concatenate (`+`), sets union/difference/intersection, matrices element-wise, a scalar broadcasts over a matrix, and arrays concatenate (`+`).
- **Integer division** — `%` ( a b -- rem quot ) truncating divmod on floats (errors on a zero divisor); `mod` (remainder, sign follows the dividend) and `quotient` (toward zero) build on it.
- **In-place matrix ops** — `+!`/`-!`/`*!`/`/!` mutate the left matrix in place (explicit; the programmer decides). Float-only fast paths (`f+`, `f-`, `f*`, `f/`, `f^`, …) skip the type dispatch when both operands are known floats.
- **Matrix construction** — `R C 0-matrix` (zeros), `[ ... ] R C matrix`, `[ ... ] vector` (an n×1 column, length inferred), `V N diagonal-matrix` (N×N with V on the diagonal), `N identity-matrix`, `start end step matrix-range` (a 1×N row over a stepped range).
- **DGEMM** — `dgemm-nn`/`tn`/`nt`/`tt` (`αAB + βC`) for all four transpose variants, each with its own loop order chosen so the inner loop runs unit-stride with `restrict` pointers: `nn` and `tn` are ikj axpy kernels, `nt` and `tt` are vectorized dot products (`tt` staging Aᵀ's column through a scratch buffer).
- **Indexing** — `@i`/`@j`/`@i,j` to read rows, columns, or single cells; `@e` reads by flat row-major index (what `argmax`/`where`/`argsort` produce); `!i,j` and `!e` store a single element in place.
- **Shape** — `dim`, `reshape`, `flatten`, `transpose`, `diagonal`, `matrix>array` (the elements as an array in row-major order; a dimensioned matrix yields per-element quantities, NaN becomes `null`).
- **Selection** — `augment`/`hstack` (concatenate two matrices column-wise), `vstack` (row-wise), `submatrix` (copy a half-open row×column block), `select-rows` (gather rows named by a float index array or an index vector; a dataset operand gathers every column by the same indices).
- **Reductions** — `sum`, `row-sums`, `column-sums`, `max`, `min`, `argmax`, `argmin` (flat row-major index of the extreme element), `row-maxes`, `row-mins`, `column-maxes`, `column-mins`, `cumulative-sum` (row-major prefix sums, shape preserved). Library `mean`, `row-means`, `column-means` on top.
- **Norms** — `norm` (Euclidean/L2) and `frobenius-norm`, both √(Σ elements²) over the matrix; `dot` ( v w -- f ) is the inner product.
- **Descriptive statistics** — `var` (sample variance) and `quantile` (linearly interpolated at p ∈ [0,1]) over all elements, and `ks-distance` (the two-sample Kolmogorov–Smirnov statistic); the embedded statistics library layers `std`, `se`, `median`, `percentile`, `iqr`, `ci`, `summary` (on vectors and per-column on datasets), `histogram-table`, `ecdf`, `binomial-deviance`, `cross-validate` (k-fold over caller-defined units), and the `bootstrap` family; the loadable LAPACK library adds `fit-logistic-ridge` and `cv-logistic-ridge`/`pcv-logistic-ridge` (L2 path selection, serial or parallel) on these — all wasm-capable. The statistics skip NaN elements (missing values) and divide by the non-NaN count (`nonmissing-count`); the correlations and regressions use complete cases.
- **Correlations** — `correlation-pearson`, `correlation-spearman` (pearson on `ranks`), `correlation-kendall` (tau-b, O(n log n) C kernel); `correlate-with` bootstraps a 95% CI for any of them, and `cor` is kendall + 500 replicates in one word; `qnorm` is the standard normal quantile.
- **Regression trees** — `fit-tree` grows a CART regression tree over a features frame and a numeric response: numeric columns split at a midpoint threshold, array columns are native categoricals split on a mean-ordered subset, and rows missing a numeric feature follow a per-split default direction learned from the split criterion. It returns the tree as a nested frame — `:prediction` and `:n_rows` at every node, `:feature` with `:threshold` or `:categories` at internal nodes, optional per-leaf `:responses` — and takes a params frame (`:max-depth`, `:min-samples`, `:store-leaf-responses`). `pfit-tree` is the parallel form, growing independent subtrees across cores into a byte-identical tree. `predict` applies a tree to a features frame, walking each row to its leaf (a numeric split sends value ≤ threshold left; a categorical split sends set membership left, an unseen value right). `feature-importance` ranks the features by normalized impurity reduction. `prune` cost-complexity-prunes a fitted tree at a given complexity, and `prune-cv` fits then prunes at the `alpha` chosen by k-fold cross-validation with the 1-SE rule. `draw-tree` prints the tree as indented rules, and `lib/plot.h2o`'s `plot-tree` renders it as an SVG node-link diagram.
- **SVG plotting** (`lib/plot.h2o`) — scatter, line series, histogram, and Tukey boxplots over a deferred-rendering figure: marks accumulate with the style in effect, the domain resolves at render (pinned or auto from the data), ticks land on round {1,2,5}×10ᵏ steps, `x-label`/`y-label` set axis titles, `panel` draws a filled ground with gridlines as negative space, and `show-figure` opens a live-reloading browser view that `save-figure` updates in place.
- **Element-wise math** — `abs`, `sqrt`, `exp`, `log`, `ln`, `sin`, `cos`, `tan`, `tanh`, `asin`, `acos`, `atan`, `round`, `truncate`, `round-up`, `round-down`. Polymorphic over floats and matrices.
- **Comparison** — `=` orders matrices structurally (shape then row-major contents), so matrices work as set members; `lt`/`gt`/`eq` compare matrices **element-wise**, returning a 1/0 matrix (a scalar broadcasts). An array operand also masks element-wise (`val_cmp` per element, a value broadcasts, equal-length arrays pair up), so `names "ann" eq where` filters a text column. On scalars and strings comparison is structural, `eq` agreeing with `=`.
- **Sorting and masks** — `sort` (ascending copy of a vector, NaNs last), `argsort` (the sorting permutation of a vector as an index vector, or of an array under structural order as an index array; ties keep index order), `where` (flat indices of a mask's nonzero elements), `nan?` (the NaN mask — NaNs compare false under `lt`/`gt`/`eq`; an array answers a mask of its `none` elements), `mesh` (masked substitution — keep where the mask is 0 or NaN, replace where it is definitely nonzero; scalars, `null`, and quantities broadcast). Masks serve both selection and alteration: `dup 0 @j 0 lt where select-rows` keeps the rows whose first column is negative, `dup nan? 0 mesh` fills a column's NaNs, `dup -1 eq null mesh` turns a sentinel into missing.

### Dimensioned quantities

A magnitude (float or matrix) carrying a unit; arithmetic propagates and checks units — dimensional algebra, not unit conversion. Units are rational-exponent vectors over user-declared base dimensions, each with a rational scale.

- **`base` / `unit`** — declare dimensions and units. `base unit m`; `1 kg 1 m * 1 s / 1 s / unit newton` (derived); `1 $ 100 / unit ¢` (scaled sub-unit). A unit word is postfix — `10 m`, `3 newton`.
- **Arithmetic** — `*`/`/` combine unit exponents and scales (a dimensionless result collapses back to a bare float/matrix); `+`/`-` require the same dimension and rescale across scales; `^`/`sqrt` scale the exponents; `= < >` compare by value, normalizing scale within a dimension. Named units print by name, unnamed compounds in base form.
- **Statistics keep the unit** — the matrix reductions and statistics accept a dimensioned matrix: `sum`/`mean`/`max`/`min`/`quantile`/`median`/`iqr`/`ci` answer in the operand's unit, `var` in the unit squared (`std`/`se` return through `sqrt`), index/count words and the correlations answer bare; `magnitude` strips a quantity to its payload, `unit-of` answers its unit as the quantity `1` in that unit.
- **Standard set** (`units.h2o`) — SI `m s kg ampere kelvin mol`, derived `hertz newton pascal joule watt coulomb volt`, `minute`/`hour`/`day`/`week`/`km`, and currencies `$`/`¢`, `£`/`penny`, `€`/`eurocent`.
- **Constants** (`constants.h2o`) — capitalized: `PI` `E` `TAU` `PHI`, and the physical set as dimensioned quantities (`C` `G` `H` `HBAR` `KB` `NA` `QE`, SI-2019 exact values) — `C 2 ^ 1 kg *` is E=mc², and prints in joules.

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
- **Array operations** — `sort` (a sorted copy in `val_cmp` order; a set projects to a sorted array, a vector sorts ascending with NaNs last), `reverse`, `take`, `concat`, `flatten-array` (flatten one level), `sample` ( arr count repl -- arr ) drawing elements with or without replacement, `shuffle` (a uniform permutation of the array), `resample` (a same-size draw with replacement — the bootstrap draw), and `first`/`second` (element 0/1 of an array, head/tail of a cons).
- **Growing at the end** — `add-last!` ( arr v -- arr ) appends over a backing buffer that doubles when full, `remove-last!` ( arr -- v ) pops the last element; both amortized O(1), indexing stays O(1).
- **Map, fold, zip-map, filter** — `map` for a single source, `reduce` for a left fold with an accumulator, `mapn` for N-ary zip, `filter` to select by predicate, with anonymous quotations as the higher-order argument.
- **Search, traversal, and reshaping** — `find` (first element satisfying a predicate, or `null`), `any?`/`all?`, `each` (side effects, no result), `flat-map` (per-element arrays concatenated), `sort-by` (sorted by an extracted key, n key evaluations), `partition` (matches and non-matches in one pass), and `group-with` (group into `{ key → set }` by a computed symbol key — the quotation-keyed kin of `group-by`).
- **Destructuring** — `destruct` spreads a set/array/frame's elements onto the stack (a frame as alternating symbol/value). `destruct-to` ( values names -- ) takes two equal-length arrays and assigns each value to the global variable named by the corresponding symbol, creating it if absent.
- **In-place slicing** — `slice!` copies a strided run from one array into another (a negative step with source and target aligned reverses in place), `to-slice!` stores values from the stack into a range.

### Random

A thread-local xoshiro256\*\* stream. Each worker thread derives its own stream from the shared base seed, so parallel draws are deterministic per worker.

- **`seed`** — `( n -- )` set the global base seed and reset the stream.
- **`random`** — `( -- f )` a uniform float in `[0, 1)`; **`random-int`** — `( bound -- f )` a uniform integer in `[0, bound)`.
- `sample` (arrays) and `resample-indices` (datasets) draw on this stream.

### Time and dates

An instant is epoch seconds as a quantity in `s`, so the units machinery is the
date arithmetic: `wall-now 2 hour +` is an instant, instant − instant is a
duration, `… 1 day /` counts days. Unsuffixed words are UTC and pure Gregorian
arithmetic, identical on every platform; `-local` twins use the process
timezone (`TZ` re-read per call).

- **`wall-now`** — `( -- instant )` the absolute wall clock; `now` is the monotonic interval clock.
- **`epoch>date`** / **`date>epoch`** — decompose to / compose from a date frame `{ :year :month :day :hour :minute :second :weekday :yearday }`; composition takes a partial frame (`:year` required, the rest defaulted) and carries out-of-range fields mktime-style (`:month 13` → next January). Plus `-local` variants.
- **`format-time`** / **`parse-time`** — strftime / strptime, with `%z` offsets on parse; **`time>iso`** / **`iso>time`** for the ISO 8601 Z form.
- **`date-shift`** — `( instant delta -- instant )` calendar-aware shifts: `:years`/`:months` step the calendar with the day clamped to the target month, `:weeks` `:days` `:hours` `:minutes` `:seconds` add exact durations; components combine and may be negative. **`days-in-month`** is leap-aware.

### Multi-core parallelism

Worker threads over one shared object heap: a quotation runs across the collection on several cores, results joining back by handle with no copy. Allocation in a region is per-worker; a region whose results don't escape is rewound wholesale, and live results are retained by handle.

- **`pmap`** — `( arr xt -- arr )` parallel `map`; **`pfilter`** — `( arr pred -- arr )` parallel `filter`, order preserved; **`pmap-reduce`** — `( arr id map-xt combine-xt -- val )` fused parallel map+fold, with `combine-xt` associative and `id` its neutral element.
- **`-ext` forms** — `pmap-ext` / `pfilter-ext` / `pmap-reduce-ext` take an explicit worker count and items-per-claim; the bare forms default to `num-cores` workers.
- **`num-cores`** — online CPU count.

### Frames

Symbol-keyed nested maps — the associative type, and the compound term the logic layer builds on. The three bracket families are distinct: `[ ]` arrays, `{ }` frames, `< >` sets. `[ ] { }` and `;` are self-delimiting — `[1 2 3]` and `{:a 1}` parse without inner spaces; `< >` still need theirs.

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
- **`edit-distance`** — `( a b -- n )` edit distance between two strings over codepoints; insertions, deletions, substitutions, and adjacent transpositions each cost one edit.

### JSON

- **`json>frame`** — parse a JSON string into native values: objects → frames (keys interned as symbols), arrays → arrays, strings → strings (escapes and `\uXXXX` decoded to UTF-8), numbers → floats, `true`/`false` → the reserved `:1`/`:0` boolean symbols, `null` → `null` (the none value). Recursive-descent, GC-safe, rejects trailing garbage.
- **`frame>json`** — serialize a value back to a JSON string: floats use a shortest round-trip representation, strings are escaped, `:1`/`:0` → `true`/`false`, none → `null`.

### I/O and persistence

- **Interactive REPL** with full isocline line editing: theme-adaptive **syntax highlighting**, **matching-brace** highlighting, **inline hints** and **Tab completion** (word names from the live dictionary, filenames inside string literals), persistent history (`.water_history`), and **multi-line editing** — `Ctrl+J` inserts a line, `Enter` submits the whole buffer. Each entry answers `ok`, followed by `count|top` (stack depth and the compressed top value) when the stack is non-empty, or the error message and trace on failure. A failed entry leaves the data stack as it was before the entry (the stack is snapshotted per entry and restored on error; in-place mutations of heap objects persist). `.` pretty-prints a nested array across lines with the opening brackets aligned; strings print quoted inside a collection and in `.s`, raw when printed bare.
- **`load`** runs a source file as if typed.
- **`save`** writes the user's vocabulary as a re-loadable `.h2o` source file.
- **`save-image`** / **`load-image`** — binary image with full state preservation (dictionary, objects, stacks, continuations).
- **`reload`** truncates user state and re-runs every file `load`ed this session, in order.
- **`read-file`** / **`write-file`** / **`append-file`** — read a whole file as one (byte-safe) string; write or append a string's bytes to a path.
- **`file-exists?`** — whether a path exists (`access`, `F_OK`); follows symlinks, any file type.
- **`find-executable`** — `( name -- path/none )` the absolute path of `name` on `$PATH`, or the none value if not found.
- **`load-library`** — `"plot" load-library` loads `lib/plot.h2o` from beside the water binary (`binary-dir`, symlinks resolved), from any cwd; the statistics library locates its LAPACK shared library the same way.
- **`env`** / **`env!`** — read an environment variable as a string (the none value if unset) and set one (process-wide, so `start-process` children inherit it).
- **`stdin`** / **`stdout`** / **`stderr`** — the standard streams as `T_STREAM` values (fds 0/1/2), composing with `read`/`write`/`close` — `s stdout write` emits, `stdin read` slurps input.

### Subprocesses and pipes

Drive external programs over pipes (`fork`/`execv`/`pipe`/`waitpid`, with a manual `PATH` search; binary-safe, no shell):

- **`argv start-process`** — launch from an argv array; returns a frame `{ :pid :in :out :err }` with the child's pid and its stdin/stdout/stderr as `T_STREAM` values.
- **`write`** / **`read`** / **`close`** — write a string to a stream, read a stream to EOF, close one (closing `:in` sends EOF).
- **`running?`** / **`wait`** / **`stop`** — non-blocking liveness check, block-until-exit, signal-and-reap.
- `subprocess.h2o` conveniences: **`run`** (split a command line and start it), **`read-out`** / **`read-err`** / **`write-in`**.
- **`commands width parallel-run`** — run a batch of argv arrays concurrently, at most `width` at a time, collecting `{ :out :err :status }` per command in input order (refills a slot as each child finishes). Process-level parallelism — e.g. firing off many `curl` requests at once.

### SQLite

Embedded relational storage via the vendored SQLite amalgamation — built into the binary, no external dependency. A database is a `T_DB` handle.

- **`db-open`** / **`db-close`** — open (creating if absent, or `":memory:"` for an in-memory DB) and push a handle; close frees the connection and is idempotent.
- **`db-exec`** — `( db statement params -- n )` — run an INSERT/UPDATE/DELETE/CREATE with `params` bound to its `?` placeholders; returns the affected-row count (0 for DDL).
- **`db-query`** — `( db query params -- rel )` — run a query; returns a fact-database relation `{ :rows <bag of row frames> :index { } }`, each row keyed by column-name symbols (INTEGER/REAL → float, TEXT → string, NULL → `null`, BLOB → raw bytes). Duplicates are kept, in result order; the result drops straight into `query` / `inner-join`.
- **`db-query>dataset`** — `( db query params -- dataset )` — the same query returned as a column-oriented dataset with typed columns: an all-numeric column arrives as an n×1 vector (NULL → NaN), a declared DATE/DATETIME column as a vector of instants in `s`, text as an array — so column statistics and `dataset>matrix` need no conversion step.
- **`tsv>db`** — `( tsv-path db table -- info )` — import a TSV: header row names the columns, per-column type inference (REAL when every non-empty cell is numeric, else TEXT), empty cells become NULL, one transaction; returns `{ :n-rows :columns }` with each column's name and type, plus a `summary` frame for numeric columns and a distinct count for text.
- **Bound parameters** — `params` is an array bound positionally to the `?` placeholders (`[ ]` for none); floats, strings, symbols, and `null` bind, so string values need no hand-escaping.
- **`create-index`** — `( rel cols -- rel )`, `logic.h2o` — index a query result on `cols`, interning those columns to symbols so the fact-db index and `query` can use them.

### Data: TSV, datasets, and statistics

TSV is the one tabular file format (convert other formats to TSV before loading).

- **`read-tsv`** / **`write-tsv`** — a TSV file with a header row as a column-oriented dataset with typed columns (a uniformly numeric column becomes a vector, empty cells NaN), and a dataset back to a header TSV, one word each.
- **`load-tsv`** / **`save-tsv`** — read a file into an array of row-arrays (a numeric cell becomes a float, an empty cell `none`, everything else a string) and write one back.
- **`rows>dataset`** — `( rows header? -- frame )` a column-oriented frame with typed columns (a uniformly numeric column becomes an n×1 vector, `none` → NaN; anything else stays an array); **`rows>relation`** — `( rows index-cols header? -- relation )` a deduped, indexed fact-database relation; **`dataset>rows`** — `( dataset -- rows )` the inverse of `true rows>dataset` (header row + row-arrays, ready for `save-tsv`); **`dataset>matrix`** — `( dataset cols -- m )` an observations×columns numeric matrix from named columns.
- **Dataset verbs** — `select-rows`, `select-columns`, `filter`, `map`, `dim`, `column-type`, and `count` work on a dataset directly: rows gather by an index array or vector across every column, `select-columns` keeps named columns, `filter` keeps the rows whose frame satisfies a predicate, `map` transforms each row frame to a new one (columns re-infer their representation), `dim` answers rows and columns, `column-type` reads a column's type from its representation (`:numeric` `:datetime` `:quantity` `:text`), and `count` tallies distinct values — or whole rows — most frequent first. `column>array` reads any column as an array of its values; `column>set` is its distinct-value set; `group-indices` maps each distinct value to its row positions (`[ [ value [indices] ] … ]`, one sort instead of a scan per value).
- **`frames>dataset`** — `( rows -- dataset )` an array of row frames (a `query` or `db-query` result, `map`-over-dataset output) as a column-oriented dataset with inferred column representations.
- **`head`** / **`headn`** — `( dataset -- )` / `( dataset n -- )` print the first 10 / n rows as an aligned table: column names as the header, numeric and quantity columns right-aligned, text left, datetime cells as ISO timestamps.
- **`replace-where`** — `( dataset sym pred replacement -- )` conditionally edit one column in place: `pipeline :rep_touches [: -1 eq :] null replace-where` turns a sentinel into missing.
- **`resample-indices`** — `( n -- arr )` n indices drawn from `[0,n)` with replacement, for bootstrap resampling.

The statistics library (`lib/statistics.h2o`, loaded on demand) builds on the matrix and FFI layers:

- **Descriptive** — `std`, `se`, `median`, `percentile`, `iqr`, `ci` (percentile confidence interval).
- **Resampling** — `bootstrap` / `pbootstrap` (parallel) over a fit quotation.
- **Linear algebra** — `svd` and `fit-linear` (least-squares) on LAPACK through the FFI; loading the library also rebinds the `dgemm-*` words to BLAS.
- **Regression** — `linear-regression` and `logistic-regression` (IRLS with Firth correction), each returning per-coefficient estimate, standard error, bias, and confidence interval from a bootstrap.
- **Generalized linear models** — `fit-glm` runs IRLS for a family object of three quotations (`:inverse-link`, `:mean-derivative`, `:variance`); `gamma-log`, `poisson-log`, `gaussian-identity`, and `binomial-logit` are provided, and `fit-gamma`/`fit-poisson` wrap the log-link fits.
- **Gradient boosting** — `fit-xgb` trains an XGBoost booster on a feature matrix and response through the system `libxgboost` (`XGBOOST_LIB`, else the default install path), taking a params frame keyed by XGBoost parameter names (`:rounds` drives the boosting loop); `xgb-predict` scores a feature matrix, `xgb-free` releases the booster. `xgb-importance` returns the per-feature importance (`"gain"`/`"weight"`/`"cover"`/`"total_gain"`/`"total_cover"`) as a k×1 matrix — `matrix>array argsort reverse` ranks the features. The matrix passes zero-copy via a NumPy array-interface handle.

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

Coroutines on the continuation primitives, in `generators.h2o`:

- **`yield`** — emit a value to the driver and suspend until resumed.
- **`start-generator`** — run a producer to its first `yield`, leaving the yielded value and a resumable continuation.
- **`gen-take`** — collect the first N values a producer yields into an array; **`gen-each`** — run a consumer on each yielded value until the producer falls off.

### Side stack

A third stack for stashing arbitrary Vals without disturbing the data or return stack: **`>side`**, **`side>`**, **`side-drop`**, **`side-depth`**.

### Exceptions (library)

Built in `generators.h2o` on top of the continuation primitives:

- **`throw`** — non-local exit with a value; uncaught, it is an interpreter error naming the value (`uncaught exception: "boom"`) with a trace from the throw site.
- **`catch`** — wraps an xt; returns `(result 0)` on success, `(exc 1)` on a throw. It also intercepts **interpreter errors** — division by zero, out-of-bounds, type mismatch, and the like — delivering a `{ :message :trace }` frame (the trace names the failing word innermost-first) as the exception value, so a runtime fault is recoverable, not just a user `throw`. A `throw`n value passes through raw.
- **`try-catch`** — wraps an xt with a recovery handler that runs on either kind of failure. Arity-agnostic.
- **`ensure`** — `( body-xt cleanup-xt -- … )` runs cleanup on both the normal and the throw/error path, then re-raises on throw. **`with-db`** / **`with-stream`** build on it to open (or take) a resource, run a body with it, and release it however the body exits.

An uncaught `throw` or interpreter error still surfaces at the REPL. The `shift-with` handler can also resume the captured continuation, giving the Common Lisp restart pattern — exceptions can recover.

An uncaught error also prints a backtrace under the message: the call chain read
off the return stack, innermost first — `in inner ← mid ← outer`. A quotation
frame prints as its source snippet (`in [: 1 0 % :]`, long ones truncated),
same-site recursion collapses to one frame (`in spin ×65536`), and deep chains
elide the middle (`… ← …+3 ← …`). A caught error prints none. The trace costs
nothing until an error happens — capture is a return-stack walk at failure
time — and quotation spans ride along in `save-image`, so frames resolve across
a `load-image`.

An unknown word names the nearest dictionary word or in-scope local when one is
within edit distance 2 — `unknown word: filtr (did you mean filter?)`. Distance
ties break toward the more-used word (every compiled token counts toward its
word's frequency, so the embedded library seeds the counts at startup), then
toward the longer shared prefix.

### Logic

Unification and committed choice, on the trail and the continuation machinery:

- **Logic variables** — `lvar` makes a fresh one; `lvar to x` names a persistent global, and a `?` prefix in a locals list (`| ?x |`) declares a fresh per-call variable inside a definition or quotation. Capitalizing logic-var names is convention, not syntax.
- **`unify`** (`~`) — unifies two terms, binding logic vars through a trail so they match: atoms by value, arrays element-wise, frames as open records (shared keys must unify, extras allowed); on a mismatch it fails. **`deref`** (`?`) follows a variable's binding chain.
- **`amb`** / **`fail`** — committed choice: run the first branch; if it fails (a `unify` mismatch or an explicit `fail`), roll its bindings back through the trail and run the second, committing to whichever succeeds. **`choose`** generalizes it to a cons list, running a continuation with each element until one succeeds.
- **`_`** — the anonymous wildcard: unifies with anything, binds nothing, and allocates nothing.
- **`matches?`** — a non-destructive `unify` test: marks the trail, unifies, rolls back, and pushes whether the two unified — so it composes in straight-line code.
- **Cons lists** — `[( a b c )]` builds cons pairs and `[( H T )]` is the `[H|T]` head/tail pattern under `unify`; with `cons`, `head-tail`, and `array`↔`cons` conversions.
- **Fact database** — `relation` / `assert` / `query` / `retract` / `count-matches` / `inner-join`. A relation is a frame of a row-set plus per-column indexes (declared symbol columns); rows are column-keyed frames that dedup; `query` matches a pattern by unification, narrowing through the index (and returning the bucket directly when the index covers the whole pattern). `inner-join` merges two relations on a shared column via index probing; `bulk-load` builds a whole relation in one sorted pass (`array>set` for the rows, `group-by` per index). The same row-frame shape is what a SQLite query would return.

### Other

- **`dup`**, **`drop`**, **`swap`**, **`over`**, **`rot`**, **`depth`**, **`roll`**, **`clear`** — stack-manipulation primitives.
- **`it`** / **`other`** / **`them`** — anaphora: push the top of the stack (`it`), the value under it (`other`), or both in order (`them`) as they stood when the current scope began — the line at top level, the word's activation in a colon definition. Pinned per scope, non-consuming, repeatable (`it it +`); in definitions they compile to hidden entry-bound locals, so `: f 2 * it + ;` still sees the argument after consuming it. `this`/`that` alias `it`/`other`.
- **`copy`** / **`reify`** — deep copy of a value (strings, arrays, sets, frames, matrices); `reify` additionally renames unbound logic vars to canonical `:_0`/`:_1`/… for a ground, storable, comparable snapshot.
- **`type-of`** — `( a -- sym )` the value's type as a symbol (`:float`, `:frame`, `:lvar`, …), with a lib predicate per type (`float?` … `lvar?`); a bound logic var answers as its value.
- **`now`** — monotonic seconds as a float, for timing intervals (`wall-now`, under Time and dates, is the absolute clock). **`timed`** — `( xt -- … )` runs xt, prints its elapsed `now` seconds, and passes its results through.
- **`see`** — prints a word's source definition; **`see-compiled`** disassembles its threaded body.
- **`man`** — `( xt -- fr )`, returns a frame of a word's reference entry (stack effect, one-line summary, cost notes). **`help name`** prints it for the named word.
- **`words`** — the dictionary grouped by reference section (session-defined words first, alphabetical, aligned columns); **`apropos`** — `( s -- )` every word whose name or reference summary matches, with stack effect and summary.
- **`variables`** — `( -- arr )` the current globals as `{ :name :value :type }` frames, oldest first: `variables [: :name @ :] map` lists the names, `variables frames>dataset head` prints a table; **`vars`** pretty-prints them.
- **`forget`**, **`bye`**, **`gc`**, **`clear`**, **`.s`**, **`.a`** — interpreter utilities.

## Future work

See `PLAN.md`.

## Project layout

```
src/c/water.h          — types, global program structs (Vocabulary/Arena/Compiler), per-run Interpreter, prototypes
src/c/core.c           — engine: interpreter, dictionary, symbol table, GC, arena, value printing, tokenizer/reader, see, text save
src/c/words.c          — arithmetic, stack ops, printing words, delimited continuations, format, math, RNG
src/c/time.c           — clocks and calendar: wall-now, epoch↔date, strftime/strptime
src/c/compiler.c       — compile-time words: colon/quotation definition, control flow, locals, to/constant/variable/symbol, forget
src/c/io.c             — file, TSV, stream, and environment I/O
src/c/image.c          — binary save-image / load-image serialization
src/c/collections.c    — sets, arrays, and frames
src/c/indexing.c       — polymorphic element access: @i/!i and their fused forms, over arrays/segments/matrices
src/c/matrix.c         — matrix words and numeric kernels
src/c/dimension.c      — dimensioned quantities: base dimensions, units, quantity arithmetic
src/c/functional.c     — higher-order operations (map, mapn, …) and multi-core parallelism
src/c/superwords.c     — compile-time instruction fusion (superwords)
src/c/strings.c        — string and PCRE2 regex operations
src/c/logic.c          — logic variables, unification, amb, fact database
src/c/database.c       — SQLite integration
src/c/foreign.c        — FFI (libffi), pointer registry, matrix/segment bridges
src/c/platform_posix.c — POSIX platform: arena mmap, isocline REPL, subprocesses
src/c/platform_wasi.c  — WASI platform: allocator + erroring stubs for FFI/subprocess
src/c/help_table.c     — generated help/man text (from docs/reference.md)
src/forth/*.h2o        — standard library (concatenated in Makefile order, embedded)
lib/                   — loadable libraries: statistics.h2o, files.h2o, claude.h2o
external/              — vendored deps: pcre2, sqlite, isocline, lapacke
tests/                 — golden-output test files
bench/                 — benchmark suite (Water vs CPython) and inventory
docs/                  — design documents and the word reference
examples/              — sample programs
PLAN.md                — future work
```

## License

See `LICENSE`.
