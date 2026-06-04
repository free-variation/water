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

## Standalone benchmarks — logicforth

Single run each. `nbody` and `spectral-norm` read an `ITERATIONS`
variable, supplied here by prepending a definition.

| benchmark              | iterations | hot-loop style                  | elapsed   |
|:-----------------------|-----------:|:--------------------------------|----------:|
| leibniz                | 1e9 `i-times` | pure tight combinator loop   | 38.97 s   |
| spectral-norm          | 50         | `each` / `times` + float work   |  2.92 s   |
| nbody                  | 20_000     | `i-times` + frame updates       | 0.114 s   |
| nbody-array            | 20_000     | `each` / `i-times` + arrays     | 0.110 s   |
| nqueens                | built-in   | mostly `begin/until`            | 0.041 s   |
| fannkuch               | built-in   | mostly `begin/until`            | 0.246 s   |

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

# standalone (ITERATIONS-driven ones need a prepended definition):
./logicforth < bench/leibniz.l4 | grep -i elapsed
{ echo "variable ITERATIONS 20000 to ITERATIONS"; cat bench/nbody.l4; } \
  | ./logicforth | grep -i elapsed
{ echo "variable ITERATIONS 50 to ITERATIONS"; cat bench/spectral-norm.l4; } \
  | ./logicforth | grep -i elapsed
```

Run each 3-5× and take medians for the sub-second figures. Checksums
must remain identical to the table above; any change to a checksum is a
correctness regression and supersedes any speed gain.
