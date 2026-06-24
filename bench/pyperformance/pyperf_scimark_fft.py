"""scimark FFT — port of pyperformance bm_scimark FFT kernel.
Random and the FFT_* functions are verbatim from pyperformance; self-times
LOOPS outer iterations of CYCLES forward+inverse round-trips. Input is
Random(7).RandomVector(2N); checksum is sum(data) after the loop."""

import math
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


def int_log2(n):
    k = 1
    log = 0
    while k < n:
        k *= 2
        log += 1
    if n != 1 << log:
        raise Exception("FFT: Data length is not a power of 2: %s" % n)
    return log


def FFT_transform_internal(N, data, direction):
    n = N // 2
    bit = 0
    dual = 1
    if n == 1:
        return
    logn = int_log2(n)
    if N == 0:
        return
    FFT_bitreverse(N, data)
    bit = 0
    while bit < logn:
        w_real = 1.0
        w_imag = 0.0
        theta = 2.0 * direction * math.pi / (2.0 * float(dual))
        s = math.sin(theta)
        t = math.sin(theta / 2.0)
        s2 = 2.0 * t * t
        for b in range(0, n, 2 * dual):
            i = 2 * b
            j = 2 * (b + dual)
            wd_real = data[j]
            wd_imag = data[j + 1]
            data[j] = data[i] - wd_real
            data[j + 1] = data[i + 1] - wd_imag
            data[i] += wd_real
            data[i + 1] += wd_imag
        for a in range(1, dual):
            tmp_real = w_real - s * w_imag - s2 * w_real
            tmp_imag = w_imag + s * w_real - s2 * w_imag
            w_real = tmp_real
            w_imag = tmp_imag
            for b in range(0, n, 2 * dual):
                i = 2 * (b + a)
                j = 2 * (b + a + dual)
                z1_real = data[j]
                z1_imag = data[j + 1]
                wd_real = w_real * z1_real - w_imag * z1_imag
                wd_imag = w_real * z1_imag + w_imag * z1_real
                data[j] = data[i] - wd_real
                data[j + 1] = data[i + 1] - wd_imag
                data[i] += wd_real
                data[i + 1] += wd_imag
        bit += 1
        dual *= 2


def FFT_bitreverse(N, data):
    n = N // 2
    nm1 = n - 1
    j = 0
    for i in range(nm1):
        ii = i << 1
        jj = j << 1
        k = n >> 1
        if i < j:
            tmp_real = data[ii]
            tmp_imag = data[ii + 1]
            data[ii] = data[jj]
            data[ii + 1] = data[jj + 1]
            data[jj] = tmp_real
            data[jj + 1] = tmp_imag
        while k <= j:
            j -= k
            k >>= 1
        j += k


def FFT_transform(N, data):
    FFT_transform_internal(N, data, -1)


def FFT_inverse(N, data):
    n = N / 2
    norm = 0.0
    FFT_transform_internal(N, data, +1)
    norm = 1 / float(n)
    for i in range(N):
        data[i] *= norm


if __name__ == "__main__":
    import sys, time
    loops = int(sys.argv[1]) if len(sys.argv) > 1 else 5
    cycles = int(sys.argv[2]) if len(sys.argv) > 2 else 50
    N = 1024
    twoN = 2 * N
    init_vec = Random(7).RandomVector(twoN)
    t0 = time.perf_counter()
    x = None
    for _ in range(loops):
        x = array('d', init_vec)
        for i in range(cycles):
            FFT_transform(twoN, x)
            FFT_inverse(twoN, x)
    elapsed = time.perf_counter() - t0
    print(f"elapsed: {elapsed:.6f} s")
    print(f"checksum: {sum(x):.6f}")
