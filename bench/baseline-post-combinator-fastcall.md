# Benchmark baseline — post combinator fast-call

Snapshot taken after the combinator fast-call change: `map`, `mapn`,
`filter`, `reduce`, `times`, and `i-times` now set up the inner-interpreter
trampoline once per call and reuse it across every iteration, instead of
saving and restoring it per element through `execute_cfa`. Single-shot
callers (`update-at`, `execute`) still use `execute_cfa`.

## Environment

- **Hardware**: Apple M4 (arm64), Darwin 25.5.0
- **Compiler**: `clang -O3 -march=native -Wall -Wextra`
- **Python**: Homebrew CPython 3.14.5 (`python3.14`), no numpy
- **Date**: 2026-06-04

## State of the interpreter

- Direct-threaded dispatch with `musttail` chaining.
- NaN-boxed values.
- Depth-0 local fast paths (`(local@0)` / `(local!0)` and the
  increment/decrement variants).
- Float shortcut primitives (`sq`, `1+`, `1-`, `f*+`, `f*-`).
- Inline binary-search frame lookup.
- Combinator fast-call (`call_open` / `call_invoke` / `call_close`) shared
  by all loop combinators.
- Tests: 79/79 passing.

## synth.l4 — logicforth

| phase | elapsed  | checksum            |
|:-----:|---------:|:--------------------|
|   1   |  144 ms  | `3.33333e+20`       |
|   2   |  121 ms  | `4.16667e+19`       |
|   3   |  0.14 ms | `25005833500000`    |
|   4   |   45 ms  | `4.16668e+16`       |
|   5   |  143 ms  | `3.33333e+20`       |
|   6   |  110 ms  | `300001500000`      |
| total |  563 ms  |                     |

Bench iteration constants:

```
ITER1     = 10_000_000   (phase 1: tight times loop)
ITER2     =  5_000_000   (phase 2: range → map → filter → reduce)
MAT_DIM   =        100   (phase 3: DGEMM 100×100)
MAT_ELEMS =     10_000
ITER4     =    500_000   (phase 4: 500k flat frames)
ITER5     = 10_000_000   (phase 5: map + reduce)
ITER6     =    200_000   (phase 6: deep nested frames)
```

## synth.py — pure CPython 3.14.5 (no numpy)

Medians of three runs.

| phase  | elapsed  |
|:------:|---------:|
|    1   |  183 ms  |
|    2   |  450 ms  |
|    3   |   21 ms  |
|    4   |  116 ms  |
|    5   |  702 ms  |
|    6   |  465 ms  |
| total  | 1937 ms  |

## Comparison (synth)

| phase | logicforth | python   | py / lf |
|:-----:|-----------:|---------:|--------:|
|   1   |   144 ms   |  183 ms  | 1.27×   |
|   2   |   121 ms   |  450 ms  | 3.72×   |
|   3   |   0.14 ms  |   21 ms  | ~150×   |
|   4   |    45 ms   |  116 ms  | 2.58×   |
|   5   |   143 ms   |  702 ms  | 4.91×   |
|   6   |   110 ms   |  465 ms  | 4.23×   |
| total |   563 ms   | 1937 ms  | 3.44×   |

## Standalone benchmarks — logicforth vs CPython 3.14.5

Ports of the pyperformance benchmarks, run at matched problem sizes
against `bench/pyperf_*.py`, verified to produce identical results
(e.g. nbody final energy −0.169089 on both). logicforth is the median
of repeated runs; Python is a single run (a few seconds or more each).
`nbody` uses `bench/nbody-destruct-to.l4` (array storage with
`destruct-to` field access, the fastest of the nbody variants); it and
`spectral-norm` read an `ITERATIONS` variable supplied by prepending a
definition. leibniz has no pyperformance port; its Python figure is the
reference loop from the benchmark's own header comment.

| benchmark      | size           | logicforth | python 3.14 | py / lf |
|:---------------|:---------------|-----------:|------------:|--------:|
| leibniz        | 1e9 iterations |   38.97 s  |    80.24 s  | 2.06×   |
| nqueens        | N = 8          |   0.042 s  |    0.041 s  | 0.97×   |
| nbody          | 20_000 steps   |   0.071 s  |    0.048 s  | 0.68×   |
| fannkuch       | N = 9          |   0.254 s  |    0.187 s  | 0.74×   |
| spectral-norm  | N = 130, 50×   |   3.01 s   |    2.57 s   | 0.85×   |

The synth comparison (3.44× overall) does not carry over to these
algorithmic ports. synth is built from logicforth's fast paths — tight
`times` counters and `map`/`reduce` over flat arrays — where the
interpreter is strongest. The pyperformance ports stress n-body force
calculation (`nbody`), array permutation and reversal (`fannkuch`), and
nested float reductions (`spectral-norm`); on those CPython 3.14's
optimized bytecode interpreter is faster than logicforth, by 1.2-1.5×.
logicforth wins only `leibniz` (a tight scalar-float loop, ~2×) and
ties `nqueens`.

## Effect of the combinator fast-call

Measured by toggling the change off and rebuilding:

| workload                  | before   | after    | Δ      |
|:--------------------------|---------:|---------:|-------:|
| synth phase 1 (`times`)   |  181 ms  |  144 ms  | −20%   |
| synth phase 2             |  139 ms  |  121 ms  | −13%   |
| synth phase 5 (map+reduce)|  172 ms  |  143 ms  | −17%   |
| leibniz                   | 42.87 s  | 38.97 s  | −9.1%  |
| spectral-norm (50)        |  3.03 s  |  2.92 s  | −3.5%  |
| nbody (20k)               | 0.116 s  | 0.114 s  | −1.7%  |
| nqueens                   | 0.042 s  | 0.041 s  | −2.2%  |
| fannkuch                  | 0.247 s  | 0.246 s  | −0.3%  |

The gain concentrates in tight loops with trivial bodies, where
per-call dispatch dominates. Loops that do substantial work per
iteration (nbody frame updates, spectral-norm float math) see the
trampoline as a small fraction of total time, and `begin/until` loops
(nqueens, fannkuch) never route through the combinator path at all.

## Reproduce

```bash
make clean && make && make test
./logicforth < bench/synth.l4 | grep -E "^phase"
python3.14 bench/synth.py | grep -E "^phase"

# standalone logicforth (ITERATIONS-driven ones need a prepended definition):
./logicforth < bench/leibniz.l4 | grep -i elapsed
{ echo "variable ITERATIONS 20000 to ITERATIONS"; cat bench/nbody-destruct-to.l4; } \
  | ./logicforth | grep -i elapsed
{ echo "variable ITERATIONS 50 to ITERATIONS"; cat bench/spectral-norm.l4; } \
  | ./logicforth | grep -i elapsed
./logicforth < bench/nqueens.l4 | grep -i elapsed
./logicforth < bench/fannkuch.l4 | grep -i elapsed

# standalone CPython 3.14, matched sizes:
python3.14 bench/pyperf_nbody.py 20000
python3.14 bench/pyperf_nqueens.py 8
python3.14 bench/pyperf_fannkuch.py 9
python3.14 bench/pyperf_spectral_norm.py 50
```

Run each 3-5× and take medians for the sub-second figures. Checksums
must remain identical to the table above; any change to a checksum is a
correctness regression and supersedes any speed gain.
