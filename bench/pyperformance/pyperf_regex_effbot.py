#!/usr/bin/env python
"""
Standalone CPython side of pyperformance bm_regex_effbot (Fredrik Lundh's
regex benchmarks). The data-building functions are copied verbatim from the
pyperformance source; only the pyperf harness is replaced by a plain timing
main that also prints a match count for cross-checking the water port.
"""

import re
import time

USE_BYTES = False


def re_compile(s):
    if USE_BYTES:
        return re.compile(s.encode('latin1'))
    else:
        return re.compile(s)


def gen_regex_table():
    return [
        re_compile('Python|Perl'),
        re_compile('Python|Perl'),
        re_compile('(Python|Perl)'),
        re_compile('(?:Python|Perl)'),
        re_compile('Python'),
        re_compile('Python'),
        re_compile('.*Python'),
        re_compile('.*Python.*'),
        re_compile('.*(Python)'),
        re_compile('.*(?:Python)'),
        re_compile('Python|Perl|Tcl'),
        re_compile('Python|Perl|Tcl'),
        re_compile('(Python|Perl|Tcl)'),
        re_compile('(?:Python|Perl|Tcl)'),
        re_compile('(Python)\\1'),
        re_compile('(Python)\\1'),
        re_compile('([0a-z][a-z0-9]*,)+'),
        re_compile('(?:[0a-z][a-z0-9]*,)+'),
        re_compile('([a-z][a-z0-9]*,)+'),
        re_compile('(?:[a-z][a-z0-9]*,)+'),
        re_compile('.*P.*y.*t.*h.*o.*n.*')]


def gen_string_table(n):
    strings = []

    def append(s):
        if USE_BYTES:
            strings.append(s.encode('latin1'))
        else:
            strings.append(s)
    append('-' * n + 'Perl' + '-' * n)
    append('P' * n + 'Perl' + 'P' * n)
    append('-' * n + 'Perl' + '-' * n)
    append('-' * n + 'Perl' + '-' * n)
    append('-' * n + 'Python' + '-' * n)
    append('P' * n + 'Python' + 'P' * n)
    append('-' * n + 'Python' + '-' * n)
    append('-' * n + 'Python' + '-' * n)
    append('-' * n + 'Python' + '-' * n)
    append('-' * n + 'Python' + '-' * n)
    append('-' * n + 'Perl' + '-' * n)
    append('P' * n + 'Perl' + 'P' * n)
    append('-' * n + 'Perl' + '-' * n)
    append('-' * n + 'Perl' + '-' * n)
    append('-' * n + 'PythonPython' + '-' * n)
    append('P' * n + 'PythonPython' + 'P' * n)
    append('-' * n + 'a5,b7,c9,' + '-' * n)
    append('-' * n + 'a5,b7,c9,' + '-' * n)
    append('-' * n + 'a5,b7,c9,' + '-' * n)
    append('-' * n + 'a5,b7,c9,' + '-' * n)
    append('-' * n + 'Python' + '-' * n)
    return strings


def init_benchmarks(n_values=None):
    if n_values is None:
        n_values = (0, 5, 50, 250, 1000, 5000, 10000)

    string_tables = {n: gen_string_table(n) for n in n_values}
    regexs = gen_regex_table()

    data = []
    for n in n_values:
        for id in range(len(regexs)):
            data.append((regexs[id], string_tables[n][id]))
    return data


if __name__ == "__main__":
    data = init_benchmarks()

    matches = sum(1 for regex, string in data if regex.search(string))
    print(f"matches: {matches}")

    search = re.search
    t0 = time.perf_counter()
    for _ in range(1000):
        for regex, string in data:
            for _ in range(10):
                regex.search(string)
    print(f"elapsed: {time.perf_counter() - t0:.6f} s")
