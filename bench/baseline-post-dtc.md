# Benchmark baseline — post-DTC, pre-optimizations

Snapshot of `bench/synth.l4` and `bench/synth.py` to compare against
once the items in `OPTIMIZATIONS.md` start being implemented.

## Environment

- **Commit**: `543ef7f` (with bench iter constants 10× the previous
  baseline; `MAX_OBJECTS` bumped to 2,097,152 to accommodate phase 4's
  500k frames and phase 6's ~1.6M total frames)
- **Date**: 2026-06-01
- **Hardware**: Apple M4 (arm64), Darwin 25.5.0
- **Compiler**: `clang -O3 -march=native -Wall -Wextra`
- **Python**: system `python3`

## State of the interpreter

- Direct threading (DTC) in place — body cells hold handler pointers,
  dispatch is one read per virtual op.
- `mark_body` walks embedded-quotation bodies cell by cell (no
  2-cell skip for inline `docol_ptr` cells), per the bug fix that
  surfaced during DTC bring-up.
- Tests: 70/70 passing.
- All super-instruction work from `OPTIMIZATIONS.md` is NOT yet
  implemented. No float fast-path, no peephole framework, no inline
  frame helpers.

## Bench iter constants (in `bench/synth-defs.l4` and `bench/synth.py`)

```
ITER1     = 10_000_000   (phase 1: tight begin/until loop)
ITER2     =  5_000_000   (phase 2: range → map → filter → reduce)
MAT_DIM   =        100   (phase 3: DGEMM kernel — left at 100 because
MAT_ELEMS =     10_000    DGEMM is O(N³); pure-Python phase 3 would
                          take minutes if scaled up)
ITER4     =    500_000   (phase 4: 500k flat frames)
ITER5     = 10_000_000   (phase 5: same math as phase 1, via map+reduce)
ITER6     =    200_000   (phase 6: ~1.6M deep-nested frames total)
```

Phase 3 was deliberately left at 100×100. It's a single C-kernel call,
not a dispatch-loop benchmark — scaling it would test BLAS performance,
not interpreter performance.

## logicforth — three consecutive runs

| phase |  run 1   |  run 2   |  run 3   | median   |
|:-----:|---------:|---------:|---------:|---------:|
|   1   |  430.8 ms|  435.0 ms|  440.5 ms|  435.0 ms|
|   2   |  162.3 ms|  162.6 ms|  163.9 ms|  162.6 ms|
|   3   |    0.15 ms|    0.14 ms|    0.14 ms|    0.14 ms|
|   4   |   56.4 ms|   58.3 ms|   56.5 ms|   56.5 ms|
|   5   |  214.4 ms|  207.6 ms|  207.4 ms|  207.6 ms|
|   6   |  123.0 ms|  124.3 ms|  126.5 ms|  124.3 ms|
| total |  987.1 ms|  988.0 ms|  994.9 ms|  988.0 ms|

Checksums (all runs):

- phase1: `3.33333e+20` (sum of squares 1..1e7, float-rounded past exact)
- phase2: `4.16667e+19` (sum of squares > 10000 in 1..5e6)
- phase3: `25005833500000` (sum of `A @ A.T` for 100×100 from 1..1e4)
- phase4: `4.16668e+16` (sum of `:sq` across 500k flat frames)
- phase5: `3.33333e+20` (same math as phase1, via map+reduce)
- phase6: `300001500000` (sum across 200k deep nested frames)

## Scale-frames: logicforth vs CPython dict (medians of 3 runs)

logicforth's single-key lookup uses `frame /k0 @` with the path literal
compiled once. CPython uses `d[k0]` with `k0` interned. Build measures
one full frame/dict construction from parallel keys/values arrays.

|  size | LF build | py build | py / lf | LF lookup | py lookup | py / lf |
|:-----:|---------:|---------:|--------:|----------:|----------:|--------:|
|    5  |  0.12 µs |  0.19 µs |  1.58×  |    38.3 ns|    17.9 ns|   0.47× |
|   25  |  0.40 µs |  0.62 µs |  1.55×  |    36.4 ns|    17.8 ns|   0.49× |
|  100  |  1.16 µs |  2.11 µs |  1.82×  |    37.1 ns|    17.3 ns|   0.47× |
|  500  |  4.99 µs | 10.76 µs |  2.16×  |    37.2 ns|    18.4 ns|   0.49× |
| 2000  | 22.62 µs | 44.54 µs |  1.97×  |    37.3 ns|    17.9 ns|   0.48× |

logicforth's frames win build cost by ~2× across all sizes — sorted-
array `frame_put` (with `memmove` on insert) is cheaper than CPython's
hash-table insert + bucket setup for the sizes tested.

CPython wins single-key lookup by ~2× across all sizes — `d[k]` is one
hash + bucket probe with no dispatch loop in the middle, while LF's
`@` goes through the interpreter (read body cell → call handler → walk
the 1-element path → `frame_find`).

Both curves are essentially flat past size 25. LF's binary-search depth
grows with size (log₂ doubles every ~25), but the per-op dispatch +
handler overhead dominates the measurement, masking the search-depth
contribution. Python's hash lookup is O(1) by design and stays flat
within measurement noise.

### Per-step LF lookup targets after optimization

- After `(@k)` / `(!k)` (step 5 of `OPTIMIZATIONS.md`): single-key
  lookup should drop to ~22-27 ns by skipping the path-validation
  loop, `frame_walk` function call, and the 1-iteration walk-loop.
  Closes ~half the gap to CPython.
- After inline `frame_find` (step 2): another ~2 ns off, putting LF
  lookup somewhere in the 20-25 ns range — within ~30% of CPython's
  hash lookup, despite using a different data structure.
- Build cost should change only slightly from current — the existing
  `frame_put` loop is already C-side and not dispatch-bound.

## Python — three consecutive runs

Pure CPython, no numpy. `bench/synth.py` mirrors the algorithms of
`bench/synth.l4` with `functools.reduce` + lambdas to preserve
per-element call overhead.

| phase  |  run 1   |  run 2   |  run 3   | median   |
|:------:|---------:|---------:|---------:|---------:|
|    1   |  338.9 ms|  348.5 ms|  338.6 ms|  338.9 ms|
|    2   |  689.6 ms|  702.9 ms|  684.1 ms|  689.6 ms|
|    3   |   42.6 ms|   39.8 ms|   41.1 ms|   41.1 ms|
|    4   |  123.0 ms|  124.4 ms|  123.4 ms|  123.4 ms|
|    5   | 1081.5 ms| 1117.3 ms| 1097.0 ms| 1097.0 ms|
|    6   |  735.2 ms|  722.6 ms|  730.1 ms|  730.1 ms|
| total  | 3010.8 ms| 3055.5 ms| 3014.3 ms| 3020.1 ms|

## Comparison summary (medians)

| phase | logicforth | python   | py / lf |
|:-----:|-----------:|---------:|--------:|
|   1   |   435.0 ms |  338.9 ms| 0.78×   |
|   2   |   162.6 ms |  689.6 ms| 4.24×   |
|   3   |     0.14 ms|   41.1 ms |  ~290×  |
|   4   |    56.5 ms |  123.4 ms| 2.18×   |
|   5   |   207.6 ms | 1097.0 ms| 5.28×   |
|   6   |   124.3 ms |  730.1 ms| 5.87×   |
| total |   988.0 ms | 3020.1 ms| 3.06×   |

Python beats logicforth on phase 1 (hand-rolled tight loop) by ~22%.
logicforth wins everywhere else: 2× on flat frames (phase 4) up to
~290× on DGEMM (phase 3, an unfair comparison since the LF side is a
single C-kernel call). On the dispatch-heavy phases that
`OPTIMIZATIONS.md` targets (1, 2, 5, 6), logicforth is already ahead by
roughly 4-6× — the optimizations are about widening that margin.

## Reproduce

```bash
make clean && make
./logicforth < bench/synth.l4 | grep -E "^(phase|size)"
python3 bench/synth.py | grep -E "^phase"
```

Run each 3-5× and take medians; phase 3 in particular has enough
variance run-to-run that a single-shot number is misleading at the
sub-millisecond scale.

## Notes for comparison after optimizations land

- Total LF time target after all of `OPTIMIZATIONS.md`: ~700 ms
  (vs. 988 ms here), i.e. ~30% off the baseline.
- Phase 1 target: ~330-360 ms (after `(local@0)`/`(local!0)`,
  `(?0branch)`, `(1+)`, `(dup*)`). This is the phase most likely to
  show the biggest absolute gain since it has the highest dispatch
  density.
- Single-key lookup target: ~22-27 ns (after `(@k)` / `(!k)`).
- Phase 3 unaffected throughout. If it changes, something is wrong.
- Checksums must remain identical to the ones above. Any change to a
  checksum is a correctness regression and supersedes any speed gain.
