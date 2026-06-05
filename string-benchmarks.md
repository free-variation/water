# String benchmarks — current task (benchmark-driven development)

**This is the active workplan.** We implement logicforth's string layer —
regex, split/replace/match, UTF-8, JSON/YAML conversion, codecs — driven by
covering these CPython [pyperformance](https://github.com/python/pyperformance)
benchmarks one at a time. For each benchmark: build the primitives it needs,
port it to a `.l4` that produces the same output, measure against CPython, then
optimize until logicforth wins. Bench by bench, the goal is to **beat the
pyperformance number** for each.

Method per benchmark:

1. Identify the primitives it exercises (listed under each below).
2. Implement those primitives (C for anything that builds a result; `lib.l4`
   for scalar/boolean composables — see PLAN.md → "Functional primitives" for
   the dividing line).
3. Port the benchmark to `.l4` with matching output.
4. Measure: best-of-N wall clock vs `python3 -m pyperf` (or a plain timed run)
   on the same input size.
5. Optimize the hot path (the matrix work already shows the levers: `restrict`,
   manual unrolling, scoped `float_control` pragmas, avoiding per-element Val
   churn) until faster than CPython, then move on.

Source corpus: CPython's `pyperformance/data-files/benchmarks/`. Only
stdlib-only benchmarks are in scope. Excluded: the template/markup engines
(chameleon, django_template, mako, genshi, docutils, sphinx, html5lib, 2to3)
and the format parsers tomli_loads / yaml — all carry third-party dependencies
— and pyflate (deflate). `pyperf` is the suite's shared harness, not a
per-benchmark dependency, so it doesn't disqualify anything.

Format-conversion decisions (full rationale in PLAN.md):

- **JSON** — hand-roll a one-pass recursive-descent `json>frame` / `frame>json`
  in C. No vendored library; the grammar is tiny and we want exact control of
  the type mapping. jsmn was considered and rejected (covers only the
  structural scan; unescaping/number-conversion/validation stay ours anyway).
- **YAML** — vendor libyaml, static-linked, behind `yaml>frame` / `frame>yaml`.
  The spec is too large to hand-roll; libyaml is pure C with no system
  dependency. (`bm_yaml` itself is dep-excluded above, but the words are
  planned, so the decision lives here too.)

Suggested order: `bm_regex_dna` first (cleanest, exercises the core
`match`/`replace` surface at scale), then the JSON pair, then `bm_bpe_tokeniser`
(adds UTF-8), then `bm_regex_compile` (validates the pattern cache), with the
two partially-portable regex benchmarks and `bm_base64` last.

---

## Regex

### bm_regex_dna

`findall` (9 patterns) plus `sub` (11 replacements) over a generated
FASTA string: 100K characters by default, ~1M after the substitution
expansion. Patterns are alternation + character classes only.

Cleanest port target — every pattern is expressible in POSIX ERE, and it
exercises `match`/`replace` at scale.

- **Needs:** `match` (with all-matches iteration / a `findall`-style helper),
  `replace` (replace-all), the lazy-compile pattern cache.
- **Status:** not started.

### bm_regex_v8

The V8 JavaScript regexp benchmark ported to Python: ~85 real-world
patterns (URLs, cookies, user-agents, HTML, CSS selectors) over a
ROT13-encoded corpus, mostly `search` with some `sub` and `split`. Most
strings are under 1 KB; 11 blocks with decreasing iteration counts.

Partially portable. Some patterns use PCRE features POSIX ERE lacks
(`\d`, `\w`, lookaround) and would need rewriting or dropping.

- **Needs:** `match`, `replace`, `split`; plus a documented pattern-rewrite
  pass for the non-ERE patterns.
- **Status:** not started.

### bm_regex_effbot

`search` only, across alternation, capturing/non-capturing groups,
wildcards, character classes, and a backreference `(Python)\1`. Strings
are core tokens (Perl/Python/CSV-like) padded with 0–10,000 repeated
prefix/suffix characters to vary length.

Partially portable — the backreference is not expressible in POSIX ERE.

- **Needs:** `match`; the backreference case dropped or rewritten.
- **Status:** not started.

### bm_regex_compile

Repeatedly compiles the pattern set; measures compilation throughput,
not matching. Directly relevant to PLAN.md's lazy-compile-and-cache
`regex_t` design — a target for validating the pattern cache.

- **Needs:** the pattern cache (compile-on-first-use, bounded LRU).
- **Status:** not started.

---

## Other string / bytes (stdlib only)

### bm_json_loads

`json.loads` over assorted JSON strings. Target corpus for `json>frame`.

- **Needs:** `json>frame` (hand-rolled recursive-descent parser).
- **Status:** not started.

### bm_json_dumps

`json.dumps` over assorted nested objects. Target corpus for `frame>json`.

- **Needs:** `frame>json` (recursive `print_val`-style serializer).
- **Status:** not started.

### bm_bpe_tokeniser

A byte-pair-encoding tokeniser (tiktoken-style) trained on a bundled
`frankenstein_intro.txt`, then encode/decode repeatedly. Uses
`re.findall` / `re.compile` to split into word units, `encode("utf-8")`,
iterative byte-pair merges, and `decode("utf-8")`.

Exercises regex splitting plus UTF-8 byte handling together — close to
the planned `split` + UTF-8-codec surface.

- **Needs:** `match`/`split`, the UTF-8 codec (encode/decode/count/advance),
  and byte-level frame/array manipulation for the merge table.
- **Status:** not started.

### bm_base64

Encode/decode across base64, URL-safe base64, base32, base16, ascii85,
base85. Inputs 20 B to 1 MB, weighted so small sizes measure call
overhead and large sizes measure throughput.

A clean codec benchmark, no regex dependency. logicforth has no base64
words planned yet; covering this means adding a codec (`base64-encode` /
`base64-decode` at minimum). Lowest priority of the set.

- **Needs:** new base64 (and possibly base32/16/85) codec primitives.
- **Status:** not started.
