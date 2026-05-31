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
ITER6 = 20_000


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


def path_set(root, path, value):
    """Auto-vivifying nested dict set, mirroring logicforth's `!` semantics."""
    node = root
    for key in path[:-1]:
        nxt = node.get(key)
        if not isinstance(nxt, dict):
            nxt = {}
            node[key] = nxt
        node = nxt
    node[path[-1]] = value


def path_get(root, path):
    node = root
    for key in path:
        node = node[key]
    return node


def make_deep(n):
    r = {}
    path_set(r, ('a', 'b', 'c', 'd'), n)
    path_set(r, ('a', 'b', 'c', 'e'), n * 2.0)
    path_set(r, ('a', 'b', 'f'),      n * 3.0)
    path_set(r, ('a', 'g'),           n * 4.0)
    path_set(r, ('h', 'i', 'j', 'k', 'l'), n * 5.0)
    return r


def read_deep(f):
    return (path_get(f, ('a', 'b', 'c', 'd'))
            + path_get(f, ('a', 'b', 'c', 'e'))
            + path_get(f, ('a', 'b', 'f'))
            + path_get(f, ('a', 'g'))
            + path_get(f, ('h', 'i', 'j', 'k', 'l')))


def phase6():
    arr = [float(i) for i in range(0, ITER6 + 1)]
    frames = list(map(make_deep, arr))
    sums = list(map(read_deep, frames))
    return reduce(lambda a, b: a + b, sums, 0.0)


def scale_frames():
    """Match bench/synth-defs.l4's scale-frames: per-size build and lookup."""
    cases = [
        (5,     100_000, 1_000_000),
        (25,     20_000, 1_000_000),
        (100,     4_000, 1_000_000),
        (500,       800, 1_000_000),
        (2000,      200, 1_000_000),
    ]
    print()
    print("frame/dict scaling — build cost and pure-lookup cost vs size (Python)")
    for size, build_reps, lookup_passes in cases:
        keys = [f"k{i}" for i in range(size)]
        vals = [float(i) for i in range(size)]

        t0 = time.monotonic()
        for _ in range(build_reps):
            d = dict(zip(keys, vals))
        t_build = time.monotonic() - t0

        d = dict(zip(keys, vals))
        k0 = "k0"
        t0 = time.monotonic()
        for _ in range(lookup_passes):
            _ = d[k0]
        t_lookup = time.monotonic() - t0

        us_per_build = t_build / build_reps * 1e6
        ns_per_lookup = t_lookup / lookup_passes * 1e9
        print(f"size={size}  build={us_per_build:g} us  lookup={ns_per_lookup:g} ns/op")


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
    run_phase("phase6", phase6)
    scale_frames()
