"""scimark SparseMatMult — port of pyperformance bm_scimark sparse kernel.
Random and SparseCompRow_matmult are verbatim from pyperformance; pyperformance
leaves val/x zero, but the original SciMark fills them via RandomVector, which we
do here so the checksum (sum of y) verifies real arithmetic. Cycles from argv."""

from array import array


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

    def RandomVector(self, n):
        return array('d', [self.nextDouble() for i in range(n)])


def SparseCompRow_matmult(M, y, val, row, col, x, num_iterations):
    for _ in range(num_iterations):
        for r in range(M):
            sa = 0.0
            for i in range(row[r], row[r + 1]):
                sa += x[col[i]] * val[i]
            y[r] = sa


if __name__ == "__main__":
    import sys, time
    cycles = int(sys.argv[1]) if len(sys.argv) > 1 else 1000
    N = 1000
    nz = 50000
    nr = nz // N
    anz = nr * N

    rnd = Random(101)
    x = rnd.RandomVector(N)
    val = rnd.RandomVector(anz)
    y = array('d', [0]) * N
    col = array('i', [0]) * nz
    row = array('i', [0]) * (N + 1)

    row[0] = 0
    for r in range(N):
        rowr = row[r]
        step = r // nr
        row[r + 1] = rowr + nr
        if step < 1:
            step = 1
        for i in range(nr):
            col[rowr + i] = i * step

    t0 = time.perf_counter()
    SparseCompRow_matmult(N, y, val, row, col, x, cycles)
    elapsed = time.perf_counter() - t0
    print(f"elapsed: {elapsed:.6f} s")
    print(f"checksum: {sum(y):.6f}")
