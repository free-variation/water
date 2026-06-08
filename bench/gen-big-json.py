"""Generate a huge, deep, varied JSON file for load-testing json>frame / frame>json.

Deterministic (fixed seed). Stresses every dimension the parser/serializer touches:
  - deep nesting (under logicforth's JSON_MAX_DEPTH = 1024 guard)
  - a very wide flat array (hundreds of thousands of mixed-type elements)
  - a very wide object (many distinct keys -> exercises symbol interning + sort)
  - multi-megabyte strings, with embedded escapes (" \\ newline tab) and raw UTF-8
  - varied numbers: ints, negatives, decimals, exponents, large-but-exact ints
  - a bulky record tree with REUSED key names (so symbols dedup, pool stays bounded)

Output is valid JSON (ensure_ascii=False -> raw UTF-8, which json>frame reads and
frame>json reproduces). Tune the SCALE_* knobs; the script prints the final size.

    python3 bench/gen-big-json.py [output_path]
"""

import json
import random
import sys

random.seed(20260608)

# ---- size knobs -----------------------------------------------------------
DEEP_LEVELS   = 800        # nested-object chain depth (< 1024 parse guard)
WIDE_ARRAY    = 300_000    # elements in the flat mixed array
WIDE_OBJECT   = 12_000     # distinct keys in one object (must fit symbol pool)
BIG_STRINGS   = 6          # number of multi-MB strings
BIG_STR_BYTES = 4_000_000  # approx bytes per big string
NUMBERS       = 120_000    # elements in the numbers array
RECORDS       = 120_000    # records in the bulk list

UNICODE = "café — naïve — Ω≈ç√ — 日本語 — \U0001f600\U0001f680"  # raw multibyte + astral


def deep_chain(levels):
    node = {"leaf": True, "depth": levels}
    for level in range(levels):
        node = {"next": node, "depth": level, "tag": f"level-{level}"}
    return node


def mixed_element(i):
    pick = i % 6
    if pick == 0:
        return i
    if pick == 1:
        return -i * 1.5
    if pick == 2:
        return f"elem-{i}"
    if pick == 3:
        return i % 2 == 0
    if pick == 4:
        return None
    return {"i": i, "sq": i * i}


def big_string(approx_bytes, salt):
    # repeated chunk laced with characters that force escaping + raw UTF-8
    chunk = (f'chunk-{salt}: quote=" backslash=\\ tab=\t newline=\n '
             f'unicode={UNICODE} ')
    reps = approx_bytes // len(chunk.encode()) + 1
    return chunk * reps


def record(i):
    # reused key names -> these symbols intern once and dedup
    return {
        "id": i,
        "name": f"record-{i}",
        "active": i % 3 == 0,
        "score": (i % 1000) / 7.0,
        "tags": [f"t{i % 50}", f"t{(i + 1) % 50}"],
        "note": None if i % 5 else f"note for {i} {UNICODE}",
        "nested": {"a": i, "b": -i, "c": {"d": i % 100}},
    }


def build():
    return {
        "meta": {
            "generator": "gen-big-json.py",
            "seed": 20260608,
            "unicode_sample": UNICODE,
            "escapes": 'tab\tnewline\nquote"backslash\\slash/',
        },
        "numbers": [
            random.choice([
                random.randint(-10**9, 10**9),
                random.uniform(-1e6, 1e6),
                random.randint(0, 2**52),          # large but exact in a double
                random.choice([0, -0.0, 1, -1]),
            ])
            for _ in range(NUMBERS)
        ],
        "wide_array": [mixed_element(i) for i in range(WIDE_ARRAY)],
        "wide_object": {f"key_{i:05d}": i for i in range(WIDE_OBJECT)},
        "deep": deep_chain(DEEP_LEVELS),
        "big_strings": [big_string(BIG_STR_BYTES, s) for s in range(BIG_STRINGS)],
        "records": [record(i) for i in range(RECORDS)],
    }


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "bench/big.json"
    data = build()
    with open(out, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False)
    import os
    size = os.path.getsize(out)
    print(f"wrote {out}: {size:,} bytes ({size / 1e6:.1f} MB)")


if __name__ == "__main__":
    main()
