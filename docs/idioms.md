# Telic idioms

Compositions that recur across the embedded library, the loadable libraries,
the benchmarks, and working analyses. Each entry is quoted from a real
definition, named in parentheses. The reference documents single words; this
documents how words combine.

## Open literals

`[`, `{`, and `[<` push a mark; `]`, `}`, and `>]` gather whatever the stack
holds above it. Anything may run in between — the delimiters bracket a
computation, not a notation. Each position's value is whatever the code there
left on the stack.

```forth
{ :year y :month m 1 + } date>epoch                 \ an expression as a frame value (days-in-month)
[ width text size - 0 max2 spaces text ] "" join    \ the pad computed inside the array (pad-left)
[ "curl" "-s" url "-H" "x-api-key: " "ANTHROPIC_API_KEY" env + "-d" body ]
                                                    \ an argv assembled mid-literal (lib/claude.telic)
[ maxx maxy maxz ] frame>json                       \ variables read into a result array (bench/float.telic)
```

A frame literal of computed columns is ordinary code:

```forth
panel
{ :log_product_dollars panel@product_dollars ln
  :has_pd panel@pd_bookings 0 >
  :tenure_capped panel@tenure dup 10 > 10 mesh
} merge to panel
```

## Code layout

The unit of layout is the **sentence**: one value's story from construction
through consumption to its destination — a `to name`, a store, a print, an
`expect=`. Nothing in the syntax marks it; the layout does.

- A short sentence is one line: `panel@adds_program mean dup 0.1724 0.0005 expect-near`.
- A long sentence breaks at major clauses, continuations indented — subject
  first, a quotation or key list on its own line, the verb and destination
  last:

  ```forth
  2019 2024 range
      [: panel@adds_program panel@fy rot addition-rate :]
      map vector to addition-rate-by-year
  ```

- A frame beyond about three keys goes vertical — one `:key value` per line,
  each value possibly a whole computation, the closing `}` on its own line:

  ```forth
  { :fy_2020 risk-panel@fy 2020 eq
    :fy_2021 risk-panel@fy 2021 eq
    :fy_2022 risk-panel@fy 2022 eq
  } merge
  ```

- A long symbol array likewise, one key per line between `[` and `]`.
- A definition is headed and paragraphed: the `( a b -- c ) \ summary` line
  above the `:`, guard clauses first (ending in `exit`), blank lines between
  the body's paragraphs — guard, work, result.
- An analysis file carries section banners — `\ ---- title ----` — each
  section a sequence of checked sentences.
- The head names what a word **receives**; its working values are declared
  where they are first assigned. A `to` on a free name makes a local, so the
  head shrinks to the inputs and a word that takes none writes no head at all.
  The opening bar is optional — `| data n-bins |` and `data n-bins |` are the
  same head, and the longer form is the one to write when the head shares a
  line with a stack comment:

  ```forth
  : histogram-table | data n-bins |
      data as-column drop-nans to data
      data num-elements to n-values
      data min to low
      data max to high
      high low - n-bins / to bin-width
      ...
  ```

- A name a sentence stores into must be free of the dictionary, because `to`
  on an existing word means that word: `to m` and `to ln` both fail, `m`
  being the metre unit and `ln` the natural logarithm. The short nouns are
  largely spoken for — `m` `s` `kg` `day` `week` are units, and `ln` `min`
  `max` `sum` `mean` `size` `count` `first` `last` are words — so a value
  takes a name that says what it holds: `price-column`, `daily-totals`. A
  local that does need a taken name is declared in the head, where the
  shadowing is deliberate and visible.

- A body that assigns a **global** names it `^global` in the head. `to`, `++`,
  `--`, `f++` and `f--` all refuse a bare global name, because the same name
  with no marker would have declared a local:

  ```forth
  variable tests-failed
  : record-failure | reason ^tests-failed |
      reason . cr
      ++ tests-failed ;
  ```

- A combinator argument that is one existing word is that word's xt, not a
  quotation around it: `' size sort-by`, not `[: size :] sort-by`. Write a
  quotation when the body is a literal, several words, or needs a head. The two
  are not only styled differently — a quotation is a compiled body the
  combinator dispatches into, an xt is called directly — and `fold-times`
  distinguishes them, taking a primitive combiner without touching the data
  stack and a quotation combiner as `( acc term -- acc' )`.

## Control structures compile

`if`/`else`/`then`, `begin`/`while`/`repeat`/`until`/`again`, and
`leave`/`continue` are compile-time words: they emit branch instructions into
the definition being compiled. They belong inside a `: … ;` or a `[: … :]`
body — at the top level of a file or the REPL there is no definition to emit
into, and the opener errors (`if: only valid inside a colon definition or
quotation`).

- A top-level conditional goes in a quotation, run on the spot:

  ```forth top-level-conditional
  variable checksum 7 to checksum
  [: checksum 7 = 0= if "checksum mismatch" throw then :] execute
  "checked" . cr
  ```
  ```output
  checked
  ```

- A top-level loop is `times`/`i-times` over a quotation, or a defined word.

- Definitions do not nest in branches: a `:` inside a top-level `if`/`then`
  (conditional definition) breaks for the same reason. Define the word
  unconditionally and branch inside it.

## The logic engine

Reach for unification early, not as a last resort: it is the shortest way to
destructure and test nested data, and backtracking search comes with it. The
machinery: `lvar` pushes a fresh logic variable, `| ?x |` declares one fresh
per call, `~` (`unify`) binds through the trail, `amb`/`fail` backtrack, and
facts are held in a dataset, so `query` (a pattern frame over columns),
`merge-by`, `aggregate` and the rest of the dataset words serve them. For the
data store itself, pick by fit: the embedded SQLite (`":memory:" db-open`) is
the better engine for large tables and multi-way SQL joins; the dataset wins
when the rows are already in hand, patterns carry logic variables, or the
result must compose with `map`/`filter`/`unify`. The two interoperate — a
`db-query` result is a dataset.

- Unify for its bindings: `~ drop` asserts a structural equation and keeps
  only the side effects. A variable buried anywhere in the term comes out
  bound:

  ```forth buried-bind
  lvar to A
  [ 1 2 3 4 5 ] [ 1 2 A 4 5 ] ~ drop
  A ? . cr
  ```
  ```output
  3
  ```

- A relation is clauses under `amb`: each clause is a quotation with fresh
  `?`-locals for its own variables, the arguments bound in by `ncurry`,
  alternatives tried in order. `[ H T rest ]` under unify is Prolog's `[H|T]`.
  Prolog's member, clause for clause:

  ```forth
  : lmember | X L |
    X L [: x l | l [ x _ rest ] ~ drop :] 2 ncurry
    X L [: x l ?T | l [ _ T rest ] ~ drop x T lmember :] 2 ncurry
    amb ;
  ```

- `choose` is n-way `amb` over an array — a backtracking iteration that
  commits to the first element the goal accepts:

  ```forth choose-commit
  [ 1 2 3 ] [: dup 2 < if fail then . cr :] choose
  ```
  ```output
  2
  ```

- `matches?` is the non-destructive test — unify, roll the trail back, answer
  a flag — so pattern tests compose in straight-line code; over an array of
  row frames it is a filter:

  ```forth
  [: row | pattern row matches? :] filter
  ```

- Keep a result past backtracking by snapshotting: `copy` (fresh variables)
  or `reify` (unbound variables become canonical `:_0`, `:_1`, … — ground,
  storable, comparable).

- Facts are a dataset and a query is a pattern frame over its columns: a
  ground value selects, a logic variable or `_` constrains nothing, and the
  rows come back as a dataset; `query-rows` answers them as frames so the
  variables bind row by row:

  ```forth dataset-query
  { :name [ :ann :bo ] :age [ 34 25 ] vector } to people
  people { :name :ann } query :age @ 0 @e . cr
  lvar to Age
  people { :name :bo } query-rows [: row | { :age Age } row ~ drop :] each Age ? . cr
  ```
  ```output
  34
  25
  ```

  Joins are `merge-by`, grouping `aggregate`, loading `rows>dataset` or
  `db-query`; several fact tables are a frame keyed by table name.

## Counted iteration and folds

`do`/`loop` is the counted loop inside a definition: the body compiles
inline, so it reads and writes the enclosing word's locals, and nested loops
read each index by name. `times`, `i-times`, and `fold-times` are the
quotation forms — for the top level, for an xt in hand, and for map-folds.

- Indexed fill — initialize a vector or array by index (the shape of
  bench/float.telic's `build-points`):

  ```forth indexed-fill
  variable xs
  4 1 0-matrix to xs
  : fill-xs
    0 4 1 do i
       i fsin to sxi
       xs sxi i !e drop
    loop ;
  fill-xs
  xs 1 @e . cr
  ```
  ```output
  0.841471
  ```

- Nested counted loops read both indices by name, and the loop's own
  accumulation writes the word's locals directly (bench/nbody.telic, `energy`):

  ```forth nested-do
  : upper-pairs
    0 to n_pairs
    0 4 1 do i
       i 1+ 4 1 do j  ++ n_pairs  loop
    loop
    n_pairs ;
  upper-pairs . cr
  ```
  ```output
  6
  ```

- Counted accumulation with a quotation in hand is `fold-times` — the
  accumulator never touches the data stack, and a primitive combiner runs
  with no dispatch:

  ```forth fold-times-sum
  0 [: dup f* :] ' f+ 5 fold-times . cr \ sum of squares 0..4
  ```
  ```output
  30
  ```

  The stack-accumulator form `0 swap [: + :] swap i-times` is the fallback
  when the body already leaves values.

- Fold a pairwise word whose identity is `null`: `hstack` and `vstack` answer
  the other operand for a `null`, so a list of blocks folds from a `null` seed
  with no first-element peel (lib/statistics.telic, `passive-columns`):

  ```forth null-seed-fold
  [ [ 1 ] vector [ 2 ] vector [ 3 ] vector ]
  null ' hstack reduce matrix>array . cr
  ```
  ```output
  [ 1 2 3 ]
  ```

  A word without such an identity peels the head: `dup 1 skip swap 0 @i xt reduce`.

- Chunked parallel sum: indices as the work list, one partial per worker,
  serial combine (bench/variants/leibniz-parallel.telic):

  ```forth
  0 chunks 1- range
  chunks 1 ' partial pmap-ext
  0.0 [: f+ :] reduce
  ```

## Higher-order traversal

`map`, `filter`, `reduce`, and `each` are the default way over a collection;
an explicit loop appears only when the body needs an index or writes the
enclosing word's locals. The derived family — `find-first`, `any?`, `all?`,
`sort-by`, `partition`, `flat-map`, `group-with`, `nmap` — keeps common
traversals to one word each (`find-first` and `any?` short-circuit).

- A named word passes by tick where a quotation would only wrap it:

  ```forth
  : print-raw string>codepoints ' emit each ;   \ repl.telic
  ' file-exists? find-first                     \ find-executable (io.telic)
  cells ' quantity? all?                        \ column-from-cells (datasets.telic)
  dataset values ' column>array map transpose   \ dataset-rows (datasets.telic)
  ```

- Map-then-reduce pipelines read as one sentence — the widest string in an
  array (`padded-column`):

  ```forth
  strings ' size map 0 ' max2 reduce to width
  ```

- `nmap` zips parallel arrays through an n-ary quotation:

  ```forth nmap-zip
  [ 1 2 ] [ 10 20 ] ' + 2 nmap . cr
  ```
  ```output
  [ 11 22 ]
  ```

- `each` is the side-effect traversal — the element is consumed, nothing is
  left:

  ```forth
  [ [ "echo" "a" ] [ "echo" "b" ] ] 2 parallel-run [: :out @ trim . :] each
  ```

## Mask algebra

Comparisons on matrix and array operands answer 1/0 masks; the algebra of
masks, `where`, and `select-rows` replaces row loops.

- Filter rows by value — mask, where, gather (`addition-rate`):

  ```forth
  : addition-rate eq where select-rows mean ;
  ```

- Conjunction is `*`, negation is `0 eq`:

  ```forth
  moves@has_assessment a eq
  moves@has_core c eq *
  moves@has_supplemental s eq *
  where
  ```

  ```forth
  rows@state "" eq 0 eq where    \ the complement: rows whose state is set
  ```

- One index vector, many gathers — compute the row set once, gather every
  parallel column with it:

  ```forth
  panel@fy 2023 <= where to train-rows
  X train-rows select-rows
  y train-rows select-rows
  ```

- Index of the first match, then fetch by it (`path-odds`):

  ```forth
  eq where 0 @e
  ```

- `mesh` is conditional replacement without a loop; the moves:

  ```forth
  dup nan? 0 mesh          \ fill missing with 0
  dup nan? 9999 mesh       \ missing → sentinel, so a comparison can run
  ```

  Bounds need no mask: `10 min2` caps at 10, `1e-10 max2` holds away from
  zero (fit-logistic-ridge), `0 1 clamp` bounds both sides — all element-wise
  on a matrix.

- Drop missing entirely: `drop-nans` on a vector (it is
  `dup nan? 0 eq where select-rows`), `complete-rows` on a dataset.

## Strings are regex

Searching, testing, splitting, and replacing take PCRE patterns: `has?`,
`index-of`, `split`, `replace`, `match`, `match-all`. Construction and
slicing have their own words — `format`, `join`, `+`, `substring`, the pad
family, the codepoint words — but wherever a string is examined, the pattern
is the argument, and one short regex usually does it:

```forth
browser "^/" has?                  \ absolute path? (env-browser)
dup "\.telic$" has? not if ".telic" + then \ ensure a suffix (load-library)
: basename "^.*/" "" replace ;     \ last path component (strings.telic)
: run " +" split start-process ;   \ tokenize on space runs (subprocess.telic)
"x=42" "(\w+)=(\d+)" match         \ parse by capture → [ "x=42" "x" "42" ]
model@predictors [: render "^:year=" has? :] filter
                                   \ keys by name pattern: render the symbol, then match
```

Anchor with `^`/`$` to test prefixes and suffixes.
The corollary: a pattern argument meaning a literal must escape regex
metacharacters — `"a.b" "." split` splits on every character, `"\." split`
on the dot.

## format

One word carries the text-building load, and its `{n}` placeholders index the
stack from the top, dropping the referenced positions when it runs.

- Report several values at once — pin them, then one template consumes both
  (dev_a4):

  ```forth
  program-adds mean dup 0.24142 0.0005 expect-near
  program-adds var dup 0.38089 0.0005 expect-near
  "program additions per account-year: mean {1:.3f} variance {0:.3f}" format print cr
  ```

- printf specs after the colon: `{0:.4f}` fixed precision, `{0:+.2f}` signed
  (dev_a4's marginal-effects table), `{0:04d}` zero-padded integer, `{0:8}`
  field width.

- Control characters: string literals are raw, so `{tab}` and `{nl}` are how
  tabs and newlines enter a string (README's taste block):

  ```forth
  1.5 250 "{0:d} ms{tab}{1:04.1f} s" format . \ 250 ms	01.5 s
  ```

- Ink directives style terminal output — `{red}`, `{bold}`, `{dim}`, reverted
  by `{plain}` — emitted only when stdout is a tty, so piped output stays
  clean (`help` renders its header this way):

  ```forth
  entry :effect @ entry :word @ "{bold}{0}{plain} {1}" format print-raw cr
  ```

- `"{0}" format` renders any one value to a string; feeding `string>symbol`
  synthesizes keys (`tsv-keys`):

  ```forth
  rows 0 @i size 1 swap range [: "col{0}" format string>symbol :] map
  ```

- Compose an error message, then throw it (`svd`):

  ```forth
  info if info "svd: dgesvd failed (info={0})" format throw then
  ```

## Quantities and units

Quantities compose through ordinary arithmetic; the idioms live at the
boundaries, and dates are the worked example — an instant is epoch seconds as
a quantity in `s`, so the units machinery is the date arithmetic.

- The boundary strip/attach pattern: dimension-blind code (a C primitive, a
  matrix kernel) sits behind a word that strips the unit going in — divide by
  one unit — and re-attaches it coming out — postfix the unit word
  (units.telic):

  ```forth
  : wall-now (wall-now) s ;
  : epoch>date 1 s / (epoch>date) ;
  ```

  `magnitude` is the polymorphic strip when the unit may vary
  (`dataset>matrix` applies it per column).

- Counting durations by division: instant minus instant is a duration, and
  dividing by one unit counts it (`days-in-month`):

  ```forth
  { :year y :month m 1 + } date>epoch
  { :year y :month m } date>epoch
  - 1 day /
  ```

  The same shape shifts dates — `wall-now 2 hour +` is an instant, and
  `date-shift` adds exact components as `delta :weeks 0 @or week +`.

- A bound compared against or applied to a dimensioned column carries the
  column's unit: `order@amount 0 $ >`, `amounts sum 1e-9 $ max2`. The
  ordering words, `max2`/`min2` and `clamp` reject a quantity
  against a plain number, and a different dimension, as errors; only `=`
  answers 0 across dimensions.

- Unit tests and transfers via `unit-of`: `unit-of 1 s =` detects an instant
  column (`column-type`'s `:datetime` branch); `x unit-of *` attaches one
  value's unit to another.

- Scaled subunit declaration — the minor unit as a rational fraction of the
  base, so same-dimension operands rescale automatically (`1 $ 50 ¢ +` is
  `1.5 $`):

  ```forth
  base unit $ 1 $ 100 / unit ¢
  ```

  lib/claude.telic prices calls this way: token counts times a ¢-per-token
  rate, answered as `¢`, printable in either unit.

## Dataset shaping

- Derive columns by merging a frame literal of expressions onto the dataset
  (the open-literals entry applied):

  ```forth
  panel
  { :log_dollars panel@dollars ln
    :has_pd panel@pd_bookings 0 >
  } merge to panel
  ```

- `filter` and `map` over a dataset are row-wise: the quotation receives each
  row as a frame and the result is a dataset again. A receiver named for the
  row makes `name@key` reads the predicate's nouns:

  ```forth
  orders
  [: order |
     order@year first-year >=
     regions order@region in?
     order@channel "direct" neq
     and and
  :] filter to training-orders
  ```

- Split-apply-combine is `aggregate`: the group keys (a symbol or symbol
  array), a quotation from the group dataset to one row frame, the key values
  written back into every row. Aggregates chain — a per-(product, year) table
  feeds a per-product one:

  ```forth
  [ :product :year ]
  [: group |
     { :due group@amount group@weight * sum
       :renewed group@renewed-amount group@weight * sum }
  :] aggregate
  [: :due @ 0 $ > :] filter
  dup :renewed @ over :due @ / :ratio !
  [ :product ]
  [: group |
     { :last-ratio group@ratio last
       :log-change-sd group@ratio 0.05 max2 ln successive-differences
                      dup size 2 < if drop 0.20 else std then 0.1 0.35 clamp }
  :] aggregate
  ```

- A join aligns its key by renaming, joins, then drops the unmatched rows:

  ```forth
  assignments [ :account :year :owner ] select-columns
  :year :due-year rename-key!
  [ :account :due-year ] :left merge-by
  [ :owner ] complete-rows
  ```

- The regression pipeline (the statistics library): keep the predictor
  columns that vary, expand each categorical present into indicator columns,
  and hand the dataset with its key array to the fit; `predict-glm` scores a
  dataset against the model frame:

  ```forth
  [: column-name column |
     predictors column-name in? if column zero-variance? not else false then
  :] filter-columns
  dup key-set :year in? if :year expand-indicators! then
  dup key-set :channel in? if :channel expand-indicators! then
  dup keys outcome weights quasibinomial-logit 0 glm-regression
  ```

  Scenario scoring toggles an indicator column and folds the predictions
  side by side, `null` seeding the fold because `hstack` answers the other
  operand for a `null`:

  ```forth
  year-columns 2 nlast
  null
  [: probabilities year-column |
     scoring-design 1 year-column repeat-column! drop
     model scoring-design predict-glm
     scoring-design 0 year-column repeat-column! drop
     probabilities hstack
  :] reduce
  row-means
  ```

- The checked pipeline: every materialization is pinned immediately with
  `expect=` / `expect-near`, so an analysis file is its own regression test:

  ```forth
  addition-panel n-rows 36443 expect=
  addition-panel@adds_program mean dup 0.1724 0.0005 expect-near
  ```

## Quotation context

How values reach a quotation body, beyond its own locals.

- Park-and-pick: leave context below a combinator's operands and read it at a
  documented depth; `nip` the leftovers after. The depth comment above the
  word is part of the idiom (`dataset>matrix`):

  ```forth
  \ the dataset arrives below cols, so the quotation reaches it at depth 2 under map
  [: 2 pick swap @ magnitude dup matrix? if as-column else vector then :] map nip
  ```

- Name the enclosing local and the quotation captures it — a copy, taken
  where the literal is evaluated, bound into a curried token the compiler
  builds for you (`scale-all`):

  ```forth capture-enclosing-local
  : scale-all | rows factor | rows [: factor * :] map ;
  [ 1 2 3 ] 10 scale-all . cr
  ```
  ```output
  [ 10 20 30 ]
  ```

  This is `factor [: factor | factor * :] curry map` with the plumbing
  removed; the per-element cost is the same. The copy is the point: `to`
  on the name inside the quotation makes a fresh local, and `++` on it is a
  compile error, since neither would reach the enclosing slot.

- Curry a fixed context into a mapped word (`tsv>db`):

  ```forth
  rows 1 skip db statement ' insert-row 2 ncurry map drop
  ```

  A curried token — hand-built or from a capture — costs about twice a bare
  quotation per call: each invocation pushes the bound values and dispatches
  through `execute`, where a bare quotation is dispatched straight into its
  fused body. In the hottest inner loop over a large array with a trivial
  body, prefer a quotation that captures nothing; capture or `curry` when a
  value must reach the body that `pick` cannot (an enclosing local, or
  context crossing into a parallel region), where the choice is
  expressibility, not speed. Build the token once and reuse it: a capturing
  literal inside a `do`/`begin` loop is rebuilt every iteration, and the
  compiler warns (`warning: quotation captures factor inside a loop; …`), so
  hoist it above the loop with `to`, as you would hoist a manual `curry`.

- Skeleton plus mapper injection: write the loop once taking a mapper xt;
  serial and parallel are one-line instantiations. Sound because each work
  cell is pre-curried and pre-seeded, so the mapper cannot change the result:

  ```forth
  : bootstrap ' map bootstrap-with ;
  : pbootstrap ' pmap bootstrap-with ;
  ```

- `>side … side>` carries a value across code that owns the stack: a handler
  across `catch` (`try-catch`), a shared FFI handle across a block of
  definitions (lib/statistics.telic), a key xt under a fold via `side-peek`
  (`group-with`).

- Extend a word by type without breaking early binding: capture the old xt in
  a constant, redefine with a type test in front (datasets.telic does this for
  `select-rows`, `dim`, `filter`, `map`):

  ```forth
  ' select-rows constant (matrix-select-rows) internal

  : select-rows
      over frame? if dataset-select-rows exit then
      (matrix-select-rows) execute ;
  ```

## Continuations and generators

`reset`/`shift`/`resume` are the substrate; exceptions, cleanup brackets, and
coroutines are short compositions over them (exceptions.telic, generators.telic).

- Resource brackets guarantee cleanup on both exits — the handler or resource
  rides the side stack across the unwind, which the return stack does not
  survive:

  ```forth resource-brackets
  : ensure >side catch side> execute if throw then ;
  ":memory:" [: "create table t(x)" [ ] db-exec . :] with-db cr
  "echo hi" run :out @ [: read trim print :] with-stream cr
  ```
  ```output
  0
  hi
  ```

- A generator is a word that `yield`s; drive it with `gen-take` (collect n
  values) or `gen-each` (consume until it falls off):

  ```forth generator-drive
  : odds 1 yield 3 yield 5 yield ;
  ' odds 3 gen-take . cr
  ' odds [: . :] gen-each cr
  ```
  ```output
  [ 1 3 5 ]
  1 3 5
  ```

- `start-generator` exposes the raw step for hand-driven iteration — the
  yielded value and a resumable continuation, `resume` for the next; the
  continuation is multi-shot, so a retained copy replays.

## Frames as records

- `@or` reads with a default in one probe — absent keys are ordinary, not
  errors (`help`, `date-shift`):

  ```forth
  entry :examples [ ] @or to examples
  delta :weeks 0 @or week +
  ```

- A path locator gets and sets through nesting, and `!` vivifies the
  intermediate frames:

  ```forth path-vivify
  { } 5 /a/b ! /a/b @ . cr
  ```
  ```output
  5
  ```

- A search path extracts from a tree in one call — `*` any child, `//` any
  depth, `[k>v]` predicates:

  ```forth search-path
  { :a { :n 1 } :b { :n 2 } } /*/n select-values . cr
  ```
  ```output
  [ 1 2 ]
  ```

- `null` is the answer for "nothing to fit" or "nothing to join": a word
  whose guard fails logs the reason and answers `null` through an early exit,
  the caller's `map` collects the nulls, and downstream `null?` guards or the
  `null`-identity of `hstack`/`vstack` absorb them:

  ```forth
  dup n-rows 200 < over :outcome @ zero-variance? or if
      product over n-rows
      "{1} skipped: {0} rows or one outcome" format :warn log
      drop null exit
  then
  ```

- Key arithmetic runs through sets — difference, then back to an array
  (`ordered-columns`):

  ```forth
  dataset keys array>set leading-columns array>set difference set>array
  ```

  and the uniformity test is a set collapse (`frames>dataset`):

  ```forth
  rows ' keys map array>set size 1= not if
      "rows have differing keys" throw
  then
  ```

## Numeric kernels

The register for hot loops: locals, unsafe f-words, flat vectors — the shapes the
compiler's fusion targets.

- Gather–compute–writeback: hoist reads into locals, run fused arithmetic,
  store with `!e drop` (bench/float.telic, `normalize-points`):

  ```forth
  xs i @e to xi ys i @e to yi zs i @e to zi
  xi xi f* yi yi f* f+ zi zi f* f+ fsqrt to norm
  xs  xi norm f/  i !e drop
  ```

- In-place matrix chains avoid allocation in an iteration
  (bench/variants/mandelbrot-matrix.telic, `step`):

  ```forth
  zi zr *! 2.0 *! c-imag +! drop
  ```

- Branch-free mask accumulation — accumulate a condition instead of testing
  per element:

  ```forth
  escaped zr2 zi2 + 4.0 > +! drop
  ```

- Coordinate grids as rank-1 products, through the statistics library's BLAS
  `dgemm` (`setup`, same file):

  ```forth
  1.0 ones-col cr-row 0.0 n n 0-matrix dgemm-nn
  ```

- Bulk field unpack and writeback: `spread` the record array onto the stack
  and receive every field into a kernel word's locals in one head; `to-slice!`
  stores several values back in one call (bench/nbody.telic, `pair-force`):

  ```forth
  b1 spread  b2 spread  b1 b2 dt pair-kernel
  vx1 vy1 vz1 3 b1 3 to-slice! drop
  ```

- Verify the fusion, don't assume it: `' word see-compiled` shows the
  compiled cells — fused ops like `(lf+)`, `(ll*0!)`, `(=0branch)` confirm
  the loop compiled tight — and `timed` settles what the disassembly leaves
  open:

  ```forth fusion-check
  : sc-demo 1.5 2.5 f+ ; ' sc-demo see-compiled
  ```
  ```output
  : sc-demo   \ 5 cells
   0: (lit) 1.5
   2: (lf+) 2.5
   4: exit
  ;
  ```

## Writing tests

The test vocabulary (test.telic) rides on `catch`/`throw`: an assertion throws
on failure, `test` catches and tallies, `test-report` throws at the end when
anything failed — so a test file run as a program exits non-zero.

- One assertion word per claim shape:

  ```forth
  flag expect                        \ truthy, else "expectation was false"
  actual expected expect=            \ deep structural =; throws "expected X, got Y"
  actual expected tolerance expect-near \ |actual − expected| ≤ tolerance;
                                     \ matrix/vector operands: largest element-wise gap
  xt expect-throws                   \ passes iff the quotation throws
  ```

- Group claims with `test` — a name and a quotation; the stack is restored and
  the run continues past a failure — and bracket the file with
  `new-tests`/`test-report`:

  ```forth test-group
  new-tests
  "adds" [: 3 4 + 7 expect= :] test
  "rejects a string count" [: [: [ 1 ] "x" ' + reduce :] expect-throws :] test
  test-report
  ```
  ```output
  ok adds
  ok rejects a string count
  2 passed, 0 failed
  ```

- Pin values as they materialize — `dup … expect-near` asserts and keeps the
  value for the sentence that follows, so an analysis file is its own
  regression test (dev_a4):

  ```forth
  program-adds mean dup 0.24142 0.0005 expect-near
  program-adds var dup 0.38089 0.0005 expect-near
  "program additions per account-year: mean {1:.3f} variance {0:.3f}" format print cr
  ```

- The same words serve as preconditions inside production definitions —
  `expect` throws with its message on a violated assumption:

  ```forth
  origin-year 2015 >= expect
  month 1 12 between? expect
  ```

- Seed anything random first — `42 seed` — so expected values are exact, and
  the vector form of `expect-near` compares whole results at once:

  ```forth
  addition-rate-by-year
  [ 0.1363 0.1300 0.1759 0.1693 0.1483 0.2250 ] vector 0.0005 expect-near
  ```

## External systems

- Subprocess capture: `run-result :out @ trim`.
- SQL is the loading layer: a string literal spans lines, so the query is
  written as SQL; one word binds and runs it inside `with-db`, so the
  connection closes on either exit:

  ```forth
  : query-db-bound | sql params |
      db-path sql params ' db-query 2curry with-db ;
  ```

  `format` substitutes values into the text (`cutoff@year sql format`),
  leaving SQLite's `?1 ?2` binds for the parameter array; the two coexist in
  one query because `format` only rewrites `{n}`. A query fragment kept in a
  global becomes a CTE the same way — `"WITH orders AS ({0}), …" format`.
  Units attach at the boundary, directly after the load:

  ```forth
  query-at-year
  ' $ :amount set-unit!
  ```

- Transaction bracket (`tsv>db`):

  ```forth
  db "BEGIN" [ ] db-exec drop
  rows 1 skip db statement ' insert-row 2 ncurry map drop
  db "COMMIT" [ ] db-exec drop
  ```

- Retry-then-rethrow (lib/claude.telic, `elicit-with-retries`):

  ```forth
  begin
      messages ' try-call curry catch
      0 = if exit then
      -- attempts
      attempts 1 < if throw then
      drop
  again
  ```

- Fallback chain over `null` — try sources in order, each `dup null?` guard
  either exits with the hit or drops and falls through (`xgb-lib-path`,
  `env-browser`):

  ```forth
  "XGBOOST_LIB" env dup null? not if exit then drop
  install-paths [: file-exists? :] find-first
  dup null? if drop "libxgboost.so" then
  ```

- The LAPACK call shape: `copy` the inputs (LAPACK overwrites its arguments),
  `matrix>pointer` each operand, out-parameters as segments, then check
  `info` (`fit-linear`):

  ```forth
  mat copy to a
  1 int-segment to rank
  ...
  (dgelsd) to info
  info if info "fit-linear: dgelsd failed (info={0})" format throw then
  ```
