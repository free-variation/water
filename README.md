# logicforth

A Forth-flavored language for matrix work, set/array manipulation,
string/regex processing, and logic programming. A compact C
interpreter built with `clang -O3`.

## Building and running

```
make           # builds ./logicforth
make test      # runs the golden-output test suite
./logicforth   # REPL
```

Self-contained: its vendored dependencies — PCRE2 (regex), isocline (REPL line
editing), and SQLite (embedded SQL) — live under `external/` and are built from
source into the binary, so `make` needs nothing but a C compiler. Refresh them
with `make vendor-pcre2`, `sh tools/vendor-isocline.sh`, and
`sh tools/vendor-sqlite.sh` (see each directory's `PROVENANCE`).

## A taste

```forth
\ Arithmetic
3 4 + .                                 \ 7

\ Matrices: * is element-wise; matrix multiply is dgemm (αAB + βC)
[ 1 2 3 4 ] 2 2 matrix dup transpose *  \ element-wise product of M and Mᵀ

\ Sets and set algebra
< 1 2 3 > < 2 3 4 > + .                 \ < 1 2 3 4 >  (union via polymorphic +)

\ Set-builder { x² | x ∈ 1..10, even x } — literal + filter/map + destruct
< 1 10 range [: 2 mod 0= :] filter [: fsq :] map destruct > .   \ < 4 16 36 64 100 >

\ Frames — symbol-keyed nested maps
{ :a 1 :b { :c 2 } } /b/c @ .           \ 2

\ Path queries — * (any child), // (any depth), [pred] filters
{ :a { :n 1 } :b { :n 2 } } /*/n select-values .   \ [ 1 2 ]

\ JSON: parse to frames/arrays, serialize back
"[1, 2, 3]" json>frame frame>json .     \ [1, 2, 3]

\ Higher-order operations
[ 1 2 3 4 5 ] [: dup * :] map .         \ [ 1 4 9 16 25 ]

\ Strings and regex (PCRE2)
"x=42" "(\w+)=(\d+)" match .            \ [ "x=42" "x" "42" ]
"hello world" "o" "0" replace .         \ hell0 w0rld

\ Exceptions
[: "missing" throw :]
[: "got " . . cr :] try-catch           \ prints "got missing"

\ Coroutines via delimited continuations
: yield shift ;
: producer 1 yield 2 yield 3 ;
reset producer                          \ leaves (1, k) — next value via resume

\ Logic: unify binds variables; amb is a committed choice
[ 1 2 3 ] [ X Y Z ] ~ drop  X $ . Y $ . Z $ . cr   \ 1 2 3
[: fail :] [: "fallback" :] amb .                  \ fallback
```

## Features

### Core language

- **Tagged Vals** — floats, strings, symbols, sets, arrays, cons pairs, frames, matrices, execution tokens, dictionary addresses, continuations, logic variables, process streams, internal marks. A single 8-byte NaN-boxed representation; the tag determines interpretation.
- **Direct-threaded inner interpreter** — each dictionary cell is a handler function pointer, dispatched by an indirect tail call (`musttail`); a colon call, literal, or branch carries its operand in the cell(s) right after the handler. The dictionary *is* the threaded code — no separate bytecode.
- **Compile-time instruction fusion** — adjacent variable-reads and float ops collapse into single instructions (`var var f+` → one op; `… var f+!` fuses the store), `f*+` / `f*-` are fused multiply-add/subtract, and a comparison immediately before a branch (`= if`, `gt while`, `0= until`) fuses into a single compare-and-branch op. Variable-fused float words (`vf+`/`vf*`/… on one named variable, `vvf+`/`vvf*`/… on two) collapse the variable load into the float op.
- **Program and execution state separated** — the dictionary, symbol pool, and object heap live in global structures (`Vocabulary`, `Compiler`, `Arena`) that are read-only during a run; the per-run mutable state — the three stacks, instruction pointer, locals, and GC roots — lives in an `Interpreter`, so one program can be shared across multiple execution contexts.
- **Three stacks** — data, return, and a side stack for stashing values that mustn't sit on the other two.
- **Colon definitions** — `: name body ;`. The body is captured as source text for `see` and the text-form `save`.
- **Anonymous quotations** — `[: ... :]` pushes a fresh xt. Works at top level and inside colon defs.
- **Control flow** — `if`/`else`/`then`, the `begin`/`until`/`again` and `begin`/`while`/`repeat` loops, counted `times` / `i-times`, `exit`, and `>r`/`r>`/`r@` for return-stack access.
- **Tick and execute** — `' word execute` for first-class invocation by name.
- **`forget`** — truncate the dictionary back to a named word; symbol identities survive.
- **Variables and symbols** — `variable foo` declares a global; read it by bare name, assign with `42 to foo` (`to` also auto-creates a global on first assignment at the REPL). `symbol bar` defines a symbol; `:foo` is a symbol literal interned on use; `string>symbol` interns a computed string.
- **Word-local variables** — `| x y |` at the head of a colon definition or quotation declares scoped slots (initialized to `0.0`); read by bare name, assign with `to name`. `++ name` / `-- name` increment/decrement a local in place (`f++` / `f--` the unsafe float-only forms). Locals nest through quotations and survive continuation capture.
- **Mark-and-sweep GC** — walks data/return/side stacks, dictionary, and a small `gc_roots` array for in-flight C-level temporaries.

### Numeric / matrix

- **Polymorphic arithmetic** — `+`/`-`/`*`/`/` dispatch on operand tags: floats compute, strings concatenate (`+`), sets union/difference/intersection, matrices element-wise, a scalar broadcasts over a matrix, and arrays concatenate (`+`).
- **In-place matrix ops** — `+!`/`-!`/`*!`/`/!` mutate the left matrix in place (explicit; the programmer decides). Float-only fast paths (`f+`, `f-`, `f*`, `f/`, `f^`, …) skip the type dispatch when both operands are known floats.
- **Matrix construction** — `R C 0-matrix` (zeros), `[ ... ] R C matrix`, `V N diagonal-matrix` (N×N with V on the diagonal), `N identity-matrix`, `start end step matrix-range` (a 1×N row over a stepped range).
- **DGEMM** — `dgemm-nn`/`tn`/`nt`/`tt` (`αAB + βC`) for all four transpose variants. The non-transposed `nn` path is ikj-ordered with `restrict` pointers for cache-friendly access; the transposed variants use a straightforward triple loop.
- **Indexing** — `@i`/`@j`/`@i,j` to read rows, columns, or single cells.
- **Shape** — `dim`, `reshape`, `flatten`, `transpose`, `diagonal`.
- **Reductions** — `sum`, `row-sums`, `column-sums`, `max`, `min`, `row-maxes`, `row-mins`, `column-maxes`, `column-mins`. Library `mean`, `row-means`, `column-means` on top.
- **Element-wise math** — `abs`, `sqrt`, `exp`, `log`, `sin`, `cos`, `tan`, `tanh`. Polymorphic over floats and matrices.
- **Total ordering** — `=`/`lt`/`gt` compare matrices by shape then row-major contents, so matrices work as set members.

### Sets, arrays, higher-order

- **Set literals** — `< 1 2 3 >`, set operations, `member?`, `size`, in-place `set-add!`/`set-remove!`, and `array>set` (sort-and-dedup an array into a set in one pass).
- **`group-by`** — `array :col group-by` groups frames by a symbol field into a frame from each value to a set of rows (the engine behind fast indexing and aggregation).
- **Array literals** — `[ 1 2 3 ]`, the `array` constructor (gather N from the stack), `array-of` (fill), `range` ( from to -- arr ) for an ascending or descending integer sequence, indexed access via `@i`.
- **Map, fold, zip-map, filter** — `map` for a single source, `reduce` for a left fold with an accumulator, `mapn` for N-ary zip, `filter` to select by predicate, with anonymous quotations as the higher-order argument.
- **Destructuring** — `destruct` spreads a set/array/frame's elements onto the stack (a frame as alternating symbol/value). `destruct-to` ( values names -- ) takes two equal-length arrays and assigns each value to the global variable named by the corresponding symbol, creating it if absent.
- **In-place slicing** — `slice!` copies a strided run from one array into another, `reverse-slice!` reverses a run in place, `to-slice!` stores values from the stack into a range.

### Multi-core parallelism

Worker threads over one shared object heap (not `fork`): a quotation runs across the collection on several cores, results joining back by handle with no copy.

- **`pmap`** — `( arr xt -- arr )` parallel `map`; **`pfilter`** — `( arr pred -- arr )` parallel `filter`, order preserved; **`pmap-reduce`** — `( arr id map-xt combine-xt -- val )` fused parallel map+fold, with `combine-xt` associative and `id` its neutral element.
- **`-ext` forms** — `pmap-ext` / `pfilter-ext` / `pmap-reduce-ext` take an explicit worker count and items-per-claim; the bare forms default to `num-cores` workers.
- **`num-cores`** — online CPU count.

### Frames

Symbol-keyed nested maps — the associative type, and the compound term the logic layer builds on. The three bracket families are distinct: `[ ]` arrays, `{ }` frames, `< >` sets.

- **Literals** — `{ :a 1 :b 2 }`; values may be any Val, including nested frames, arrays, and sets.
- **Builders** — `frame` ( keys values -- frame ) from two parallel collections, `array>frame` ( kv-array -- frame ) from an alternating key/value array, and `frame>array` ( frame -- kv-array ) the inverse, flattening to a key-sorted alternating array.
- **Path literals** — `/a/b/c` is a symbol array `[ :a :b :c ]`, built once at compile time, used to address into the tree — and usable as a key when constructing a frame (`{ /a/b/c v }` / `array>frame`), where it vivifies nested frames. A path may also be a *search* pattern: `*` matches any child at that level, `//` matches at any depth (descendant-or-self), and `[…]` filters by predicate (`[city=:NYC]`, `[age>30]`, `[.>0]` on the node itself, `[addr/zip]` on a sub-path).
- **Access** — `@` ( frame key/path -- value ) get, `!` ( frame key/path value -- frame ) set with auto-vivified intermediates, `has?` existence test, `delete-at` remove, `update-at` apply a quotation to a leaf, `merge` combine two frames (right wins), plus `keys` / `values` / `size`. The single-location words (`@`, `!`, `delete-at`, `update-at`) take a `:symbol` key or a plain `/a/b/c` locator and reject a search pattern; `has?` accepts either, answering whether any node matches.
- **Path queries** — `select-values` ( frame pattern -- array ) returns every value matched by a `*`/`//`/predicate search pattern, in document order; `select-keys` returns the full root-to-match path for each match (each round-trips back through `@`). Convert the result with `array>set` for distinct values or `array>cons` to feed matches to `choose`.
- **Representation** — sorted parallel key/value arrays with binary-search lookup; mutable in place, reference semantics. Structurally comparable, so frames work as set members and round-trip through their `{ }` literal.

### Strings and regex

- **String literals** are raw (newlines allowed; `""` is the one escape → a literal `"`); **`format`** fills `{n}` placeholders from the stack — `"got {0} of {1}" format`; **polymorphic concatenation** via `+`.
- **Regex** on PCRE2 (Perl-compatible, JIT-compiled): `match` (first match as a flat `[ whole cap… ]`), `match-all` (all matches, nested), `replace` (replace-all, with `&` / `\1`–`\9` backrefs), and the `has?` string overload (does the pattern match?). Patterns are plain `"..."` literals — PCRE2 reads `\d`, `\w`, `\n`, lookaround, `\p{...}`.
- **Slicing / building** — `substring` (half-open byte range), `split` (split at each non-overlapping match of a pattern, empty fields kept), `join` (concatenate an array of strings with a separator).

### JSON

- **`json>frame`** — parse a JSON string into native values: objects → frames (keys interned as symbols), arrays → arrays, strings → strings (escapes and `\uXXXX` decoded to UTF-8), numbers → floats, `true`/`false` → the reserved `:1`/`:0` boolean symbols, `null` → `null` (the none value). Recursive-descent, GC-safe, rejects trailing garbage.
- **`frame>json`** — serialize a value back to a JSON string: floats use a shortest round-trip representation, strings are escaped, `:1`/`:0` → `true`/`false`, none → `null`.

### I/O and persistence

- **Interactive REPL** with full isocline line editing: theme-adaptive **syntax highlighting**, **matching-brace** highlighting, **inline hints** and **Tab completion** (word names from the live dictionary, filenames inside string literals), persistent history (`.logicforth_history`), and **multi-line editing** — `Ctrl+J` inserts a line, `Enter` submits the whole buffer. A `count|top` prompt shows stack depth and the top value, green on a terminal, red on error. `.` pretty-prints a nested array across lines with the opening brackets aligned; strings print quoted inside a collection and in `.s`, raw when printed bare.
- **`load`** runs a source file as if typed.
- **`save`** writes the user's vocabulary as a re-loadable `.l4` source file.
- **`save-image`** / **`load-image`** — binary image with full state preservation (dictionary, objects, stacks, continuations).
- **`reload`** truncates user state and re-runs every file `load`ed this session, in order.
- **`read-file`** / **`write-file`** / **`append-file`** — read a whole file as one (byte-safe) string; write or append a string's bytes to a path.
- **`env`** / **`env!`** — read an environment variable as a string (the none value if unset) and set one (process-wide, so `start-process` children inherit it).

### Subprocesses and pipes

Drive external programs over pipes (`fork`/`execvp`/`pipe`/`waitpid`; binary-safe, no shell):

- **`argv start-process`** — launch from an argv array; returns a frame `{ :pid :in :out :err }` with the child's pid and its stdin/stdout/stderr as `T_STREAM` values.
- **`write`** / **`read`** / **`close`** — write a string to a stream, read a stream to EOF, close one (closing `:in` sends EOF).
- **`running?`** / **`wait`** / **`stop`** — non-blocking liveness check, block-until-exit, signal-and-reap.
- `lib.l4` conveniences: **`run`** (split a command line and start it), **`read-out`** / **`read-err`** / **`write-in`**.
- **`commands width parallel-run`** — run a batch of argv arrays concurrently, at most `width` at a time, collecting `{ :out :err :status }` per command in input order (refills a slot as each child finishes). Process-level parallelism — e.g. firing off many `curl` requests at once.

### SQLite

Embedded relational storage via the vendored SQLite amalgamation — built into the binary, no external dependency. A database is a `T_DB` handle.

- **`db-open`** / **`db-close`** — open (creating if absent, or `":memory:"` for an in-memory DB) and push a handle; close frees the connection and is idempotent.
- **`db-exec`** — `( db statement params -- n )` — run an INSERT/UPDATE/DELETE/CREATE with `params` bound to its `?` placeholders; returns the affected-row count (0 for DDL).
- **`db-query`** — `( db query params -- rel )` — run a query; returns a fact-database relation `{ :rows <bag of row frames> :index { } }`, each row keyed by column-name symbols (INTEGER/REAL → float, TEXT → string, NULL → `null`, BLOB → raw bytes). Duplicates are kept, in result order; the result drops straight into `query` / `inner-join`.
- **Bound parameters** — `params` is an array bound positionally to the `?` placeholders (`[ ]` for none); floats, strings, symbols, and `null` bind, so string values need no hand-escaping.
- **`create-index`** — `( rel cols -- rel )`, `lib.l4` — index a query result on `cols`, interning those columns to symbols so the fact-db index and `query` can use them.

### Delimited continuations

A four-primitive substrate the rest of the control story is built on. See `docs/continuations.md` for the full treatment.

- **`reset`** — installs a delimiter (a uniquely-tagged mark on the return stack).
- **`shift`** — captures the slice up to the nearest reset, removes the mark and captured frames, pushes the continuation as a `T_CONT` Val. Used for coroutines and generators.
- **`shift-with`** — same capture, but runs a handler xt in the outer context after the unwind. Used for exceptions and restarts.
- **`resume`** — re-enters a captured continuation. Multi-shot.

### Generators

Coroutines on the continuation primitives, in `lib.l4`:

- **`yield`** — emit a value to the driver and suspend until resumed.
- **`start-generator`** — run a producer to its first `yield`, leaving the yielded value and a resumable continuation.
- **`gen-take`** — collect the first N values a producer yields into an array; **`gen-each`** — run a consumer on each yielded value until the producer falls off.

### Side stack

A third stack for stashing arbitrary Vals without disturbing the data or return stack: **`>side`**, **`side>`**, **`side-drop`**, **`side-depth`**.

### Exceptions (library)

Built in `lib.l4` on top of the continuation primitives:

- **`throw`** — non-local exit with a value.
- **`catch`** — wraps an xt; returns `(result 0)` on success, `(exc 1)` on throw.
- **`try-catch`** — wraps an xt with a recovery handler. Arity-agnostic.

The `shift-with` handler can also resume the captured continuation, giving the Common Lisp restart pattern — exceptions can recover rather than just abort.

### Logic

Unification and committed choice, on the trail and the continuation machinery:

- **Logic variables** — `lvar` makes a fresh one; a **capitalized identifier** is a logic-var literal: a persistent global at the REPL, or a fresh per-call variable when declared in `| X |` inside a definition or quotation.
- **`unify`** (`~`) — unifies two terms, binding logic vars through a trail so they match: atoms by value, arrays element-wise, frames as open records (shared keys must unify, extras allowed); on a mismatch it fails. **`deref`** (`$`) follows a variable's binding chain.
- **`amb`** / **`fail`** — committed choice: run the first branch; if it fails (a `unify` mismatch or an explicit `fail`), roll its bindings back through the trail and run the second, committing to whichever succeeds.
- **`_`** — the anonymous wildcard: unifies with anything, binds nothing, and allocates nothing.
- **`matches?`** — a non-destructive `unify` test: marks the trail, unifies, rolls back, and pushes whether the two unified — so it composes in straight-line code.
- **Cons lists** — `[( a b c )]` builds cons pairs and `[( H T )]` is the `[H|T]` head/tail pattern under `unify`; with `cons`, `head-tail`, and `array`↔`cons` conversions.
- **Fact database** — `relation` / `assert` / `query` / `retract` / `count-matches` / `inner-join`. A relation is a frame of a row-set plus per-column indexes (declared symbol columns); rows are column-keyed frames that dedup; `query` matches a pattern by unification, narrowing through the index (and returning the bucket directly when the index covers the whole pattern). `inner-join` merges two relations on a shared column via index probing; `bulk-load` builds a whole relation in one sorted pass (`array>set` for the rows, `group-by` per index) instead of row-by-row. The same row-frame shape is what a SQLite query would return.

### Other

- **`dup`**, **`drop`**, **`swap`**, **`over`**, **`rot`**, **`depth`**, **`roll`**, **`clear`** — stack-manipulation primitives.
- **`copy`** / **`reify`** — deep copy of a value (strings, arrays, sets, frames, matrices); `reify` additionally renames unbound logic vars to canonical `:_0`/`:_1`/… for a ground, storable, comparable snapshot.
- **`now`** — current Unix time as a float (seconds since epoch).
- **`see`** — prints a word's source definition; **`see-compiled`** disassembles its threaded body.
- **`man`** — `( xt -- fr )`, returns a frame of a word's reference entry (stack effect, one-line summary, cost notes). **`help name`** prints it for the named word.
- **`words`**, **`forget`**, **`bye`**, **`gc`**, **`clear`**, **`.s`**, **`.a`** — interpreter utilities.

## Planned work

Roadmap and design notes live in `PLAN.md`.

## Project layout

```
src/c/logicforth.h     — types, global program structs (Vocabulary/Arena/Compiler), per-run Interpreter, prototypes
src/c/core.c           — engine: interpreter, dictionary, GC, printing, image, REPL
src/c/words.c          — arithmetic, stack, I/O, control flow, defining words, continuations
src/c/collections.c    — sets, arrays, and frames
src/c/matrix.c         — matrix words and numeric kernels
src/c/functional.c     — higher-order operations (map, mapn, …)
src/c/superwords.c     — compile-time instruction fusion (superwords)
src/c/strings.c        — string and PCRE2 regex operations
src/c/logic.c          — logic variables, unification, amb, fact database
src/c/database.c       — SQLite integration
src/c/help_table.c     — generated help/man text (from docs/reference.md)
src/forth/lib.l4       — standard library (auto-loaded at startup)
tests/                 — golden-output test files
docs/                  — design documents
examples/              — sample programs
PLAN.md                — deferred work and design notes
```

## License

See `LICENSE`.
