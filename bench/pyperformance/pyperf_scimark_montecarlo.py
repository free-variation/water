"""scimark MonteCarlo pi estimate — port of pyperformance bm_scimark MonteCarlo.
Random and MonteCarlo are verbatim from pyperformance; self-times LOOPS runs."""

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


def MonteCarlo(Num_samples):
    rnd = Random(113)
    under_curve = 0
    for count in range(Num_samples):
        x = rnd.nextDouble()
        y = rnd.nextDouble()
        if x * x + y * y <= 1.0:
            under_curve += 1
    return float(under_curve) / Num_samples * 4.0


if __name__ == "__main__":
    import sys, time
    samples = int(sys.argv[1]) if len(sys.argv) > 1 else 1000000
    loops = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    t0 = time.perf_counter()
    est = None
    for _ in range(loops):
        est = MonteCarlo(samples)
    elapsed = time.perf_counter() - t0
    print(f"estimate: {est!r}")
    print(f"elapsed: {elapsed:.6f} s")
