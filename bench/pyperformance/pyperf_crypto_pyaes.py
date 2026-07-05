"""
AES-128 CTR benchmark, the reference for crypto-pyaes.h2o. Mirrors the .h2o's
exact workload (same key, message, loop count, counter, checksum) so the
py / water comparison and verification line line up. Uses the pure-Python `pyaes`
module (pip install pyaes), as pyperformance bm_crypto_pyaes does.
"""
import sys
import time as _t

import pyaes

KEY = bytes(range(97, 113))            # b'abcdefghijklmnop' (bytes 97..112)
NBYTES = 8192
MSG = bytes((167 * i + 31) % 251 for i in range(NBYTES))


def encrypt(data):
    # default Counter starts at 1, matching the .h2o (counter = block index + 1)
    return pyaes.AESModeOfOperationCTR(KEY).encrypt(data)


def decrypt(data):
    return pyaes.AESModeOfOperationCTR(KEY).decrypt(data)


if __name__ == "__main__":
    loops = int(sys.argv[1]) if len(sys.argv) > 1 else 10
    ct = back = b""
    t0 = _t.perf_counter()
    for _ in range(loops):
        ct = encrypt(MSG)
        back = decrypt(ct)
    elapsed = _t.perf_counter() - t0
    print(f"roundtrip ok: {1 if back == MSG else 0}")
    print(f"checksum: {sum(ct)}")
    print(f"elapsed: {elapsed:.6f} s ({NBYTES} bytes x {loops})")
