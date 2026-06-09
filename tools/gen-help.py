#!/usr/bin/env python3
"""Generate src/c/help_table.c from docs/reference.md.

Maintainer tool: run only when reference.md changes. The generated file is
committed, so the normal cc build needs no Python.

Contract: in any markdown table whose first column header is "Word", a row is
a word entry iff its first cell is a single bare backtick token (no spaces).
Column 2 is the stack effect (or, for superwords, the usage syntax), column 3
the one-line summary. Tables that also carry Ops/Alloc/O columns contribute
those three cost strings; tables without them leave the cost fields NULL.
"""

import re
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REFERENCE = os.path.join(ROOT, "docs", "reference.md")
OUTPUT = os.path.join(ROOT, "src", "c", "help_table.c")

# Split a table row on pipes that are not backslash-escaped.
ROW_SPLIT = re.compile(r"(?<!\\)\|")
# A bare single-token word cell: one backtick group, no internal whitespace.
WORD_CELL = re.compile(r"^`([^`\s]+)`$")


def clean(cell):
    text = cell.strip()
    text = text.replace(r"\|", "|")
    text = text.replace("`", "")
    return text.strip()


def split_row(line):
    parts = ROW_SPLIT.split(line.strip())
    # A pipe-delimited row has empty first and last fields; drop them.
    if parts and parts[0].strip() == "":
        parts = parts[1:]
    if parts and parts[-1].strip() == "":
        parts = parts[:-1]
    return parts


def is_separator(parts):
    return all(re.fullmatch(r":?-{2,}:?", p.strip()) for p in parts) and parts


def c_string(value):
    if value is None:
        return "NULL"
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return '"%s"' % escaped


def parse():
    entries = {}
    with open(REFERENCE, encoding="utf-8") as handle:
        lines = handle.read().splitlines()

    i = 0
    while i < len(lines):
        line = lines[i]
        if not line.lstrip().startswith("|"):
            i += 1
            continue

        header = split_row(line)
        # Need a header row, a separator row, then body rows.
        if i + 1 >= len(lines) or not is_separator(split_row(lines[i + 1])):
            i += 1
            continue

        i += 2  # past header + separator
        is_word_table = header and header[0].strip() == "Word"
        cost_columns = [h.strip() for h in header[3:6]] == ["Ops", "Alloc", "O"] \
            if len(header) >= 6 else False

        while i < len(lines) and lines[i].lstrip().startswith("|"):
            row = split_row(lines[i])
            i += 1
            if not is_word_table or len(row) < 3:
                continue
            match = WORD_CELL.match(row[0].strip())
            if not match:
                continue  # syntax/construct row, not a word
            name = match.group(1)
            effect = clean(row[1])
            summary = clean(row[2])
            if cost_columns and len(row) >= 6:
                ops = clean(row[3])
                alloc = clean(row[4])
                order = clean(row[5])
            else:
                ops = alloc = order = None
            if name in entries:
                sys.stderr.write("note: %r cross-listed; keeping first occurrence\n" % name)
                continue
            entries[name] = (name, effect, summary, ops, alloc, order)

    return sorted(entries.values(), key=lambda entry: entry[0])


def emit(entries):
    out = []
    out.append("/* Generated from docs/reference.md by tools/gen-help.py.")
    out.append("   Do not edit by hand; rerun the generator instead. */")
    out.append("")
    out.append('#include "logicforth.h"')
    out.append("")
    out.append("const HelpEntry help_entries[] = {")
    for name, effect, summary, ops, alloc, order in entries:
        fields = ", ".join(c_string(value) for value in (name, effect, summary, ops, alloc, order))
        out.append("\t{ %s }," % fields)
    out.append("};")
    out.append("")
    out.append("const int help_entry_count = %d;" % len(entries))
    out.append("")
    return "\n".join(out)


def main():
    entries = parse()
    with open(OUTPUT, "w", encoding="utf-8") as handle:
        handle.write(emit(entries))
    sys.stderr.write("wrote %d entries to %s\n" % (len(entries), OUTPUT))


if __name__ == "__main__":
    main()
