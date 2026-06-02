# Benchmark baseline — post-DTC + `times`, pre-other-optimizations

Snapshot of `bench/synth.l4` and `bench/synth.py` to compare against
once the items in `OPTIMIZATIONS.md` start being implemented.

## Methodology change vs the previous baseline

`bench/synth-defs.l4`'s hand-rolled `begin/until` loops in `phase1`,
`bench-build`, and `bench-lookup` were rewritten to use the new
`times` combinator. The previous baseline's numbers — especially the
~37 ns "scale-frames lookup" figure — were significantly inflated by
loop-control overhead (counter local fetch/store, comparison, branch
per iteration). The new numbers measure the actual operations.

Total LF benchmark dropped from ~988 ms to ~925 ms (-6.4%) and
scale-frames lookup dropped from a flat ~37 ns to a *growing* curve
12-17 ns across sizes 5-2000. The growth-with-size is the binary-search
depth of `frame_find` finally becoming visible above the (now-tiny)
loop-control overhead.

Phase 1's algorithm is unchanged — same loop body, same iteration
count, same checksum — only the loop-control mechanism changed.

## Environment

- **Commit**: `543ef7f` + post-DTC additions (DTC landed; `times`
  primitive added; `bench/synth-defs.l4` updated to use it; the
  comparison-primitive float fast-path on `lt`/`gt`/`eq` is also in)
- **Date**: 2026-06-02
- **Hardware**: Apple M4 (arm64), Darwin 25.5.0
- **Compiler**: `clang -O3 -march=native -Wall -Wextra`
- **Python**: system `python3`

## State of the interpreter

- Direct threading (DTC) in place.
- `mark_body` walks embedded-quotation bodies cell by cell.
- Float fast-path on `p_lt` / `p_gt` / `p_eq` (step 1 of
  `OPTIMIZATIONS.md`).
- `times` combinator (new primitive added to support a cleaner counted
  loop than the hand-rolled `begin/until` pattern).
- Tests: 71/71 passing.
- Items 2-8 of `OPTIMIZATIONS.md` (inline frame helpers, peephole
  framework, `(local@0)`, `(@k)`, `?0branch`, `1+`, `dup *`) are NOT
  yet implemented.

## Bench iter constants

```
ITER1     = 10_000_000   (phase 1: tight loop, now via `times`)
ITER2     =  5_000_000   (phase 2: range → map → filter → reduce)
MAT_DIM   =        100   (phase 3: DGEMM 100×100)
MAT_ELEMS =     10_000
ITER4     =    500_000   (phase 4: 500k flat frames)
ITER5     = 10_000_000   (phase 5: map+reduce)
ITER6     =    200_000   (phase 6: deep nested frames; ~1.6M total)
```

## logicforth — three consecutive runs

| phase |  run 1   |  run 2   |  run 3   | median   |
|:-----:|---------:|---------:|---------:|---------:|
|   1   |  343.0 ms|  337.5 ms|  340.0 ms|  340.0 ms|
|   2   |  154.3 ms|  154.2 ms|  155.1 ms|  154.3 ms|
|   3   |    0.14 ms|    0.14 ms|    0.15 ms|    0.14 ms|
|   4   |   55.5 ms|   56.8 ms|   56.6 ms|   56.6 ms|
|   5   |  205.9 ms|  206.4 ms|  206.7 ms|  206.4 ms|
|   6   |  119.4 ms|  122.0 ms|  119.6 ms|  119.6 ms|
| total |  878.3 ms|  877.0 ms|  878.2 ms|  877.5 ms|

Checksums (all runs):

- phase1: `3.33333e+20` (sum of squares 1..1e7, float-rounded past exact)
- phase2: `4.16667e+19` (sum of squares > 10000 in 1..5e6)
- phase3: `25005833500000` (sum of `A @ A.T` for 100×100 from 1..1e4)
- phase4: `4.16668e+16` (sum of `:sq` across 500k flat frames)
- phase5: `3.33333e+20` (same math as phase1, via map+reduce)
- phase6: `300001500000` (sum across 200k deep nested frames)

## Scale-frames: logicforth vs CPython dict (medians of 3 runs)

|  size | LF build | py build | py / lf | LF lookup | py lookup | py / lf |
|:-----:|---------:|---------:|--------:|----------:|----------:|--------:|
|    5  |  0.12 µs |  0.19 µs |  1.63×  |    12.6 ns|    17.5 ns|   1.39× |
|   25  |  0.40 µs |  0.60 µs |  1.50×  |    13.0 ns|    17.2 ns|   1.32× |
|  100  |  1.16 µs |  2.09 µs |  1.80×  |    14.1 ns|    17.3 ns|   1.23× |
|  500  |  4.97 µs | 10.42 µs |  2.10×  |    15.3 ns|    18.5 ns|   1.21× |
| 2000  | 21.74 µs | 44.67 µs |  2.05×  |    17.3 ns|    18.5 ns|   1.07× |

**Lookup result has inverted vs the previous baseline.** logicforth now
*wins* single-key lookup at every size — by 7-40% depending on size.

The previous baseline showed Python winning lookup ~2× because the
~37 ns LF figure was almost entirely `begin/until` loop overhead, with
the actual `frame_find` cost hidden underneath. Now that the loop
overhead is gone, the actual cost surfaces: LF's binary search on a
small sorted array beats Python's hash + bucket probe on small frames
(L1-cache friendly, no hash computation), with the advantage shrinking
as N grows and binary-search depth approaches Python's lookup cost.

By the time N=2000, the curves are nearly equal (~17 vs ~18 ns).
Beyond that, Python would pull ahead — but we can't easily test
beyond ~3-5K due to symbol-pool size constraints.

Build cost: LF still wins ~2× across all sizes — sorted-array insert
with `memmove` is cheaper than hash-table insert + bucket setup for
these sizes.

### Per-step LF lookup targets after further optimization

- After `(@k)` / `(!k)` (step 5 of `OPTIMIZATIONS.md`): drop another
  ~10 ns by skipping the path-validation loop and `frame_walk`
  function call. LF lookup → ~5-7 ns territory at small sizes.
- After inline `frame_find` (step 2 of `OPTIMIZATIONS.md`): another
  ~2 ns. LF lookup → ~3-5 ns at small sizes — 3-4× faster than
  CPython.

## Python — three consecutive runs

Pure CPython, no numpy. `bench/synth.py` mirrors the algorithms of
`bench/synth.l4`. The Python side was *not* updated to match the
`times`-based loop — Python phase 1 still uses `while i <= ITER1`,
which is fine: the loop semantics are identical, the languages just
spell their counted iteration differently.

| phase  |  run 1   |  run 2   |  run 3   | median   |
|:------:|---------:|---------:|---------:|---------:|
|    1   |  332.1 ms|  334.5 ms|  342.1 ms|  334.5 ms|
|    2   |  675.2 ms|  686.4 ms|  684.8 ms|  684.8 ms|
|    3   |   40.3 ms|   41.9 ms|   42.2 ms|   41.9 ms|
|    4   |  119.0 ms|  120.2 ms|  120.9 ms|  120.2 ms|
|    5   | 1079.2 ms| 1078.9 ms| 1093.6 ms| 1079.2 ms|
|    6   |  718.8 ms|  727.4 ms|  726.6 ms|  726.6 ms|
| total  | 2964.6 ms| 2989.3 ms| 3010.2 ms| 2987.2 ms|

## Comparison summary (medians)

| phase | logicforth | python   | py / lf |
|:-----:|-----------:|---------:|--------:|
|   1   |   340.0 ms |  334.5 ms| 0.98×   |
|   2   |   154.3 ms |  684.8 ms| 4.44×   |
|   3   |     0.14 ms|   41.9 ms |  ~290×  |
|   4   |    56.6 ms |  120.2 ms| 2.12×   |
|   5   |   206.4 ms | 1079.2 ms| 5.23×   |
|   6   |   119.6 ms |  726.6 ms| 6.08×   |
| total |   877.5 ms | 2987.2 ms| 3.40×   |

Phase 1 — LF and pure CPython are now essentially tied (340 vs 335
ms), where the old baseline had Python ~22% ahead. CPython's bytecode
interpreter is still tight and runs the same algorithm in roughly the
same time, but LF closed the gap by switching the loop primitive.

Other phases unchanged in shape: LF wins 2-6× on the dispatch-heavy
work, 290× on the DGEMM kernel (single C-kernel call, not really an
interpreter comparison).

Total LF time: **877 ms vs previous 988 ms = -11%**. Most of the win
comes from phase 1's switch to `times`.

## Reproduce

```bash
make clean && make
./logicforth < bench/synth.l4 | grep -E "^(phase|size)"
python3 bench/synth.py | grep -E "^(phase|size)"
```

Run each 3-5× and take medians.

## Notes for comparison after further optimizations

- Total LF time target after all of `OPTIMIZATIONS.md`: ~600-650 ms
  (vs. 877 ms here), i.e. another ~25-30% off this baseline.
- Phase 1 target: ~280-310 ms (after `(local@0)` / `(local!0)` —
  the locals fetch/store inside the times body is now the dominant
  remaining dispatch).
- Single-key lookup target: ~5-7 ns at small sizes (after `(@k)`
  skips path validation + `frame_walk`).
- Phase 3 unaffected throughout. If it changes, something is wrong.
- Checksums must remain identical to the ones above. Any change to a
  checksum is a correctness regression and supersedes any speed gain.
