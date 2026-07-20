# Water reference

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

Tokens are whitespace-delimited, with self-delimiting punctuation: `;`, `]`,
and `}` always end a token and `[` and `{` always start one (the two-char
openers `[:` `[(` `[|` `[>` and closers `:]` `)]` stay whole), so `[1 2 3]`,
`{:a 1}`, and `dup *;` parse without inner spaces. A path literal's predicate
brackets (`/a[x>3]`) are kept whole by bracket balance. `<` and `>` are
ordinary word characters — set literals still need their spaces.

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
| `it` | `( -- v )` | Push the value that was on top of the stack when the current scope began — the line at top level, the word's activation inside a colon definition. Pinned per scope: non-consuming, repeatable (`it it +`), and it survives the scope consuming the operand (`: f 2 * it + ;` — `it` is still the argument). In a definition it compiles to a hidden entry-bound local (`see-compiled` shows the `(enter-anaphors)` frame; fetches fuse like any local), so using it makes stack depth 1 an entry requirement of the word; quotations inside the definition inherit the binding. At top level it reads the entry snapshot and errors at use when the line began with an empty stack; a failed line restores the stack, so `it` is unchanged after an error, and `clear it` recovers the pre-line top. `this` is an alias | 2 | none | O(1) |
| `other` | `( -- v )` | Push the second value (under the top) as it stood when the current scope began; the companion of `it` for two-antecedent lines and words. Same binding rules as `it`; requires two values at scope entry. `that` is an alias | 2 | none | O(1) |
| `them` | `( -- v₁ v₀ )` | Push the top two values as they stood at scope entry, in their original order (`them +` adds them as they lay) — equivalent to `other it`. Requires two values at scope entry | 3 | none | O(1) |
| `this` | `( -- v )` | Alias of `it` | 2 | none | O(1) |
| `that` | `( -- v )` | Alias of `other` | 2 | none | O(1) |
| `2dup` | `( a b -- a b a b )` | core.h2o: `over over` (inlined) | 10 | none | O(1) |
| `2drop` | `( a b -- )` | core.h2o: `drop drop` (inlined) | 6 | none | O(1) |
| `nip` | `( a b -- b )` | core.h2o: `swap drop` (inlined) | 5 | none | O(1) |

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
| `min2` | `( a b -- smaller )` | core.h2o: the `lt`-ordered lesser of two scalars (`2dup gt if swap then drop`, inlined); `min`/`max` reduce a matrix, these order a pair | 5 | none | O(1) |
| `max2` | `( a b -- larger )` | core.h2o: the `lt`-ordered greater of two scalars, `min2`'s twin | 5 | none | O(1) |
| `pi` | `( -- f )` | core.h2o: `variable` initialized to π (3.141592653589793); invoking it pushes the stored float | 1 | none | O(1) |

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
| `feq` | `( a b -- f )` ⚠ | float `=`, result `1.0`/`0.0`; no type check | 2 | none | O(1) |
| `flt` | `( a b -- f )` ⚠ | float `lt`, result `1.0`/`0.0`; no type check | 2 | none | O(1) |
| `fgt` | `( a b -- f )` ⚠ | float `gt`, result `1.0`/`0.0`; no type check | 2 | none | O(1) |
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

## Constants

constants.h2o, capitalized by convention. Mathematical values are computed at load;
physical values are the exact SI-2019 definitions (G is CODATA 2018, the one
measured value). The dimensioned constants are quantities, so unit algebra
applies: `KB 300 kelvin *` is an energy, `C 2 ^ 1 kg *` is E=mc².

| Word | Stack effect | Behavior |
|------|-------------|----------|
| `PI` | `( -- f )` | π |
| `E` | `( -- f )` | Euler's number e |
| `TAU` | `( -- f )` | 2π |
| `PHI` | `( -- f )` | The golden ratio (1+√5)/2 |
| `C` | `( -- q )` | Speed of light, 299792458 m/s (exact) |
| `G` | `( -- q )` | Gravitational constant, 6.67430×10⁻¹¹ m³·kg⁻¹·s⁻² |
| `H` | `( -- q )` | Planck constant, 6.62607015×10⁻³⁴ J·s (exact) |
| `HBAR` | `( -- q )` | Reduced Planck constant h/2π |
| `KB` | `( -- q )` | Boltzmann constant, 1.380649×10⁻²³ J/K (exact) |
| `NA` | `( -- q )` | Avogadro constant, 6.02214076×10²³ mol⁻¹ (exact) |
| `QE` | `( -- q )` | Elementary charge, 1.602176634×10⁻¹⁹ C (exact) |

## Unary math (polymorphic: float or matrix)

Tag-checked; safe. Float input → float; matrix input → new matrix, element-wise.
A float result that would be NaN (`-1 sqrt`, `-1 ln`) is `null` — NaN-boxing
reserves NaN bit patterns for tags, so `null` is Water's NaN, and it is falsy,
`none?`, and `= null`. Matrix buffers hold raw NaN elements untouched
(element-wise math writes them, `sort` places them last); a NaN read out of a
matrix (`@i,j`, `@e`) surfaces as `null` the same way.

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
| `mod` | `( a b -- remainder )` | core.h2o: `% drop`; sign follows dividend | 5 | none | O(1) |
| `quotient` | `( a b -- quotient )` | core.h2o: `% swap drop`; toward zero | 9 | none | O(1) |

---

## Comparison and logic

Result is `1.0` (true) or `0.0` (false), with a float fast path. `=` uses `val_cmp` (structural): matrices compare by shape then row-major contents, so they order for set membership. `lt`/`gt` are structural too, **except on matrices**, where they compare element-wise and return a 1.0/0.0 matrix (same shape, or a scalar broadcasts over the matrix). A dimensioned matrix on either side of `lt`/`gt`/`eq` also masks element-wise: the right operand rescales into the left's unit (`prices 10 $ lt` works whether prices are in `$` or `¢`), the mask comes back bare, and a quantity against a plain number or a different dimension errors. An array operand masks element-wise too: each element compares by `val_cmp` against the other operand (or pairwise against an equal-length array — unequal lengths error), yielding an n×1 mask, so `names "ann" eq where` filters a text column and string order is lexicographic. Directly before `if`/`while`/`until` a comparison fuses into a compare-and-branch, which stays structural — branching on a matrix result isn't meaningful.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `=` | `( a b -- bool )` | structural equality | 3 (float) | none | float O(1); string O(\|s\|); array/set O(n); frame O(n); matrix O(r×c) |
| `lt` | `( a b -- bool )` or `( m/arr x -- m )` | less-than; element-wise 1/0 mask on matrix operands (scalar broadcast) and on array operands (`val_cmp` per element, n×1) | 3 (float) | matrix `1m(r×c)` | same; matrix O(r×c) |
| `true` | `( -- bool )` | core.h2o: pushes 1 (inline) | 1 | none | O(1) |
| `false` | `( -- bool )` | core.h2o: pushes 0 (inline) | 1 | none | O(1) |
| `gt` | `( a b -- bool )` or `( m/arr x -- m )` | greater-than; element-wise 1/0 mask on matrix operands (scalar broadcast) and on array operands (`val_cmp` per element, n×1) | 3 (float) | matrix `1m(r×c)` | same; matrix O(r×c) |
| `eq` | `( a b -- bool )` or `( m/arr x -- m )` | equality; element-wise 1/0 mask on matrix and array operands (scalar broadcast; `val_cmp` per array element) — the mask-producing twin of `=`, which stays structural on collections. NaN elements equal nothing | 3 (float) | matrix `1m(r×c)` | same; matrix O(r×c) |
| `nan?` | `( v -- bool )` or `( m/arr -- m )` | NaN test: 1/0 mask over a matrix's elements; an array answers an n×1 mask marking `none` elements (a text column's missing cells), composing with `where`/`select-rows`; `1` on `null` itself (a scalar NaN *is* `null`), `0` on any float. The only mask route to NaNs — they compare false under `lt`/`gt`/`eq` | 2 | `1m(r×c)` | O(1); matrix/array O(n) |
| `0=` | `( a -- bool )` | `!truthy(a)`; any type | 2 | none | O(1) |
| `1=` | `( a -- bool )` | core.h2o: `1 =` (inlined) | 5 | none | O(1) |
| `type-of` | `( a -- sym )` | The value's type as a symbol: `:float` `:string` `:symbol` `:array` `:set` `:pair` `:frame` `:matrix` `:quantity` `:xt` `:continuation` `:stream` `:db` `:ptr` `:segment` `:none` `:wildcard` `:lvar`. A bound logic var reports its value's type; an unbound one is `:lvar` | 2 | none | O(1) |
| `float?` | `( a -- bool )` | core.h2o: `type-of :float =` (inlined) | 5 | none | O(1) |
| `string?` | `( a -- bool )` | core.h2o: `type-of :string =` (inlined) | 5 | none | O(1) |
| `symbol?` | `( a -- bool )` | core.h2o: `type-of :symbol =` (inlined) | 5 | none | O(1) |
| `array?` | `( a -- bool )` | core.h2o: `type-of :array =` (inlined) | 5 | none | O(1) |
| `set?` | `( a -- bool )` | core.h2o: `type-of :set =` (inlined) | 5 | none | O(1) |
| `pair?` | `( a -- bool )` | core.h2o: `type-of :pair =` (inlined) | 5 | none | O(1) |
| `frame?` | `( a -- bool )` | core.h2o: `type-of :frame =` (inlined) | 5 | none | O(1) |
| `matrix?` | `( a -- bool )` | core.h2o: `type-of :matrix =` (inlined) | 5 | none | O(1) |
| `quantity?` | `( a -- bool )` | core.h2o: `type-of :quantity =` (inlined) | 5 | none | O(1) |
| `xt?` | `( a -- bool )` | core.h2o: `type-of :xt =` (inlined) | 5 | none | O(1) |
| `continuation?` | `( a -- bool )` | core.h2o: `type-of :continuation =` (inlined) | 5 | none | O(1) |
| `stream?` | `( a -- bool )` | core.h2o: `type-of :stream =` (inlined) | 5 | none | O(1) |
| `db?` | `( a -- bool )` | core.h2o: `type-of :db =` (inlined) | 5 | none | O(1) |
| `ptr?` | `( a -- bool )` | core.h2o: `type-of :ptr =` (inlined) | 5 | none | O(1) |
| `segment?` | `( a -- bool )` | core.h2o: `type-of :segment =` (inlined) | 5 | none | O(1) |
| `none?` | `( a -- bool )` | core.h2o: `type-of :none =` (inlined) | 5 | none | O(1) |
| `wildcard?` | `( a -- bool )` | core.h2o: `type-of :wildcard =` (inlined) | 5 | none | O(1) |
| `lvar?` | `( a -- bool )` | core.h2o: `type-of :lvar =` (inlined) | 5 | none | O(1) |
| `and` | `( a b -- bool )` | logical and of truthiness | 3 | none | O(1) |
| `or` | `( a b -- bool )` | logical or of truthiness | 3 | none | O(1) |
| `not` | `( a -- bool )` | logical not of truthiness | 2 | none | O(1) |

`truthy` of a float is `≠ 0.0`; of any heap value, its handle `≠ 0`.

### Bitwise

Each operand is read as a two's-complement integer (exact within the double's
53-bit integer range), the operation is applied, and the result is pushed as a
float. `rshift` is arithmetic (sign-preserving).

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `bit-and` | `( a b -- f )` | bitwise AND | 2 | none | O(1) |
| `bit-or` | `( a b -- f )` | bitwise OR | 2 | none | O(1) |
| `bit-xor` | `( a b -- f )` | bitwise XOR | 2 | none | O(1) |
| `bit-not` | `( a -- f )` | two's-complement complement | 1 | none | O(1) |
| `lshift` | `( a n -- f )` | left shift `a` by `n` bits | 2 | none | O(1) |
| `rshift` | `( a n -- f )` | arithmetic right shift, = `floor(a / 2ⁿ)` | 2 | none | O(1) |
| `lowest-bit` | `( a -- i )` | 0-indexed position of the lowest set bit (`-1` if `a` is 0) | 1 | none | O(1) |

---

## Dimensioned quantities

A quantity is a magnitude (a float or a matrix) carrying a unit. Units are
rational-exponent vectors over user-declared base dimensions, each with a
rational scale relative to its dimension's base; arithmetic propagates and
checks them. Same-dimension units at any rational scale coexist and convert
(`$`/`¢`, `kg`/`g`, `inch`/`cm`). What's excluded is affine offsets — `°C`/`°F`
need an added zero, not just a scale factor.

Declare with `base` and `unit`:

    base unit m   base unit s   base unit kg      \ base dimensions
    1 kg 1 m * 1 s / 1 s / unit newton            \ derived, named
    base unit $   1 $ 100 / unit ¢                \ scaled sub-unit (1/100)

A unit word is postfix — it attaches its unit to the number before it (`10 m`,
`3 newton`). Attaching a unit to a matrix makes a dimensioned matrix (`M m`).

- `*` / `/` — multiply/divide magnitudes; combine unit exponents and scales. A
  dimensionless result collapses back to a bare float or matrix, folding the
  scale ratio into the magnitude (`3 m 3 m / → 1`, `1 hour 1 s / → 3600`).
- `+` / `-` — require the same dimension; a different-scale operand is rescaled
  into the left operand's unit (`1 $ 50 ¢ + → 1.5 $`). Different
  dimension, or a quantity ± a bare number, errors.
- `^` — rational exponent only; raises the unit's exponents (`q 2 ^`, `q 0.5 ^`).
- `sqrt` — halves the unit's exponents (`sqrt(m²) → m`).
- `negate` / `abs` — keep the unit; transcendentals (`sin`, `log`, …) reject a quantity.
- `=` `lt` `gt` — compare by value, normalizing scale within a dimension (`100 ¢ 1 $ = → 1`). On a dimensioned matrix, `lt`/`gt`/`eq` answer an element-wise bare mask with the same normalization (`prices 10 $ lt`); `=` stays structural everywhere.

Printing shows magnitude then unit: a named unit prints its name (`3 newton`); an
unnamed compound prints its dimensional form with the scale folded into the
magnitude (`10 m 2 s / → 5 m.s^-1`), positive exponents first. Quantities,
units, and unit words round-trip through `save-image`/`load-image`.

The matrix statistics accept a dimensioned matrix and keep its unit: `sum`
`mean` `max` `min` `quantile` (so `median` `percentile` `iqr` `ci`) `row-sums`
`column-sums` `cumulative-sum` `norm` `reshape` `transpose` `select-rows`
answer in the operand's unit, `matrix>array` yields per-element quantities,
`var` answers in the unit squared (`std`/`se` return to the unit through
`sqrt`), and the index and count words (`argsort` `argmax` `argmin` `size`
`dim`) and the correlations answer bare — correlation is scale-invariant, so
dimensioned inputs are computed over their magnitudes.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `magnitude` | `( v -- v' )` | A quantity's bare magnitude (float or matrix, the unit dropped); any other value passes through unchanged | 2 | none | O(1) |
| `unit-of` | `( v -- q\|1 )` | A quantity's unit as the quantity `1` in that unit (`10 km` → `1 km`, a matrix column in `m` → `1 m`, computed units in dimensional form — `1 m.s^-1`); a bare value answers `1.0`. Composes: `x unit-of *` attaches x's unit, `1 s =` tests for a unit | 2 | 1 pair | O(1) |

`units.h2o` predeclares a standard set (names spelled out and lowercase):
length `m` (`km`), time `s` (`minute`, `hour`, `day`, `week`), mass `kg`, current `ampere`,
temperature `kelvin`, amount `mol`; derived `hertz` `newton` `pascal` `joule`
`watt` `coulomb` `volt`; and three currencies, each its own dimension —
`$`/`¢`, `£`/`penny`, `€`/`eurocent`.

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
| `leave` | — | Branch past the innermost loop's closing word; conditional form is `if leave then` |
| `continue` | — | Branch back to the innermost loop's `begin`: a `while` loop re-runs its test; an `until` loop skips its trailing test and repeats unconditionally |
| `exit` | `( -- )` | Return early from the current definition (this one runs at run time) |

`leave` and `continue` are plain compiled branches — zero runtime cost. Both
are compile errors outside a loop, and a quotation opens its own frame, so a
`[: leave :]` inside a loop body does not see that loop. A `begin` with no
`until`/`again`/`repeat` is a compile error at `;` or `:]` (an unpatched
`leave` would otherwise be a wild branch); the partial definition rolls back.
In `times` / `i-times` quotations, `exit` already ends the current iteration.

---

## Delimiters

Literal-building words. Each is an ordinary dictionary word; the openers and
closers are self-delimiting tokens (see the note in the introduction).

| Word | Stack effect | Behavior |
|------|-------------|----------|
| `[` | — | Open an array literal; `]` closes it |
| `]` | — | Close an array literal |
| `{` | — | Open a frame literal (alternating key/value); `}` closes it |
| `}` | — | Close a frame literal |
| `<` | — | Open a set literal; `>` closes it (both need surrounding spaces) |
| `>` | — | Close a set literal |
| `[(` | — | Open a cons-list literal; `)]` closes it |
| `)]` | — | Close a cons-list literal |
| `:]` | — | Close a quotation (any `[:` / `[\|` / `[>` form; `[:` itself is under Defining) |
| `[\|` | — | Open a quotation whose body starts with a `\| … \|` locals list |
| `[>` | — | Open a quotation whose locals list receives every slot from the stack |
| `\|` | — | Declare word-locals at a definition's head: `\| x y \|` |
| `\|>` | — | Locals list in which every slot receives from the stack |

## Defining and compiling words

These parse following tokens and/or compile code. Costs are dominated by compilation, not by a stack effect, so no cost columns.

| Word | Stack effect | Behavior |
|------|-------------|----------|
| `:` | — | Begin a colon definition; read the following name; enter compile mode |
| `;` | — | End a colon definition; emit `exit`; store the source text for `see`. Self-delimiting: `dup *;` parses |
| `variable` | — | Read the following name; declare a global variable initialized to `0.0` |
| `constant` | `( val -- )` | Pop a value and read the following name; define an inline word that pushes it as a literal, so call sites fold to the literal with no run-time fetch. Fixed at definition — `to` cannot reassign it |
| `to` | `( val -- )` | Assign to the named local (in a definition) or global. At the REPL, auto-creates the global if absent. In a definition, the variable must already exist. May trigger superword store-fusion while compiling. |
| `symbol` | — | Read the following name; declare a word that pushes a specific interned symbol |
| `base` | `( -- q )` | Push a base quantity — a fresh dimension with its base unit, magnitude `1.0`. Paired with `unit` to declare a base dimension (`base unit m`) |
| `unit` | `( q -- )` | Read the following name; pop a quantity whose magnitude is a positive whole number, and define a postfix word attaching that unit. The magnitude is the unit's integer scale relative to its dimension's base (`100 cent unit dollar`). A single unnamed base dimension gets named after the word |
| `:name` | `( -- sym )` | Symbol literal; interns the name at read time |
| `string>symbol` | `( s -- sym )` | Intern a computed string as a symbol |
| `[:` | `( -- xt )` | Open an anonymous quotation (closed by `:]`); compiles its body and pushes its xt |
| `'` | `( "name" -- xt )` | Parse the following word at compile time and push its xt (immediate; folds the xt in as a literal) |
| `lookup` | `( "name" -- xt )` | Parse the following word at run time and push its xt — the non-immediate counterpart of `'` |
| `execute` | `( xt -- … )` | Call the word at xt |
| `curry` | `( value xt -- xt' )` | Bind a value into a new anonymous word: xt' pushes value, then calls xt. Compiles ~10 permanent dictionary cells per call (reclaimed only by `forget`); errors inside a parallel region — curry before `pmap`, execute freely within |
| `inline` | — | Mark the most recent definition inline; future calls splice its body. A body containing a quotation is not spliced — such calls compile as plain calls, since a copied quotation header would have no recorded span |
| `internal` | — | Mark the most recent definition internal: hidden from `words`, `apropos`, and completion (still findable by name and tick) |
| `forget` | — | Read the following name; truncate the dictionary back to before it |

### Locals

Declared only at the **head** of a definition or quotation body. Live on the return stack: up to 128 names across up to 16 nested scopes. Quotations close over the enclosing definition's locals **by frame position, resolved at execution**: run at the depth it was compiled for (passed to a C word like `map`/`execute` called in the defining word itself), the capture works; executed inside another colon word's locals frame, the same reference silently reads that word's slots instead. A quotation that must travel through other words takes its values from the stack (`[>` receive-all) with `curry` binding them in.

| Syntax | Behavior |
|--------|----------|
| `\| x y z \|` | Declare x, y, z, **uninitialized** (slots keep stale return-stack contents — deliberately no per-call zeroing; assign with `to` before reading — at `;`/`:]` the compiler rejects a slot fetched but stored nowhere, naming a shadowed word when one exists); read by bare name |
| `\|> x y z \|` | Declare and receive from the stack: z ← top, y ← second, x ← third |
| `\| x >y z \|` | Mixed: a `>` prefix marks an individual name as a receive slot; the rest are uninitialized |
| `\| ?x \|` | A `?` prefix marks a slot initialized with a fresh logic variable per call; read by bare name. Cannot combine with `>`, and not allowed in the all-receive `\|>` / `[>` forms |
| `[\| x y z \| … :]` | Lambda sugar: `[\|` fuses `[:` and `\|` into one token, opening an anonymous quotation whose body begins with a `\|` locals list (`>` prefixes receive selectively) |
| `[> x y z \| … :]` | Lambda sugar for the receive-all case: `[>` fuses `[:` and `\|>`, so x, y, z are received from the stack |

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
| `render` | `( a -- s )` | The text `.` would print, returned as a string instead of printed: no truncation, no trailing separator (a matrix grid's final newline is dropped). Strings render raw, symbols by name, collections/frames/matrices in their laid-out form | 1 + size | `1o` | O(size) |
| `.s` | `( -- )` | Print every stack value, bottom to top; leaves the stack intact | print | none | O(depth) |
| `peek` | `( a -- a )` | core.h2o: print the top value then a space without consuming it (`dup .`, inlined) — a stack probe | 1 + print | none | O(size printed) |
| `,` | `( a -- a )` | core.h2o: `peek` under a one-character name, for splicing probes into a pipeline (inlined) | 1 + print | none | O(size printed) |
| `print` | `( x -- )` | core.h2o: alias for `.` | 1 + print | none | O(size printed) |
| `print-stack` | `( -- )` | core.h2o: alias for `.s` | print | none | O(depth) |
| `cr` | `( -- )` | Print a newline | 1 | none | O(1) |
| `emit` | `( code -- )` | Print the character with codepoint `code`, UTF-8 encoded (1–4 bytes); range-checked `[0, 0x10FFFF]` | 1 | none | O(1) |

String literals `"…"` are **raw**: bytes between the quotes are copied verbatim and an embedded newline is kept; the only escape is a doubled `""`, which yields one `"` (a lone `"` closes the string). There is no `{n}` substitution — a regex `\d{3}` literal is safe, and template-filling is the explicit word `format` (in String operations below).

---

## String operations

Regex words run on PCRE2 with JIT-compiled patterns. Each distinct pattern is compiled once and cached (a 1024-slot hash table keyed on the pattern bytes, bounded probe window), so reusing a pattern costs a hash plus one comparison, then the match. Patterns are PCRE syntax in raw `"…"` literals — PCRE itself interprets `\n`, `\t`, `\d`, `\x22`, and the rest. Matching is multiline: `^` and `$` bind to line boundaries. Patterns and subjects are treated as **UTF-8**: `.` matches one codepoint, and `\w` `\d` `\s` `\b` use Unicode properties (accented letters, non-ASCII digits). Invalid byte sequences are tolerated — they simply fail to match rather than raising an error. Match offsets are **byte** offsets (pair them with `byte-substring`). Captures come back as strings; an optional group that didn't participate is `0.0`. Booleans are `1.0`/`0.0`. In the cost columns `n` is the subject length.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `match` | `( s pat -- [ whole cap… ] \| 0 )` | First (leftmost) match as a flat array: whole match then each capture; no match returns `0` | n | `1a` + captures | O(n) |
| `match-all` | `( s pat -- [ [whole cap…] … ] \| 0 )` | Every non-overlapping leftmost match, each a flat sub-array; a zero-width match advances one byte; no match returns `0` | n | `1a` per match + captures | O(n + m·g) |
| `replace` | `( s pat rep -- s' )` | Replace **all** matches; in `rep`, `&` or `\0` is the whole match, `\1`–`\9` a capture, `\&` and `\\` literals | n | `1o` + buffer growth | O(n) |
| `xml-escape` | `( s -- s' )` | strings.h2o: `&` `<` `>` `'` to their XML entities, for element text and single-quoted attributes; four `replace` passes | 4n | 4 strings | O(n) |
| `basename` | `( path -- filename )` | strings.h2o: the path's last component (`"^.*/" "" replace`, inlined); a path with no `/` passes through | n | `1o` | O(n) |
| `split` | `( s pat -- [ piece… ] )` | Split `s` at each non-overlapping match of `pat`; the pieces are the gaps between matches, empty fields kept; no match → `[ s ]` | n | `1a` + pieces | O(n) |
| `substring` | `( s start end -- sub )` | Half-open **codepoint** range `[start, end)`; bounds-checked against the codepoint count | 2 + n | `1o` | O(n) |
| `byte-substring` | `( s start end -- sub )` | Half-open **byte** range `[start, end)`; bounds-checked. Pairs with byte offsets from `match`/`match-all` | 2 + k | `1o` | O(k), k = end − start |
| `char-at` | `( s index -- char )` | The one-character string at codepoint `index`; bounds-checked against the codepoint count | 2 + n | `1o` | O(n) |
| `codepoint-at` | `( s index -- code )` | The integer codepoint at codepoint `index`; bounds-checked | 2 + n | none | O(n) |
| `string>chars` | `( s -- [ char… ] )` | Array of one-character strings, one per codepoint | n | `1a` + `1o`/char | O(n) |
| `string>codepoints` | `( s -- [ code… ] )` | Array of integer codepoints, one per codepoint | n | `1a` | O(n) |
| `codepoint>char` | `( code -- char )` | One-character string for codepoint `code`; range-checked `[0, 0x10FFFF]` | 1 | `1o` | O(1) |
| `codepoints>string` | `( [ code… ] -- s )` | Encode each codepoint to UTF-8 and concatenate; per-element type- and range-checked | n | `1o` | O(n) |
| `trim` | `( s -- s' )` | Strip leading and trailing ASCII whitespace (`' ' \t \n \v \f \r`); a backward/forward byte-scan, one allocation of the surviving span | n | `1o` | O(n) |
| `join` | `( arr sep -- s )` | Concatenate the string elements of `arr` separated by `sep`; errors on a non-string element | 2 + total | `1o` | O(total) |
| `index-of` | `( s pat -- i )` | strings.h2o: codepoint index of `pat`'s first regex match in `s`, or `-1` if none (`split 0 @i size` guarded by `has?`) | n | `1a` + pieces | O(n) |
| `string>number` | `( s -- n \| none )` | Parse a decimal/float string (via `strtod`, like a numeric literal) to a float, ignoring surrounding whitespace; the none value if `s` is not entirely a number | n | none | O(n) |
| `edit-distance` | `( a b -- n )` | Edit distance between two strings over codepoints: insertions, deletions, substitutions, and adjacent transpositions each cost 1 (Levenshtein with transpositions — optimal string alignment); symmetric | n·m | none | O(n·m) |
| `format` | `( … template -- s )` | Fill `template`'s `{n}` (or `{n:spec}`) placeholders with the nth-from-top stack value, then drop exactly the referenced positions (unreferenced values stay); renders floats/strings/symbols. `{nl}` and `{tab}` emit a newline and a tab — string literals have no escapes, so format is where control characters come from. Only these directives substitute; other brace content is left literal | len + refs | `1o` | O(len) |

A placeholder may carry a format spec after a colon — `{n:spec}` — a printf-style mini-language controlling how the value renders. `spec` is optional flags (`-`, `+`, space, `#`, `0`), an optional field width, an optional `.precision`, and an optional conversion letter:

- `f` `e` `g` (and `F` `E` `G`) render the value as a float: `{0:.2f}` fixes the precision, `{0:8.2f}` also pads to a field width.
- `d` / `i` render it as an integer, truncated toward zero: `{0:04d}`.
- `s`, or no conversion letter, places the value's default rendering in a field: `{0:8}` right-justifies, `{0:-8}` left-justifies, `{0:.3}` truncates to three characters.

A float or integer conversion requires a float operand; a non-float operand, an unknown conversion letter, or trailing characters in the spec is an error. With no colon, `{n}` renders the value in its default form.

`first match` and `findall` are spelled `match` and `match-all`; there is no separate search/match/fullmatch split. Anchor with `^`/`$` (or `\A`/`\z`) when you need it.

---

## Sets

Sorted `Val` arrays with binary-search insertion; equality is structural. `+`/`*`/`-` on two sets are union/intersection/difference.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `< v… >` | `( -- set )` | Set literal; `<` pushes a mark, `>` gathers everything above it in one sort-and-dedup pass, like `set` | n log n | `1o` + realloc | O(n log n) |
| `set` | `( v₀ … vₙ₋₁ n -- set )` | Gather the top n values into a new set (the set analog of `array`) | 2 + n log n | `1o` + reallocs | O(n log n) |
| `union` | `( s₁ s₂ -- s₃ )` | Union into a new set, merging the two sorted arrays | m+n | `1o` + reallocs | O(m+n) |
| `intersection` | `( s₁ s₂ -- s₃ )` | Intersection into a new set, merging the two sorted arrays | m+n | `1o` + reallocs | O(m+n) |
| `difference` | `( s₁ s₂ -- s₃ )` | s₁ − s₂ into a new set, merging the two sorted arrays | m+n | `1o` + reallocs | O(m+n) |
| `set-add!` | `( set v -- set )` | Insert v in sorted position if absent (dedups); leaves set on the stack | log n + n | reallocs | O(n) |
| `set-remove!` | `( set v -- set )` | Remove v if present (no-op if absent); leaves set on the stack | log n + n | none | O(n) |
| `member?` | `( set v -- bool )` | Binary-search membership | 3 + log n | none | O(log n) |
| `array>set` | `( array -- set )` | Sort a copy of the array once and dedup into a set — the fast bulk constructor (one sort, not n inserts); the source array is unchanged | n log n | `1o` + realloc | O(n log n) |
| `group-by` | `( array col -- frame )` | Group an array of frames by their symbol-valued `col` into a frame from each value to a set of the matching rows; one sorted pass, distinct values sorted | n log n | frame + sets | O(n log n) |
| `size` | `( coll -- n )` | Element count: set/array members, **codepoints** of a string, pair count of a frame; a string's codepoint count is computed on first use and memoized on the object | 2 | none | O(1); a string's first `size` is O(n) |
| `byte-size` | `( s -- n )` | Byte length of a string | 2 | none | O(1) |

---

## Arrays

0-indexed, elements of any type. Grows at the end in amortized O(1) over a backing buffer that doubles on demand; indexing stays O(1).

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `[ v… ]` | `( -- arr )` | Array literal; `[` marks, `]` gathers above the mark | n | `1a(n)` | O(n) |
| `array` | `( v₀ … vₙ₋₁ n -- arr )` | Gather the top n values into an array | 2 + n | `1a(n)` | O(n) |
| `array-of` | `( val n -- arr )` | New n-element array, every slot = val | 3 + n | `1a(n)` | O(n) |
| `@i` | `( arr i -- val )` | Array element; on a matrix returns row i as a 1×c matrix | 3 (array) | matrix `1m(1×c)` | O(1) array; O(c) matrix |
| `!i` | `( arr i val -- arr )` | Store val at index i in place; leaves arr on the stack | 4 | none | O(1) |
| `add-last!` | `( arr v -- arr )` | Append v at the end, doubling the backing buffer when full; leaves arr on the stack | 2 | `≤1a` on grow | amortized O(1) |
| `remove-last!` | `( arr -- v )` | Remove and return the last element; errors on an empty array | 2 | none | O(1) |
| `take` | `( arr/set n -- arr )` | First n elements (clamped) | 2 + n | `1a(n)` | O(n) |
| `reverse` | `( arr/set -- arr )` | Reversed copy | 1 + n | `1a(n)` | O(n) |
| `concat` | `( arr/set arr/set -- arr )` | Concatenated copy | 2 + m + n | `1a(m+n)` | O(m+n) |
| `range` | `( from to -- arr )` | Inclusive integer range, step ±1 | 3 + n | `1a(n)` | O(n) |
| `destruct` | `( arr/set/fr -- v… )` | Spread elements onto the stack; a frame spreads alternating sym/value | 1 + n | none | O(n) |
| `destruct-to` | `( source targets -- )` | source and target arrays; assign each source element to the variable named by the corresponding target (symbol or xt), creating it if needed | 2 + n | may create variables | O(n) |
| `slice!` | `( arr tstart src sstart sstep slen -- arr )` | Copy `slen` elements `src[sstart], src[sstart+sstep], …` into `arr[tstart…]` in place | 6 + slen | self-overlap may malloc slen | O(slen) |
| `to-slice!` | `( v₀ … vₙ₋₁ arr offset n -- arr )` | Store the n values just below `arr` into `arr[offset…offset+n)`; leaves arr | 2 + n | none | O(n) |
| `last` | `( arr n -- arr )` | arrays.h2o: `swap reverse swap take reverse` | 3n | 3×`1a(n)` | O(n) |
| `first` | `( arr/pair -- v )` | core.h2o: element 0 of an array, or a cons's head — reads pairs-shaped results (`count`, `group-indices`) and logic pairs alike | 4 | none | O(1) |
| `second` | `( arr/pair -- v )` | core.h2o: element 1 of an array, or a cons's tail (`5 6 cons second` → 6; on a list literal the rest, not the next element) | 4 | none | O(1) |
| `skip` | `( arr n -- arr )` | arrays.h2o: `over size swap - swap reverse swap take reverse` | 3n | 3×`1a(n)` | O(n) |
| `sort` | `( arr/set/v -- arr/v )` | Sorted copy: an array orders by `val_cmp`; a set projects its already-ordered elements to an array; an nx1 or 1xn vector sorts ascending with NaNs last (other matrix shapes error) | 1 + n log n | `1a(n)` / `1m(n)` | O(n log n); vectors above 8k elements O(n) radix |
| `flatten-array` | `( arr -- arr )` | Flatten one level; returns the input unchanged if no element is itself an array | 1 + m | `1a(m)` | O(m) |
| `sample` | `( arr/set count repl -- arr )` | Draw `count` elements; `repl` truthy = with replacement, else without (count ≤ len) | 3 + n | `1a(count)` (+ `malloc(n)` without replacement) | O(n) |
| `shuffle` | `( arr -- arr )` | datasets.h2o: new array, elements uniformly permuted (a full `sample` without replacement); input untouched | 3 + n | as `sample` | O(n) |
| `resample` | `( arr/set -- arr )` | datasets.h2o: same-size draw with replacement (a full `sample` with replacement, the bootstrap draw); input untouched — the value-space sibling of `resample-indices` | 3 + n | `1a(n)` | O(n) |
| `iota` | `( n -- arr )` | arrays.h2o: `[0…n−1]`, empty when n ≤ 0 | 3 + n | `1a(n)` | O(n) |

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
| `{ :k v … }` | `( -- fr )` | Frame literal from alternating key/value pairs above the `{` mark; a path key (`/a/b/c`) vivifies nested frames. Built by sorted insertion — a binary search plus a shift per pair; `frame` / `array>frame` are the sort-once bulk constructors | n·(log n + n) | `1o` + reallocs | O(n²) |
| `frame` | `( keys values -- fr )` | Build from parallel key and value arrays of equal length | 2 + n log n | `1o` + reallocs | O(n log n) |
| `array>frame` | `( arr -- fr )` | Build from an even-length alternating-kv array; a path key (`/a/b/c`) vivifies nested frames | 1 + n log n | `1o` + reallocs | O(n log n) |
| `frame>array` | `( fr -- arr )` | Flatten to a key-sorted alternating-kv array; inverse of `array>frame` | 1 + n | `1o` | O(n) |
| `@` | `( fr sym/path -- val )` | Get by key or path; errors if absent or if the path is a search path | 3 + d log n | none | O(d log n) |
| `@or` | `( fr sym/path fallback -- val )` | Get by key or path, the fallback when absent — `has? if @` in one probe, no error on miss; the fallback is already evaluated, so it suits values, not expensive computations | 4 + d log n | none | O(d log n) |
| `!` | `( fr sym/path val -- fr )` | Set by key or path, vivifying intermediates; mutates fr; errors on a search path | d log n | realloc on growth; `1o` per vivified frame | O(d log n) amortized |
| `has?` | `( fr sym/path -- bool )` | Existence test for a frame key or path, no error on miss; a search path is true if any node matches (short-circuits at the first); on a string `( s pat -- bool )`, true if regex `pat` matches anywhere | 3 + d log n | none | O(d log n) |
| `delete-at` | `( fr sym/path -- fr )` | Remove a key (errors if absent or on a search path); mutates fr | n | none | O(n) |
| `update-at` | `( fr sym/path xt -- fr )` | Apply xt to the value at the key, store the result back; errors on a search path | d log n + xt | none | O(d log n + xt) |
| `keys` | `( fr -- arr )` | Keys (symbols) in sorted order | 1 + n | `1a(n)` | O(n) |
| `values` | `( fr -- arr )` | Values in key order | 1 + n | `1a(n)` | O(n) |
| `merge` | `( fr₁ fr₂ -- fr )` | New frame with all keys; fr₂ wins collisions. A linear two-pointer merge of the two sorted key arrays | m+n | `1o` | O(m+n) |
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
| `json>frame` | `( s -- val )` | Parse a JSON string. Escapes and `\uXXXX` (with surrogate pairs) decode to UTF-8; recursive-descent, depth-guarded; rejects trailing non-whitespace. Each object's keys are sorted after collection | scan + build | one object per node | O(\|s\| log \|s\|) |
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
| `vector` | `( arr -- v )` | matrix.h2o: the array as an nx1 matrix, length inferred (`dup size 1 matrix`, inlined) | 3 + n | `1m(n)` | O(n) |
| `diagonal-matrix` | `( fill n -- m )` | n×n matrix with `fill` on the diagonal | 2 + n | `1m(n×n)` | O(n) |
| `identity-matrix` | `( n -- m )` | matrix.h2o: `1 swap diagonal-matrix` | n | `1m(n×n)` | O(n) |
| `matrix-range` | `( start end step -- m )` | 1×N row of evenly spaced values | 3 + N | `1m(1×N)` | O(N) |

### Shape and indexing

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `@j` | `( m j -- col )` | Column j as an r×1 matrix (copy) | 2 + r | `1m(r×1)` | O(r) |
| `@i,j` | `( m i j -- f )` | Single element as a float | 4 | none | O(1) |
| `@e` | `( m i -- f )` | Element at flat row-major index i as a float — consumes `argmin`/`argmax`/`where` indices; the same access on n×1 and 1×n vectors | 3 | none | O(1) |
| `!e` | `( m i v -- m )` | Store v (a float, or `null` for NaN) at flat row-major index i, in place | 4 | none | O(1) |
| `!i,j` | `( m i j v -- m )` | Store v (a float, or `null` for NaN) at row i, column j, in place | 5 | none | O(1) |
| `dim` | `( m/dataset -- r c )` | Push rows then columns; datasets.h2o extends it to a dataset — rows from the first column's length, columns from the key count | 3 | none | O(1) |
| `reshape` | `( m r c -- m' )` | Same elements, new shape (must match); memcpy | 3 + r×c | `1m(r×c)` | O(r×c) |
| `transpose` | `( m -- m' )` | Rows/columns swapped | 1 + r×c | `1m(c×r)` | O(r×c) |
| `diagonal` | `( m -- m' )` | Diagonal as a 1×min(r,c) matrix | 1 + min(r,c) | `1m(1×min)` | O(min(r,c)) |
| `flatten` | `( m -- m' )` | matrix.h2o: 1×(r·c) reshape | r×c | `1m(1×r·c)` | O(r×c) |
| `as-column` | `( v -- v' )` | matrix.h2o: any vector shape as n×1 (`dup dim * 1 reshape`, inlined) | r×c | `1m(n×1)` | O(n) |
| `matrix>array` | `( m -- arr )` | The elements as an array in row-major order: floats from a bare matrix; a dimensioned matrix yields one quantity per element in its unit; a NaN element becomes `null` either way | 1 + r×c | `1a(r×c)`; dimensioned + 1 pair per non-NaN element | O(r×c) |
| `num-elements` | `( m -- n )` | matrix.h2o: `dim *` (inlined) | 5 | none | O(1) |
| `n-rows` | `( m/dataset -- n )` | datasets.h2o: `dim drop` | 6 | none | O(1) |
| `n-columns` | `( m/dataset -- n )` | datasets.h2o: `dim nip` | 8 | none | O(1) |

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
| `argmax` | `( m -- f )` | Flat row-major index of the maximum element (first on ties) | 1 + r×c | none | O(r×c) |
| `argmin` | `( m -- f )` | Flat row-major index of the minimum element (first on ties) | 1 + r×c | none | O(r×c) |
| `row-sums` | `( m -- m' )` | r×1 of per-row sums | 1 + r×c | `1m(r×1)` | O(r×c) |
| `row-maxes` | `( m -- m' )` | r×1 of per-row maxima | 1 + r×c | `1m(r×1)` | O(r×c) |
| `row-mins` | `( m -- m' )` | r×1 of per-row minima | 1 + r×c | `1m(r×1)` | O(r×c) |
| `column-sums` | `( m -- m' )` | 1×c of per-column sums | 1 + r×c | `1m(1×c)` | O(r×c) |
| `column-maxes` | `( m -- m' )` | 1×c of per-column maxima | 1 + r×c | `1m(1×c)` | O(r×c) |
| `column-mins` | `( m -- m' )` | 1×c of per-column minima | 1 + r×c | `1m(1×c)` | O(r×c) |
| `mean` | `( m -- f )` | matrix.h2o: sum ÷ element count | r×c | none | O(r×c) |
| `row-means` | `( m -- m' )` | matrix.h2o: `row-sums` then scalar ÷ | r×c | 2×`1m(r×1)` | O(r×c) |
| `column-means` | `( m -- m' )` | matrix.h2o: `column-sums` then scalar ÷ | r×c | 2×`1m(1×c)` | O(r×c) |

### Reshaping, selection, statistics

The statistics treat NaN elements as missing values and skip them: `sum` and
`norm` count nothing missing as 0; `mean` `var` `std` `se` `min` `max`
`quantile` `median` `percentile` `iqr` `ci` `argmax` `argmin` error with "all
elements are NaN (missing)" (`var`: "needs at least 2 non-NaN elements") when
too little remains; the correlations delete row i from both vectors when
either element i is NaN, and error below 2 complete pairs; `regress-with`
deletes incomplete rows of its design matrix before fitting. The positional
operations (`cumulative-sum`, the row/column reductions, `dot`, `dgemm-*`)
keep NaN in place.

`c` below is the output column count; `k` the index count; `n = r×c`.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `augment` | `( a b -- m )` | Concatenate two matrices column-wise; errors unless row counts match | 2 + r·c | `1m(r×c)` | O(r·c) |
| `vstack` | `( a b -- m )` | Stack two matrices row-wise (a on top of b); errors unless column counts match | 2 + r·c | `1m(r×c)` | O(r·c) |
| `hstack` | `( a b -- m )` | matrix.h2o: `augment` under its numpy name (inlined) | 2 + r·c | `1m(r×c)` | O(r·c) |
| `submatrix` | `( m rs re cs ce -- m )` | Copy the half-open block rows [rs,re) × cols [cs,ce); errors out of bounds or start > end | 5 + r·c | `1m(r×c)` | O(r·c) |
| `select-rows` | `( m/dataset/arr idx -- same )` | New matrix of the rows named by `idx` — a float index array or an index vector (nx1 or 1xn, as `where`/`argsort` return); a dimensioned matrix keeps its unit; errors on a non-float or out-of-range index. datasets.h2o extends it to a dataset (every column gathered by the same indices — matrix and dimensioned columns through the matrix path, array columns element-wise) and to a bare array (elements gathered by index) | 2 + k·c | `1m(k×c)`; dataset one column each; array `1a(k)` | O(k·c) |
| `mesh` | `( v mask b -- v' )` | Masked substitution: element i of the result is `b`'s where `mask[i]` is a definite nonzero, `v`'s where it is 0 **or NaN** (an unknown mask cell changes nothing). `v` is a matrix, dimensioned matrix, or array; the mask a bare matrix of `v`'s shape (element count, for an array). `b` is shape-matched same-representation, or broadcasts: a float, `null` (→ NaN), a quantity, or — for an array subject — any single value. Units reconcile as `+`: `b` rescales into `v`'s unit, which the result keeps; a quantity against a bare number errors. Conditional-mutate idioms: `dup nan? 0 mesh` fills NaNs, `dup -1 eq null mesh` turns a sentinel into NaN, `dup 100 gt 100 mesh` caps | 3 + n | `1m(r×c)` / `1a(n)` | O(n) |
| `argsort` | `( v -- v' )` or `( arr -- arr )` | The sorting permutation of a vector, shape preserved: element i is the source index of the i-th smallest value; ties keep index order, NaNs go last in index order; ranks are argsort twice. arrays.h2o extends it to an array: the permutation under `val_cmp` (structural, so mixed types order), ties keep index order, returned as a float-index array | 1 + n log n | `1m(n)` + `malloc(16n)`; array 3×`1a(n)` + n×`1a(2)` | O(n log n); above 8k elements O(n) radix |
| `ranks` | `( v -- v' )` | statistics.h2o: 0-based ordinal ranks as nx1, `as-column argsort argsort` (inlined); ties ranked in index order, not midranked | 2n log n | `2m(n)` + `malloc(16n)` ×2 | O(n log n) |
| `where` | `( m -- v )` | Flat row-major indices of the nonzero elements, as a k×1 index vector (1×k for a 1×n mask); composes with the `lt`/`gt` masks and `select-rows` | 1 + n | `1m(k)` | O(n) |
| `drop-nans` | `( v -- v' )` | matrix.h2o: the finite elements of a vector, NaNs dropped (`dup nan? 0 eq where select-rows`, inlined) | 4n | mask + index + `1m(k)` | O(n) |
| `cumulative-sum` | `( m -- m' )` | Running sum over the elements in row-major order, shape preserved — a vector's prefix sums (ecdf, ROC, and calibration plumbing) | 1 + n | `1m(r×c)` | O(n) |
| `var` | `( m -- f )` | Sample variance (÷ n−1) over all elements; errors with fewer than 2 | 1 + n | none | O(n) |
| `quantile` | `( m p -- f )` | Linearly-interpolated quantile at p ∈ [0,1] over all elements (sorts a copy); errors if p out of range or empty | 2 + n log n | `malloc(n)` | O(n log n) |
| `histogram-table` | `( v n-bins -- fr )` | statistics.h2o: equal-width bin counts over a vector's value range, as `{ :counts (n-bins×1) :low :bin-width }`. NaNs dropped, the top value lands in the last bin, a constant vector takes the range value ± 1; errors on n-bins < 1 or no finite values | n + n-bins | `1m(n-bins)` + `1fr` | O(n + n-bins) |
| `ecdf` | `( v -- xs ys )` | statistics.h2o: the empirical CDF as two n×1 vectors — the finite elements sorted ascending, and the cumulative fractions (i+1)/n, so `ys` at index i is F(`xs` at i). Ties stay as consecutive points; NaNs are excluded from the points and from n; errors when no finite values remain | 2n log n | `2m(n)` + `1a(n)` | O(n log n) |
| `binomial-deviance` | `( y p -- dev )` | statistics.h2o: −2 Σ[y ln p + (1−y) ln(1−p)] over n×1 vectors — the proper scoring rule for probability models; p is clamped to [1e-12, 1−1e-12], so an overconfident prediction scores finitely bad rather than losing its ln 0 term to `sum`'s NaN skipping | 10n | clamp + term vectors | O(n) |
| `cross-validate` | `( units n-folds fit-xt score-xt -- loss )` | statistics.h2o: k-fold cross-validation — units deal round-robin into folds in the order given (shuffle first when order matters; reuse one shuffle to compare configurations on the same folds); per fold, `fit` `( train-units -- model )` then `score` `( model test-units -- loss )`, both taking everything from the stack since they run in this word's frame; answers the mean fold loss. A unit is whatever the array holds — rows, or per-cluster index arrays for cluster CV | k·(fit + score) + n·k | fold index arrays | O(k·(fit + score)) |
| `ks-distance` | `( a b -- d )` | Two-sample Kolmogorov–Smirnov statistic: the largest absolute gap between the two samples' ECDFs, both advanced past each pooled value before measuring (ties). Symmetric; d ∈ [0, 1]; NaNs excluded per sample, each sample's own n; dimensioned inputs are computed over their magnitudes; errors when either sample has no finite values | (n+m) log(n+m) | `malloc(n)` + `malloc(m)` | O((n+m) log(n+m)); above 8k elements the sorts are O(n) radix |
| `std` | `( m -- f )` | statistics.h2o: standard deviation, `var sqrt` (inlined) | n | none | O(n) |
| `se` | `( m -- f )` | statistics.h2o: standard error of the mean, `std / sqrt(n)` | n | none | O(n) |
| `median` | `( m -- f )` | statistics.h2o: `0.5 quantile` (inlined) | n log n | `malloc(n)` | O(n log n) |
| `percentile` | `( m pct -- f )` | statistics.h2o: `quantile` at pct ∈ [0,100] (inlined) | n log n | `malloc(n)` | O(n log n) |
| `iqr` | `( m -- f )` | statistics.h2o: interquartile range, Q3 − Q1 | 2n log n | `malloc(n)` ×2 | O(n log n) |
| `nonmissing-count` | `( m -- n )` | The number of non-NaN elements — the divisor `mean` and `se` use | 1 + n | none | O(n) |
| `summary` | `( v/dataset -- fr )` | statistics.h2o: a vector answers `{ :min :q1 :median :mean :q3 :max }` over its finite elements — a dimensioned vector in its unit, an instant vector (unit exactly `s`) with each statistic rendered through `time>iso` — plus `:missing` with the NaN count when any; an all-missing vector answers `{ :missing n }`, an empty one `{ }`. A dataset answers that frame per numeric column and `{ :distinct }` (distinct non-missing cells, plus `:missing`) per text column, keyed by column name; any other column value errors naming the column | 4n log n (per column) | `malloc(n)` ×4 + `1fr` (per column) | O(n log n) |
| `ci` | `( m level -- low high )` | statistics.h2o: percentile confidence interval — level 0.95 gives the 0.025 and 0.975 quantiles | 2n log n | `malloc(n)` ×2 | O(n log n) |
| `correlation-pearson` | `( xs ys -- f )` | statistics.h2o: Pearson r — center both vectors, then `dot` products for covariance and the two variances; accepts nx1 or 1xn; `null` when either vector is constant (R's NA) | 12n | `6m(n)` | O(n) |
| `correlation-spearman` | `( xs ys -- f )` | statistics.h2o: Spearman rho — `correlation-pearson` on the `ranks` of both vectors (inlined); tied values take index-order ranks, so heavily tied data drifts from the midrank definition | 2n log n | `4m(n)` + `malloc(16n)` ×2 | O(n log n) |
| `correlation-kendall` | `( xs ys -- f )` | Kendall tau-b: concordant minus discordant pairs over sqrt of tie-corrected pair counts, via one (x,y) sort and a merge-sort exchange count; NaN when all x or all y are tied; errors on length mismatch or fewer than 2 elements | 2n log n | `malloc(16n)` ×2–3 | O(n log n); above 8k elements the pair sort is O(n) radix |
| `correlate-with` | `( xs ys xt B -- fr )` | statistics.h2o: bootstrap 95% CI for the correlation word at xt — resamples (x, y) pairs jointly, B refits via a curried fit through `pbootstrap`, as `{ :estimate :se :bias :ci-low :ci-high }`; deterministic under a fixed seed | B·(n + xt) | pairs matrix + per-worker resample + `1fr` | O(B·(n + xt) / cores) |
| `cor` | `( xs ys -- fr )` | statistics.h2o: `correlation-kendall` with a 500-replicate bootstrap CI — `' correlation-kendall 500 correlate-with` (inlined) | as `correlate-with` | as `correlate-with` | as `correlate-with` |
| `qnorm` | `( p -- z )` | statistics.h2o: standard normal quantile (inverse CDF), Acklam's rational approximation — relative error below 1.15e-9, matching R's qnorm to 1e-8 over both tails; errors unless p strictly inside (0, 1) | 30 | none | O(1) |
| `sample-without-replacement` | `( arr n -- arr )` | statistics.h2o: `false sample` (inlined) | n | as `sample` | O(n) |
| `sample-with-replacement` | `( arr n -- arr )` | statistics.h2o: `true sample` (inlined) | n | as `sample` | O(n) |
| `bootstrap` | `( data fit-xt B -- arr )` | statistics.h2o: B refits of fit-xt over resamples of data — dataset/matrix rows, or an array's elements. One serial draw sets the run seed; replicate i draws its indices via `resample-indices-ext` at run-seed + i, so no resample outlives its fit and results don't depend on scheduling — deterministic under a fixed seed | B(n + fit) | per-fit resample + `1a(B)` | O(B·(n + fit)) |
| `pbootstrap` | `( data fit-xt B -- arr )` | statistics.h2o: `bootstrap` with the fits run under `pmap` — identical results (per-replicate seeding), parallel resample+fit | as `bootstrap` | as `bootstrap` | O(B·(n + fit) / cores) |
| `bootstrap-with` | `( data fit-xt B mapper-xt -- arr )` | statistics.h2o: the bootstrap skeleton `bootstrap`/`pbootstrap` instantiate; mapper-xt is `map`-shaped | as `bootstrap` | as `bootstrap` | as `bootstrap` |
| `column>indicators` | `( column -- m )` | statistics.h2o: one 0/1 indicator column per distinct value above the first (the reference) — an n×(k−1) matrix from a numeric vector or text array column, levels in `val_cmp` order (`column>set` lists them); a missing cell lands in no column; errors on fewer than 2 distinct values | n·k + n log n | level masks + `1m` per fold | O(n·k + n log n) |
| `indicators!` | `( design column sym -- design )` | statistics.h2o: `column>indicators`' named twin for a design dataset (a frame of columns): adds one 0/1 column per distinct value above the first (the reference), keyed `sym=level`, mutating and returning the frame — keys and columns derive from the same data, so a level change grows both together; `keys` then names the design and `dataset>matrix` over them is the aligned matrix; errors on fewer than 2 distinct values | n·k + n log n | level masks + frame growth | O(n·k + n log n) |
| `with-intercept` | `( X/design -- X'/design )` | statistics.h2o: a matrix gets a prepended column of ones, so a fit's beta[0] is the intercept; a design dataset gets an `:intercept` ones column keyed like any term (errors on an empty design — the rows are read from it) | r×c | matrix `1m(r×(c+1))`; design `1m(r×1)` | O(r×c) |
| `sigmoid` | `( m -- m' )` | statistics.h2o: elementwise logistic 1/(1+e⁻ˣ), mapping reals to (0,1) | 4n | `1m(r×c)` | O(n) |
| `regress-with` | `( dataset predictors response B fit-xt -- arr )` | statistics.h2o: the shared regression pipeline — design matrix with intercept, point estimate, then B bootstrap refits for per-coefficient `{ :estimate :se :bias :ci-low :ci-high }` frames; the loadable statistics library's `linear-regression`/`logistic-regression` pass the fit | fit + B·fit | matrices + B refits + `1a(k)` | O(B·fit) |
| `norm` | `( m -- f )` | Euclidean (L2) norm: √(Σ aᵢⱼ²) over all elements — a vector's length; for a matrix the Frobenius (entrywise 2-)norm, not the spectral norm | 1 + n | none | O(n) |
| `dot` | `( v w -- f )` | matrix.h2o: inner product (`* sum`, inlined); shapes must broadcast, so match the vectors | 2 + 2n | `1m(n)` | O(n) |
| `frobenius-norm` | `( m -- f )` | √(Σ aᵢⱼ²) over all elements (same value as `norm`) | 1 + n | none | O(n) |

---

## Segments

Flat, fixed-length typed numeric buffers stored off the arena (one `calloc`, freed by GC). Both `int-segment` and `double-segment` store values as doubles internally; `@i` reads a float and `!i` stores a float, so they share the array index ops. Use them for FFI scratch (`segment>pointer`) and dense numeric data without per-element boxing.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `int-segment` | `( n -- seg )` | n-element int segment, zero-filled; errors if n < 0 | 1 | `1seg(n)` | O(n) |
| `double-segment` | `( n -- seg )` | n-element double segment, zero-filled; errors if n < 0 | 1 | `1seg(n)` | O(n) |
| `@i` | `( seg i -- f )` | Read element i as a float (see Arrays) | 3 | none | O(1) |
| `!i` | `( seg i f -- seg )` | Store float f at index i in place; leaves seg | 4 | none | O(1) |
| `segment>pointer` | `( seg -- ptr )` | Intern the backing buffer and return an FFI pointer handle (no copy; see Foreign function interface) | 1 | none | O(1)† |

`†` amortized; the pointer-intern table grows occasionally.

---

## Random

A thread-local xoshiro256\*\* stream; each worker derives its own stream from the shared base seed, so parallel runs are deterministic per worker.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `seed` | `( n -- )` | Set the global base seed and reset the stream counter; per-thread streams derive from it | 1 | none | O(1) |
| `random` | `( -- f )` | Uniform float in [0,1) | 1 | none | O(1) |
| `random-int` | `( bound -- f )` | Uniform integer in [0,bound) as a float, by rejection sampling; errors if bound ≤ 0 | 1 | none | O(1)† |

`†` expected O(1); rejection sampling may retry. `sample` (Arrays) and `resample-indices` (Datasets and TSV) draw on this stream.

---

## Wall-clock time and dates

An *instant* is epoch seconds as a quantity in `s`, anchored at
1970-01-01T00:00:00Z, so the units machinery is the date arithmetic:
`wall-now 2 hour +` is an instant, instant − instant is a duration, and
`… 1 day /` counts days. (`now`, under REPL and introspection, is the
monotonic interval clock; `wall-now` is the absolute one.) A *date* is a frame
`{ :year :month :day :hour :minute :second :weekday :yearday }` — `:second`
carries fractions, `:weekday` is 0–6 with 0 = Sunday, `:yearday` is 1-based.
Composition accepts a partial frame — `:year` required, `:month`/`:day`
default 1, clock fields 0, other keys ignored — and out-of-range fields carry
mktime-style: `:month 13` is next January, `:day 0` the last day of the
previous month. Unsuffixed words are UTC and pure Gregorian arithmetic,
identical on every platform; the `-local` twins go through libc in the
process's timezone, re-reading `TZ` on every call. WASI has no timezone
machinery, so there the `-local` words behave as UTC and `parse-time` lacks
`%z`.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `wall-now` | `( -- instant )` | units.h2o: CLOCK_REALTIME epoch seconds as a quantity in `s`; steps when the system clock is adjusted, so time intervals with `now` | 1 | 1 pair | O(1) |
| `epoch>date` | `( instant -- date )` | Decompose an instant into a date frame, UTC | 40 | `1o` | O(1) |
| `epoch>date-local` | `( instant -- date )` | Decompose in the process's timezone | 40 | `1o` | O(1) |
| `date>epoch` | `( date -- instant )` | Compose an instant from a date frame, UTC; `:year` required, absent fields default, out-of-range fields carry | 30 | 1 pair | O(1) |
| `date>epoch-local` | `( date -- instant )` | Compose via `mktime` in the process's timezone; the zone rules resolve DST | 30 | 1 pair | O(1) |
| `format-time` | `( instant format -- string )` | Render with strftime, UTC | len | `1s` | O(len) |
| `format-time-local` | `( instant format -- string )` | Render with strftime in the process's timezone | len | `1s` | O(len) |
| `parse-time` | `( string format -- instant )` | Parse with strptime; uncaptured fields default to 1970-01-01 00:00:00, read as UTC unless the format captures an offset with `%z`; errors on a mismatch | len | 1 pair | O(len) |
| `time>iso` | `( instant -- string )` | units.h2o: `"%Y-%m-%dT%H:%M:%SZ" format-time` | len | `1s` | O(1) |
| `iso>time` | `( string -- instant )` | units.h2o: parse the Z form `time>iso` emits | len | 1 pair | O(1) |
| `days-in-month` | `( year month -- days )` | units.h2o: length of the month, leap-aware (first of next month minus first of this) | 60 | frames | O(1) |
| `date-shift` | `( instant delta -- instant )` | units.h2o: calendar shift, UTC. `:years`/`:months` step the calendar with the day clamped to the target month (Jan 31 + 1 month = Feb 28/29); `:weeks` `:days` `:hours` `:minutes` `:seconds` add exact durations. Components combine and may be negative | 200 | frames + pairs | O(1) |

---

## Datasets and TSV

*Rows* are an array of row-arrays (as `load-tsv` returns) — the raw I/O interchange, the only form preserving a file's physical column order. A *dataset* is a column-oriented frame; a *relation* is a deduped, indexed fact set (see Fact database). `r`/`c` are rows/columns, `n`/`k` observations/selected columns. `select-rows` (under Matrices) accepts a dataset, gathering every column by one index array or vector — with `where` masks and `argsort` that is filtering and sorting.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `load-tsv` | `( path -- rows )` | Read a TSV file into an array of row-arrays; an empty cell → `none`, a numeric cell → float, else a string. No header handling | 1 + bytes | `1a(r)` + one array per row + a string per text cell | O(bytes) |
| `read-tsv` | `( path -- dataset )` | datasets.h2o: a TSV file with a header row as a column-oriented dataset (`load-tsv true rows>dataset`, inlined), columns typed as `rows>dataset` types them; a headerless file goes through `load-tsv` + `rows>dataset` | bytes + 2·r·c | rows + one array per column + `1m` per numeric column + `1fr` | O(bytes + r·c) |
| `write-tsv` | `( dataset path -- )` | datasets.h2o: write a dataset as a TSV with a header row — `dataset>rows` then `save-tsv` (inlined), `read-tsv`'s inverse; a dimensioned column errors in `save-tsv` (strip with `magnitude` first) | 2·r·c | transient rows | O(r·c) |
| `save-tsv` | `( rows path -- )` | Write an array of row-arrays as TSV; `none` → empty, a whole-number float → integer, strings raw; errors on a tab/newline inside a string or a non-array row | 2 + r·c | none (to file) | O(r·c) |
| `rows>dataset` | `( rows header? -- dataset )` | datasets.h2o: column-oriented frame from rows with typed columns — uniformly float-or-`none` cells become an n×1 vector (`none` → NaN), uniform-unit quantity cells a dimensioned vector, anything else stays the cell array; keys come from row 0 when header? is true, else `:col1…` are synthesized | 2·r·c | `k×1a(r)` + `1m` per numeric column + `1fr` | O(r·c) |
| `rows>relation` | `( rows index-cols header? -- relation )` | datasets.h2o: deduped relation indexed on `index-cols` (coerced to symbols) | r·c | one frame per row + relation + index buckets | O(r·c) |
| `dataset>rows` | `( dataset -- rows )` | datasets.h2o: the inverse of `true rows>dataset` — an array of row-arrays led by a header row of the column names as strings, columns in key order, cells through `column>array` (NaN → `null`, dimensioned cells as quantities); feeds `save-tsv` directly (`1 skip` for headerless rows) | r·c | header + one array per row + `1a(r·c)` cells | O(r·c) |
| `dataset>matrix` | `( dataset cols -- m )` | datasets.h2o: build an n×k matrix from the named numeric columns (rows are observations) | n·k | flat `1a(n·k)` + `2m(n×k)` | O(n·k) |
| `column-type` | `( dataset sym -- sym )` | datasets.h2o: the named column's type from its representation — matrix `:numeric`, quantity in exactly `s` `:datetime`, other quantity `:quantity`, array `:text`; a missing key errors through `@` | 5 | 1 pair | O(log c) |
| `column>array` | `( column -- arr )` | datasets.h2o: a column in any representation as an array of its values — arrays pass through unchanged, matrix/quantity columns go through `matrix>array` (NaN → `null`, dimensioned elements become quantities) | n | `1a(n)` for matrix columns, none for arrays | O(n) |
| `column>set` | `( column -- set )` | datasets.h2o: the set of the column's distinct values — `column>array array>set` | 2n log n | `1a(n)` + `1o` | O(n log n) |
| `select-columns` | `( dataset cols -- dataset )` | datasets.h2o: the named columns as a new dataset (a fresh frame sharing the column values); a missing name errors through `@` | k log c | `1a(k)` + `1o` | O(k log c) |
| `count` | `( arr/v/dataset -- pairs )` | datasets.h2o: occurrences of each distinct value as `[ [ value n ] … ]`, most frequent first, ties in value order (`val_cmp`); a vector counts its elements (a dimensioned one counts quantities), a dataset counts whole rows, each a frame keyed by column name | 2n log n | rows + pairs + 3×`1a` | O(n log n) |
| `group-indices` | `( column -- pairs )` | datasets.h2o: `[ [ value [indices] ] … ]` per distinct value in `val_cmp` order — each index array holds the value's row positions, ascending (one `argsort`, the permutation cut at run boundaries); `count`'s shape with positions instead of tallies, so one pass replaces a per-value `eq where` scan | 2n log n | permutation + one pair and array per value | O(n log n) |
| `map-rows` | `( dataset xt -- dataset )` | datasets.h2o: xt maps each row frame to a new row frame — derive, rename, or drop fields; the returned frames rebuild through `frames>dataset`, so all rows must share keys and columns re-infer their representation | n·xt + n·k log k | rows + new columns | O(n·(xt + k log k)) |
| `frames>dataset` | `( rows -- dataset )` | datasets.h2o: an array of row frames (as `query`, `db-query` `:rows`, or `map-rows` produce) as a column-oriented dataset, keys from row 0 — differing keys throw. Each column's representation is inferred: all-float cells (`none` → NaN) become an n×1 vector, uniform-unit quantities a dimensioned vector, anything else stays an array | n·k log k | one column per key + `1o` | O(n·k log k) |
| `replace-where` | `( dataset sym pred replacement -- )` | datasets.h2o: replace the named column's cells passing `pred` `( column -- mask )`, in place — `update-at` around `mesh`, so the replacement broadcasts and units reconcile: `pipeline :rep_touches [: -1 eq :] null replace-where` nulls a sentinel, `[: nan? :] 0` fills missing, `[: 10 $ lt :] 5 $` floors prices | pred + n | mask + one column | O(n) |
| `resample-indices` | `( n -- arr )` | datasets.h2o: n indices drawn from [0,n) with replacement (bootstrap), from the global stream | 2n | `2×1a(n)` | O(n) |
| `resample-indices-ext` | `( n seed -- arr )` | n indices drawn from [0,n) with replacement by a private generator seeded from `seed` (splitmix64-expanded) — same draw for the same seed regardless of thread or stream position; the bootstrap words seed replicate i at run-seed + i | n | `1a(n)` | O(n)† |

---

## Higher-order

The quotation/predicate cost dominates; `xt` denotes one call.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `map` | `( arr/set xt -- arr )` | Apply xt to each element; xt must net exactly one value | 2 + n·xt | `1a(n)` | O(n·xt) |
| `mapn` | `( arr₁ … arr_N xt N -- arr )` | N-ary zip-map over equal-length arrays | rows·(N+xt) | `1a(rows)` | O(rows·xt) |
| `filter` | `( arr/set xt -- arr )` or `( dataset xt -- dataset )` | Keep elements where xt is truthy. datasets.h2o extends it to a dataset: xt sees each row as a frame keyed by column name and answers a bool (1.0/0.0); the kept rows come back through `select-rows`, so every column keeps its representation | 2 + n·xt | malloc(n) flags + `1a(k)`; dataset rows + mask + one column each | O(n·xt) |
| `reduce` | `( arr/set init xt -- val )` | Left fold; xt is `( acc elem -- acc )` | 3 + n·xt | none | O(n·xt) |
| `times` | `( xt n -- )` | Run xt n times, no index pushed | 2 + n·xt | none | O(n·xt) |
| `i-times` | `( xt n -- )` | Run xt n times, pushing index 0..n-1 first | 2 + n·(1+xt) | none | O(n·xt) |
| `find` | `( items pred -- element )` | arrays.h2o: the first element for which pred is truthy, or the none value; stops at the first hit | n·xt | none | O(n·xt) |
| `any?` | `( items pred -- bool )` | arrays.h2o: `find none? not` | n·xt | none | O(n·xt) |
| `all?` | `( items pred -- bool )` | arrays.h2o: false at the first element failing pred, else true (vacuously true on empty) | n·xt | none | O(n·xt) |
| `each` | `( items xt -- )` | arrays.h2o: run xt `( element -- )` on every element for its side effects; no result, no allocation | n·xt | none | O(n·xt) |
| `flat-map` | `( items xt -- arr )` | arrays.h2o: `map flatten-array`; xt returns an array per element, results concatenated | n·xt + total | `1a(n)` + `1a(total)` | O(n·xt + total) |
| `sort-by` | `( items xt -- arr )` | arrays.h2o: sorted by the key xt `( element -- key )` extracts — decorate-sort-undecorate over `[ key element ]` pairs, so n key evaluations | n·xt + n log n | 3×`1a(n)` + n×`1a(2)` | O(n·xt + n log n) |
| `partition` | `( items pred -- matches rest )` | arrays.h2o: the elements satisfying pred and the others, one pass, input order kept | n·xt | 2 arrays | O(n·xt) |
| `group-with` | `( items xt -- fr )` | arrays.h2o: group elements into `{ key → set }` by the symbol key xt `( element -- sym )` computes — the quotation-keyed kin of `group-by` | n·(xt + log n) | frame + sets | O(n·xt + n log n) |

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
| `throw` | `( exc -- )` | Unwind to the nearest exception prompt, leaving `exc 1` (what `catch` consumes); with no enclosing prompt it is an interpreter error, `uncaught exception: <value>`, the trace captured at the throw site. The prompt search skips locals regions, so stale bytes in uninitialized local slots are never read as prompts | L | none | O(L) |
| `catch` | `( xt -- result 0 \| exc 1 )` | exceptions.h2o: `reset (execute-catching) 0`; `(result 0)` on success, `(exc 1)` on a `throw` **or** an interpreter error (an error frame `{ :message :trace }` becomes the exception value) | — | cont if thrown; `1f` + `2s` on a caught interpreter error | O(xt) |
| `try-catch` | `( normal-xt err-xt -- … )` | exceptions.h2o: run normal-xt; on a `throw` or interpreter error, run err-xt with the exception (the `{ :message :trace }` error frame, for an interpreter error) on the stack | — | cont if thrown; `1f` + `2s` on a caught interpreter error | O(normal-xt) |
| `ensure` | `( body-xt cleanup-xt -- … )` | exceptions.h2o: run cleanup-xt (stack-neutral) whether body-xt returns normally or throws/errors, then re-raise on the throw path | — | cont if thrown | O(body-xt) |
| `with-db` | `( path body-xt -- … )` | exceptions.h2o: `db-open` the path, run body-xt `( db -- … )` with the handle, `db-close` on either exit | — | 1 db + cont if thrown | O(body-xt) |
| `with-stream` | `( stream body-xt -- … )` | exceptions.h2o: run body-xt `( stream -- … )` over an already-open stream, `close` it on either exit | — | cont if thrown | O(body-xt) |

---

## Generators

Coroutines over the continuation substrate: a producer `yield`s values one at a time and a driver `resume`s it for the next. All generators.h2o on `shift`/`reset`/`resume`. `L` = captured return-stack length per step.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `yield` | `( v -- resumed )` | generators.h2o: `shift` — emit v to the driver; returns whatever the driver passes back via `resume` | L | `1o` (cont) | O(L) |
| `start-generator` | `( producer -- value generator )` | generators.h2o: `reset execute` — run producer to its first `yield`; leaves the yielded value and a resumable continuation | L | `1o` (cont) | O(producer to first yield) |
| `gen-take` | `( producer count -- array )` | generators.h2o: the first `count` values the producer yields, collected into an array | — | `1a(count)` + cont/step | O(count · L) |
| `gen-each` | `( producer consumer -- )` | generators.h2o: run consumer on each value the producer yields until the producer falls off (a `:gen-end` sentinel marks exhaustion) | — | cont/step | O(values · consumer) |

---

## Logic

Logic variables, unification, and committed choice, built on the trail and a `PROMPT_CHOICE` prompt. A logic var is always created explicitly: `lvar` pushes a fresh one, `lvar to x` names a persistent global (`to` auto-creates the global at the top level), and a `?` prefix in a locals list (`| ?x |`) declares a fresh per-call local. Capitalizing logic-var names (`X`, `Hs`) is stylistic convention, not syntax — case carries no meaning. `unify` records every binding on the trail; a `unify` mismatch or an explicit `fail` backtracks to the nearest `amb`. Lists are cons pairs (see Pairs): `[( H T )]` is the `[H|T]` head/tail pattern under `unify`. To keep a result past backtracking, snapshot it with `copy` (fresh vars) or `reify` (canonical `:_N`). A logic var prints by the name of a variable that holds it — `?x` while free (the `?` marks the hole, echoing the `| ?x |` declaration form), `x=value` once bound — or `_N` when anonymous; an anonymous bound var prints its value.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `lvar` | `( -- v )` | Push a fresh, unbound logic variable | 2 | `1 lvar` | O(1) |
| `_` | `( -- wild )` | The anonymous wildcard — unifies with anything, binds nothing, allocates nothing (a constant, not a fresh var) | 2 | none | O(1) |
| `unify` | `( a b -- term )` | Unify a and b, binding logic vars (recorded on the trail) so the two match, then leave the dereffed left term. Atoms by value; pairs head then tail; arrays element-wise; frames as open records — shared keys must unify, extra keys on either side allowed. A `_` on either side matches anything and binds nothing. On a mismatch, `fail`s. | n | none | O(n) |
| `~` | `( a b -- term )` | C primitive alias of `unify`, so `cons ~` fuses to `(cons~)` | n | none | O(n) |
| `deref` | `( v -- val )` | Follow a logic var's binding chain to the first non-variable value (v itself if unbound). Shallow — a returned structure still has bound vars inside; for a fully resolved snapshot use `reify` or `copy` | d | none | O(d) |
| `?` | `( v -- val )` | logic.h2o: `deref` (inlined) | d | none | O(d) |
| `amb` | `( xt1 xt2 -- … )` | Run xt1; if it fails (a `unify` mismatch or `fail`), roll its bindings back through the trail and run xt2. Commits to the first branch that succeeds. | xt1 | none | O(xt1 + xt2) |
| `fail` | `( -- )` | Backtrack to the nearest enclosing `amb`, failing the current branch; with no enclosing `amb`, an error | 1 | none | O(L) |
| `choose` | `( list cont -- )` | logic.h2o: run cont with each element of a cons list in turn, committing to the first for which it succeeds; `fail` if none do (n-way `amb` over a list) | n·cont | none | O(n·cont) |
| `matches?` | `( a b -- flag )` | Non-destructive unify test: mark the trail, unify a and b, roll the trail back, push whether they unified. Leaves no bindings and never backtracks (so it composes in straight-line code, unlike `unify`) | n | none | O(n) |

---

## Fact database

A relational store built entirely from frames and sets — no new type. A **relation** is `{ :rows <set of rows> :index <index> }`; a **row** is a frame keyed by column name; a **database**, if you want several relations, is just a frame keyed by relation name (`db :father @` reaches one — no words of its own). The same shape describes a SQLite query result, so a fetched table and a hand-built relation are interchangeable (see the SQLite section below).

Rows live in a set, so an identical row asserted twice dedups to one (a relation is a set of tuples). A caller-supplied `:id` column keeps otherwise-identical rows distinct. Indexed columns are declared at creation and must be symbol-valued; `:index` maps each to a `{ value → <rows> }` frame whose buckets share the row frames in `:rows`.

`query` is unification: a pattern frame unifies against rows as an open record — shared keys must match, a logic var matches anything (projection), extra columns are ignored — which is SQL selection and projection. It collects every match (returning an array of the matching rows) by testing each candidate with `matches?` and rolling bindings back, so the pattern is left unbound. Candidates come from the index when the pattern grounds an indexed column to a symbol (intersecting buckets across several such columns, empty when a value was never asserted); otherwise it scans `:rows`.

The relation/query machinery is built from logic.h2o helpers (`bucket-of`, `candidates`, `covering?`, `smallest-set`, `tsv-keys`, `retract-row`, `update-row!`) that are internal implementation details and are not listed individually.

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

These are logic.h2o over the C primitives `matches?`, `set-add!`, `set-remove!`, `array>set`, and `group-by`, plus the `symbol?` type predicate. Building a relation with one `assert` per row is super-linear (each insert shifts the sorted `:rows` set, and per-value frames grow the same way); `bulk-load` avoids that with `array>set` for `:rows` (one sort) and a one-pass `group-by` per indexed column (which buckets by the interned symbol value, then sorts each small bucket — no global sort). `load-bag` and `create-index` skip the `:rows` dedup entirely, keeping a bag; `create-index` also interns the indexed columns to symbols. Candidate narrowing drives from the smallest matching bucket.

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

These are normally produced by the compiler's auto-fuser rather than typed by hand; `see-compiled` reveals them. The fuser triggers only on the unsafe f-words (`f+`, `fsqrt`, `fexp`, …) — the polymorphic names (`+`, `sqrt`, `exp`) never fuse, so their tag dispatch (matrix, quantity) is never bypassed.

The auto-fuser also collapses a comparison immediately before a branch — `= if`, `gt while`, `0= until` — into a single compare-and-branch instruction (shown by `see-compiled` as `(=0branch)`, `(gt0branch)`, and the like). These are internal and never typed; the source stays the plain comparison followed by the control word.

---

## REPL and introspection

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `words` | `( -- )` | List all non-internal words in aligned columns, grouped by reference section, alphabetical within a group; session-defined words first | dict scan | none | O(\|dict\| log \|dict\|) |
| `water` | `( -- )` | Print the water logo and the interpreter version | print | none | O(1) |
| `apropos` | `( s -- )` | Print every word whose name or reference summary contains s (case-insensitive): name, stack effect, summary per line; session-defined words match by name | table scan | none | O(entries) |
| `see` | `( xt -- )` | Print a word's source (`: name … ;`), or `variable`/`symbol`/primitive form | dict scan | none | O(\|dict\|) |
| `see>string` | `( xt -- s )` | The text `see` would print, returned as a string (trailing newline stripped) | dict scan | `1o` | O(\|dict\|) |
| `see-compiled` | `( xt -- )` | Disassemble a colon definition's compiled cells | body scan | none | O(body) |
| `see-compiled>string` | `( xt -- s )` | The text `see-compiled` would print, returned as a string (trailing newline stripped) | body scan | `1o` | O(body) |
| `see-tree` | `( xt -- )` | Like `see-compiled`, but each colon-word call is expanded inline, indented two spaces, recursively down to primitives; recursive calls print as `name ...` | body scan | none | O(expanded body) |
| `see-tree>string` | `( xt -- s )` | The text `see-tree` would print, returned as a string (trailing newline stripped) | body scan | `1o` | O(expanded body) |
| `man` | `( xt -- fr )` | Frame of a word's reference entry (`:word :effect :summary`, plus `:ops :alloc :order` for runtime words); a unit word synthesizes its entry from the unit's definition (`unit: m × 1000`); `T_NONE` if undocumented | dict scan + log n | `1o` + strings | O(\|dict\|) |
| `help` | `( "name" -- )` | repl.h2o: parse the next word and print its `man` frame; bare `help` (no name on the line) prints a starter cheat sheet, and an unknown name prints `unknown word: <name>` without erroring. Distinguishes the three cases by `catch`ing `lookup`'s message | dict scan + log n | `1o` + strings + print | O(\|dict\|) |
| `gc` | `( -- )` | Force a mark-sweep now | walks stacks + dict + roots, frees unmarked | none | O(objects + dict) |
| `alloc-stats` | `( -- )` | Print and reset the allocation counters since the last call (`lvars=… arrays=…`) | 2 | none | O(1) |
| `bye` | `( -- )` | `exit(0)` | — | — | — |
| `now` | `( -- f )` | `CLOCK_MONOTONIC` seconds as a float | 1 | none | O(1) |
| `sleep` | `( seconds -- )` | Block for the given float seconds (sub-second supported); `nanosleep` | blocks | none | O(1) |
| `timed` | `( xt -- … )` | Run xt, print its elapsed `now` (`CLOCK_MONOTONIC`) seconds, then pass through whatever it left on the stack | 2 + xt + print | none | O(xt) |

---

## Persistence

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `load` | `( s -- )` | Run a source file as if typed; record it for `reload`. An error raised while loading is prefixed `file:line: ` (the line of the failing token); a nested `load` locates to the innermost file | file read + run | input buffer | O(file) |
| `load-library` | `( name -- )` | core.h2o: `load` `lib/<name>` from beside the water binary (`binary-dir`), so `"plot" load-library` works from any cwd; a name without `.h2o` gains it | file read + run | input buffer | O(file) |
| `reload` | `( -- )` | Truncate user state, re-run every loaded file in order | forget + N loads | — | O(Σ files) |
| `save` | `( s -- )` | Write all user words as re-loadable `.h2o` source | dict scan + write | file I/O | O(\|user dict\|) |
| `save-image` | `( s -- )` | Binary snapshot of full state (dict, objects, stacks, continuations) | serialize all | file I/O | O(objects + dict) |
| `load-image` | `( s -- )` | Restore a binary snapshot, replacing current state | deserialize all | reallocates all objects | O(objects) |

---

## Files and environment

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `read-file` | `( path -- s )` | Read a whole file as one string (byte-safe); errors if it can't be opened | file read | `1o` + buffer | O(file) |
| `write-file` | `( s path -- )` | Create or truncate the file, then write the string's bytes | file write | none | O(\|s\|) |
| `append-file` | `( s path -- )` | Open in append mode, write the string's bytes | file write | none | O(\|s\|) |
| `file-exists?` | `( path -- bool )` | Whether the path exists (`access` with `F_OK`); follows symlinks, tests any file type, not just regular files | 1 | none | O(1) |
| `env` | `( name -- val )` | Environment variable as a string, or the none value if unset (so set-empty `""` and unset stay distinct) | 1 | `1o` on hit | O(\|val\|) |
| `env!` | `( name value -- )` | Set an environment variable (overwriting); process-wide, so subsequent `start-process` children inherit it | 1 | none | O(1) |
| `cwd` | `( -- path )` | The interpreter's current working directory as a string (`getcwd`) | 1 | `1o` | O(\|path\|) |
| `binary-dir` | `( -- s )` | The directory holding the running water binary, symlinks resolved (`realpath`), so an installation's resources are reachable from any cwd; errors on the wasm build (no executable path) | 1 | `1o` | O(\|path\|) |
| `cd` | `( path -- )` | Change the interpreter's working directory (`chdir`); process-wide, so it moves the base for relative file I/O and is inherited by subsequent `start-process` children | 1 | none | O(1) |
| `find-executable` | `( name -- path\|none )` | `io.h2o`: the absolute path of `name` on `$PATH` (first directory holding it), or the none value if unset or not found; a name containing `/` is not special-cased (it just won't match a bare `PATH` entry) | split + probe | `1o` per candidate | O(dirs) |

---

## Subprocesses and streams

A stream (`T_STREAM`) wraps an OS file descriptor — a pipe to a child process. `start-process` launches a program directly from an argv array (no shell, so no quoting or injection surface) and returns a frame `{ :pid :in :out :err }` whose `:in`/`:out`/`:err` are streams. The lifecycle is: `write` input → `close` `:in` (sends EOF) → `read` the output → `wait`. `SIGPIPE` is ignored process-wide, so a `write` to a child that has exited returns an error rather than killing the interpreter. Bytes are raw and length-counted, so streams are binary-safe.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `start-process` | `( argv -- proc )` | fork/exec `argv[0]` with `argv` as its arguments; return `{ :pid :in :out :err }` (the three streams are `T_STREAM`) | fork + 3 pipes | `1o` frame + 3 streams | O(argc) |
| `run-result` | `( argv -- frame )` | subprocess.h2o: run `argv` to completion and return `{ :out :err :status }`, closing the streams and reaping the child | fork + drain | `1fr` + output strings | O(output) |
| `write` | `( s stream -- )` | Write the string's bytes to the stream; loops over partial writes, retries `EINTR` | write syscalls | none | O(\|s\|) |
| `read` | `( stream -- s )` | Read the stream to EOF into one string | read syscalls | `1o` + buffer growth | O(bytes) |
| `close` | `( stream -- )` | Close the fd; closing a child's `:in` sends it EOF | 1 syscall | none | O(1) |
| `stdin` | `( -- stream )` | Standard input as a `T_STREAM` over fd 0; `stdin read` slurps it. (Conflicts with the REPL reading its own program from stdin — for file-loaded programs.) | 1 | none | O(1) |
| `stdout` | `( -- stream )` | Standard output as a `T_STREAM` over fd 1; `s stdout write` emits | 1 | none | O(1) |
| `stderr` | `( -- stream )` | Standard error as a `T_STREAM` over fd 2; composes with `write`/`close` like any stream | 1 | none | O(1) |
| `wait` | `( pid -- status )` | Block until the child exits; return its exit code, or `128 + signo` if it was killed by a signal | blocks | none | O(1) |
| `stop` | `( pid -- status )` | `SIGKILL` the child then reap it (137 = 128+9, or its code if it had already exited) | 2 syscalls | none | O(1) |
| `running?` | `( pid -- bool )` | Non-blocking liveness via `waitid`+`WNOHANG`+`WNOWAIT`; true while running, false once exited. Non-reaping, so a later `wait` still returns the status | 1 syscall | none | O(1) |
| `run` | `( s -- proc )` | subprocess.h2o: split a command string on runs of spaces and `start-process` it (`" +" split start-process`) | split + fork | `1a` + `1o` frame + 3 streams | O(\|s\| + argc) |
| `write-in` | `( s proc -- )` | subprocess.h2o: write the string to the child's `:in` stream | write syscalls | none | O(\|s\|) |
| `read-out` | `( proc -- s )` | subprocess.h2o: read the child's `:out` stream to EOF | read syscalls | `1o` + buffer growth | O(bytes) |
| `read-err` | `( proc -- s )` | subprocess.h2o: read the child's `:err` stream to EOF | read syscalls | `1o` + buffer growth | O(bytes) |
| `end-process` | `( proc -- )` | subprocess.h2o: the teardown mirror of `start-process` — close `:in`/`:out`/`:err` and `wait` `:pid` (graceful, blocks until exit) | 3 closes + wait | none | O(1) |
| `parallel-run` | `( commands width -- results )` | subprocess.h2o: run each argv array in `commands` as a subprocess, at most `width` at once; collect `{ :out :err :status }` per command in input order, refilling a slot as each child finishes | fork per command + poll | `1a` + per-child frames/streams | O(critical path) |

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
| `db-query>dataset` | `( db query params -- dataset )` | database.h2o: the same query, returned as a column-oriented dataset with **typed columns**: a column whose every cell is numeric or NULL becomes an n×1 vector (NULL → NaN), a column declared DATE/DATETIME/TIMESTAMP becomes a vector of instants in `s` (numeric cells read as epoch seconds, text cells parsed as ISO Z), and anything else stays an array with `none` for NULL. An empty column declared numeric stays an empty vector, so the type survives an empty result; a repeated column name keeps its last occurrence. The C primitive `(db-query>dataset)` returns the raw columns plus each column's declared type from the same prepared statement | n·c | `1o` frame + `1a`/column + `1m` per numeric column + a string per text cell | O(n·c) |
| `tsv>db` | `( tsv-path db table -- info )` | database.h2o: import a TSV file into a new table. The header row names the columns (identifiers quoted, so any header text works); a column whose every non-empty cell is numeric is REAL, else TEXT; empty cells insert as NULL; all rows go in one transaction. `info` is `{ :n-rows N :columns [ … ] }` — a `:real` column carries `{ :name :type :summary }` with a `:summary` from `summary`, a `:text` column `{ :name :type :distinct }` with `COUNT(DISTINCT)` (NULLs uncounted). Errors before creating anything on a missing or ragged file; an existing table errors on the CREATE, leaving it untouched | r·c | rows + dataset + `1s`/statement | O(r·c) |

Using a closed handle errors (`database is closed`). Do selection, projection, and joins in the SQL itself; Water materializes the result. Indexing a result is a separate, explicit step — `create-index` (see Fact database) — because it interns the indexed columns to symbols, which only makes sense for low-cardinality categorical columns you choose.

---

## Foreign function interface

Call C functions in any shared library at runtime via `libdl` + `libffi` — no per-library glue. `ffi-open` loads a library; `ffi-function` / `ffi-variadic` resolve a symbol and define a Water word that marshals its arguments and result. Types are symbols: `:void :int :long :double :ptr :string` — Water floats marshal to/from C `int`/`long`/`double`, strings pass as `const char*` (a returned `char*` is copied into a Water string), and `:ptr` is an opaque C pointer held as a `T_PTR` handle (a registry index, since a 64-bit pointer doesn't fit a Val's 44-bit payload). FFI is unsafe: a wrong signature corrupts or crashes — argument *count* is checked, types are the caller's responsibility.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `ffi-open` | `( path -- lib )` | `dlopen` the library at `path` and push a `T_PTR` handle; `""` opens the running process itself (`dlopen(NULL)`) for already-linked symbols. Errors if not found | dlopen | 1 handle (not GC'd) | O(1) |
| `ffi-function` | `( lib symbol arg-types ret-type -- ) <name>` | Resolve `symbol` in `lib`, build a libffi call interface, and define the following word `<name>` to call it. `arg-types` is an array of type symbols, `ret-type` a single symbol. The interface is prepared once; calls are ~30–100 ns | dlsym + prep_cif | 1 binding | O(argc) |
| `matrix>pointer` | `( m -- ptr )` | Intern the matrix's row-major element buffer and return a `T_PTR` handle to pass as a `:ptr` argument; no copy — aliases the live buffer (amortized intern) | 1 | none | O(1) |
| `segment>pointer` | `( seg -- ptr )` | Intern a segment's data buffer and return a `T_PTR` handle (no copy) | 1 | none | O(1) |
| `ffi-variadic` | `( lib symbol arg-types ret-type n-fixed -- ) <name>` | Like `ffi-function` for a variadic C function: `n-fixed` leading arguments use the fixed convention, the rest the variadic one (`ffi_prep_cif_var`). Variadic argument types are fixed per binding, so declare one word per type combination (e.g. a `:string` `setopt` and a `:long` `setopt`) | dlsym + prep_cif_var | 1 binding | O(argc) |
| `ffi-free` | `( ptr -- )` | `free` a C buffer held as a `T_PTR` (e.g. from `malloc`) and clear its registry slot. Not for library handles | free | none | O(1) |

A defined FFI word pops its arguments, marshals each per the declared signature, calls through libffi, and pushes the marshalled return (`:void` pushes nothing). The build links `-lffi`; `dlopen` is in libSystem. Callbacks (C → Water), struct-by-value, varargs-per-call, and finer numeric types (`float`, unsigned) are not yet supported.

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
| `T_QUANTITY` | a magnitude (float or matrix) plus a unit id, in a pair-table slot `{magnitude, unit}`; see Dimensioned quantities. Dimensionless results collapse away, so a live quantity always carries a real unit |
| `T_XT` | execution token (dict index); first-class callable |
| `T_ADDR` | dict index; used internally for return-stack frames |
| `T_STREAM` | OS file descriptor (a pipe end to a child process); an inline `int`, like `T_ADDR` |
| `T_DB` | inline handle into the per-interpreter registry of open SQLite connections; not GC'd (closed with `db-close`) |
| `T_PTR` | opaque C pointer from the FFI (library handle or data pointer); a registry index, not the raw 64-bit address; not GC'd |
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
