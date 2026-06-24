"""
Artificial, floating point-heavy benchmark originally used by Factor.
pyperformance bm_float, adapted: pyperf dependency replaced by a timing shim;
__main__ self-times benchmark(n) and prints the maximized point for checking.
"""
import time as _t
import types as _types
pyperf = _types.SimpleNamespace(perf_counter=_t.perf_counter)

from math import sin, cos, sqrt

POINTS = 100000


class Point(object):
    __slots__ = ('x', 'y', 'z')

    def __init__(self, i):
        self.x = x = sin(i)
        self.y = cos(i) * 3
        self.z = (x * x) / 2

    def normalize(self):
        x = self.x
        y = self.y
        z = self.z
        norm = sqrt(x * x + y * y + z * z)
        self.x /= norm
        self.y /= norm
        self.z /= norm

    def maximize(self, other):
        self.x = self.x if self.x > other.x else other.x
        self.y = self.y if self.y > other.y else other.y
        self.z = self.z if self.z > other.z else other.z
        return self


def maximize(points):
    next = points[0]
    for p in points[1:]:
        next = next.maximize(p)
    return next


def benchmark(n):
    points = [None] * n
    for i in range(n):
        points[i] = Point(i)
    for p in points:
        p.normalize()
    return maximize(points)


if __name__ == "__main__":
    import sys
    n = int(sys.argv[1]) if len(sys.argv) > 1 else POINTS
    repeat = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    t0 = _t.perf_counter()
    p = None
    for _ in range(repeat):
        p = benchmark(n)
    elapsed = _t.perf_counter() - t0
    print(f"result: {p.x!r} {p.y!r} {p.z!r}")
    print(f"elapsed: {elapsed:.6f} s ({n} points x {repeat})")
