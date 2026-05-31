"""Python mirror of bench/synth.l4.

Same five phases, same iteration counts, same algorithmic structure so that
per-phase checksums can be diffed and per-phase wall times compared.

All numeric work is forced to float64 so we match logicforth's float-only
arithmetic — otherwise CPython's arbitrary-precision ints would diverge from
logicforth's rounded floats at the higher iteration counts.

The higher-order phases (2, 4, 5) use functools.reduce + lambda rather than
sum(); that pays a per-element Python call, which is the closest analogue to
logicforth's execute_cfa per element under map/filter/reduce.

Phase 3 is a pure-Python triple-nested DGEMM, not numpy — the logicforth side
runs its own hand-coded C kernel, so this is the apples-to-apples comparison:
"what a portable interpreter can do without delegating to a vendor BLAS."
"""

import time
from functools import reduce

ITER1 = 1_000_000
ITER2 = 500_000
MAT_DIM = 100
MAT_ELEMS = MAT_DIM * MAT_DIM
ITER4 = 50_000
ITER5 = 1_000_000


def phase1():
    acc = 0.0
    i = 1.0
    limit = float(ITER1)
    while not (i > limit):
        acc = acc + i * i
        i = i + 1.0
    return acc


def phase2():
    arr = [float(i) for i in range(1, ITER2 + 1)]
    squares = list(map(lambda x: x * x, arr))
    filtered = list(filter(lambda s: s > 10000.0, squares))
    return reduce(lambda a, b: a + b, filtered, 0.0)


def phase3():
    n = MAT_DIM
    A = [[float(i * n + j + 1) for j in range(n)] for i in range(n)]
    AT = [[A[j][i] for j in range(n)] for i in range(n)]
    C = [[0.0] * n for _ in range(n)]
    for i in range(n):
        Ai = A[i]
        Ci = C[i]
        for p in range(n):
            a = Ai[p]
            Bp = AT[p]
            for j in range(n):
                Ci[j] += a * Bp[j]
    total = 0.0
    for row in C:
        for v in row:
            total += v
    return total


def make_rec(i):
    return {'id': i, 'sq': i * i, 'tri': i * (i + 1.0) / 2.0}


def phase4():
    arr = [float(i) for i in range(0, ITER4 + 1)]
    frames = list(map(make_rec, arr))
    squares = list(map(lambda f: f['sq'], frames))
    return reduce(lambda a, b: a + b, squares, 0.0)


def phase5():
    arr = [float(i) for i in range(1, ITER5 + 1)]
    squares = list(map(lambda x: x * x, arr))
    return reduce(lambda a, b: a + b, squares, 0.0)


def run_phase(name, fn):
    t0 = time.monotonic()
    r = fn()
    t1 = time.monotonic()
    print(f"{name} elapsed={t1 - t0:g}s")
    print(f"{name}   result={r:g}")


if __name__ == '__main__':
    print()
    run_phase("phase1", phase1)
    run_phase("phase2", phase2)
    run_phase("phase3", phase3)
    run_phase("phase4", phase4)
    run_phase("phase5", phase5)
