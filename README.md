# <img src="telic_logo.png" alt="" height="40" align="top"> Telic

A Forth-styled language for numeric, statistical, and symbolic work, and for
general scripting: matrices and linear algebra, statistics and regression,
dimensioned quantities and calendar arithmetic, sets/arrays/frames and columnar
datasets, strings and regex, subprocesses and pipes, logic programming with
backtracking, and multi-core data parallelism — with embedded SQLite, a
runtime C FFI, an SVG plotting library, and an MCP server. A compact, self-contained C
interpreter: NaN-boxed tagged values, direct-threaded code with compile-time
fusion, mark-and-sweep GC, and a WASI build from the same source.

The language is designed to be LLM-friendly from the start. 
`make pack` concatenates the whole documentation set — the reference, the library reference, and the idioms — into one file sized to fit a single LLM prompt.

Dedicated to Chuck Peddle and Tony Wilkinson.

## Building and running

```
make           # builds ./telic
make test      # runs the golden-output test suite
make bench     # runs the benchmark suite (Telic vs CPython)
./telic              # REPL
./telic prog.telic     # run program files and exit (repeatable, in order; -i to drop into the REPL after)
./telic -e '3 4 + .' # run a code string and exit (repeatable, in argument order with files; implies -b)
```

Self-contained: its vendored dependencies — PCRE2 (regex), isocline (REPL line
editing), and SQLite (embedded SQL) — live under `external/` and are built from
source into the binary, so `make` needs only a C compiler and the system
`libffi`. Refresh them with `make vendor-pcre2`, `make vendor-sqlite`, and
`make vendor-isocline` (see each directory's `PROVENANCE`).

`make` also builds `liblapacke_telic.so`, a thin shared library that wraps
the platform BLAS/LAPACK (Accelerate on macOS, OpenBLAS on Linux) behind
the LAPACKE C interface. The statistics library `dlopen`s it through the
FFI and requires it — the stats module is native-only; the wasm build
excludes the FFI. Re-vendor with `make vendor-lapacke`.

```
make wasm        # cross-builds telic.wasm (needs wasi-sdk in ~/wasi-sdk, or set WASI_SDK)
make test-wasm   # runs the golden suite against telic.wasm under wasmtime
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

\ Exact rationals — and they take units (money)
1/3 1/6 + .                             \ 1/2
1/2 $ 50/1 ¢ + .                        \ 1 $

\ Matrices: * is element-wise; matrix multiply is matmul
[ 1 2 3 4 ] 2 2 matrix dup transpose *  \ element-wise product of M and Mᵀ

\ Dimensioned quantities: units propagate, combine, and collapse
10 m 2 s / .                            \ 5 m.s^-1
1 kg 1 m * 1 s / 1 s / .                \ 1 newton   (interns to the named unit)

\ Complex numbers — and they take units (phasors)
-1+0i sqrt .                            \ 0+1i
3+4i volt 1-1i volt + dup . abs .       \ 4+3i volt 5 volt

\ Dates: instants are quantities in s, so units do the date arithmetic
wall-now 2 week + time>iso .            \ the ISO timestamp two weeks from now
"2026-01-31T09:00:00Z" iso>time { :months 1 } date-shift time>iso .   \ clamps to Feb 28

\ Sets and set algebra
[< 1 2 3 >] [< 2 3 4 >] + .                 \ [< 1 2 3 4 >]  (union via polymorphic +)

\ Set-builder { x² | x ∈ 1..10, even x } — literal + filter/map + spread
[< 1 10 range [: 2 mod 0= :] filter ' fsq map spread >] .   \ [< 4 16 36 64 100 >]

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

\ format fills {n} with the stack entry n deep from the top ({0} = top, {1} under
\ it), with a printf-style {n:spec}; {tab} and {nl} emit the control characters
1.5 250 "{0:d} ms{tab}{1:04.1f} s" format .   \ 250 ms	01.5 s
"{bold}{red}alert{plain} ok" format .         \ colored on a tty; the ink escapes vanish when piped

\ Exceptions
[: "missing" throw :]
[: "got " . . cr :] try-catch           \ prints "got missing"

\ Generators — coroutines on the delimited-continuation primitives
: primes 2 yield 3 yield 5 yield 7 yield ;
' primes 4 gen-take .                   \ [ 2 3 5 7 ]

\ Subprocesses over pipes
"echo hi" run read-out .                \ hi

\ Logic: unify binds variables; amb keeps the first branch that succeeds
lvar to X  lvar to Y  lvar to Z
[ 1 2 3 ] [ X Y Z ] ~ drop  X ? . Y ? . Z ? . cr   \ 1 2 3
[: fail :] [: "fallback" :] amb .                  \ fallback

\ Multi-core: run a quotation across the array on every core
[ 1 2 3 4 5 6 7 8 ] [: dup * :] pmap .  \ [ 1 4 9 16 25 36 49 64 ]

\ Datasets: column-oriented tables with verbs
[ [ "name" "age" ] [ "ann" 34 ] [ "bo" 25 ] [ "cy" 61 ] ] true rows>dataset
dup :age @ mean .                       \ 40  (a numeric column is already a vector)
[: :age @ 30 > :] filter :name @ .     \ [ "ann" "cy" ]

\ Count distinct values, most frequent first; masks alter as well as select
[ :b :a :b :c :b ] count first .        \ [ :b 3 ]
[ 1 2 999 4 ] vector dup 999 eq null mesh matrix>array .   \ [ 1 2 null 4 ]

\ Statistics over a matrix column: mean and the median (0.5 quantile)
[ 2 4 4 4 5 5 7 9 ] 8 1 matrix dup mean . 0.5 quantile .  \ 5  4.5

\ A fit over a real table: income on three columns of 32,561 rows. The statistics
\ library reaches LAPACK through the FFI, so this part is native-only.
"statistics" load-library
"data/adult.tsv" read-tsv to adult
adult [ :age :education-num :hours-per-week ] dataset>matrix with-intercept
adult@income  50 1e-8 1  fit-logistic-ridge transpose .
\ 1x4: -8.523  0.04691  0.3453  0.04283   (intercept, age, education, hours)

\ SQLite, in-memory: create, insert a bound param, query back
":memory:" db-open
dup "create table t(x)" [ ] db-exec drop
dup "insert into t values (?)" [ 42 ] db-exec drop
"select x from t" [ ] db-query :x @ 0 @e .   \ 42
```

## Benchmarks

`make bench` runs `bench/run-benchmarks.sh`, which builds the binary, runs each
port five times against three CPython runs, and reports medians with a
verification table pairing every result against its reference. The `-matrix`
rows are vectorized and answer to numpy, the `-parallel` rows to a process pool
of the same width, and both time pool creation as telic times spawning its
threads. `mandelbrot-matrix` and `spectral-norm-matrix` take their matrix
multiply from the platform BLAS through the statistics library, so those two
rows put Accelerate against numpy rather than telic's own kernels. Refresh the
table below from a report with
`python3 tools/update-readme-bench.py <report.md>`; never edit a cell by hand.

<!-- bench:begin -->
Medians of 5 telic reps against 3 CPython reps (2026-08-21): Apple M4 Max, 16 cores (12P + 4E), Mac16,5, 128 GB memory, Darwin 25.5.0, `clang -O3 -march=native -Wall -Wextra`, CPython 3.14.6, numpy 2.5.1.

| benchmark | size | telic | python | py / telic |
|:----------|:-----|-----------:|-------:|--------:|
| leibniz | 1000000000 iterations | 8.373 s | 42.258 s | 5.05× |
| leibniz-matrix | 1000000000, vectorized vs numpy | 0.7167 s | 1.786 s | 2.49× |
| leibniz-matrix | 1000000000, vectorized vs R 4.5.2 `sum(4 / seq.int(...))` | 0.7167 s | 1.720 s | 2.40× |
| leibniz-parallel | 1000000000, pmap vs pool of 16 | 1.221 s | 3.335 s | 2.73× |
| nqueens | N = 8 ×45 | 0.5167 s | 1.841 s | 3.56× |
| nqueens-iter | N = 8 ×45 | 1.115 s | 1.841 s | 1.65× |
| nbody | 500000 steps | 0.5178 s | 1.236 s | 2.39× |
| raytrace | 40× 100×100 | 0.5713 s | 5.350 s | 9.37× |
| raytrace-parallel | 420× 100×100, pmap vs pool | 0.5149 s | 5.170 s | ~10× |
| float | 100000 pts × 60 | 0.7669 s | 1.895 s | 2.47× |
| crypto-pyaes | 23000 B, 70× enc+dec | 0.5119 s | 2.673 s | 5.22× |
| fannkuch | N = 9 ×6 | 0.6113 s | 1.099 s | 1.80× |
| binary-trees | depth 16 ×2 | 0.6387 s | 1.397 s | 2.19× |
| mandelbrot | N = 1000 ×2 | 0.6956 s | 2.628 s | 3.78× |
| mandelbrot-matrix | N = 1000 ×7, vectorized vs numpy | 0.5685 s | 0.6907 s | 1.21× |
| mandelbrot-parallel | N = 1000 ×16, pmap vs numpy pool | 0.5438 s | 2.604 s | 4.79× |
| spectral-norm | N = 130, 50× | 0.6281 s | 2.573 s | 4.10× |
| spectral-norm-matrix | N = 260, 7000× vs numpy | 0.6062 s | 0.5687 s | 0.94× |
| scimark-lu | N=100, 200× | 0.8276 s | 11.383 s | ~14× |
| scimark-sparse | N=1000, 1000× | 0.5684 s | 2.196 s | 3.86× |
| scimark-fft | N=1024, 5×150 | 0.5529 s | 2.130 s | 3.85× |
| barnes-hut | 200 bodies, 6×50 | 0.5211 s | 1.373 s | 2.64× |
| scimark-sor | N=100, 10 cyc × 200 | 0.6641 s | 11.446 s | ~17× |
| scimark-montecarlo | 1000000 × 6 | 0.7160 s | 1.883 s | 2.63× |
| montecarlo-parallel | 20000000 samples × 13, pmap 10w vs pool 10w | 0.6124 s | 2.600 s | 4.25× |
| meteor | 30 solves | 0.5609 s | 1.615 s | 2.88× |
| hexiom | level 25, 250 solves | 0.5553 s | 0.8110 s | 1.46× |
| regex-dna | 100K → 1M ×17 | 0.5799 s | 0.0003 s | 0.00× |
| regex-compile | 239 patterns, cold | 0.0010 s | 0.0071 s | 7.24× |
| regex-effbot | 21 pat × 0..10k | 2.721 s | 15.767 s | 5.79× |
| regex-v8 | 12 blocks ×200, browser trace | 0.7663 s | 2.143 s | 2.80× |
| deepcopy | N=100000, 60 copies/N | 0.5783 s | 11.436 s | ~20× |
| json-loads | 222k parses | 0.5321 s | 0.9706 s | 1.82× |
| json-dumps | EMPTY/SIMPLE/NESTED/HUGE ×500 | 0.7272 s | 2.521 s | 3.47× |
<!-- bench:end -->

The ports live in `bench/pyperformance/` beside the CPython sources they answer
to, and `bench/variants/` holds the vectorized and parallel forms. Where a port
departs from its pyperformance original the file's header says so.

## Features

### Core language

- **Tagged Vals** — `null`, floats, strings, symbols, sets, arrays, frames, matrices, quantities, segments, execution tokens, curried tokens, dictionary addresses, continuations, logic variables and the unbound/wildcard sentinel, process streams, database handles, C pointers, internal marks. A single 8-byte NaN-boxed representation; the tag determines interpretation.
- **Direct-threaded inner interpreter** — each dictionary cell is a handler function pointer, dispatched by an indirect tail call (`musttail`); a colon call, literal, or branch carries its operand in the cell(s) right after the handler. The dictionary *is* the threaded code.
- **Compile-time instruction fusion** — a float op collapses with its operands and its store into one instruction, whether they are globals (`vvf+ a b`), locals (`zr zr f* to zr2`), a literal, or a stack slot read by depth (`2 pick f+`), so a quotation reading values parked below a combinator's operands costs the same as one reading locals. Also fused: `f*+` / `f*-` multiply-add, a comparison before a branch (`= if`, `> while`), an array read-modify-write (`arr i arr i @i f1- !i`), and `++ name` / `f++ name`. `see-compiled` shows the fused ops.
- **Program image and execution state separated** — the dictionary, symbol pool, and object heap are global (`Vocabulary`, `Compiler`, `Arena`); the three stacks, instruction pointer, locals, and GC roots live in a per-run `Interpreter`. Several execution contexts share one image, which is how the parallel words give each worker its own stacks over the shared heap — and why a worker xt must not mutate shared inputs or print.
- **Three stacks** — data, return, and a side stack for values that mustn't sit on either: `>side`, `side>`, `side-drop`, `side-peek`, `side-depth`.
- **Colon definitions** — `: name body ;`. The body is captured as source text for `see` and the text-form `save`.
- **Anonymous quotations** — `[: ... :]` pushes a fresh xt. Works at top level and inside colon defs.
- **`recurse`** — compiles a call to the innermost definition being compiled (the enclosing quotation, else the colon word), so an anonymous quotation can self-call.
- **Tail-call elimination** — a call in tail position compiles to a frame-reusing jump, so a self-recursive or `recurse` loop runs in constant return-stack space. Disabled where it would be unsafe (a body using `>r`/`reset`/`shift`/`fail`, or locals plus a quotation).
- **Partial application** — `curry` ( value xt -- xt' ) binds a value into a curried token, a heap value accepted wherever an xt is; the token travels through other words' frames intact, works inside parallel regions, and is garbage-collected.
- **Control flow** — `if`/`else`/`then`, the `begin`/`until`/`again` and `begin`/`while`/`repeat` loops with `leave` / `continue` for early exit, the counted `start limit delta do k … loop` over a named index local, `case`/`of`/`endof`/`endcase` dispatching by unification (clauses are patterns — ground values, open-record frames, `_`, logic vars that bind for the body), counted `times` / `i-times`, `exit`, and `>r`/`r>`/`r@` for return-stack access.
- **Delimited continuations** — four primitives, the substrate for generators, exceptions, and restarts (`docs/continuations.md`): `reset` installs a delimiter, `shift` captures the slice up to it, `shift-with` captures and then runs a handler in the outer context, `resume` re-enters a captured continuation, multi-shot.
- **Tick and execute** — `' word execute` for first-class invocation by name.
- **Forward declaration** — `defer name` declares a word with no target, for mutual recursion or late binding; `embodies` installs one and stays retargetable, `embodies!` finalizes it and rewrites the existing call sites to call direct.
- **`forget`** — truncate the dictionary back to a named word; symbol identities survive.
- **Variables and symbols** — `variable foo` declares a global, read by bare name and assigned with `42 to foo`; at top level `to` creates the global on first assignment. `symbol bar` defines a symbol, `:foo` is a symbol literal, `string>symbol` interns a computed string.
- **Word-local variables** — a head at the start of a colon definition or quotation names what the body receives from the stack, rightmost from the top: `| x y |`, or `x y |` with the opening bar left off. Everything else is declared where `to` first assigns it, and the compiler collects those names to the head, so a name means one thing throughout the body. In the head, `^name` is an enclosing global the body assigns and `?name` a fresh logic variable per call. `++ name` / `-- name` increment/decrement in place (`f++` / `f--` the unsafe float-only forms).
- **A quotation captures enclosing locals by copy** — naming a local of the enclosing body inside `[: … :]` binds a copy of it into the quotation, which then evaluates to a curried token; a quotation naming none stays a constant xt. Values also reach it through its own head, `pick` from the stack below the combinator's operands, or `curry`.
- **Mark-and-sweep GC** — walks the three stacks, the C-level roots, and the dictionary (each global's value cell and every compiled literal). Triggers on object-table and live-byte pressure, at a safepoint between words.

### Generators

Coroutines on the continuation primitives, in `generators.telic`:

- **`yield`** — emit a value to the driver and suspend until resumed.
- **`start-generator`** — run a producer to its first `yield`, leaving the yielded value and a resumable continuation.
- **`gen-take`** — collect the first N values a producer yields into an array; **`gen-each`** — run a consumer on each yielded value until the producer is exhausted.

### Exceptions (library)

Built in `exceptions.telic` on top of the continuation primitives:

- **`throw`** — non-local exit with a value.
- **`catch`** — wraps an xt, answering `(result 0)` or `(exc 1)`. Interpreter errors are caught too — division by zero, out of bounds, a type mismatch — arriving as a `{ :message :trace }` frame.
- **`try-catch`** — run an xt, and on either kind of failure run a handler with the exception.
- **`ensure`** — cleanup on both the normal and the failing path; **`with-db`** / **`with-stream`** scope a resource that way.

An uncaught error prints its message with a backtrace of the call chain,
innermost first (`in inner ← mid ← outer`), and an unknown word suggests the
nearest name in scope (`unknown word: filtr (did you mean filter?)`). A
`shift-with` handler can resume the captured continuation, giving the Common
Lisp restart pattern.

### Logic

A logic variable stands for a value not yet known. `unify` makes two terms equal
by binding the variables inside them, and `amb` tries alternatives, undoing
those bindings when one fails — on the same delimited-continuation substrate as
exceptions.

- **Variables** — `lvar` pushes a fresh one; `| ?x |` in a locals head declares one that is fresh on every call. `?` reads a variable, answering what it is bound to.
- **`unify`** (`~`) — makes two terms equal, binding variables on either side: values by comparison, arrays element-wise, with a trailing `rest` pattern taking an array's remaining elements, frames as open records where shared keys must agree and extra keys are ignored. A mismatch fails. `_` matches anything and binds nothing.
- **Search** — `amb` runs the first of two quotations; if it fails — a mismatch, or an explicit `fail` — its bindings are undone and the second runs. `choose` does the same across an array. The first branch that succeeds is the one kept; `solutions` and `take-solutions` collect every success instead of the first.
- **Tests** — `matches?` answers whether two terms could unify and leaves nothing bound; `unify?` keeps the bindings when they do. `case`/`of` dispatches through `unify?`, so a clause pattern may hold variables that bind for its body.
- **Destructuring** — `[ H T rest ]` under `unify` is Prolog's `[H|T]`: `H` takes the first element, `T` the rest as an array; `[ _ rest ]` matches any array.
- **Pattern queries over datasets** — `query` keeps the rows whose columns equal a pattern frame's ground values (logic variables and `_` constrain nothing), and `query-rows` answers them as frames for binding the pattern's variables row by row under `~`, `matches?` or `choose`. Joins, grouping and loading are the dataset words (`merge-by`, `aggregate`, `rows>dataset`, `db-query`).

### Numeric / matrix

- **Polymorphic arithmetic** — `+`/`-`/`*`/`/` dispatch on operand tags: floats compute, strings concatenate (`+`), sets union/difference/intersection, matrices element-wise, and a scalar broadcasts over a matrix.
- **Integer division** — `%` truncating divmod, with `mod` (sign follows the dividend) and `quotient` (toward zero) on top; all three broadcast element-wise like the arithmetic words.
- **`min2`** / **`max2`** — pairwise minimum and maximum, element-wise with scalar broadcast.
- **In-place matrix ops** — `+!`/`-!`/`*!`/`/!` mutate the left matrix in place. Float-only fast paths (`f+`, `f-`, `f*`, `f/`, `f^`, …) skip the type dispatch when both operands are known floats.
- **Matrix construction** — `R C 0-matrix` (zeros), `[ ... ] R C matrix`, `[ ... ] vector` (an n×1 column, length inferred), `V N diagonal-matrix` (N×N with V on the diagonal), `N identity-matrix`, `start end step matrix-range` (a 1×N row over a stepped range).
- **`matmul`** — the matrix product (`*` is element-wise); the statistics library adds a size dispatch, keeping the built-in loop for small operands and handing larger ones to the platform BLAS.
- **Indexing** — `@i`/`@j`/`@i,j` to read rows, columns, or single cells; `@e` reads by flat row-major index (what `argmax`/`where`/`argsort` produce); `!i,j` and `!e` store a single element in place.
- **Shape** — `dim`, `reshape`, `flatten`, `transpose`, `diagonal`, `matrix>array` (the elements as an array in row-major order; a dimensioned matrix yields per-element quantities, NaN becomes `null`).
- **Selection** — `augment`/`hstack` (concatenate two matrices column-wise), `vstack` (row-wise), `submatrix` (copy a half-open row×column block), `select-rows` (gather rows named by a float index array or an index vector; a dataset operand gathers every column by the same indices).
- **Reductions** — `sum`, `row-sums`, `column-sums`, `max`, `min`, `argmax`, `argmin` (flat row-major index of the extreme element), `row-maxes`, `row-mins`, `column-maxes`, `column-mins`, `cumulative-sum` (row-major prefix sums, shape preserved). Library `mean`, `row-means`, `column-means` on top.
- **Norms** — `norm` and `frobenius-norm`, both √(Σ elements²); `dot` is the inner product.
- **Descriptive statistics** — `var`, `quantile`, and `ks-distance` in C, with `std`, `se`, `median`, `percentile`, `quantiles`, `iqr`, `ci`, `summary`, `histogram-table`, `ecdf`, `binomial-deviance`, `cross-validate`, and the `bootstrap` family in the embedded library — LAPACK-free, so wasm-capable. NaN elements are missing values: the statistics skip them, and the correlations and regressions use complete cases.
- **Correlations** — `covariance`, `correlation-pearson`, `correlation-spearman`, `correlation-kendall` (tau-b); `correlate-with` bootstraps a confidence interval for any of them, `cor` does it with kendall in one word. `qnorm` and `pnorm` are the standard normal quantile and CDF, `random-normal` a standard normal deviate.
- **SVG plotting** (`lib/plot.telic`) — scatter, line series, histograms, bar charts, and Tukey boxplots over a deferred-rendering figure: marks accumulate with the style in effect and nothing maps to pixels until render, so draw order is free and the domain may be set after the data. `save-figure` writes a version, `show-figure` opens a browser view that later versions appear in.
- **Element-wise math** — `abs`, `sqrt`, `exp`, `log`, `ln`, `ln1+`, `log2`, `sin`, `cos`, `tan`, `sinh`, `cosh`, `tanh`, `asin`, `acos`, `atan`, `atan2`, `erf`, `erfc`, `round`, `truncate`, `round-up`, `round-down`. Polymorphic over floats and matrices.
- **Comparison** — `=` is structural, so matrices work as set members; `<`/`>`/`eq` on a matrix or array operand mask **element-wise** into a 1/0 matrix, so `names "ann" eq where` filters a text column. On scalars and strings all of them are structural.
- **Sorting and masks** — `sort` (an ascending copy, NaNs last), `argsort` (the sorting permutation), `where` (the flat indices of a mask's nonzero elements), `nan?` (the missing-value mask), and `mesh` (masked substitution). Masks serve selection and alteration alike: `dup 0 @j 0 < where select-rows` keeps the rows whose first column is negative, `dup nan? 0 mesh` fills a column's NaNs, `dup -1 eq null mesh` turns a sentinel into missing.

### Exact rationals

Fractions of arbitrarily large integers, always reduced; an integer is the case
with denominator 1. `1/3` is a literal, and an integer literal, JSON integer, or
SQLite INTEGER too large for a float reads as an exact rather than rounding
silently. The arithmetic, comparison, and rounding words compute exactly;
comparison crosses exact and float, arithmetic between them errors, and
`float>exact` / `exact>float` convert. A quantity's magnitude may be an exact,
so currency arithmetic is exact (`1/2 $ 50/1 ¢ +` is `1 $`). `rationalize`
answers the simplest fraction that reads back as the same float. Matrices and
the ⚠ words take only floats.

### Complex numbers

A pair of floats with literals `3+4i` / `4i`. `+ - * / ^` take two complexes or a complex and a float (floats promote losslessly); `sqrt`, `exp`, `ln`, and the trigonometric words answer principal values; `negate` keeps the type, `abs` answers the modulus; `complex` / `real-part` / `imaginary-part` / `conjugate` / `arg` construct and destructure. Ordering is by real then imaginary part, and a complex equals a float of its value. A complex takes a unit (`3+4i ohm`), so impedance arithmetic is quantity arithmetic.

### Dimensioned quantities

A magnitude (float, matrix, exact, or complex) carrying a unit; arithmetic propagates and checks units, rescaling same-dimension operands. Units are rational-exponent vectors over user-declared base dimensions, each with a rational scale.

- **`base` / `unit`** — declare dimensions and units. `base unit m`; `1 kg 1 m * 1 s / 1 s / unit newton` (derived); `1 $ 100 / unit ¢` (scaled sub-unit). A unit word is postfix — `10 m`, `3 newton`.
- **Arithmetic** — `*`/`/` combine unit exponents and scales (a dimensionless result collapses back to a bare float/matrix); `+`/`-` require the same dimension and rescale across scales; `^`/`sqrt` scale the exponents; `= < >` compare by value, normalizing scale within a dimension. Named units print by name, unnamed compounds in base form.
- **Statistics keep the unit** — the matrix reductions and statistics accept a dimensioned matrix: `sum`/`mean`/`max`/`min`/`quantile`/`median`/`iqr`/`ci` answer in the operand's unit, `var` in the unit squared (`std`/`se` return through `sqrt`), index/count words and the correlations answer bare; `magnitude` strips a quantity to its payload, `unit-of` answers its unit as the quantity `1` in that unit.
- **Standard set** (`units.telic`) — SI `m s kg ampere kelvin mol`, derived `hertz newton pascal joule watt coulomb volt`, `minute`/`hour`/`day`/`week`/`km`, and currencies `$`/`¢`, `£`/`penny`, `€`/`eurocent`.
- **Constants** (`constants.telic`) — capitalized: `PI` `E` `TAU` `PHI`, and the physical set as dimensioned quantities (`C` `G` `H` `HBAR` `KB` `NA` `QE`, SI-2019 exact values) — `C 2 ^ 1 kg *` is E=mc², and prints in joules.

### Bitwise

Integer bitwise operators over the float representation: a value is read as a two's-complement integer (exact within the double's 53-bit range), the operation runs, and the result is pushed back as a float — byte- and bit-level work such as block ciphers, codecs, and bit-stream packing.

- **`bit-and`** / **`bit-or`** / **`bit-xor`** / **`bit-not`** — bitwise logic, named apart from the truthiness words `and`/`or`/`not`.
- **`lshift`** / **`rshift`** — left shift and arithmetic right shift (`= floor(a / 2ⁿ)`); **`lowest-bit`** — 0-indexed position of the lowest set bit (−1 when zero).
- **Hex literals** — `0xff` reads as 255; `{0:x}` under `format` renders hex back out.

### Random

A thread-local xoshiro256\*\* stream. Each worker thread derives its own stream from the shared base seed, so parallel draws are deterministic per worker.

- **`seed`** sets the base seed; **`random`** draws a uniform float in `[0, 1)` and **`random-int`** a uniform integer below a bound.
- `sample` and `resample-indices` draw on this stream.

### Strings and regex

- **String literals** are raw, `""` being the one escape; **`format`** fills `{n}` placeholders from the stack — `"got {0} of {1}" format` — and on a terminal colors text with ink directives (`{red}`…`{plain}`) that vanish when piped; `+` concatenates.
- **Regex** on PCRE2, JIT-compiled: `match`, `match-all`, `replace` (all matches, with `&` and `\1`–`\9` backrefs), and `has?` to test one. Patterns are plain `"..."` literals, read by PCRE2 itself.
- **Slicing and building** — `substring`, `char-at`, `split` on a pattern, `join` with a separator, `trim`, `upper-case` / `lower-case` (ASCII).
- **Unicode** — strings are UTF-8 and the bare words work in *codepoints*: `size`, `substring`, `char-at`, `codepoint-at`, with `byte-size` and `byte-substring` for the raw layer and for regex byte offsets. `string>chars` / `string>codepoints` decompose, `codepoint>char` / `codepoints>string` rebuild, `emit` encodes one. Regex is Unicode-aware, and invalid bytes fail to match rather than erroring.
- **`edit-distance`** — over codepoints, counting insertions, deletions, substitutions, and adjacent transpositions.

### Sets, arrays, higher-order

- **Set literals** — `[< 1 2 3 >]`, set operations, `has?` (membership), `in?` (an element-wise membership mask of an array or vector), `size`, in-place `set-add!`/`set-remove!`, and `array>set` (sort-and-dedup an array into a set in one pass).
- **`group-by`** — `array :col group-by` groups frames by a symbol field into a frame from each value to a set of rows; an execution token key computes each element's group symbol instead.
- **Array literals** — `[ 1 2 3 ]`, `array` to gather N from the stack, `array-of` to fill, `range` and `iota` for integer sequences, `@i` and `!i` to read and store by index.
- **Array operations** — `sort`, `reverse`, `take`, `concat`, `flatten`, `sample` (with or without replacement), `shuffle`, `resample` (the bootstrap draw), and `first`/`second`.
- **Growing at the end** — `add-last!` and `remove-last!` over a doubling buffer, both amortized O(1) with indexing still O(1).
- **Map, fold, zip-map, filter** — `map` for a single source, `reduce` for a left fold over a collection, `nmap` for N-ary zip, `filter` to select by predicate, with anonymous quotations as the higher-order argument.
- **Counted map-fold** — `fold-times` folds over an index range with no collection, the accumulator staying off the data stack; `sum-times` and `product-times` are the common defaults and `pmap-reduce` the parallel form.
- **Search, traversal, and reshaping** — `find-first` (short-circuits), `any?` / `all?`, `each` for side effects, `flat-map`, `sort-by` on an extracted key, and `partition`.
- **Destructuring** — `spread` pushes a set/array/frame's elements onto the stack (a frame as alternating symbol/value); a locals head, `unify`, or a `case` pattern receives the pieces by name.
- **In-place slicing** — `slice!` copies a strided run from one array into another (a negative step with source and target aligned reverses in place), `to-slice!` stores values from the stack into a range.

### Frames

Symbol-keyed nested maps — the associative type, and the compound term the logic layer builds on. The three bracket families are distinct: `[ ]` arrays, `{ }` frames, `[< >]` sets. `[ ] { }` and `;` are self-delimiting — `[1 2 3]` and `{:a 1}` parse without inner spaces; `[< >]` still need theirs.

- **Literals** — `{ :a 1 :b 2 }`; values may be any Val, including nested frames, arrays, and sets.
- **Builders** — `frame` from parallel key and value collections, `array>frame` from an alternating key/value array, `frame>array` back again.
- **Path literals** — `/a/b/c` is a symbol array `[ :a :b :c ]`, built once at compile time, used to address into the tree — and usable as a key when constructing a frame (`{ /a/b/c v }` / `array>frame`), where it vivifies nested frames. A path may also be a *search* pattern: `*` matches any child at that level, `//` matches at any depth (descendant-or-self), and `[…]` filters by predicate (`[city=:NYC]`, `[age>30]`, `[.>0]` on the node itself, `[addr/zip]` on a sub-path).
- **Access** — `@` gets, `!` sets and vivifies intermediates, `has?` tests, `delete-at` removes, `update-at` applies a quotation to a leaf, `merge` combines two frames, and `keys` / `values` / `size` report. All but `has?` take a single key or locator, not a search pattern.
- **Key tokens** — `row@price` joins a frame reference to a key in one token, the left part being a local or a word that supplies the frame. Gets chain — `row@address@city` — `row!price` sets, and an empty left part takes the frame from the stack, so `@price` is the postfix form. A defined word always wins, so `@i` and `@or` keep their meanings.
- **Path queries** — `select-values` returns every value a search pattern matches, in document order; `select-keys` returns the path to each match.
- **Representation** — parallel key/value arrays in **symbol-id order**, which is interning order rather than alphabetical, so `keys`, `values`, `spread` and printing are stable for a program but not name-sorted. Mutable in place, reference semantics, structurally comparable.

### Segments

A flat, fixed-length `int` buffer stored off the arena (one allocation, freed by GC), for the `int *` arguments and out-parameters of C libraries; dense double storage is a matrix.

- **`int-segment`** — `( n -- seg )` an n-element zero-filled buffer; `@i` reads an element as a float and `!i` writes one truncated to an int, sharing the array indexing words.
- **`segment>pointer`** — intern the backing buffer as a `T_PTR` for an FFI `:ptr` argument, no copy.

### Time and dates

An instant is epoch seconds as a quantity in `s`, so the units machinery is the
date arithmetic: `wall-now 2 hour +` is an instant, instant − instant is a
duration, `… 1 day /` counts days. Unsuffixed words are UTC and pure Gregorian
arithmetic, identical on every platform; `-local` twins use the process
timezone (`TZ` re-read per call).

- **`wall-now`** — the absolute wall clock; `now` is the monotonic interval clock.
- **`epoch>date`** / **`date>epoch`** — an instant to and from a date frame `{ :year :month :day :hour :minute :second :weekday :yearday }`, composition accepting a partial frame and carrying out-of-range fields mktime-style. Plus `-local` variants.
- **`format-time`** / **`parse-time`** — strftime and strptime; **`time>iso`** / **`iso>time`** for the ISO 8601 Z form.
- **`date-shift`** — calendar shifts by `:years` `:months` `:weeks` `:days` `:hours` `:minutes` `:seconds`, the day clamped to the target month. **`days-in-month`** is leap-aware.

### Multi-core parallelism

Worker threads over one shared object heap: a quotation runs across the collection on several cores, results joining back by handle with no copy. Allocation inside a region is per-worker.

- **`pmap`**, **`pfilter`** (order preserved), and **`pmap-reduce`**, whose combiner must be associative.
- **`-ext` forms** take an explicit worker count and items-per-claim; the bare forms use `num-cores` workers.

### JSON

- **`json>frame`** / **`frame>json`** — parse and serialize: objects ↔ frames with interned symbol keys, arrays ↔ arrays, strings ↔ strings, numbers ↔ floats, `true`/`false` ↔ the reserved `:1`/`:0` symbols, `null` ↔ `null`. An integer too large for a float reads as an exact and writes back without loss.

### Value serialization

- **`value>bytes`** / **`bytes>value`** — a whole value graph as bytes and back, sharing and cycles preserved; compiled code and OS handles are refused by type. **`save-value`** / **`load-value`** do the same through a file.

### I/O and persistence

- **`load`** runs a source file as if typed.
- **`save`** writes the user's vocabulary as a re-loadable `.telic` source file.
- **`reload`** truncates user state and re-runs every file `load`ed this session, in order.
- **`read-file`** / **`write-file`** / **`append-file`** — read a whole file as one (byte-safe) string; write or append a string's bytes to a path.
- **`open-file`** — a file as a read-only stream, for the stream words below.
- **`args`** — the command-line arguments after the program file, as a string array; a leading `#!` line is skipped.
- **`file-exists?`** — whether a path exists (`access`, `F_OK`); follows symlinks, any file type.
- **Files and directories** — **`list-directory`**, **`file-info`**, **`make-directory`**, **`delete-file`**, **`delete-directory`**, **`rename-file`**, **`copy-file`**, **`touch-file`**, with **`ls`** **`mkdir`** **`rm`** **`rmdir`** **`mv`** **`pwd`** **`cat`** **`cp`** **`touch`** as shell names.
- **`find-executable`** — `( name -- path/null )` the absolute path of `name` on `$PATH`, or `null` if not found.
- **`load-library`** — `"plot" load-library` loads `lib/plot.telic` from beside the telic binary (`binary-dir`, symlinks resolved), from any cwd; the statistics library locates its LAPACK shared library the same way.
- **`env`** / **`env!`** — read an environment variable as a string (`null` if unset) and set one (process-wide, so `start-process` children inherit it).
- **`stdin`** / **`stdout`** / **`stderr`** — the standard streams as `T_STREAM` values (fds 0/1/2), composing with `read`/`write`/`close` — `s stdout write` emits, `stdin read` reads input whole.

### Subprocesses and pipes

Drive external programs over pipes (`fork`/`execv`/`pipe`/`waitpid`, with a manual `PATH` search; binary-safe, no shell):

- **`start-process`** — launch from an argv array, answering `{ :pid :in :out :err }` with the three streams.
- **`write`** / **`read`** / **`read-line`** / **`read-available`** / **`wait-readable`** / **`close`** — stream I/O, blocking or not.
- **`running?`** / **`wait`** / **`stop`** — liveness, block-until-exit, signal-and-reap.
- `subprocess.telic` conveniences: **`run`**, **`read-out`**, **`read-err`**, **`write-in`**, **`run-result`**, **`end-process`**.
- **`parallel-run`** — a batch of commands at a bounded width, collecting each one's output and status in input order.

### SQLite

Embedded relational storage via the vendored SQLite amalgamation — built into the binary, no external dependency. A database is a `T_DB` handle.

- **`db-open`** / **`db-close`** — open a file, or `":memory:"`, and close it.
- **`db-exec`** — run a statement with no result set, answering the affected-row count.
- **`db-query`** — run a query, answering the result as a dataset with typed columns.
- **`tsv>db`** — import a TSV into a new table, inferring each column's type.
- **Bound parameters** — every query and statement takes an array bound to its `?` placeholders, so values need no hand-escaping.

### Data: TSV, datasets, and statistics

TSV is the one tabular file format (convert other formats to TSV before loading).

- **`read-tsv`** / **`write-tsv`** — a header TSV to a column-oriented dataset with typed columns, and back.
- **`load-tsv`** / **`save-tsv`** — the same file as an array of row-arrays, untyped and in file column order.
- **Conversions** — **`rows>dataset`** types the columns of an array of row-arrays, **`dataset>rows`** inverts it, and **`dataset>matrix`** builds an observations×columns matrix from named columns.
- **Dataset verbs** — `select-rows`, `select-columns`, `sort-rows`, `filter`, `map`, `dim`, `column-type`, and `count` work on a dataset directly, `filter` and `map` seeing each row as a frame keyed by column name and every column keeping its representation. `column>array` reads any column as an array, `column>set` its distinct values, `column-type` its type (`:numeric` `:datetime` `:quantity` `:text`), and `group-indices` maps each distinct value to its row positions in one sort.
- **`frames>dataset`** — an array of row frames, as `query-rows` answers, into a dataset with inferred column types.
- **`aggregate`** — split-apply-combine: group rows by a column, reduce each group to a row frame, reassemble as a dataset.
- **`head`** / **`headn`** — print the first rows as an aligned table, `headn` taking the row count and the columns to lead with.
- **`replace-where!`** — edit one column in place where a predicate holds.
- **`resample-indices`** — indices drawn with replacement, for bootstrap resampling.

The statistics library (`lib/statistics.telic`, loaded on demand) builds on the matrix and FFI layers:

- **Descriptive** — `std`, `se`, `median`, `percentile`, `quantiles`, `iqr`, `ci` (percentile confidence interval).
- **Resampling** — `bootstrap` / `pbootstrap` (parallel) over a fit quotation.
- **Linear algebra** — matrix multiply (`dgemm-nn`/`tn`/`nt`/`tt`) and matrix-vector products (`dgemv-n` / `dgemv-t`) on the platform BLAS, `svd` and `fit-linear` on LAPACK, all through the FFI.
- **Design matrices** — `column>indicators` expands a categorical column to 0/1 level columns, `indicators!` adds them to a design dataset under `sym=level` keys, `expand-indicators!` replaces a design's categorical column with them in place, and `with-intercept` prepends the ones column.
- **Regression** — `linear-regression`, `logistic-regression` (Firth-penalized IRLS), and `glm-regression` (any GLM family, with prior weights), each answering a model frame of per-coefficient estimates and confidence intervals — Wald at 0 replications, bootstrap above — with the predictor names and the complete-case data it fitted; `regression-report` prints a model as fit statistics (deviance, null deviance, pseudo-R², dispersion) and a coefficient table with odds or rate ratios; `predict-glm` applies a model to a dataset. `fit-logistic-ridge` is the L2-penalized fit, with `cv-logistic-ridge` / `pcv-logistic-ridge` choosing `lambda` by cross-validation.
- **Generalized linear models** — `fit-glm` takes a family as three quotations and a dispersion mode, with `gaussian-identity`, `poisson-log`, `gamma-log`, `binomial-logit`, `quasibinomial-logit`, and `negative-binomial-log` provided and `fit-poisson` / `fit-gamma` wrapping the log-link fits; `fit-glm-weighted` takes prior weights, `glm-dispersion` answers φ (Pearson for quasi families), and `glm-covariance` the coefficient covariance. `fit-negative-binomial` estimates the dispersion alongside the coefficients; `fit-multinomial`, `fit-multinomial-ridge`, and `predict-multinomial` handle several classes.
- **Gradient boosting** — `fit-xgb` trains an XGBoost booster on a feature matrix and response through the system `libxgboost`, taking a params frame keyed by XGBoost parameter names; `xgb-predict` scores, `xgb-importance` ranks the features, `xgb-free` releases the booster, and `xgb-save`/`xgb-load` use XGBoost's own model format, readable by Python and R.

### Foreign function interface

Call C functions in any shared library at runtime via `libffi` — no per-library glue. An opaque C pointer is a `T_PTR` handle (a registry index, since a 64-bit pointer doesn't fit a Val).

- **`ffi-open`** — `dlopen` a library; `""` opens the running process for already-linked symbols.
- **`ffi-function`** — resolve a symbol and define a word that calls it. Types are symbols: `:void :int :long :double :ptr :string`. **`ffi-variadic`** does the same for a variadic function, with the variadic types fixed per binding.
- **`ffi-free`** — `free` a C buffer held as a `T_PTR`.
- **`matrix>pointer`** / **`segment>pointer`** — pass a matrix's or segment's buffer to a `:ptr` parameter, no copy.
- FFI is unsafe: a wrong signature corrupts or crashes; argument *count* is checked, types are the caller's responsibility. `lib/statistics.telic` drives LAPACK's `dgesvd`/`dgelsd` this way, and `ffi-open` on `libcurl` makes an HTTPS request in-process without a subprocess.

### HTTP

`lib/http.telic` — `http-get`, `http-post`, and the general `http-request`
(`{ :status :body }`) over a `curl` subprocess; `lib/claude.telic` calls the
Anthropic API through it.

### MCP server

`lib/mcp.telic` serves the Model Context Protocol over stdio — `telic -e '"mcp"
load-library mcp-serve'` — at revision 2026-07-28. Two tools: `telic-eval` runs
source in a named session, a child interpreter that keeps its definitions, data,
database handles and fitted models between calls, and `telic-help` answers a
word's reference entry. Sessions compute concurrently. For remote access, put
the stdio server behind a stdio-to-Streamable-HTTP gateway such as mcp-proxy.

### Other

- **`dup`**, **`drop`**, **`swap`**, **`over`**, **`nip`**, **`rot`**, **`depth`**, **`pick`**, **`roll`**, **`clear`** — stack-manipulation primitives; `pick` copies the nth item and `roll` moves it, both counting from the top.
- **`copy`** / **`reify`** — deep copy of a value (strings, arrays, sets, frames, matrices); `reify` additionally renames unbound logic vars to canonical `:_0`/`:_1`/… for a ground, storable, comparable snapshot.
- **`type-of`** — `( a -- sym )` the value's type as a symbol (`:float`, `:frame`, `:lvar`, …), with a lib predicate per type (`float?` … `lvar?`); a bound logic var answers as its value.
- **`now`** — monotonic seconds as a float, for timing intervals (`wall-now`, under Time and dates, is the absolute clock).
- **`halt`** — stop the program with an exit status.

## The REPL

- **Line editing** on isocline: theme-adaptive syntax highlighting, matching-brace highlighting, inline hints, Tab completion (dictionary words, filenames inside string literals), persistent history, and multi-line editing with auto-indent.
- **Each entry answers** `ok` with the stack depth and top value, or the error message and its trace with file and line; a failed entry leaves the data stack as it was. Ctrl-C while an entry runs aborts it with `interrupted` and returns to the prompt.
- **`help name`** prints a word's reference entry — stack effect, summary, cost line, examples as a transcript — and for a word defined in a loaded file, the `( a b -- c ) \ summary` comment above its definition; bare `help` prints a cheat sheet. **`man`** answers the same entry as a frame; **`apropos`** finds words by name or summary; **`words`** lists the dictionary by reference section.
- **`see`** prints a word's source; **`see-compiled`** disassembles its threaded body; **`see-tree`** expands the calls inside it down to primitives; **`callers`** names every definition that calls a word, reads a variable, or holds the word as a literal.
- **`edit name`** opens the word's source in `$EDITOR` (`vi` when unset); saving redefines the word from the edited text, quitting without a change leaves it; a name not yet defined starts as an empty definition.
- **`trace`** runs a quotation printing each op with the stack before it, filtered by an array of regexes — the first selects ops by name, the rest match the whole line (`docs/tracing.md`).
- **`gauges`** answers the interpreter's resource readings — dictionary, heap and collection headroom, stacks, open resources, the computer — as a frame with units; **`print-gauges`** prints the readings that move during a run as a table with rates against the previous call; `lib/gauges-view.telic` publishes them after every entry (`publish-gauges`) for a `gauges-view` redrawing the table in another terminal.
- **`after-entry`** — a deferred word the REPL runs after every entry's report; `' xt embodies after-entry` installs a hook. **`tick-every`** arms a periodic timer whose ticks run the deferred word **`on-tick`** between ops of running code.
- **`timed`** runs a quotation and prints its elapsed seconds; **`,`** prints the top of the stack without consuming it; **`.s`** shows the whole stack, **`.a`** prints a value in full precision without truncation; **`alloc-stats`** prints and resets the allocation counters since its last call.
- **`log`** — `( str level -- )` writes one stamped line, `<time> <level> <message>`, to stderr, or under `TELIC_LOG_DIR` to a per-run file named at the first write and recorded in `TELIC_LOG_FILE`.
- **`vars`** prints the current globals, **`variables`** answers them as frames; **`forget`** truncates the dictionary back to a word; **`reload`** re-runs every file `load`ed this session; **`save`** writes the session's definitions as source.
- **Shell names** at the prompt: **`ls`**, **`pwd`**, **`cat`**, **`mkdir`**, **`rm`**, **`mv`**, **`cp`**, **`touch`**.
- **`bye`** quits; **`clear`** empties the data stack; **`gc`** collects now.

## Future work

See `PLAN.md`.

## Project layout

```
src/c/telic.h          — types, global program structs (Vocabulary/Arena/Compiler), per-run Interpreter, prototypes
src/c/core.c           — engine: interpreter, dictionary, symbol table, GC, arena, value printing, tokenizer/reader, see, text save
src/c/words.c          — arithmetic, stack ops, printing words, delimited continuations, format, math, RNG
src/c/time.c           — clocks and calendar: wall-now, epoch↔date, strftime/strptime
src/c/compiler.c       — compile-time words: colon/quotation definition, control flow, locals, to/constant/variable/symbol, forget
src/c/io.c             — file, TSV, stream, and environment I/O
src/c/collections.c    — sets, arrays, and frames
src/c/indexing.c       — polymorphic element access: @i/!i and their fused forms, over arrays/segments/matrices
src/c/matrix.c         — matrix words and numeric kernels
src/c/statistics.c     — statistics kernels: var, quantile, kendall's tau-b
src/c/dimension.c      — dimensioned quantities: base dimensions, units, quantity arithmetic
src/c/functional.c     — higher-order operations (map, nmap, …) and multi-core parallelism
src/c/superwords.c     — compile-time instruction fusion (superwords)
src/c/strings.c        — string and PCRE2 regex operations
src/c/logic.c          — logic variables, unification, amb
src/c/database.c       — SQLite integration
src/c/foreign.c        — FFI (libffi), pointer registry, matrix/segment bridges
src/c/platform_posix.c — POSIX platform: arena mmap, isocline REPL, subprocesses
src/c/platform_wasi.c  — WASI platform: allocator + erroring stubs for FFI/subprocess
src/c/help_table.c     — generated help/man text (from docs/reference.md)
src/forth/*.telic        — standard library (concatenated in Makefile order, embedded)
lib/                   — loadable libraries: statistics.telic, plot.telic, http.telic, claude.telic, mcp.telic
external/              — vendored deps: pcre2, sqlite, isocline, lapacke
tests/                 — golden-output test files
bench/                 — benchmark suite (Telic vs CPython) and inventory
docs/                  — the word reference (reference.md, reference-libraries.md), idioms.md,
                         and the primers: continuations, logic, tracing
PLAN.md                — future work
```

## License

See `LICENSE`.
