"""scimark SOR (successive over-relaxation), N=100 — port of pyperformance bm_scimark SOR.
Array2D, Random, and SOR_execute are verbatim from pyperformance. pyperformance's
bench_SOR leaves G zero; the original SciMark seeds it via RandomMatrix, which we
do here so the checksum verifies real arithmetic. Outer loops come from argv."""

from array import array


class Array2D(object):

    def __init__(self, w, h, data=None):
        self.width = w
        self.height = h
        self.data = array('d', [0]) * (w * h)
        if data is not None:
            self.setup(data)

    def _idx(self, x, y):
        if 0 <= x < self.width and 0 <= y < self.height:
            return y * self.width + x
        raise IndexError

    def __getitem__(self, x_y):
        (x, y) = x_y
        return self.data[self._idx(x, y)]

    def __setitem__(self, x_y, val):
        (x, y) = x_y
        self.data[self._idx(x, y)] = val

    def indexes(self):
        for y in range(self.height):
            for x in range(self.width):
                yield x, y


class Random(object):
    MDIG = 32
    ONE = 1
    m1 = (ONE << (MDIG - 2)) + ((ONE << (MDIG - 2)) - ONE)
    m2 = ONE << MDIG // 2
    dm1 = 1.0 / float(m1)

    def __init__(self, seed):
        self.initialize(seed)
        self.haveRange = False

    def initialize(self, seed):
        self.seed = seed
        seed = abs(seed)
        jseed = min(seed, self.m1)
        if (jseed % 2 == 0):
            jseed -= 1
        k0 = 9069 % self.m2
        k1 = 9069 / self.m2
        j0 = jseed % self.m2
        j1 = jseed / self.m2
        self.m = array('d', [0]) * 17
        for iloop in range(17):
            jseed = j0 * k0
            j1 = (jseed / self.m2 + j0 * k1 + j1 * k0) % (self.m2 / 2)
            j0 = jseed % self.m2
            self.m[iloop] = j0 + self.m2 * j1
        self.i = 4
        self.j = 16

    def nextDouble(self):
        I, J, m = self.i, self.j, self.m
        k = m[I] - m[J]
        if (k < 0):
            k += self.m1
        self.m[J] = k
        if (I == 0):
            I = 16
        else:
            I -= 1
        self.i = I
        if (J == 0):
            J = 16
        else:
            J -= 1
        self.j = J
        return self.dm1 * float(k)

    def RandomMatrix(self, a):
        for x, y in a.indexes():
            a[x, y] = self.nextDouble()
        return a


def SOR_execute(omega, G, cycles, Array):
    for p in range(cycles):
        for y in range(1, G.height - 1):
            for x in range(1, G.width - 1):
                G[x, y] = (omega * 0.25 * (G[x, y - 1] + G[x, y + 1] + G[x - 1, y]
                                           + G[x + 1, y])
                           + (1.0 - omega) * G[x, y])


if __name__ == "__main__":
    import sys, time
    loops = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    N = 100
    cycles = 10
    t0 = time.perf_counter()
    G = None
    for _ in range(loops):
        G = Random(101).RandomMatrix(Array2D(N, N))
        SOR_execute(1.25, G, cycles, Array2D)
    elapsed = time.perf_counter() - t0
    s = sum(G.data)
    print(f"elapsed: {elapsed:.6f} s")
    print(f"checksum: {s:.6f}")
