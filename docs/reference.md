# logicforth reference

Every entry is derived from reading the C source. Stack effects are exact;
`--` separates the state before (bottom to top, leftmost = deepest) from after.
Shorthand: `f` float, `s` string, `xt` execution token, `m` matrix, `arr`
array, `set` set, `fr` frame, `sym` symbol, `k` continuation.

Three cost columns appear on runtime words:

- **Ops** — an approximate count of primitive operations (stack pushes/pops
  plus the dominant inner work). An integer for constant-time words; a leading
  term such as `n` or `r×c` otherwise. It is a rough constant-factor guide, not
  an instruction count.
- **Alloc** — heap activity. `1o` = one object slot + its payload allocation;
  `1s` = one string; `1a(n)` = one n-element array; `1m(r×c)` = one r×c matrix.
- **O** — asymptotic time.

Compile-time words (control flow, defining words, superwords) carry no cost
columns: their work happens while a definition is being compiled, not at run
time.

**Unsafe** words are marked ⚠. They read the raw `.number` field of a stack
slot with no tag check; a non-float operand yields a garbage float silently.
All `f`-prefixed words and all superwords are unsafe.

Allocation note: an object slot is a pointer bump into a 2M-entry table; when
the table fills, a mark-sweep GC runs and the allocation retries. There is no
incremental collection.

---

## Stack manipulation

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `dup` | `( a -- a a )` | Duplicate top | 3 | none | O(1) |
| `drop` | `( a -- )` | Discard top | 1 | none | O(1) |
| `swap` | `( a b -- b a )` | Exchange top two | 4 | none | O(1) |
| `over` | `( a b -- a b a )` | Copy second over top | 5 | none | O(1) |
| `rot` | `( a b c -- b c a )` | Rotate top three | 6 | none | O(1) |
| `depth` | `( -- n )` | Push current depth | 1 | none | O(1) |
| `roll` | `( xₙ … x₀ n -- xₙ₋₁ … x₀ xₙ )` | Move the item n deep to the top; memmoves the n above it down | 2 + n | none | O(n) |
| `clear` | `( … -- )` | Reset data stack depth to 0 | 1 | none | O(1) |
| `2dup` | `( a b -- a b a b )` | lib.l4: `over over` (inlined) | 10 | none | O(1) |
| `nip` | `( a b -- b )` | lib.l4: `swap drop` (inlined) | 5 | none | O(1) |

---

## Arithmetic

Polymorphic; dispatch on operand tags at run time. Ops/Alloc/O below give the
float fast path first; the heavy cases are captured by the O column.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `+` | `( a b -- a+b )` | float: add. string+string: concat → new string. set+set: union → new set. matrix+matrix: element-wise → new matrix. scalar+matrix / matrix+scalar: broadcast → new matrix. array+array: defers to `concat`. | 3 (float) | float none; string `1s` + temp buffer; set `1o`; matrix `1m(r×c)`; array `1a(m+n)` | float O(1); string O(\|s\|); set O(n log n); matrix O(r×c); array O(m+n) |
| `-` | `( a b -- a-b )` | float: subtract. set−set: difference. matrix: element-wise. scalar/matrix broadcast. | 3 (float) | as `+` | as `+` |
| `*` | `( a b -- a*b )` | float: multiply. set∩set: intersection. matrix: element-wise. scalar/matrix broadcast. | 3 (float) | as `+` | as `+` |
| `/` | `( a b -- a/b )` | float: divide (errors on zero divisor). matrix÷matrix: element-wise (errors on any zero element). scalar/matrix broadcast. | 3 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `%` | `( a b -- remainder quotient )` | floats only; truncating division: pushes `a − trunc(a/b)·b` then `trunc(a/b)`; errors on zero | 4 | none | O(1) |
| `^` | `( a b -- a^b )` | `pow`; float or matrix (element-wise) / scalar broadcast | 3 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `negate` | `( a -- -a )` | float or matrix (element-wise) | 2 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `1+` | `( a -- a+1 )` | float or matrix | 2 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `1-` | `( a -- a-1 )` | float or matrix | 2 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `sq` | `( a -- a² )` | float or matrix | 2 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |

### In-place matrix arithmetic

Mutate the left operand and return it; no allocation. Programmer is responsible for uniqueness (no implicit refcounting — see PLAN.md).

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `+!` | `( m a -- m )` | matrix+matrix or matrix+scalar (and scalar+matrix, mutating the matrix) in place | 3 + r×c | none | O(r×c) |
| `-!` | `( m a -- m )` | in-place subtract | 3 + r×c | none | O(r×c) |
| `*!` | `( m a -- m )` | in-place multiply | 3 + r×c | none | O(r×c) |
| `/!` | `( m a -- m )` | in-place divide | 3 + r×c | none | O(r×c) |

### Float-only arithmetic ⚠

Operate directly on stack slots' `.number`, in place, with only a depth check — no tag check.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `f+` | `( a b -- a+b )` ⚠ | add, result in deeper slot | 2 | none | O(1) |
| `f-` | `( a b -- a-b )` ⚠ | subtract | 2 | none | O(1) |
| `f*` | `( a b -- a*b )` ⚠ | multiply | 2 | none | O(1) |
| `f/` | `( a b -- a/b )` ⚠ | divide; checks divisor ≠ 0 | 2 | none | O(1) |
| `f^` | `( a b -- a^b )` ⚠ | `pow` | 2 | none | O(1) |
| `fmod` | `( a b -- fmod(a,b) )` ⚠ | `fmod` | 2 | none | O(1) |
| `f*+` | `( a b c -- a*b+c )` ⚠ | fused multiply-add; result in slot of `a` | 3 | none | O(1) |
| `f*-` | `( a b c -- c-a*b )` ⚠ | fused multiply-subtract | 3 | none | O(1) |
| `f1+` | `( a -- a+1 )` ⚠ | in place | 1 | none | O(1) |
| `f1-` | `( a -- a-1 )` ⚠ | in place | 1 | none | O(1) |
| `fsq` | `( a -- a² )` ⚠ | in place | 1 | none | O(1) |
| `fnegate` | `( a -- -a )` ⚠ | in place | 1 | none | O(1) |
| `fabs` | `( a -- \|a\| )` ⚠ | in place | 1 | none | O(1) |
| `fsqrt` | `( a -- √a )` ⚠ | in place | 1 | none | O(1) |
| `fexp` | `( a -- eᵃ )` ⚠ | in place | 1 | none | O(1) |
| `flog` | `( a -- log₁₀ a )` ⚠ | base-10 log, in place | 1 | none | O(1) |
| `fln` | `( a -- ln a )` ⚠ | natural log, in place | 1 | none | O(1) |
| `fsin` `fcos` `ftan` `ftanh` | `( a -- fn a )` ⚠ | trig/hyperbolic, in place | 1 | none | O(1) |
| `fasin` `facos` `fatan` | `( a -- fn a )` ⚠ | inverse trig, in place | 1 | none | O(1) |
| `fround` | `( a -- round a )` ⚠ | nearest, in place | 1 | none | O(1) |
| `ftruncate` | `( a -- trunc a )` ⚠ | toward zero, in place | 1 | none | O(1) |
| `fround-up` | `( a -- ceil a )` ⚠ | in place | 1 | none | O(1) |
| `fround-down` | `( a -- floor a )` ⚠ | in place | 1 | none | O(1) |

---

## Unary math (polymorphic: float or matrix)

Tag-checked; safe. Float input → float; matrix input → new matrix, element-wise.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `abs` | `( a -- \|a\| )` | `fabs` | 2 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `sqrt` | `( a -- √a )` | `sqrt` | 2 | matrix `1m(r×c)` | same |
| `exp` | `( a -- eᵃ )` | `exp` | 2 | matrix `1m(r×c)` | same |
| `log` | `( a -- log₁₀ a )` | `log10` | 2 | matrix `1m(r×c)` | same |
| `ln` | `( a -- ln a )` | `log` — natural log | 2 | matrix `1m(r×c)` | same |
| `sin` `cos` `tan` `tanh` | `( a -- fn a )` | trig/hyperbolic | 2 | matrix `1m(r×c)` | same |
| `asin` `acos` `atan` | `( a -- fn a )` | inverse trig | 2 | matrix `1m(r×c)` | same |
| `round` | `( a -- round a )` | `round` | 2 | matrix `1m(r×c)` | same |
| `truncate` | `( a -- trunc a )` | `trunc` | 2 | matrix `1m(r×c)` | same |
| `round-up` | `( a -- ceil a )` | `ceil` | 2 | matrix `1m(r×c)` | same |
| `round-down` | `( a -- floor a )` | `floor` | 2 | matrix `1m(r×c)` | same |
| `mod` | `( a b -- remainder )` | lib.l4: `% drop`; sign follows dividend | 5 | none | O(1) |
| `quotient` | `( a b -- quotient )` | lib.l4: `% swap drop`; toward zero | 9 | none | O(1) |

---

## Comparison and logic

Result is `1.0` (true) or `0.0` (false). `=`/`lt`/`gt` use `val_cmp` (structural), with a float fast path.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `=` | `( a b -- bool )` | structural equality | 3 (float) | none | float O(1); string O(\|s\|); array/set O(n); frame O(n); matrix O(r×c) |
| `lt` | `( a b -- bool )` | less-than | 3 (float) | none | same |
| `gt` | `( a b -- bool )` | greater-than | 3 (float) | none | same |
| `0=` | `( a -- bool )` | `!truthy(a)`; any type | 2 | none | O(1) |
| `and` | `( a b -- bool )` | logical and of truthiness | 3 | none | O(1) |
| `or` | `( a b -- bool )` | logical or of truthiness | 3 | none | O(1) |
| `not` | `( a -- bool )` | logical not of truthiness | 2 | none | O(1) |

`truthy` of a float is `≠ 0.0`; of any heap value, its handle `≠ 0`.

---

## Return stack

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `>r` | `( a -- )` → return stack | Move top to return stack | 2 | none | O(1) |
| `r>` | return stack → `( -- a )` | Move return-stack top to data stack | 2 | none | O(1) |
| `r@` | `( -- a )` | Copy return-stack top to data stack | 2 | none | O(1) |

---

## Side stack

A third stack (depth 256) for stashing values out of the way; used by `try-catch` to hold the handler.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `>side` | `( a -- )` | Push to side stack | 2 | none | O(1) |
| `side>` | `( -- a )` | Pop from side stack | 2 | none | O(1) |
| `side-drop` | `( -- )` | Discard side-stack top | 1 | none | O(1) |
| `side-depth` | `( -- n )` | Push side-stack depth | 1 | none | O(1) |

---

## Control flow (compile-time)

Immediate words that emit branch instructions into the current definition. They have no effect outside a definition or quotation.

| Word | Runtime effect | Behavior |
|------|---------------|----------|
| `if` | `( flag -- )` | Branch past the `then`/`else` if flag is falsy |
| `?if` | `( flag -- flag )` | Like `if`, but peeks the flag instead of consuming it — the flag stays on the stack in both branches |
| `else` | — | Separate the true and false arms |
| `then` | — | Close an `if`/`if…else`; patches the forward branch |
| `begin` | — | Mark a loop top |
| `until` | `( flag -- )` | Branch back to `begin` if flag is falsy |
| `again` | — | Unconditional branch back to `begin` |
| `while` | `( flag -- )` | Exit the loop forward if flag is falsy (`begin … while … repeat`) |
| `repeat` | — | Branch back to `begin`; patches the `while` exit |
| `exit` | `( -- )` | Return early from the current definition (this one runs at run time) |

---

## Defining and compiling words

These parse following tokens and/or compile code. Costs are dominated by compilation, not by a stack effect, so no cost columns.

| Word | Stack effect | Behavior |
|------|-------------|----------|
| `: name` | — | Begin a colon definition; read the name; enter compile mode |
| `;` | — | End a colon definition; emit `exit`; store the source text for `see` |
| `variable name` | — | Declare a global variable initialized to `0.0` |
| `to name` | `( val -- )` | Assign to a local (in a definition) or a global. At the REPL, auto-creates the global if absent. In a definition, the variable must already exist. May trigger superword store-fusion while compiling. |
| `symbol name` | — | Declare a word that pushes a specific interned symbol |
| `:name` | `( -- sym )` | Symbol literal; interns the name at read time |
| `string>symbol` | `( s -- sym )` | Intern a computed string as a symbol |
| `[: … :]` | `( -- xt )` | Anonymous quotation; compiles its body and pushes its xt |
| `' name` | `( -- xt )` | Push the xt of the named word |
| `execute` | `( xt -- … )` | Call the word at xt |
| `inline` | — | Mark the most recent definition inline; future calls splice its body |
| `forget name` | — | Truncate the dictionary back to before `name` |

### Locals

Declared only at the **head** of a definition or quotation body. Live on the return stack: up to 128 names across up to 16 nested scopes. Quotations close over the enclosing definition's locals.

| Syntax | Behavior |
|--------|----------|
| `\| x y z \|` | Declare x, y, z, each initialized to `0.0`; read by bare name, assign with `to` |
| `\|> x y z \|` | Declare and receive from the stack: z ← top, y ← second, x ← third |
| `\| x >y z \|` | Mixed: a `>` prefix marks an individual name as a receive slot; the rest initialize to 0 |

`++ name` and `-- name` are compile-time words that increment / decrement a local by 1 in place, emitting a single fused instruction at depth 0.

---

## I/O and printing

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `.` | `( a -- )` | Print value then a space; matrices print as a grid, frames pretty-print | 1 + print | none | O(size printed) |
| `.a` | `( a -- )` | Like `.` but disables print truncation (show all elements) | 1 + print | none | O(size printed) |
| `.s` | `( -- )` | Print every stack value, bottom to top; leaves the stack intact | print | none | O(depth) |
| `cr` | `( -- )` | Print a newline | 1 | none | O(1) |
| `emit` | `( n -- )` | Print the character with codepoint n | 1 | none | O(1) |

String literals `"…"` are **raw**: bytes between the quotes are copied verbatim, no escape processing, no substitution; an embedded newline is kept. (So a regex `\d{3}` literal is safe — the braces are not touched.) Template-filling is the explicit word `format` (in String operations below).

---

## String operations

Regex words run on PCRE2 with JIT-compiled patterns. Each distinct pattern is compiled once and cached (64-slot round-robin), so reusing a pattern costs only the match. Patterns are PCRE syntax in raw `"…"` literals — PCRE itself interprets `\n`, `\t`, `\d`, `\x22`, and the rest. Matching is multiline: `^` and `$` bind to line boundaries. Captures come back as strings; an optional group that didn't participate is `0.0`. Booleans are `1.0`/`0.0`. Indices are byte offsets (no UTF-8 codepoint model yet). In the cost columns `n` is the subject length.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `match` | `( s pat -- [ whole cap… ] \| 0 )` | First (leftmost) match as a flat array: whole match then each capture; no match returns `0` | n | `1a` + captures | O(n) |
| `match-all` | `( s pat -- [ [whole cap…] … ] \| 0 )` | Every non-overlapping leftmost match, each a flat sub-array; a zero-width match advances one byte; no match returns `0` | n | `1a` per match + captures | O(n + m·g) |
| `replace` | `( s pat rep -- s' )` | Replace **all** matches; in `rep`, `&` or `\0` is the whole match, `\1`–`\9` a capture, `\&` and `\\` literals | n | `1o` + buffer growth | O(n) |
| `split` | `( s pat -- [ piece… ] )` | Split `s` at each non-overlapping match of `pat`; the pieces are the gaps between matches, empty fields kept; no match → `[ s ]` | n | `1a` + pieces | O(n) |
| `substring` | `( s start end -- sub )` | Half-open byte range `[start, end)`; bounds-checked | 2 + k | `1o` | O(k), k = end − start |
| `join` | `( arr sep -- s )` | Concatenate the string elements of `arr` separated by `sep`; errors on a non-string element | 2 + total | `1o` | O(total) |
| `has?` | `( s pat -- bool )` | True if `pat` matches anywhere in `s` (string overload of frame `has?`) | n | none | O(n) |
| `format` | `( … template -- s )` | Fill `template`'s `{n}` placeholders with the nth-from-top stack value, then drop exactly the referenced positions (unreferenced values stay); renders floats/strings/symbols. Only `{digits}` substitute — other brace content is left literal | len + refs | `1o` | O(len) |

`first match` and `findall` are spelled `match` and `match-all`; there is no separate search/match/fullmatch split. Anchor with `^`/`$` (or `\A`/`\z`) when you need it.

---

## Sets

Sorted `Val` arrays with binary-search insertion; equality is structural. `+`/`*`/`-` on two sets are union/intersection/difference.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `< v… >` | `( -- set )` | Set literal; `<` pushes a mark, `>` gathers everything above it into a sorted set | n log n | `1o` + reallocs | O(n log n) |
| `set` | `( v₀ … vₙ₋₁ n -- set )` | Gather the top n values into a new set (the set analog of `array`) | 2 + n log n | `1o` + reallocs | O(n log n) |
| `union` | `( s₁ s₂ -- s₃ )` | Union into a new set | (m+n) log(m+n) | `1o` + reallocs | O((m+n) log(m+n)) |
| `intersection` | `( s₁ s₂ -- s₃ )` | Intersection into a new set | m log n | `1o` + reallocs | O(m log n) |
| `difference` | `( s₁ s₂ -- s₃ )` | s₁ − s₂ into a new set | m log n | `1o` + reallocs | O(m log n) |
| `member?` | `( set v -- bool )` | Binary-search membership | 3 + log n | none | O(log n) |
| `size` | `( set -- n )` | Element count (also arrays, strings, frames) | 2 | none | O(1) |

---

## Arrays

Fixed length, 0-indexed, elements of any type.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `[ v… ]` | `( -- arr )` | Array literal; `[` marks, `]` gathers above the mark | n | `1a(n)` | O(n) |
| `array` | `( v₀ … vₙ₋₁ n -- arr )` | Gather the top n values into an array | 2 + n | `1a(n)` | O(n) |
| `array-of` | `( val n -- arr )` | New n-element array, every slot = val | 3 + n | `1a(n)` | O(n) |
| `@i` | `( arr i -- val )` | Array element; on a matrix returns row i as a 1×c matrix | 3 (array) | matrix `1m(1×c)` | O(1) array; O(c) matrix |
| `!i` | `( arr i val -- arr )` | Store val at index i in place; leaves arr on the stack | 4 | none | O(1) |
| `take` | `( arr/set n -- arr )` | First n elements (clamped) | 2 + n | `1a(n)` | O(n) |
| `reverse` | `( arr/set -- arr )` | Reversed copy | 1 + n | `1a(n)` | O(n) |
| `reverse-slice!` | `( arr offset n -- arr )` | Reverse the `n` elements at `offset` in place; leaves arr | 2 + n | none | O(n) |
| `concat` | `( arr/set arr/set -- arr )` | Concatenated copy | 2 + m + n | `1a(m+n)` | O(m+n) |
| `range` | `( from to -- arr )` | Inclusive integer range, step ±1 | 3 + n | `1a(n)` | O(n) |
| `destruct` | `( arr/set/fr -- v… )` | Spread elements onto the stack; a frame spreads alternating sym/value | 1 + n | none | O(n) |
| `destruct-to` | `( source targets -- )` | source and target arrays; assign each source element to the variable named by the corresponding target (symbol or xt), creating it if needed | 2 + n | may create variables | O(n) |
| `slice!` | `( arr tstart src sstart sstep slen -- arr )` | Copy `slen` elements `src[sstart], src[sstart+sstep], …` into `arr[tstart…]` in place | 6 + slen | self-overlap may malloc slen | O(slen) |
| `to-slice!` | `( v₀ … vₙ₋₁ arr offset n -- arr )` | Store the n values just below `arr` into `arr[offset…offset+n)`; leaves arr | 2 + n | none | O(n) |
| `last` | `( arr n -- arr )` | lib.l4: `swap reverse swap take reverse` | 3n | 3×`1a(n)` | O(n) |
| `skip` | `( arr n -- arr )` | lib.l4: `over size swap - swap reverse swap take reverse` | 3n | 3×`1a(n)` | O(n) |

---

## Frames

Symbol-keyed sorted maps; binary-search lookup. A path is an array of symbols; the literal `/a/b/c` is a compile-time constant array and allocates nothing at run time. `d` = path depth, `n` = frame size.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `{ :k v … }` | `( -- fr )` | Frame literal from alternating symbol/value pairs above the `{` mark | n log n | `1o` + reallocs | O(n log n) |
| `frame` | `( keys values -- fr )` | Build from parallel key and value arrays of equal length | 2 + n log n | `1o` + reallocs | O(n log n) |
| `>frame` | `( arr -- fr )` | Build from an even-length alternating-kv array | 1 + n log n | `1o` + reallocs | O(n log n) |
| `@` | `( fr sym/path -- val )` | Get by key or path; errors if absent | 3 + d log n | none | O(d log n) |
| `!` | `( fr sym/path val -- fr )` | Set by key or path, vivifying intermediates; mutates fr | d log n | realloc on growth; `1o` per vivified frame | O(d log n) amortized |
| `has?` | `( fr sym/path -- bool )` | Existence test; no error on miss | 3 + d log n | none | O(d log n) |
| `delete-at` | `( fr sym/path -- fr )` | Remove a key (errors if absent); mutates fr | n | none | O(n) |
| `update-at` | `( fr sym/path xt -- fr )` | Apply xt to the value at the key, store the result back | d log n + xt | none | O(d log n + xt) |
| `keys` | `( fr -- arr )` | Keys (symbols) in sorted order | 1 + n | `1a(n)` | O(n) |
| `values` | `( fr -- arr )` | Values in key order | 1 + n | `1a(n)` | O(n) |
| `merge` | `( fr₁ fr₂ -- fr )` | New frame with all keys; fr₂ wins collisions | (m+n) log(m+n) | `1o` + reallocs | O((m+n) log(m+n)) |
| `size` | `( fr -- n )` | Pair count | 2 | none | O(1) |
| `copy` | `( a -- a' )` | Deep copy of any value (recurses into frames, arrays, matrices, strings, sets, continuations); identity for scalars. Defined generally, not frame-specific. | tree size | one object per node | O(tree size) |

---

## Matrices

Row-major `double` storage. `r` rows, `c` columns.

### Construction

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `0-matrix` | `( r c -- m )` | r×c zero matrix (calloc) | 3 | `1m(r×c)` | O(1)+ |
| `matrix` | `( arr r c -- m )` or `( arr r -- m )` | Build from a float array; two-arg form takes r = rows and infers columns | 3 + r×c | `1m(r×c)` | O(r×c) |
| `diagonal-matrix` | `( fill n -- m )` | n×n matrix with `fill` on the diagonal | 2 + n | `1m(n×n)` | O(n) |
| `identity-matrix` | `( n -- m )` | lib.l4: `1 swap diagonal-matrix` | n | `1m(n×n)` | O(n) |
| `matrix-range` | `( start end step -- m )` | 1×N row of evenly spaced values | 3 + N | `1m(1×N)` | O(N) |

### Shape and indexing

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `@i` | `( m i -- row )` | Row i as a 1×c matrix (copy) | 2 + c | `1m(1×c)` | O(c) |
| `@j` | `( m j -- col )` | Column j as an r×1 matrix (copy) | 2 + r | `1m(r×1)` | O(r) |
| `@i,j` | `( m i j -- f )` | Single element as a float | 4 | none | O(1) |
| `dim` | `( m -- r c )` | Push rows then columns | 3 | none | O(1) |
| `reshape` | `( m r c -- m' )` | Same elements, new shape (must match); memcpy | 3 + r×c | `1m(r×c)` | O(r×c) |
| `transpose` | `( m -- m' )` | Rows/columns swapped | 1 + r×c | `1m(c×r)` | O(r×c) |
| `diagonal` | `( m -- m' )` | Diagonal as a 1×min(r,c) matrix | 1 + min(r,c) | `1m(1×min)` | O(min(r,c)) |
| `flatten` | `( m -- m' )` | lib.l4: 1×(r·c) reshape | r×c | `1m(1×r·c)` | O(r×c) |
| `num-elements` | `( m -- n )` | lib.l4: `dim *` | 5 | none | O(1) |

### Multiplication and reductions

`dgemm` variants do real matrix multiply; element-wise `*` does not.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `dgemm-nn` | `( α A B β C -- R )` | `R = α·A·B + β·C`, ikj fast path | 5 + m·k·n | `1m(m×n)` | O(m·k·n) |
| `dgemm-tn` | `( α A B β C -- R )` | `R = α·Aᵀ·B + β·C` | 5 + m·k·n | `1m(m×n)` | O(m·k·n) |
| `dgemm-nt` | `( α A B β C -- R )` | `R = α·A·Bᵀ + β·C` | 5 + m·k·n | `1m(m×n)` | O(m·k·n) |
| `dgemm-tt` | `( α A B β C -- R )` | `R = α·Aᵀ·Bᵀ + β·C` | 5 + m·k·n | `1m(m×n)` | O(m·k·n) |
| `sum` | `( m -- f )` | Sum of all elements (4-way unrolled, fast-math) | 1 + r×c | none | O(r×c) |
| `max` | `( m -- f )` | Maximum element | 1 + r×c | none | O(r×c) |
| `min` | `( m -- f )` | Minimum element | 1 + r×c | none | O(r×c) |
| `row-sums` | `( m -- m' )` | r×1 of per-row sums | 1 + r×c | `1m(r×1)` | O(r×c) |
| `row-maxes` | `( m -- m' )` | r×1 of per-row maxima | 1 + r×c | `1m(r×1)` | O(r×c) |
| `row-mins` | `( m -- m' )` | r×1 of per-row minima | 1 + r×c | `1m(r×1)` | O(r×c) |
| `column-sums` | `( m -- m' )` | 1×c of per-column sums | 1 + r×c | `1m(1×c)` | O(r×c) |
| `column-maxes` | `( m -- m' )` | 1×c of per-column maxima | 1 + r×c | `1m(1×c)` | O(r×c) |
| `column-mins` | `( m -- m' )` | 1×c of per-column minima | 1 + r×c | `1m(1×c)` | O(r×c) |
| `mean` | `( m -- f )` | lib.l4: sum ÷ element count | r×c | none | O(r×c) |
| `row-means` | `( m -- m' )` | lib.l4: `row-sums` then scalar ÷ | r×c | 2×`1m(r×1)` | O(r×c) |
| `column-means` | `( m -- m' )` | lib.l4: `column-sums` then scalar ÷ | r×c | 2×`1m(1×c)` | O(r×c) |

---

## Higher-order

The quotation/predicate cost dominates; `xt` denotes one call.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `map` | `( arr/set xt -- arr )` | Apply xt to each element; xt must net exactly one value | 2 + n·xt | `1a(n)` | O(n·xt) |
| `mapn` | `( arr₁ … arr_N xt N -- arr )` | N-ary zip-map over equal-length arrays | rows·(N+xt) | `1a(rows)` | O(rows·xt) |
| `filter` | `( arr/set xt -- arr )` | Keep elements where xt is truthy | 2 + n·xt | malloc(n) flags + `1a(k)` | O(n·xt) |
| `reduce` | `( arr/set init xt -- val )` | Left fold; xt is `( acc elem -- acc )` | 3 + n·xt | none | O(n·xt) |
| `times` | `( xt n -- )` | Run xt n times, no index pushed | 2 + n·xt | none | O(n·xt) |
| `i-times` | `( xt n -- )` | Run xt n times, pushing index 0..n-1 first | 2 + n·(1+xt) | none | O(n·xt) |

---

## Delimited continuations

The substrate for exceptions, coroutines, generators. See `docs/continuations.md`. `L` = captured return-stack length.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `reset` | `( -- )` | Push a unique mark on the return stack, delimiting the captured region | 1 | none | O(1) |
| `shift` | `( -- k )` | Capture the return-stack slice up to the nearest `reset`, remove the mark and that slice, push k | L | `1o` (cont) | O(L) |
| `shift-with` | `( xt -- )` | Capture as `shift`, then run xt in the outer context with k on the stack and begin unwinding | L + xt | `1o` (cont) | O(L + xt) |
| `resume` | `( k -- … )` | Pop k and re-enter it (multi-shot — the continuation object survives, so a retained copy can be resumed again); pushes whatever the resumed code yields | L + resumed | none | O(L + resumed) |
| `throw` | `( exc -- )` | lib.l4: `[: drop 1 :] shift-with` | — | `1o` (cont) | O(stack depth) |
| `catch` | `( xt -- result 0 \| exc 1 )` | lib.l4: `reset execute 0` | — | cont if thrown | O(xt) |
| `try-catch` | `( normal-xt err-xt -- … )` | lib.l4: run normal-xt; on throw, run err-xt with exc on the stack | — | cont if thrown | O(normal-xt) |

---

## Superwords (compile-time fusion) ⚠

Immediate compiler words usable only inside a definition. They detect a preceding variable-load and emit a single fused instruction that reads the variable's dict slot directly. All read `.number` without a tag check. Followed by `to dest`, they fuse further into a store variant that writes the result straight to the destination slot.

| Word(s) | Syntax | Behavior |
|---------|--------|----------|
| `vvf+` `vvf-` `vvf*` `vvf/` | `vvf+ a b` | Load variables a and b, apply the op, push the result |
| `vf+` `vf-` `vf*` `vf/` | `vf+ a` | Combine variable a with the stack top using the op, in place |
| `vfsq` `vfneg` `vfabs` `vfsqrt` `vfexp` `vflog` `vfsin` `vfcos` `vftan` `vftanh` | `vfsq a` | Apply the unary function to variable a, push the result |
| `vvf*+` | `vvf*+ b c` | `( t -- t*b+c )`, reading variables b and c |
| `vvf*-` | `vvf*- b c` | `( t -- c-t*b )`, reading variables b and c |

These are normally produced by the compiler's auto-fuser rather than typed by hand; `see-compiled` reveals them.

---

## REPL and introspection

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `words` | `( -- )` | List all non-internal words, newest first, 8 per line | dict scan | none | O(\|dict\|) |
| `see` | `( xt -- )` | Print a word's source (`: name … ;`), or `variable`/`symbol`/primitive form | dict scan | none | O(\|dict\|) |
| `see-compiled` | `( xt -- )` | Disassemble a colon definition's compiled cells | body scan | none | O(body) |
| `.s` | `( -- )` | Print the stack, intact | print | none | O(depth) |
| `gc` | `( -- )` | Force a mark-sweep now | walks stacks + dict + roots, frees unmarked | none | O(objects + dict) |
| `bye` | `( -- )` | `exit(0)` | — | — | — |
| `now` | `( -- f )` | `CLOCK_MONOTONIC` seconds as a float | 1 | none | O(1) |

---

## Persistence

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `load` | `( s -- )` | Run a source file as if typed; record it for `reload` | file read + run | input buffer | O(file) |
| `reload` | `( -- )` | Truncate user state, re-run every loaded file in order | forget + N loads | — | O(Σ files) |
| `save` | `( s -- )` | Write all user words as re-loadable `.l4` source | dict scan + write | file I/O | O(\|user dict\|) |
| `save-image` | `( s -- )` | Binary snapshot of full state (dict, objects, stacks, continuations) | serialize all | file I/O | O(objects + dict) |
| `load-image` | `( s -- )` | Restore a binary snapshot, replacing current state | deserialize all | reallocates all objects | O(objects) |

---

## Subprocesses and streams

A stream (`T_STREAM`) wraps an OS file descriptor — a pipe to a child process (later, a socket). `start-process` launches a program directly from an argv array (no shell, so no quoting or injection surface) and returns a frame `{ :pid :in :out :err }` whose `:in`/`:out`/`:err` are streams. The lifecycle is: `write` input → `close` `:in` (sends EOF) → `read` the output → `wait`. `SIGPIPE` is ignored process-wide, so a `write` to a child that has exited returns an error rather than killing the interpreter. Bytes are raw and length-counted, so streams are binary-safe.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `start-process` | `( argv -- proc )` | fork/exec `argv[0]` with `argv` as its arguments; return `{ :pid :in :out :err }` (the three streams are `T_STREAM`) | fork + 3 pipes | `1o` frame + 3 streams | O(argc) |
| `write` | `( s stream -- )` | Write the string's bytes to the stream; loops over partial writes, retries `EINTR` | write syscalls | none | O(\|s\|) |
| `read` | `( stream -- s )` | Read the stream to EOF into one string | read syscalls | `1o` + buffer growth | O(bytes) |
| `close` | `( stream -- )` | Close the fd; closing a child's `:in` sends it EOF | 1 syscall | none | O(1) |
| `wait` | `( pid -- status )` | Block until the child exits; return its exit code, or `128 + signo` if it was killed by a signal | blocks | none | O(1) |
| `stop` | `( pid -- status )` | `SIGKILL` the child then reap it (137 = 128+9, or its code if it had already exited) | 2 syscalls | none | O(1) |
| `running?` | `( pid -- bool )` | Non-blocking liveness via `waitpid`+`WNOHANG`; true while running, false once exited — reaping it as a side effect | 1 syscall | none | O(1) |

`lib.l4` conveniences: `run` ( s -- proc ) splits a command string on spaces and `start-process`es it (`s " " split start-process`); `write-in` ( s proc -- ), `read-out` ( proc -- s ), `read-err` ( proc -- s ) write/read the child's `:in`/`:out`/`:err`. Line access is `read "\n" split`.

---

## Type tags

| Tag | Description |
|-----|-------------|
| `T_FLOAT` | 64-bit double; any bit pattern that is not a boxed NaN |
| `T_STRING` | heap object; NUL-terminated UTF-8 bytes, `len` = byte count |
| `T_SYMBOL` | symbol-pool offset; equal names share one offset |
| `T_ARRAY` | heap object; `Val[]` |
| `T_SET` | heap object; sorted `Val[]`, binary-search membership |
| `T_FRAME` | heap object; sorted parallel keys (`cell[]`) and values (`Val[]`) |
| `T_MATRIX` | heap object; r×c row-major `double[]` |
| `T_XT` | execution token (dict index); first-class callable |
| `T_ADDR` | dict index; used internally for return-stack frames |
| `T_STREAM` | OS file descriptor (pipe or socket end); an inline `int`, like `T_ADDR` |
| `T_CONT` | heap object; a captured return-stack slice plus a resume IP |
| `T_MARK` | ephemeral sentinel from `<`, `[`, `{`, `reset`; not user-visible |
| `T_NONE` | uninitialized / sentinel |

Boolean convention: `1.0` true, `0.0` false.

---

## Object allocation

Every heap value uses one slot in the 2M-entry `objects[]` table (pointer-bump, GC on exhaustion) plus a `calloc`'d `Object` struct plus one payload allocation:

| Type | Payload |
|------|---------|
| String | `len + 1` bytes (NUL-terminated) |
| Array | `max(n,1) × sizeof(Val)` |
| Set | 4 × `sizeof(Val)` initial, doubles on overflow |
| Frame | 4 × (`sizeof(cell)` keys + `sizeof(Val)` values), doubles on overflow |
| Matrix | `r × c × sizeof(double)` (calloc, zero-filled) |
| Continuation | `max(L,1) × sizeof(Val)` |
