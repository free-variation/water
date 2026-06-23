#!/usr/bin/env python
"""
The Computer Language Benchmarks Game
http://benchmarksgame.alioth.debian.org/

regex-dna Python 3 #5 program: contributed by Dominique Wahli, 2to3, modified
by Justin Peel. fasta Python 3 #3 program: modified by Ian Osgood, Heinrich
Acker, Justin Peel, Christopher Sean Forgeron.

Standalone harness version of pyperformance's bm_regex_dna: generates the same
FASTA, then prints a `result:` line (ilen clen <9 counts> flen) and an
`elapsed:` line for one timed run of the measured phase. Order of the result
line matches bench/regex-dna.l4 so the suite can diff them.
"""

import bisect
import re
import sys
import time


DEFAULT_INIT_LEN = 100000
DEFAULT_RNG_SEED = 42

ALU = ('GGCCGGGCGCGGTGGCTCACGCCTGTAATCCCAGCACTTTGG'
       'GAGGCCGAGGCGGGCGGATCACCTGAGGTCAGGAGTTCGAGA'
       'CCAGCCTGGCCAACATGGTGAAACCCCGTCTCTACTAAAAAT'
       'ACAAAAATTAGCCGGGCGTGGTGGCGCGCGCCTGTAATCCCA'
       'GCTACTCGGGAGGCTGAGGCAGGAGAATCGCTTGAACCCGGG'
       'AGGCGGAGGTTGCAGTGAGCCGAGATCGCGCCACTGCACTCC'
       'AGCCTGGGCGACAGAGCGAGACTCCGTCTCAAAAA')

IUB = list(zip('acgtBDHKMNRSVWY', [0.27, 0.12, 0.12, 0.27] + [0.02] * 11))

HOMOSAPIENS = [
    ('a', 0.3029549426680),
    ('c', 0.1979883004921),
    ('g', 0.1975473066391),
    ('t', 0.3015094502008),
]


def make_cumulative(table):
    P = []
    C = []
    prob = 0.
    for char, p in table:
        prob += p
        P += [prob]
        C += [ord(char)]
    return (P, C)


def repeat_fasta(src, n, nprint):
    width = 60
    is_trailing_line = False
    count_modifier = 0.0
    len_of_src = len(src)
    ss = src + src + src[:n % len_of_src]
    s = bytearray(ss, encoding='utf8')
    if n % width:
        is_trailing_line = True
        count_modifier = 1.0
    count = 0
    end = (n / float(width)) - count_modifier
    while count < end:
        i = count * 60 % len_of_src
        nprint(s[i:i + 60] + b'\n')
        count += 1
    if is_trailing_line:
        nprint(s[-(n % width):] + b'\n')


def random_fasta(table, n, seed, nprint):
    width = 60
    r = range(width)
    bb = bisect.bisect
    is_trailing_line = False
    count_modifier = 0.0
    line = bytearray(width + 1)
    probs, chars = make_cumulative(table)
    im = 139968.0
    seed = float(seed)
    if n % width:
        is_trailing_line = True
        count_modifier = 1.0
    count = 0.0
    end = (n / float(width)) - count_modifier
    while count < end:
        for i in r:
            seed = (seed * 3877.0 + 29573.0) % 139968.0
            line[i] = chars[bb(probs, seed / im)]
        line[60] = 10
        nprint(line)
        count += 1.0
    if is_trailing_line:
        for i in range(n % width):
            seed = (seed * 3877.0 + 29573.0) % 139968.0
            line[i] = chars[bb(probs, seed / im)]
        nprint(line[:i + 1] + b"\n")
    return seed


def init_benchmarks(n, rng_seed):
    result = bytearray()
    nprint = result.extend
    nprint(b'>ONE Homo sapiens alu\n')
    repeat_fasta(ALU, n * 2, nprint=nprint)
    nprint(b'>TWO IUB ambiguity codes\n')
    seed = random_fasta(IUB, n * 3, seed=rng_seed, nprint=nprint)
    nprint(b'>THREE Homo sapiens frequency\n')
    random_fasta(HOMOSAPIENS, n * 5, seed, nprint=nprint)
    return bytes(result)


VARIANTS = (
    b'agggtaaa|tttaccct',
    b'[cgt]gggtaaa|tttaccc[acg]',
    b'a[act]ggtaaa|tttacc[agt]t',
    b'ag[act]gtaaa|tttac[agt]ct',
    b'agg[act]taaa|ttta[agt]cct',
    b'aggg[acg]aaa|ttt[cgt]ccct',
    b'agggt[cgt]aa|tt[acg]accct',
    b'agggta[cgt]a|t[acg]taccct',
    b'agggtaa[cgt]|[acg]ttaccct',
)

SUBST = (
    (b'B', b'(c|g|t)'), (b'D', b'(a|g|t)'), (b'H', b'(a|c|t)'),
    (b'K', b'(g|t)'), (b'M', b'(a|c)'), (b'N', b'(a|c|g|t)'),
    (b'R', b'(a|g)'), (b'S', b'(c|g)'), (b'V', b'(a|c|g)'),
    (b'W', b'(a|t)'), (b'Y', b'(c|t)'),
)


def run_benchmarks(seq):
    ilen = len(seq)
    seq = re.sub(b'>.*\n|\n', b'', seq)
    clen = len(seq)
    results = []
    for f in VARIANTS:
        results.append(len(re.findall(f, seq)))
    for f, r in SUBST:
        seq = re.sub(f, r, seq)
    return results, ilen, clen, len(seq)


if __name__ == '__main__':
    n = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_INIT_LEN
    seq = init_benchmarks(n, DEFAULT_RNG_SEED)

    counts, ilen, clen, flen = run_benchmarks(seq)
    print("result: " + str(ilen) + " " + str(clen) + " "
          + " ".join(str(c) for c in counts) + " " + str(flen))

    t0 = time.perf_counter()
    run_benchmarks(seq)
    print(f"elapsed: {time.perf_counter() - t0:.6f} s")
