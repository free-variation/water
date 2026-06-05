# Benchmark baseline — post polymorphic arithmetic shortcuts

Snapshot after making `1+`/`1-`/`sq` polymorphic (float + element-wise matrix)
with `f1+`/`f1-`/`fsq` as the monomorphic fast counterparts, migrating the
numeric benchmarks onto the f-prefixed ops, and removing the redundant
`scalar+`/`scalar-`/`scalar*`/`scalar/` library words (the `+ - * /`
primitives broadcast scalar↔matrix directly).

## Environment

- **Hardware**: Apple M4 (arm64), Darwin 25.5.0
- **Compiler**: `clang -O3 -march=native -Wall -Wextra`
- **Python**: Homebrew CPython 3.14.5 (`python3.14`), no numpy
- **Date**: 2026-06-04

## State of the interpreter

- Direct-threaded dispatch with `musttail` chaining.
- NaN-boxed values; `truthy`/`make_bool` and all `make_*` constructors inline.
- Combinator fast-call (`call_open`/`call_invoke`/`call_close`) shared by
  `map`/`mapn`/`filter`/`reduce`/`times`/`i-times`.
- Polymorphic math (`+ - * / ^ abs sqrt sin … 1+ 1- sq`) dispatches on tag
  for float vs element-wise matrix; the `f`-prefixed forms (`f+ fsq f1+ …`)
  are monomorphic, depth-guarded, no tag check.
- Matrix reductions (`sum`/`max`/`min`/`row-*`/`column-*`) are SIMD-vectorized:
  the reduce kernels enable FP reassociation via `float_control(precise, off)`,
  so the 4-accumulator reduction compiles to lane-parallel partial sums.
- In-place elementwise matrix arithmetic (`+! -! *! /!`) reuses the matrix
  operand's buffer instead of allocating; the programmer asserts the operand is
  a dead temporary.
- Superinstructions in `src/c/superwords.c`, generated from four operator
  lists via X-macros:
  - two variables + float op (`vvf+ vvf- vvf* vvf/`),
  - one variable + stack op (`vf+ vf- vf* vf/`),
  - one variable, unary function (`vfsq vfneg vfsqrt vfsin vfcos vftan vftanh vfexp vflog vfabs`),
  - fused multiply with two variables (`vvf*- vvf*+`).
- Compile-time auto-fusion: idiomatic `x y f-`, `x fsq`, `mag vx1 f*-` are
  rewritten to the superwords at emit time (lookback in `run_outer`). Fusion
  keys on the monomorphic handlers, so hot float code uses the `f`-prefixed ops.
- Store fusion: `<superword> to <var>` folds into a store variant that writes
  the variable directly, eliminating the intermediate push and `(to-var)`.
- Local-accumulate fusion: `<value> acc <fop> to acc` folds into one
  read-modify-write op on the local accumulator (both depth-0 and across a
  quotation), covering the reduction pattern in `reduce`-shaped loops.
- `begin … while … repeat` test-at-top loop, alongside `begin … until`.
- Logical primitives `and`/`or`/`not`; in-place `reverse-slice!` (subrange reverse).
- `see-compiled` decompiles a word's body to show the fused form.
- Tests: 93/93 passing.

## synth.l4 — logicforth vs CPython 3.14

| phase | logicforth | python 3.14 | py / lf |
|:-----:|-----------:|------------:|--------:|
|   1   |   143 ms   |   183 ms    | 1.29×   |
|   2   |   118 ms   |   446 ms    | 3.78×   |
|   3   |   0.58 ms  |    21 ms    | ~37×    |
|   4   |    46 ms   |   116 ms    | 2.51×   |
|   5   |   143 ms   |   707 ms    | 4.96×   |
|   6   |   121 ms   |   459 ms    | 3.80×   |
| total |   570 ms   |  1932 ms    | 3.39×   |

## Standalone benchmarks — logicforth vs CPython 3.14

Ports of the pyperformance benchmarks at matched problem sizes, verified to
produce identical results. logicforth is the median of repeated runs; Python
is the median of 3–5 runs. nbody uses array storage with `destruct-to` field
access. leibniz has no pyperformance port; its Python figure is the reference
`leibniz.py` from niklas-heer/speed-comparison run under python3.14.
leibniz-matrix is logicforth's vectorized variant of the same computation
(build the denominator sequence as a 1×N matrix, in-place broadcast-divide with
`/!`, vectorized sum); it is compared against the same scalar Python reference
at the same 1e9 size. It also beats R's vectorized `sum(4 / seq.int(...))`,
which is the direct analogue — R 4.5.2 runs that in 1.58 s at 7.5 GB peak;
leibniz-matrix matches the footprint (7.45 GB, one array via `/!`) and is ~2×
faster, with identical exact IEEE division.

| benchmark      | size            | logicforth | python 3.14 | py / lf |
|:---------------|:----------------|-----------:|------------:|--------:|
| leibniz        | 1e9 iterations  |   17.23 s  |   40.02 s   | 2.32×   |
| leibniz-matrix | 1e9, vectorized |    0.77 s  |   40.02 s   | ~52×    |
| nqueens        | N = 8           |   0.0331 s |   0.0423 s  | 1.28×   |
| nbody          | 20_000 steps    |   0.0476 s |   0.0499 s  | 1.05×   |
| fannkuch       | N = 9           |   0.1845 s |   0.1890 s  | 1.02×   |
| spectral-norm  | N = 130, 50×    |   2.36 s   |   2.566 s   | 1.09×   |
| scimark-lu     | N = 100, 100×   |    2.08 s  |    5.67 s   | 2.73×   |

logicforth leads every standalone benchmark. fannkuch and spectral-norm crossed
this session: fannkuch once its pre-test loops moved to `begin … while … repeat`
(dropping the sentinel-flag idiom), and spectral-norm once `eval_A` — its
~17M-call hot spot — was marked `inline`, then local-accumulate fusion folded its
`acc f+ to acc` reduction into a single op (~1.03× → ~1.09×). scimark-lu is a
direct translation of pyperformance's dense LU kernel (Gaussian elimination with
partial pivoting, array-of-rows, scalar inner loops); both sides factor the
identical RNG-filled matrix to the same checksum, and logicforth's faster per-op
dispatch carries it to ~2.7× with no matrix machinery involved.

## Reproduce

`make bench` runs the whole comparison and emits this report; the steps below
are the individual commands it wraps.

```bash
make clean && make && make test
./logicforth < bench/synth.l4 | grep -E "^phase"
python3.14 bench/synth.py | grep -E "^phase"

./logicforth < bench/leibniz.l4 | grep -i elapsed
./logicforth < bench/leibniz-matrix.l4 | grep -i elapsed
./logicforth < bench/nqueens.l4 | grep -i elapsed
./logicforth < bench/fannkuch.l4 | grep -i elapsed
{ echo "variable ITERATIONS 20000 to ITERATIONS"; cat bench/nbody.l4; } \
  | ./logicforth | grep -i elapsed
{ echo "variable ITERATIONS 50 to ITERATIONS"; cat bench/spectral-norm.l4; } \
  | ./logicforth | grep -i elapsed
{ echo "variable ITERATIONS 100 to ITERATIONS"; cat bench/scimark-lu.l4; } \
  | ./logicforth | grep -i elapsed

python3.14 bench/pyperf_nbody.py 20000
python3.14 bench/pyperf_nqueens.py 8
python3.14 bench/pyperf_fannkuch.py 9
python3.14 bench/pyperf_spectral_norm.py 50
python3.14 bench/pyperf_scimark_lu.py 100

# leibniz Python reference (reads rounds.txt in the cwd):
curl -fsSL https://raw.githubusercontent.com/niklas-heer/speed-comparison/master/src/leibniz.py -o leibniz_ref.py
echo 1000000000 > rounds.txt
time python3.14 leibniz_ref.py
```

Run each 3–5× and take medians for the sub-second figures. Any change to a
synth checksum or a benchmark result is a correctness regression and
supersedes any speed gain.
