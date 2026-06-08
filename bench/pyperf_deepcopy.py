"""Standalone harness version of pyperformance's bm_deepcopy.

Only the `benchmark` case (standard data types: a nested dict plus a small
dataclass) is ported — it is the part logicforth's deep `copy` mirrors. The
reference's `benchmark_reduce` and `benchmark_memo` cases are excluded: they
exercise custom __reduce__ classes and deepcopy's reference-identity memo,
neither of which logicforth has.

The `benchmark` body is copied verbatim from the pyperformance reference; only
pyperf.perf_counter is replaced with time.perf_counter. Prints an `equal:` line
(1 if a deep copy equals its original — the logicforth side reproduces this
with `copy =`) and an `elapsed:` line.
"""

import copy
import time
from dataclasses import dataclass


@dataclass
class A:
    string: str
    lst: list
    boolean: bool


def benchmark(n):
    """ Benchmark on some standard data types """
    a = {
        'list': [1, 2, 3, 43],
        't': (1 ,2, 3),
        'str': 'hello',
        'subdict': {'a': True}
    }
    dc = A('hello', [1, 2, 3], True)

    dt = 0
    for ii in range(n):
        for jj in range(30):
            t0 = time.perf_counter()
            _ = copy.deepcopy(a)
            dt += time.perf_counter() - t0
        for s in ['red', 'blue', 'green']:
            dc.string = s
            for kk in range(5):
                dc.lst[0] = kk
                for b in [True, False]:
                    dc.boolean = b
                    t0 = time.perf_counter()
                    _ = copy.deepcopy(dc)
                    dt += time.perf_counter() - t0
    return dt


def verify():
    a = {
        'list': [1, 2, 3, 43],
        't': (1, 2, 3),
        'str': 'hello',
        'subdict': {'a': True}
    }
    return int(copy.deepcopy(a) == a)


N = 20000


if __name__ == "__main__":
    print(f"equal: {verify()}")
    print(f"elapsed: {benchmark(N):.6f} s")
