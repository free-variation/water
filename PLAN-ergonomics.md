# Water — ergonomics batch

A mini plan: small, cold-path improvements to the daily experience, in
priority order. Each new word gets the full checklist (reference row,
README if user-facing, golden, both suites).

---

## Did-you-mean on unknown words

Suggest near misses in the unknown-word error: `unknown word: filtr (did
you mean filter?)`. Edit distance ≤ 2 over dictionary names and in-scope
locals, computed only on the error path (a linear dictionary walk is fine
there). Also improves LLM-driven sessions: the model corrects itself in
one round trip.

To settle: how many candidates to show when several tie (suggest one, or
`filter, filter!`); whether internal-flagged words are excluded.

---

## file:line in load-time errors

An error during `load` names the file and line: `prog.h2o:42: unknown
word: x`. The loader knows the filename; the line is a newline count over
the input buffer up to `input_buffer_pos` at `fail` time. Matches the
existing convention that filename contexts keep the filename prefix.

To settle: whether run-time errors raised while executing loaded
top-level code carry the location too, or only compile/resolve errors.

---

## Compile check: locals read but never assigned

Already planned in PLAN.md ("Compile check: locals read but never
assigned") — elevated by this batch; it catches the shadowing bug class
that produced frames>dataset's `keys` incident.

---

## -e 'code'

`water -e '3 4 + .'` runs the string as a program and exits; repeatable
and composable with program files in argument order. The scripting and
LLM-harness entry point.

To settle: whether -e implies -b (no banner) unconditionally.

---

## timed

`( xt -- … )` — run a quotation, print elapsed wall time, pass through
whatever it leaves on the stack. Library forth on `now`, ~5 lines
(core.h2o). Written by hand three times in one benchmarking session; the
need is proven.

---

## Dataset tabular preview

`( dataset n -- )` `head` — print the first n rows as an aligned table,
column names as the header, matrix/quantity/text columns rendered by
their existing cell printers. Datasets currently print as the raw column
frame, a wall at any size; matrices already solved this with the
corner-elided grid.

To settle: word name (`head` vs `peek-rows`); wide datasets (truncate
columns like the matrix corner print, or wrap); whether `head` also
answers the rows as a value or is print-only.

---

## load-tsv

`( path -- dataset )` — `read-tsv` + header `rows>dataset` as one word
(datasets.h2o). The first line of every data session.

To settle: header-row handling for headerless files (a `load-tsv-ext`
with the header flag, defaulting word on top, per the -ext convention).

---

## First-contact bundle

Three small kindnesses for the first minute of a new user's session:

- One banner line after the logo at the interactive REPL: how to list
  words, get help on one, and quit.
- Bare `help` (no following name) prints a short cheat sheet — the
  bracket families, `.s`/`clear`, `see`, `apropos` — instead of erroring.
- `quit` and `exit`-at-top-level hint at `bye` (exit is a control-flow
  word inside definitions; at the top level it can afford a hint).

To settle: exact cheat-sheet contents; whether the banner hint respects
-b only or also a quiet flag.
