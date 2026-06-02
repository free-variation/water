# Benchmark snapshot — post-DTC + `times` + 1+/1-/sq

Snapshot of `bench/synth.l4` and `bench/synth.py` after adding the
plain-primitive arithmetic shortcuts (`1+`, `1-`, `sq`). Compare
against `baseline-post-dtc.md` (which had DTC and `times` but not the
shortcuts) and against the older state implied by that file's history.

## Changes since `baseline-post-dtc.md`

- Added `1+` ( n -- n+1 ), `1-` ( n -- n-1 ), `sq` ( n -- n*n ) as
  float-only primitives.
- Rewrote the bench to use them: phase 1 (`i sq` and `i 1+`),
  phase 2 / 5 (`[: sq :] map`), phase 4 (`i sq` and `i 1+` inside
  `make-rec`), and `build-keys` / `build-vals` (`1-` for range
  endpoints).

No semantic changes — all checksums identical to the previous baseline.

## Environment

- **Commit**: `f398986`
- **Date**: 2026-06-02
- **Hardware**: Apple M4 (arm64), Darwin 25.5.0
- **Compiler**: `clang -O3 -march=native -Wall -Wextra`
- **Python**: system `python3`

## Bench iter constants

```
ITER1     = 10_000_000
ITER2     =  5_000_000
MAT_DIM   =        100
MAT_ELEMS =     10_000
ITER4     =    500_000
ITER5     = 10_000_000
ITER6     =    200_000
```

## logicforth — three consecutive runs

| phase |  run 1   |  run 2   |  run 3   | median   |
|:-----:|---------:|---------:|---------:|---------:|
|   1   |  246.6 ms|  248.8 ms|  251.5 ms|  248.8 ms|
|   2   |  140.8 ms|  143.1 ms|  141.0 ms|  141.0 ms|
|   3   |    0.17 ms|    0.15 ms|    0.15 ms|    0.15 ms|
|   4   |   50.0 ms|   50.9 ms|   51.0 ms|   50.9 ms|
|   5   |  180.5 ms|  180.5 ms|  180.2 ms|  180.5 ms|
|   6   |  118.7 ms|  120.0 ms|  118.7 ms|  118.7 ms|
| total |  736.7 ms|  743.5 ms|  742.6 ms|  740.8 ms|

Checksums (all runs):

- phase1: `3.33333e+20`
- phase2: `4.16667e+19`
- phase3: `25005833500000`
- phase4: `4.16668e+16`
- phase5: `3.33333e+20`
- phase6: `300001500000`

## Scale-frames: logicforth vs CPython dict (medians of 3 runs)

|  size | LF build | py build | py / lf | LF lookup | py lookup | py / lf |
|:-----:|---------:|---------:|--------:|----------:|----------:|--------:|
|    5  |  0.12 µs |  0.19 µs |  1.62×  |    11.4 ns|    17.4 ns|   1.52× |
|   25  |  0.36 µs |  0.63 µs |  1.74×  |    12.2 ns|    17.4 ns|   1.43× |
|  100  |  1.10 µs |  2.16 µs |  1.97×  |    13.5 ns|    17.5 ns|   1.30× |
|  500  |  4.82 µs | 10.72 µs |  2.23×  |    14.6 ns|    18.6 ns|   1.27× |
| 2000  | 21.69 µs | 44.55 µs |  2.05×  |    16.1 ns|    17.7 ns|   1.10× |

LF wins both build and lookup at every size. The `1-` substitution in
`build-keys` / `build-vals` shaved a small amount off the build curve.
Lookup curve unchanged in shape — `frame_find` itself hasn't changed.

## Python — three consecutive runs

Python side unchanged since the previous baseline (the shortcut
primitives are LF-only).

| phase  |  run 1   |  run 2   |  run 3   | median   |
|:------:|---------:|---------:|---------:|---------:|
|    1   |  320.9 ms|  330.7 ms|  328.3 ms|  328.3 ms|
|    2   |  670.5 ms|  682.6 ms|  685.9 ms|  682.6 ms|
|    3   |   41.7 ms|   40.7 ms|   41.4 ms|   41.4 ms|
|    4   |  121.9 ms|  122.7 ms|  123.1 ms|  122.7 ms|
|    5   | 1080.9 ms| 1090.1 ms| 1102.3 ms| 1090.1 ms|
|    6   |  721.3 ms|  717.2 ms|  725.1 ms|  721.3 ms|
| total  | 2956.2 ms| 2983.9 ms| 3006.1 ms| 2986.4 ms|

## Comparison summary (medians)

| phase | logicforth | python   | py / lf |
|:-----:|-----------:|---------:|--------:|
|   1   |   248.8 ms |  328.3 ms| 1.32×   |
|   2   |   141.0 ms |  682.6 ms| 4.84×   |
|   3   |     0.15 ms|   41.4 ms |  ~276×  |
|   4   |    50.9 ms |  122.7 ms| 2.41×   |
|   5   |   180.5 ms | 1090.1 ms| 6.04×   |
|   6   |   118.7 ms |  721.3 ms| 6.08×   |
| total |   740.8 ms | 2986.4 ms| 4.03×   |

**Phase 1: LF now wins by 32%.** Previous baseline had LF and pure
CPython essentially tied at 340 vs 335 ms. The shortcut primitives
flipped that to 249 vs 328.

Total speed ratio: **4.03× faster than pure CPython**, up from 3.40×
in the previous baseline. Single fastest gain came from phase 1 —
`i sq` (instead of `i i *`) and `i 1+` (instead of `i 1 +`) save a
few ns per iter across 10M iterations.

## Change since previous LF baselines

| baseline | total time | scale lookup (size 5) |
|---|---:|---:|
| `baseline-post-dtc.md` (with `times`) | 877 ms | 12.6 ns |
| `baseline-post-shortcuts.md` (this) | 741 ms | 11.4 ns |
| Δ | -16% | -10% |

## Reproduce

```bash
make clean && make
./logicforth < bench/synth.l4 | grep -E "^(phase|size)"
python3 bench/synth.py | grep -E "^(phase|size)"
```

Run each 3-5× and take medians.

## Notes for comparison after further optimizations

The remaining items in `OPTIMIZATIONS.md`:

- `key@` / `key!` — target single-key lookup at ~5-7 ns (from the
  current 11.4 ns).
- `(local@0)` / `(local!0)` — target another 5-7% off phase 1, which
  is now the dominant remaining phase.
- `(?0branch)` — target 2-3% on `begin … until` patterns (mostly
  affecting code not in the bench, since the bench rewrites used
  `times`).

Phase 3 unaffected by any of these. Checksums must remain identical.
