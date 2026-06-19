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

Allocation note: an object slot is a pointer bump into the object table, which
grows on demand (doubling) up to a 64M-entry ceiling; when the ceiling is
reached, a mark-sweep GC runs and the allocation retries. There is no
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
| `2drop` | `( a b -- )` | lib.l4: `drop drop` (inlined) | 6 | none | O(1) |
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
| `pi` | `( -- f )` | lib.l4: `variable` initialized to π (3.141592653589793); invoking it pushes the stored float | 1 | none | O(1) |

### In-place matrix arithmetic

Mutate the left operand and return it; no allocation. Programmer is responsible for uniqueness (no implicit refcounting).

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
| `fsin` | `( a -- sin a )` ⚠ | sine (radians), in place | 1 | none | O(1) |
| `fcos` | `( a -- cos a )` ⚠ | cosine (radians), in place | 1 | none | O(1) |
| `ftan` | `( a -- tan a )` ⚠ | tangent (radians), in place | 1 | none | O(1) |
| `ftanh` | `( a -- tanh a )` ⚠ | hyperbolic tangent, in place | 1 | none | O(1) |
| `fasin` | `( a -- asin a )` ⚠ | inverse sine, in place | 1 | none | O(1) |
| `facos` | `( a -- acos a )` ⚠ | inverse cosine, in place | 1 | none | O(1) |
| `fatan` | `( a -- atan a )` ⚠ | inverse tangent, in place | 1 | none | O(1) |
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
| `sin` | `( a -- sin a )` | sine (radians) | 2 | matrix `1m(r×c)` | same |
| `cos` | `( a -- cos a )` | cosine (radians) | 2 | matrix `1m(r×c)` | same |
| `tan` | `( a -- tan a )` | tangent (radians) | 2 | matrix `1m(r×c)` | same |
| `tanh` | `( a -- tanh a )` | hyperbolic tangent | 2 | matrix `1m(r×c)` | same |
| `asin` | `( a -- asin a )` | inverse sine | 2 | matrix `1m(r×c)` | same |
| `acos` | `( a -- acos a )` | inverse cosine | 2 | matrix `1m(r×c)` | same |
| `atan` | `( a -- atan a )` | inverse tangent | 2 | matrix `1m(r×c)` | same |
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

A third stack (depth 1024) for stashing values out of the way; used by `try-catch` to hold the handler.

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
| `:` | — | Begin a colon definition; read the following name; enter compile mode |
| `;` | — | End a colon definition; emit `exit`; store the source text for `see` |
| `variable` | — | Read the following name; declare a global variable initialized to `0.0` |
| `to` | `( val -- )` | Assign to the named local (in a definition) or global. At the REPL, auto-creates the global if absent. In a definition, the variable must already exist. May trigger superword store-fusion while compiling. |
| `symbol` | — | Read the following name; declare a word that pushes a specific interned symbol |
| `:name` | `( -- sym )` | Symbol literal; interns the name at read time |
| `string>symbol` | `( s -- sym )` | Intern a computed string as a symbol |
| `[:` | `( -- xt )` | Open an anonymous quotation (closed by `:]`); compiles its body and pushes its xt |
| `'` | `( "name" -- xt )` | Parse the following word at compile time and push its xt (immediate; folds the xt in as a literal) |
| `lookup` | `( "name" -- xt )` | Parse the following word at run time and push its xt — the non-immediate counterpart of `'` |
| `execute` | `( xt -- … )` | Call the word at xt |
| `inline` | — | Mark the most recent definition inline; future calls splice its body |
| `forget` | — | Read the following name; truncate the dictionary back to before it |

### Locals

Declared only at the **head** of a definition or quotation body. Live on the return stack: up to 128 names across up to 16 nested scopes. Quotations close over the enclosing definition's locals.

| Syntax | Behavior |
|--------|----------|
| `\| x y z \|` | Declare x, y, z, each initialized to `0.0`; read by bare name, assign with `to` |
| `\|> x y z \|` | Declare and receive from the stack: z ← top, y ← second, x ← third |
| `\| x >y z \|` | Mixed: a `>` prefix marks an individual name as a receive slot; the rest initialize to 0 |

These compile-time words read a following local name and emit a single fused depth-0 instruction:

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `++` | `( -- )` | Increment the named local by 1 in place | 1 | none | O(1) |
| `--` | `( -- )` | Decrement the named local by 1 in place | 1 | none | O(1) |
| `f++` | `( -- )` ⚠ | Unsafe float increment: raw `.number` mutation, no tag check, for a local known to hold a float | 1 | none | O(1) |
| `f--` | `( -- )` ⚠ | Unsafe float decrement: raw `.number` mutation, no tag check | 1 | none | O(1) |

---

## I/O and printing

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `.` | `( a -- )` | Print value then a space; matrices print as a grid, frames pretty-print | 1 + print | none | O(size printed) |
| `.a` | `( a -- )` | Like `.` but disables print truncation (show all elements) | 1 + print | none | O(size printed) |
| `.s` | `( -- )` | Print every stack value, bottom to top; leaves the stack intact | print | none | O(depth) |
| `cr` | `( -- )` | Print a newline | 1 | none | O(1) |
| `emit` | `( n -- )` | Print the character with codepoint n | 1 | none | O(1) |

String literals `"…"` are **raw**: bytes between the quotes are copied verbatim and an embedded newline is kept; the only escape is a doubled `""`, which yields one `"` (a lone `"` closes the string). There is no `{n}` substitution — a regex `\d{3}` literal is safe, and template-filling is the explicit word `format` (in String operations below).

---

## String operations

Regex words run on PCRE2 with JIT-compiled patterns. Each distinct pattern is compiled once and cached (1024-slot round-robin), so reusing a pattern costs only the match. Patterns are PCRE syntax in raw `"…"` literals — PCRE itself interprets `\n`, `\t`, `\d`, `\x22`, and the rest. Matching is multiline: `^` and `$` bind to line boundaries. Captures come back as strings; an optional group that didn't participate is `0.0`. Booleans are `1.0`/`0.0`. Indices are byte offsets (no UTF-8 codepoint model). In the cost columns `n` is the subject length.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `match` | `( s pat -- [ whole cap… ] \| 0 )` | First (leftmost) match as a flat array: whole match then each capture; no match returns `0` | n | `1a` + captures | O(n) |
| `match-all` | `( s pat -- [ [whole cap…] … ] \| 0 )` | Every non-overlapping leftmost match, each a flat sub-array; a zero-width match advances one byte; no match returns `0` | n | `1a` per match + captures | O(n + m·g) |
| `replace` | `( s pat rep -- s' )` | Replace **all** matches; in `rep`, `&` or `\0` is the whole match, `\1`–`\9` a capture, `\&` and `\\` literals | n | `1o` + buffer growth | O(n) |
| `split` | `( s pat -- [ piece… ] )` | Split `s` at each non-overlapping match of `pat`; the pieces are the gaps between matches, empty fields kept; no match → `[ s ]` | n | `1a` + pieces | O(n) |
| `substring` | `( s start end -- sub )` | Half-open byte range `[start, end)`; bounds-checked | 2 + k | `1o` | O(k), k = end − start |
| `trim` | `( s -- s' )` | Strip leading and trailing ASCII whitespace (`' ' \t \n \v \f \r`); a backward/forward byte-scan, one allocation of the surviving span | n | `1o` | O(n) |
| `join` | `( arr sep -- s )` | Concatenate the string elements of `arr` separated by `sep`; errors on a non-string element | 2 + total | `1o` | O(total) |
| `format` | `( … template -- s )` | Fill `template`'s `{n}` placeholders with the nth-from-top stack value, then drop exactly the referenced positions (unreferenced values stay); renders floats/strings/symbols. Only `{digits}` substitute — other brace content is left literal | len + refs | `1o` | O(len) |

`first match` and `findall` are spelled `match` and `match-all`; there is no separate search/match/fullmatch split. Anchor with `^`/`$` (or `\A`/`\z`) when you need it.

---

## Sets

Sorted `Val` arrays with binary-search insertion; equality is structural. `+`/`*`/`-` on two sets are union/intersection/difference.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `< v… >` | `( -- set )` | Set literal; `<` pushes a mark, `>` gathers everything above it into a sorted set | n log n | `1o` + reallocs | O(n log n) |
| `set` | `( v₀ … vₙ₋₁ n -- set )` | Gather the top n values into a new set (the set analog of `array`) | 2 + n log n | `1o` + reallocs | O(n log n) |
| `union` | `( s₁ s₂ -- s₃ )` | Union into a new set, merging the two sorted arrays | m+n | `1o` + reallocs | O(m+n) |
| `intersection` | `( s₁ s₂ -- s₃ )` | Intersection into a new set, merging the two sorted arrays | m+n | `1o` + reallocs | O(m+n) |
| `difference` | `( s₁ s₂ -- s₃ )` | s₁ − s₂ into a new set, merging the two sorted arrays | m+n | `1o` + reallocs | O(m+n) |
| `set-add!` | `( set v -- set )` | Insert v in sorted position if absent (dedups); leaves set on the stack | log n + n | reallocs | O(n) |
| `set-remove!` | `( set v -- set )` | Remove v if present (no-op if absent); leaves set on the stack | log n + n | none | O(n) |
| `member?` | `( set v -- bool )` | Binary-search membership | 3 + log n | none | O(log n) |
| `array>set` | `( array -- set )` | Sort a copy of the array once and dedup into a set — the fast bulk constructor (one sort, not n inserts); the source array is unchanged | n log n | `1o` + realloc | O(n log n) |
| `group-by` | `( array col -- frame )` | Group an array of frames by their symbol-valued `col` into a frame from each value to a set of the matching rows; one sorted pass, distinct values sorted | n log n | frame + sets | O(n log n) |
| `size` | `( coll -- n )` | Element count of a set, array, or string; pair count of a frame | 2 | none | O(1) |

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

## Pairs (cons lists)

Cons cells in a dense, GC'd table — the linked, recursively-decomposable counterpart to arrays (O(1) prepend, tail-sharing, head/tail recursion). A list is a chain of pairs; `null` is the empty list and the terminator. The `[( … )]` reader takes the **last element as the tail**, so `[( a b c )]` is `cons(a, cons(b, c))` and a proper list is written `[( a b c null )]`. That makes `[( H T )]` exactly Prolog's `[H|T]` under `unify`. Printing resolves bound vars; output round-trips.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `[( v… )]` | `( -- list )` | List literal; the last element is the tail (`[( a b c )]` = `cons(a, cons(b, c))`; `[( )]` = `null`; one element = itself) | n | `n−1` pairs | O(n) |
| `cons` | `( head tail -- pair )` | Build a cons cell | 2 | `1 pair` | O(1) |
| `head-tail` | `( pair -- head tail )` | Split a pair — head under, tail on top; no auto-deref; errors on a non-pair | 1 | none | O(1) |
| `array>cons` | `( arr -- list )` | Cons chain from an array's elements (last element becomes the tail; `[ ]` → `null`) | n | `n−1` pairs | O(n) |
| `cons>array` | `( list -- arr )` | Walk a cons chain into an array, **dereferencing** the spine and each element and including the terminal (works on relational results) | n | `1a(n)` | O(n) |

`unify` decomposes/builds pairs (head then tail), and `=` compares them structurally — see Logic.

---

## Frames

Symbol-keyed sorted maps; binary-search lookup. A **path** is an array of steps; a plain *locator* is all symbols, and the literal `/a/b/c` is a compile-time constant array that allocates nothing at run time. A path may instead be a **search path** matching a set of nodes (see Path queries below). The single-target words (`@`, `!`, `delete-at`, `update-at`) require a locator and reject a search path, pointing the caller at `select-values`/`select-keys`; `has?` accepts either. `d` = path depth, `n` = frame size.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `{ :k v … }` | `( -- fr )` | Frame literal from alternating key/value pairs above the `{` mark; a path key (`/a/b/c`) vivifies nested frames | n log n | `1o` + reallocs | O(n log n) |
| `frame` | `( keys values -- fr )` | Build from parallel key and value arrays of equal length | 2 + n log n | `1o` + reallocs | O(n log n) |
| `array>frame` | `( arr -- fr )` | Build from an even-length alternating-kv array; a path key (`/a/b/c`) vivifies nested frames | 1 + n log n | `1o` + reallocs | O(n log n) |
| `frame>array` | `( fr -- arr )` | Flatten to a key-sorted alternating-kv array; inverse of `array>frame` | 1 + n | `1o` | O(n) |
| `@` | `( fr sym/path -- val )` | Get by key or path; errors if absent or if the path is a search path | 3 + d log n | none | O(d log n) |
| `!` | `( fr sym/path val -- fr )` | Set by key or path, vivifying intermediates; mutates fr; errors on a search path | d log n | realloc on growth; `1o` per vivified frame | O(d log n) amortized |
| `has?` | `( fr sym/path -- bool )` | Existence test for a frame key or path, no error on miss; a search path is true if any node matches (short-circuits at the first); on a string `( s pat -- bool )`, true if regex `pat` matches anywhere | 3 + d log n | none | O(d log n) |
| `delete-at` | `( fr sym/path -- fr )` | Remove a key (errors if absent or on a search path); mutates fr | n | none | O(n) |
| `update-at` | `( fr sym/path xt -- fr )` | Apply xt to the value at the key, store the result back; errors on a search path | d log n + xt | none | O(d log n + xt) |
| `keys` | `( fr -- arr )` | Keys (symbols) in sorted order | 1 + n | `1a(n)` | O(n) |
| `values` | `( fr -- arr )` | Values in key order | 1 + n | `1a(n)` | O(n) |
| `merge` | `( fr₁ fr₂ -- fr )` | New frame with all keys; fr₂ wins collisions | (m+n) log(m+n) | `1o` + reallocs | O((m+n) log(m+n)) |
| `copy` | `( a -- a' )` | Deep copy of any value, `copy_term`-style: dereferences bound logic vars to their values and gives each unbound var a fresh shared var; recurses into frames, arrays, matrices, strings, sets, continuations, pairs; identity for scalars. Defined generally, not frame-specific. | tree size | one object per node | O(tree size) |
| `reify` | `( a -- a' )` | Like `copy`, but each unbound var becomes a canonical inert symbol `:_0`, `:_1`, … numbered by first appearance — a ground, storable, comparable snapshot. | tree size | one object per node | O(tree size) |

### Path queries

A search path generalizes a locator with three step kinds, matching a set of nodes instead of one. Descent is through nested frames only; an array, set, or scalar is a leaf, and `//` is depth-capped against cycles.

- `*` — any one child at this level.
- `//` — descendant-or-self: any depth at or below the current node.
- `[…]` — a predicate filtering the current node: `[k]` (key `k` exists), `[k=v]`, `[k<v]`, `[k>v]` (compare key `k`'s value to `v`), `[.=v]`/`[.<v]`/`[.>v]` (compare the node itself, via `.`), or `[a/b op v]` (a sub-path subject). Several predicates on one step chain: `[role=admin][age>45]`.

So `/users/*/name` is the `:name` of every child of `:users`, `/root//city` is every `:city` at any depth, and `/people/*[age>30]` filters by predicate. `s` = nodes visited.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `select-values` | `( fr path -- arr )` | Every matched value, in document (pre-order) order, duplicates kept; no path built per match | s | `1a` + reallocs | O(s) |
| `select-keys` | `( fr path -- arr )` | The full root-to-match path (a symbol array) for every match, document order; each round-trips through `@` | s | `1a` + `1a` per match | O(s + total path length) |

`select-values` is the cheaper word (it captures the node directly, no per-match path array); `array>set` the result when distinct values are wanted, or `array>cons` to feed matches to `choose` as backtracking choice points.

---

## JSON

Objects ↔ frames (keys interned as symbols), arrays ↔ arrays, strings ↔ strings, numbers ↔ floats. JSON `true`/`false` ↔ the reserved `:1`/`:0` symbols; `null` ↔ the none value.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `json>frame` | `( s -- val )` | Parse a JSON string. Escapes and `\uXXXX` (with surrogate pairs) decode to UTF-8; recursive-descent, depth-guarded; rejects trailing non-whitespace | scan + build | one object per node | O(\|s\|) |
| `frame>json` | `( val -- s )` | Serialize a value to JSON. Floats use the shortest round-trip form; strings are escaped (non-ASCII emitted raw); object keys are the symbol names | walk + build | `1o` string | O(tree size) |
| `null` | `( -- none )` | Push the none value (`T_NONE`) — what JSON `null` parses to, and what an unset `env` returns | 1 | none | O(1) |

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

### Parallel (`docs/multicore.md`)

Run the xt across worker threads over the shared heap; `w` worker threads, `c` items per claim. The bare forms default to `num-cores` workers and claim 1. xt runs concurrently, so it must produce fresh values, not mutate shared inputs, and not print. A faulting xt aborts the region and raises an error.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `pmap` | `( arr xt -- arr )` | Parallel `map` (num-cores workers, claim 1) | 2 + n·xt | `1a(n)` | O(n·xt / w) |
| `pmap-ext` | `( arr w c xt -- arr )` | `pmap` with explicit worker count and items-per-claim | 2 + n·xt | `1a(n)` | O(n·xt / w) |
| `pfilter` | `( arr pred -- arr )` | Parallel `filter`, order preserved | 2 + n·xt | malloc(n) flags + `1a(k)` | O(n·xt / w) |
| `pfilter-ext` | `( arr w c pred -- arr )` | `pfilter` with explicit worker count and items-per-claim | 2 + n·xt | malloc(n) flags + `1a(k)` | O(n·xt / w) |
| `pmap-reduce` | `( arr id map-xt combine-xt -- val )` | Fused parallel map+fold; `combine-xt` must be associative with `id` as neutral element | 2 + n·xt | per-worker partials | O(n·xt / w) |
| `pmap-reduce-ext` | `( arr w c id map-xt combine-xt -- val )` | `pmap-reduce` with explicit worker count and items-per-claim | 2 + n·xt | per-worker partials | O(n·xt / w) |
| `num-cores` | `( -- n )` | Online CPU count (`sysconf`) | 1 | none | O(1) |

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

## Generators

Coroutines over the continuation substrate: a producer `yield`s values one at a time and a driver `resume`s it for the next. All lib.l4 on `shift`/`reset`/`resume`. `L` = captured return-stack length per step.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `yield` | `( v -- resumed )` | lib.l4: `shift` — emit v to the driver; returns whatever the driver passes back via `resume` | L | `1o` (cont) | O(L) |
| `start-generator` | `( producer -- value generator )` | lib.l4: `reset execute` — run producer to its first `yield`; leaves the yielded value and a resumable continuation | L | `1o` (cont) | O(producer to first yield) |
| `gen-take` | `( producer count -- array )` | lib.l4: the first `count` values the producer yields, collected into an array | — | `1a(count)` + cont/step | O(count · L) |
| `gen-each` | `( producer consumer -- )` | lib.l4: run consumer on each value the producer yields until the producer falls off (a `:gen-end` sentinel marks exhaustion) | — | cont/step | O(values · consumer) |

---

## Logic

Logic variables, unification, and committed choice, built on the trail and a `PROMPT_CHOICE` prompt. A capitalized identifier is a logic-var literal: at the REPL it names a persistent global logic var (created on first mention); inside a definition or quotation, declare it in `| X |` for a fresh per-call variable. `unify` records every binding on the trail; a `unify` mismatch or an explicit `fail` backtracks to the nearest `amb`. Lists are cons pairs (see Pairs): `[( H T )]` is the `[H|T]` head/tail pattern under `unify`. To keep a result past backtracking, snapshot it with `copy` (fresh vars) or `reify` (canonical `:_N`).

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `lvar` | `( -- v )` | Push a fresh, unbound logic variable | 2 | `1 lvar` | O(1) |
| `_` | `( -- wild )` | The anonymous wildcard — unifies with anything, binds nothing, allocates nothing (a constant, not a fresh var) | 2 | none | O(1) |
| `unify` | `( a b -- term )` | Unify a and b, binding logic vars (recorded on the trail) so the two match, then leave the dereffed left term. Atoms by value; pairs head then tail; arrays element-wise; frames as open records — shared keys must unify, extra keys on either side allowed. A `_` on either side matches anything and binds nothing. On a mismatch, `fail`s. | n | none | O(n) |
| `~` | `( a b -- term )` | lib.l4: `unify` (inlined) | n | none | O(n) |
| `deref` | `( v -- val )` | Follow a logic var's binding chain to the first non-variable value (v itself if unbound). Shallow — a returned structure still has bound vars inside; for a fully resolved snapshot use `reify` or `copy` | d | none | O(d) |
| `$` | `( v -- val )` | lib.l4: `deref` (inlined) | d | none | O(d) |
| `amb` | `( xt1 xt2 -- … )` | Run xt1; if it fails (a `unify` mismatch or `fail`), roll its bindings back through the trail and run xt2. Commits to the first branch that succeeds. | xt1 | none | O(xt1 + xt2) |
| `fail` | `( -- )` | Backtrack to the nearest enclosing `amb`, failing the current branch; with no enclosing `amb`, an error | 1 | none | O(L) |
| `choose` | `( list cont -- )` | lib.l4: run cont with each element of a cons list in turn, committing to the first for which it succeeds; `fail` if none do (n-way `amb` over a list) | n·cont | none | O(n·cont) |
| `matches?` | `( a b -- flag )` | Non-destructive unify test: mark the trail, unify a and b, roll the trail back, push whether they unified. Leaves no bindings and never backtracks (so it composes in straight-line code, unlike `unify`) | n | none | O(n) |
| `symbol?` | `( v -- flag )` | True when v is a symbol | 2 | none | O(1) |

---

## Fact database

A relational store built entirely from frames and sets — no new type. A **relation** is `{ :rows <set of rows> :index <index> }`; a **row** is a frame keyed by column name; a **database**, if you want several relations, is just a frame keyed by relation name (`db :father @` reaches one — no words of its own). The same shape describes a SQLite query result, so a fetched table and a hand-built relation are interchangeable (see the SQLite section below).

Rows live in a set, so an identical row asserted twice dedups to one (a relation is a set of tuples). A caller-supplied `:id` column keeps otherwise-identical rows distinct. Indexed columns are declared at creation and must be symbol-valued; `:index` maps each to a `{ value → <rows> }` frame whose buckets share the row frames in `:rows`.

`query` is unification: a pattern frame unifies against rows as an open record — shared keys must match, a logic var matches anything (projection), extra columns are ignored — which is SQL selection and projection. It collects every match (returning an array of the matching rows) by testing each candidate with `matches?` and rolling bindings back, so the pattern is left unbound. Candidates come from the index when the pattern grounds an indexed column to a symbol (intersecting buckets across several such columns, empty when a value was never asserted); otherwise it scans `:rows`.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `relation` | `( [cols] -- rel )` | New empty relation; `cols` is an array of column symbols to index | k | frames + sets | O(k) |
| `assert` | `( rel row -- rel )` | Add row to `:rows` and to each indexed column's bucket; identical row is a no-op. Mutates rel in place, returns it | k + n | reallocs | O(n) |
| `retract` | `( rel pattern -- rel )` | Remove every row matching pattern from `:rows` and all buckets. Mutates rel, returns it | matches·(k+n) | `1a` | O(matches·n) |
| `query` | `( rel pattern -- [rows] )` | Array of rows matching pattern; uses an index when the pattern grounds an indexed column, else scans. When every constraint is a ground indexed column the narrowed bucket *is* the answer, so the per-row `matches?` is skipped (covering query) | candidates·n | `1a` + set ops | O(candidates·n) |
| `count-matches` | `( rel pattern -- n )` | How many rows match; for a covering query this is the bucket's `size` with no scan, otherwise `query size` | — | (covering: none) | O(candidates) |
| `inner-join` | `( driver probed col -- [rows] )` | Inner join: each `driver` row merged (`probed` columns win collisions) with each `probed` row sharing `col`'s value; `probed` must index `col` | — | `1a` | O(driver·log probed) |
| `bulk-load` | `( rel rows-array -- rel )` | Load all rows at once: builds `:rows` (a deduped set) and each declared column's index, instead of row-by-row | — | sets + frame | O(n log n) |
| `load-bag` | `( rel rows-array -- rel )` | Like `bulk-load`, but `:rows` stays a **bag** (the array, duplicates kept) rather than a deduped set; only `:index` is built | n | frame + sets | O(n) |
| `create-index` | `( rel cols -- rel )` | Index a relation on the symbol columns `cols`: intern each indexed column's value to a symbol (so it keys the bucket and matches a `{ :col :val }` pattern), then `load-bag` into a `cols`-indexed relation. Other columns keep their type; `:rows` stays a bag. The explicit bridge from a `db-query` result to an indexed relation | n | frame + sets | O(n) |

These are lib.l4 over the C primitives `matches?`, `symbol?`, `set-add!`, `set-remove!`, `array>set`, and `group-by`. Building a relation with one `assert` per row is super-linear (each insert shifts the sorted `:rows` set, and per-value frames grow the same way); `bulk-load` avoids that with `array>set` for `:rows` (one sort) and a one-pass `group-by` per indexed column (which buckets by the interned symbol value, then sorts each small bucket — no global sort). `load-bag` and `create-index` skip the `:rows` dedup entirely, keeping a bag; `create-index` also interns the indexed columns to symbols. Candidate narrowing drives from the smallest matching bucket.

---

## Superwords (compile-time fusion) ⚠

Immediate compiler words usable only inside a definition. They detect a preceding variable-load and emit a single fused instruction that reads the variable's dict slot directly. All read `.number` without a tag check. Followed by `to dest`, they fuse further into a store variant that writes the result straight to the destination slot.

| Word | Syntax | Behavior |
|------|--------|----------|
| `vvf+` | `vvf+ a b` | Load variables a and b, add, push the result |
| `vvf-` | `vvf- a b` | Load variables a and b, subtract (a−b), push the result |
| `vvf*` | `vvf* a b` | Load variables a and b, multiply, push the result |
| `vvf/` | `vvf/ a b` | Load variables a and b, divide (a/b), push the result |
| `vf+` | `vf+ a` | Add variable a to the stack top, in place |
| `vf-` | `vf- a` | Subtract variable a from the stack top, in place |
| `vf*` | `vf* a` | Multiply the stack top by variable a, in place |
| `vf/` | `vf/ a` | Divide the stack top by variable a, in place |
| `vfsq` | `vfsq a` | Square variable a, push the result |
| `vfneg` | `vfneg a` | Negate variable a, push the result |
| `vfabs` | `vfabs a` | Absolute value of variable a, push the result |
| `vfsqrt` | `vfsqrt a` | Square root of variable a, push the result |
| `vfexp` | `vfexp a` | eᵃ of variable a, push the result |
| `vflog` | `vflog a` | base-10 log of variable a, push the result |
| `vfsin` | `vfsin a` | sine of variable a, push the result |
| `vfcos` | `vfcos a` | cosine of variable a, push the result |
| `vftan` | `vftan a` | tangent of variable a, push the result |
| `vftanh` | `vftanh a` | hyperbolic tangent of variable a, push the result |
| `vvf*+` | `vvf*+ b c` | `( t -- t*b+c )`, reading variables b and c |
| `vvf*-` | `vvf*- b c` | `( t -- c-t*b )`, reading variables b and c |

These are normally produced by the compiler's auto-fuser rather than typed by hand; `see-compiled` reveals them.

The auto-fuser also collapses a comparison immediately before a branch — `= if`, `gt while`, `0= until` — into a single compare-and-branch instruction (shown by `see-compiled` as `(=0branch)`, `(gt0branch)`, and the like). These are internal and never typed; the source stays the plain comparison followed by the control word.

---

## REPL and introspection

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `words` | `( -- )` | List all non-internal words, newest first, 8 per line | dict scan | none | O(\|dict\|) |
| `see` | `( xt -- )` | Print a word's source (`: name … ;`), or `variable`/`symbol`/primitive form | dict scan | none | O(\|dict\|) |
| `see-compiled` | `( xt -- )` | Disassemble a colon definition's compiled cells | body scan | none | O(body) |
| `man` | `( xt -- fr )` | Frame of a word's reference entry (`:word :effect :summary`, plus `:ops :alloc :order` for runtime words); `T_NONE` if undocumented | dict scan + log n | `1o` + strings | O(\|dict\|) |
| `help` | `( "name" -- )` | lib.l4: parse the next word and print its `man` frame (`lookup man .`) | dict scan + log n | `1o` + strings + print | O(\|dict\|) |
| `gc` | `( -- )` | Force a mark-sweep now | walks stacks + dict + roots, frees unmarked | none | O(objects + dict) |
| `alloc-stats` | `( -- )` | Print and reset the allocation counters since the last call (`lvars=… arrays=…`) | 2 | none | O(1) |
| `bye` | `( -- )` | `exit(0)` | — | — | — |
| `now` | `( -- f )` | `CLOCK_MONOTONIC` seconds as a float | 1 | none | O(1) |
| `sleep` | `( seconds -- )` | Block for the given float seconds (sub-second supported); `nanosleep` | blocks | none | O(1) |

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

## Files and environment

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `read-file` | `( path -- s )` | Read a whole file as one string (byte-safe); errors if it can't be opened | file read | `1o` + buffer | O(file) |
| `write-file` | `( s path -- )` | Create or truncate the file, then write the string's bytes | file write | none | O(\|s\|) |
| `append-file` | `( s path -- )` | Open in append mode, write the string's bytes | file write | none | O(\|s\|) |
| `env` | `( name -- val )` | Environment variable as a string, or the none value if unset (so set-empty `""` and unset stay distinct) | 1 | `1o` on hit | O(\|val\|) |
| `env!` | `( name value -- )` | Set an environment variable (overwriting); process-wide, so subsequent `start-process` children inherit it | 1 | none | O(1) |
| `cwd` | `( -- path )` | The interpreter's current working directory as a string (`getcwd`) | 1 | `1o` | O(\|path\|) |
| `cd` | `( path -- )` | Change the interpreter's working directory (`chdir`); process-wide, so it moves the base for relative file I/O and is inherited by subsequent `start-process` children | 1 | none | O(1) |

---

## Subprocesses and streams

A stream (`T_STREAM`) wraps an OS file descriptor — a pipe to a child process. `start-process` launches a program directly from an argv array (no shell, so no quoting or injection surface) and returns a frame `{ :pid :in :out :err }` whose `:in`/`:out`/`:err` are streams. The lifecycle is: `write` input → `close` `:in` (sends EOF) → `read` the output → `wait`. `SIGPIPE` is ignored process-wide, so a `write` to a child that has exited returns an error rather than killing the interpreter. Bytes are raw and length-counted, so streams are binary-safe.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `start-process` | `( argv -- proc )` | fork/exec `argv[0]` with `argv` as its arguments; return `{ :pid :in :out :err }` (the three streams are `T_STREAM`) | fork + 3 pipes | `1o` frame + 3 streams | O(argc) |
| `write` | `( s stream -- )` | Write the string's bytes to the stream; loops over partial writes, retries `EINTR` | write syscalls | none | O(\|s\|) |
| `read` | `( stream -- s )` | Read the stream to EOF into one string | read syscalls | `1o` + buffer growth | O(bytes) |
| `close` | `( stream -- )` | Close the fd; closing a child's `:in` sends it EOF | 1 syscall | none | O(1) |
| `wait` | `( pid -- status )` | Block until the child exits; return its exit code, or `128 + signo` if it was killed by a signal | blocks | none | O(1) |
| `stop` | `( pid -- status )` | `SIGKILL` the child then reap it (137 = 128+9, or its code if it had already exited) | 2 syscalls | none | O(1) |
| `running?` | `( pid -- bool )` | Non-blocking liveness via `waitid`+`WNOHANG`+`WNOWAIT`; true while running, false once exited. Non-reaping, so a later `wait` still returns the status | 1 syscall | none | O(1) |
| `run` | `( s -- proc )` | lib.l4: split a command string on runs of spaces and `start-process` it (`" +" split start-process`) | split + fork | `1a` + `1o` frame + 3 streams | O(\|s\| + argc) |
| `write-in` | `( s proc -- )` | lib.l4: write the string to the child's `:in` stream | write syscalls | none | O(\|s\|) |
| `read-out` | `( proc -- s )` | lib.l4: read the child's `:out` stream to EOF | read syscalls | `1o` + buffer growth | O(bytes) |
| `read-err` | `( proc -- s )` | lib.l4: read the child's `:err` stream to EOF | read syscalls | `1o` + buffer growth | O(bytes) |
| `end-process` | `( proc -- )` | lib.l4: the teardown mirror of `start-process` — close `:in`/`:out`/`:err` and `wait` `:pid` (graceful, blocks until exit) | 3 closes + wait | none | O(1) |
| `parallel-run` | `( commands width -- results )` | lib.l4: run each argv array in `commands` as a subprocess, at most `width` at once; collect `{ :out :err :status }` per command in input order, refilling a slot as each child finishes | fork per command + poll | `1a` + per-child frames/streams | O(critical path) |

Line access is `read "\n" split`.

---

## SQLite

Embedded relational storage via the vendored SQLite amalgamation, built into the binary. A database is a `T_DB` value — an inline handle into a per-interpreter registry of open connections, like a stream. `db-exec` and `db-query` take a `params` array bound positionally to the statement's `?` placeholders (`[ ]` for none): a float binds as a double, a string or symbol as text, `null` as NULL, anything else errors — so string parameters need no hand-escaping. A `db-query` result is a fact-database relation (see Fact database), so it drops straight into `query` / `inner-join` and is indexed with `create-index`. `n` = rows returned, `c` = columns.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `db-open` | `( path -- db )` | Open (creating if absent) the database file at `path` and push a handle; `":memory:"` is a private in-memory database. Errors if it can't be opened | open | 1 connection (not GC'd) | O(1)+ |
| `db-close` | `( db -- )` | Close the connection and free its registry slot. Idempotent — closing an already-closed handle is a no-op. A handle that is dropped without closing leaks the connection until process exit | 1 syscall | none | O(1) |
| `db-exec` | `( db statement params -- n )` | Bind `params` to the statement's `?` placeholders and run it with no result set (INSERT / UPDATE / DELETE / CREATE / …); return the affected-row count as a float (0 for DDL). One statement per call. On a bad statement, errors with SQLite's message | per statement | none | O(statement) |
| `db-query` | `( db query params -- rel )` | Bind `params` to the query's `?` placeholders and run it; return an index-less relation `{ :rows <array of row frames> :index { } }`. Each row is a frame keyed by column-name symbols, with INTEGER/REAL → float, TEXT → string, NULL → `null`, BLOB → string of raw bytes. `:rows` is a **bag** — duplicates kept, in result order. On a bad query, errors with SQLite's message | n·c | `1o` relation + `1a(n)` + `1o`/row + a string per text/blob cell | O(n·c) |

Using a closed handle errors (`database is closed`). Do selection, projection, and joins in the SQL itself; logicforth materializes the result. Indexing a result is a separate, explicit step — `create-index` (see Fact database) — because it interns the indexed columns to symbols, which only makes sense for low-cardinality categorical columns you choose.

---

## Type tags

| Tag | Description |
|-----|-------------|
| `T_FLOAT` | 64-bit double; any bit pattern that is not a boxed NaN |
| `T_STRING` | heap object; NUL-terminated UTF-8 bytes, `len` = byte count |
| `T_SYMBOL` | symbol-pool offset; equal names share one offset |
| `T_ARRAY` | heap object; `Val[]` |
| `T_SET` | heap object; sorted `Val[]`, binary-search membership |
| `T_PAIR` | cons cell in the dense, GC'd pair table; `{head, tail}`. Lists are `null`-terminated chains |
| `T_FRAME` | heap object; sorted parallel keys (`cell[]`) and values (`Val[]`) |
| `T_MATRIX` | heap object; r×c row-major `double[]` |
| `T_XT` | execution token (dict index); first-class callable |
| `T_ADDR` | dict index; used internally for return-stack frames |
| `T_STREAM` | OS file descriptor (a pipe end to a child process); an inline `int`, like `T_ADDR` |
| `T_DB` | inline handle into the per-interpreter registry of open SQLite connections; not GC'd (closed with `db-close`) |
| `T_CONT` | heap object; a captured return-stack slice plus a resume IP |
| `T_MARK` | ephemeral sentinel from `<`, `[`, `{`, `reset`; not user-visible |
| `T_LOGIC_VAR` | index into the logic-var stack; unbound, or bound to a Val (resolve with `deref`) |
| `T_UNBOUND` | binding sentinel for an unbound logic var; also the `_` wildcard value when on the stack |
| `T_NONE` | uninitialized / sentinel; the empty list and `null` |

Boolean convention: `1.0` true, `0.0` false.

---

## Object allocation

Most heap values use one slot in the `objects[]` table (pointer-bump, grown on demand to a 64M-entry ceiling, GC on exhaustion) plus a `calloc`'d `Object` struct plus one payload allocation. Two types are exceptions: **pairs** live in a separate dense, GC'd table (`{head, tail}` inline, no payload), and **logic vars** on a bump-allocated stack reclaimed by truncation on backtrack.

| Type | Payload |
|------|---------|
| String | `len + 1` bytes (NUL-terminated) |
| Array | `max(n,1) × sizeof(Val)` |
| Set | 4 × `sizeof(Val)` initial, doubles on overflow |
| Frame | 4 × (`sizeof(cell)` keys + `sizeof(Val)` values), doubles on overflow |
| Matrix | `r × c × sizeof(double)` (calloc, zero-filled) |
| Continuation | `max(L,1) × sizeof(Val)` |
