# Static analysis findings (src/c)

Crash scenarios and performance bottlenecks found by a read-through of `src/c`
(excluding `external/`). Each entry is anchored to source lines. Findings the
audit dismissed as false positives are not listed here.

Status legend: **verified** = checked against the source in this pass;
**reported** = surfaced by the audit but not independently confirmed.

## Crashes

### 1. `(ffi-call)` — unchecked binding index → OOB deref (verified)
`foreign.c:203`
```c
POP_INT(index, "ffi-call", "binding");
FFIBinding *binding = ffi_bindings[index];   // no 0 <= index < count check
```
`(ffi-call)` is registered in the vocabulary (`core.c:4058`), so `999999
(ffi-call)` indexes out of `ffi_bindings` and dereferences garbage → crash.
Reachable from the REPL with no library loaded. FFI is documented-unsafe, but
the binding index itself should be bounds-checked.

### 2. Adversarial image: unbounded counts + unchecked realloc (verified)
`image.c:616-623`
```c
if (!r_i32(file, &saved_n_pairs) || saved_n_pairs < 0) { ... }   // no upper bound
while (pairs.space.cap < saved_n_pairs) GROW_PAIR_TABLE(pairs.space.cap * 2);  // unchecked realloc
... pairs.table[i].head ...   // NULL-deref if realloc failed
```
A crafted `.image` with a huge pair/object count drives a giant `realloc`;
`GROW_PAIR_TABLE` never checks for failure, so `pairs.table` can be NULL when
indexed. `load-image` on an untrusted file → OOM/crash. Related:
`loaded_handle_ok`'s `default: return 1` lets unknown tags pass validation.

### 3. fork/pipe failure leaks file descriptors (verified)
`platform_posix.c:254-270` — on partial pipe failure or `fork() < 0`, the error
paths free `argv`/`program_path` but never `close()` the pipe fds already
opened, leaking up to six per failure. Minor; requires fork/pipe to fail.

## Correctness / undefined behavior

### 4. Rational arithmetic overflows `int` — FIXED
`dimension.c:97-113`
```c
left.numerator * right.denominator + right.numerator * left.denominator   // int * int
```
`rational_add`/`rational_multiply`/`rational_divide` multiplied 32-bit ints with
no guard, while `rational_pow` (140-151) already guarded with `long long` +
`INT_MAX`. Unit arithmetic with large scales (e.g. a unit scaled 10^6 multiplied
by itself) overflowed → signed-overflow UB / wrong units, reachable through
normal use.

Fix: the three ops now compute intermediates in `long long`, reduce via a
`long long` gcd, and signal failure if the reduced result does not fit `int`
(`rational_from_ll`). `unit_combine` returns `-1` on overflow; `unit_multiply` /
`unit_divide` gained an `Interpreter *` and `fail()` on it; `unit_pow` and
`p_unit` check their scale/exponent products. Regression test in
`tests/114_units.h2o` (`1 bigm 1 bigm *` → clean error, not a wrap).

## Performance

### 5. `unit_intern` linear scan — measured negligible; unrelated alloc fixed
`dimension.c:200` — the flagged concern (O(units^2) scan) does not show up in
practice: a hot dimensioned loop reuses units interned early, so the scan is 2-3
comparisons regardless of `n_units`. Measured (2M ops): pure float `/` 14 ns,
dim `+` (no combine) 38 ns, dim `/` (combine) ~53 ns. The gap is intrinsic
boxing (~24 ns: unwrap two operands + allocate the result pair) plus
`unit_combine` (~15 ns), not the scan.

Applied fix: `unit_combine` allocated and freed the `merged` term array on every
op; replaced with a 16-slot stack buffer (heap fallback for many dimensions),
which took a measurable slice off the dim path. A `(left, right, sign) -> result`
memo for `unit_combine` was considered (would pull dim `/` toward dim `+`) and
declined as too much machinery for ~15 ns.

### 6. `rational_of_double` up to 1,000,000 iterations — reject-path only, left as is
`dimension.c:158` — the million-iteration worst case runs *only* when rejecting
a scale with no rational approximation within tolerance (a one-time user error).
Normal declarations exit early (e.g. a 1/100 scale in ~100 iterations). Not a
hot path; not worth a continued-fraction rewrite and its behavior-change risk.

## Plausible — reported, not verified

- `p_enter_locals_to` refill path (`core.c:1701`) computes `dsp - n_locals`
  without the `dsp >= n_locals` guard the normal path has (`core.c:1713`);
  underflow if a loop body under-fills the data stack. Hard to trigger.
- `fmod` (`words.c:1677`) does not reject a zero divisor (returns NaN), unlike
  `mod` / `%` which error. Low.
- `functional.c` parallel worker slot / `dsp` reads — needs a concurrency review
  a static read cannot settle.
- `forget` O(n^2) source-pool rescan (`compiler.c:651`); GC-time insertion sort
  over CFAs (`core.c:3162`); symbol-hash linear probing — reported, unconfirmed.

## Note for a follow-up pass

The audit's "stale `Object*` across GC" reports were false positives (objects
live at stable arena addresses and are not moved). The genuine analog is the
**pair table** (`pairs.table`), which is contiguous and realloc'd
(`water.h:367`): caching `&pairs.table[slot]` across a pair allocation would
dangle. No confirmed instance found; worth a targeted check.
