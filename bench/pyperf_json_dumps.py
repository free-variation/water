"""Standalone harness version of pyperformance's bm_json_dumps.

The data builders (EMPTY/SIMPLE/NESTED/HUGE, CASES, bench_json_dumps) are
copied verbatim from the pyperformance reference; only the pyperf Runner is
replaced with a plain timed loop. Prints a `bytes:` line (total canonical
length, sort_keys=True + ensure_ascii=False — logicforth's `frame>json` emits
raw UTF-8 rather than \\uXXXX-escaping non-ASCII, so the verification measures
against that same form; the timed run still uses the default json.dumps) and an
`elapsed:` line over LOOPS passes of the measured phase.
"""

import json
import time


EMPTY = ({}, 2000)
SIMPLE_DATA = {'key1': 0, 'key2': True, 'key3': 'value', 'key4': 'foo',
               'key5': 'string'}
SIMPLE = (SIMPLE_DATA, 1000)
NESTED_DATA = {'key1': 0, 'key2': SIMPLE[0], 'key3': 'value', 'key4': SIMPLE[0],
               'key5': SIMPLE[0], 'key': 'ąćż'}
NESTED = (NESTED_DATA, 1000)
HUGE = ([NESTED[0]] * 1000, 1)

CASES = ['EMPTY', 'SIMPLE', 'NESTED', 'HUGE']

LOOPS = 250


def bench_json_dumps(data):
    for obj, count_it in data:
        for _ in count_it:
            json.dumps(obj)


def total_bytes():
    total = 0
    for case in CASES:
        obj, _ = globals()[case]
        total += len(json.dumps(obj, sort_keys=True, ensure_ascii=False).encode())
    return total


if __name__ == '__main__':
    print(f"bytes: {total_bytes()}")

    data = []
    for case in CASES:
        obj, count = globals()[case]
        data.append((obj, range(count)))

    t0 = time.perf_counter()
    for _ in range(LOOPS):
        bench_json_dumps(data)
    print(f"elapsed: {time.perf_counter() - t0:.6f} s")
