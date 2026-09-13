# Telic reference

Every entry is derived from reading the C source. Stack effects are exact;
`--` separates the state before (bottom to top, leftmost = deepest) from after.
Shorthand: `f` float, `str` string, `xt` execution token, `mat` matrix, `arr`
array, `set` set, `fr` frame, `sym` symbol, `k` continuation. `mat` and `str`
are spelled out because `m` is the metre unit and `s` the second, and `to`
refuses to shadow a word; `set` is itself the set constructor, so a variable
holding one takes a name of its own.

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

A word's examples follow its section's table as fence pairs: a
` ```forth <word> ` fence holding self-contained code — deterministic (random
draws seeded), newline-terminated, leaving both stacks empty — and a
` ```output ` fence holding exactly what it prints (trailing blanks
insignificant). `make test` extracts and verifies every pair, and
`help <word>` prints them. A ` ```forth-noexec <word> ` fence is shown by
`help` but never run.

Tokens are whitespace-delimited, with self-delimiting punctuation: `;`, `]`,
and `}` always end a token and `[` and `{` always start one (the two-char
openers `[:` `[<` and closers `:]` `>]` stay whole), so
`[1 2 3]`, `{:a 1}`, and `dup *;` parse without inner spaces. A path literal's
predicate brackets (`/a[x>3]`) are kept whole by bracket balance. `<` `>` `<=`
`>=` are ordinary comparison words; set literals `[< … >]` still need spaces
around their contents.

Allocation note: an object slot is a pointer bump into the object table, which
grows on demand (doubling) up to a 64M-entry ceiling; when the ceiling is
reached, a mark-sweep GC runs and the allocation retries; every collection
is a full pass.

---

## Stack manipulation

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `dup` | `( a -- a a )` | Duplicate top | 3 | none | O(1) |
| `drop` | `( a -- )` | Discard top | 1 | none | O(1) |
| `swap` | `( a b -- b a )` | Exchange top two | 4 | none | O(1) |
| `over` | `( a b -- a b a )` | Copy second over top | 5 | none | O(1) |
| `rot` | `( a b c -- b c a )` | Rotate top three | 6 | none | O(1) |
| `-rot` | `( a b c -- c a b )` | core.telic: reverse rotate — brings the top down under the other two | 12 | none | O(1) |
| `depth` | `( -- n )` | Push current depth | 1 | none | O(1) |
| `pick` | `( xₙ … x₀ n -- xₙ … x₀ xₙ )` | Copy the item n deep to the top, leaving it in place; `0 pick` copies the top, `1 pick` copies the second, and n counts down from the top. Reads a value a caller parked below a combinator's operands: where a combinator peeks its source and leaves the current element on top, the element is at 0, the source at 1, and a parked value at 2 | 3 | none | O(1) |
| `roll` | `( xₙ … x₀ n -- xₙ₋₁ … x₀ xₙ )` | Move the item n deep to the top, shifting the n items above it down | 2 + n | none | O(n) |
| `clear` | `( … -- )` | Reset data stack depth to 0 | 1 | none | O(1) |
| `2dup` | `( a b -- a b a b )` | core.telic: duplicate the top two items | 10 | none | O(1) |
| `2drop` | `( a b -- )` | core.telic: discard the top two items | 2 | none | O(1) |
| `identity` | `( a -- a )` | core.telic: the value unchanged — the no-op xt for higher-order words | 1 | none | O(1) |
| `nip` | `( a b -- b )` | Drop the second item, keeping the top | 1 | none | O(1) |
| `tuck` | `( a b -- b a b )` | core.telic: copy the top under the second — Forth's CORE EXT word | 9 | none | O(1) |

```forth dup
3 dup * . cr
```
```output
9
```

```forth drop
1 2 drop . cr
```
```output
1
```

```forth swap
1 2 swap . . cr
```
```output
1 2
```

```forth over
1 2 over . . . cr
```
```output
1 2 1
```

```forth rot
1 2 3 rot . . . cr
```
```output
1 3 2
```

```forth -rot
1 2 3 -rot . . . cr
```
```output
2 1 3
```

```forth depth
1 2 3 depth . clear cr
```
```output
3
```

```forth pick
10 20 30 1 pick . clear cr
```
```output
20
```

```forth roll
10 20 30 2 roll . . . cr
```
```output
10 30 20
```

```forth clear
1 2 3 clear depth . cr
```
```output
0
```

```forth 2dup
3 4 2dup * . + . cr
```
```output
12 7
```

```forth 2drop
1 2 3 2drop . cr
```
```output
1
```

```forth identity
[ 1 2 ] ' identity map . cr
```
```output
[ 1 2 ]
```

```forth nip
1 2 nip . cr
```
```output
2
```

```forth tuck
1 2 tuck . . . cr
```
```output
2 1 2
```

---

## Arithmetic

Polymorphic; dispatch on operand tags at run time. Ops/Alloc/O below give the
float fast path first; the heavy cases are captured by the O column.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `+` | `( a b -- a+b )` | float: add. string+string: concat → new string. set+set: union → new set. matrix+matrix: element-wise → new matrix. scalar+matrix / matrix+scalar: broadcast → new matrix. exact+exact: exact sum. complex (either side, floats promote): complex sum. | 3 (float) | float none; string `1s` + temp buffer; set `1o`; matrix `1m(r×c)` | float O(1); string O(\|s\|); set O(n log n); matrix O(r×c) |
| `-` | `( a b -- a-b )` | float: subtract. set−set: difference. matrix: element-wise. scalar/matrix broadcast. exact−exact: exact. complex: complex. | 3 (float) | as `+` | as `+` |
| `*` | `( a b -- a*b )` | float: multiply. set∩set: intersection. matrix: element-wise. scalar/matrix broadcast. exact×exact: exact. complex: complex. | 3 (float) | as `+` | as `+` |
| `/` | `( a b -- a/b )` | float: divide (errors on zero divisor). matrix÷matrix: element-wise (errors on any zero element). scalar/matrix broadcast. exact÷exact: exact, closed. complex: complex. | 3 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `%` | `( a b -- remainder quotient )` | truncating division: pushes `a − trunc(a/b)·b` then `trunc(a/b)`; float or matrix element-wise with scalar broadcast, answering a remainder matrix under a quotient matrix, or exact over exacts; a zero divisor or zero element errors | 4 (float) | matrix `2m(r×c)` | float O(1); matrix O(r×c) |
| `mod` | `( a b -- remainder )` | remainder with the sign of the dividend (`fmod`); float or matrix element-wise with scalar broadcast, or exact over exacts; a zero divisor or zero element errors | 3 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `^` | `( a b -- a^b )` | `pow`; float or matrix (element-wise) / scalar broadcast; an exact base with an integer exponent stays exact; a complex base or exponent (floats promote) is `cpow` | 3 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `negate` | `( a -- -a )` | float, matrix (element-wise), or exact | 2 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `1+` | `( a -- a+1 )` | float, matrix, or exact | 2 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `1-` | `( a -- a-1 )` | float, matrix, or exact | 2 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `sq` | `( a -- a² )` | float, matrix, or exact | 2 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `min2` | `( a b -- smaller )` | the lesser of two values in natural order — floats, strings, quantities (within one dimension — a plain number or another dimension errors; the right side rescales and a matrix result keeps the left unit); NaN orders below every number, so a NaN operand answers NaN. With a matrix operand it is element-wise with scalar broadcast; it orders a pair rather than reducing one collection | 3 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `max2` | `( a b -- larger )` | the greater of two values in natural order — floats, strings, quantities (within one dimension — a plain number or another dimension errors; the right side rescales and a matrix result keeps the left unit); a NaN operand answers the other value. With a matrix operand it is element-wise with scalar broadcast; it orders a pair rather than reducing one collection | 3 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `atan2` | `( y x -- f )` | the angle of the point (x, y) in (−π, π] — `atan2(y, x)`, using the signs of both arguments to place the quadrant; float, or matrix element-wise with scalar broadcast | 3 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |

```forth +
3 4 + . cr
"con" "cat" + . cr
[< 1 2 >] [< 2 3 >] + . cr
```
```output
7
concat
[< 1 2 3 >]
```

```forth -
10 3 - . cr
[< 1 2 3 >] [< 2 >] - . cr
```
```output
7
[< 1 3 >]
```

```forth *
6 7 * . cr
[< 1 2 3 >] [< 2 3 4 >] * . cr
```
```output
42
[< 2 3 >]
```

```forth /
10 4 / . cr
```
```output
2.5
```

```forth %
7 3 % . . cr
```
```output
2 1
```

```forth mod
7 3 mod . -7 3 mod . cr
```
```output
1 -1
```

```forth ^
2 10 ^ . cr
```
```output
1024
```

```forth negate
5 negate . cr
```
```output
-5
```

```forth 1+
41 1+ . cr
```
```output
42
```

```forth 1-
43 1- . cr
```
```output
42
```

```forth sq
9 sq . cr
```
```output
81
```

```forth min2
3 7 min2 . cr
```
```output
3
```

```forth max2
3 7 max2 . cr
```
```output
7
```

```forth atan2
1 1 atan2 . cr
0 -1 atan2 . cr
```
```output
0.785398
3.14159
```

### In-place matrix arithmetic

Mutate the left operand and return it; no allocation. The caller ensures the matrix is unshared.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `+!` | `( mat a -- mat )` | matrix+matrix or matrix+scalar (and scalar+matrix, mutating the matrix) in place | 3 + r×c | none | O(r×c) |
| `-!` | `( mat a -- mat )` | in-place subtract | 3 + r×c | none | O(r×c) |
| `*!` | `( mat a -- mat )` | in-place multiply | 3 + r×c | none | O(r×c) |
| `/!` | `( mat a -- mat )` | in-place divide | 3 + r×c | none | O(r×c) |

```forth +!
[ 1 2 ] vector 10 +! matrix>array . cr
```
```output
[ 11 12 ]
```

```forth -!
[ 10 20 ] vector 1 -! matrix>array . cr
```
```output
[ 9 19 ]
```

```forth *!
[ 1 2 ] vector 3 *! matrix>array . cr
```
```output
[ 3 6 ]
```

```forth /!
[ 10 20 ] vector 4 /! matrix>array . cr
```
```output
[ 2.5 5 ]
```

### Float-only arithmetic ⚠

Operate directly on stack slots' `.number`, in place, with only a depth check — no tag check.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `f+` | `( a b -- a+b )` ⚠ | add, result in deeper slot | 2 | none | O(1) |
| `f-` | `( a b -- a-b )` ⚠ | subtract | 2 | none | O(1) |
| `f*` | `( a b -- a*b )` ⚠ | multiply | 2 | none | O(1) |
| `f/` | `( a b -- a/b )` ⚠ | divide; checks divisor ≠ 0 | 2 | none | O(1) |
| `f=` | `( a b -- f )` ⚠ | float `=`, result `1.0`/`0.0`; no type check | 2 | none | O(1) |
| `f<` | `( a b -- f )` ⚠ | float `<`, result `1.0`/`0.0`; no type check | 2 | none | O(1) |
| `f>` | `( a b -- f )` ⚠ | float `>`, result `1.0`/`0.0`; no type check | 2 | none | O(1) |
| `f<=` | `( a b -- f )` ⚠ | float `<=` (≤), result `1.0`/`0.0`; no type check | 2 | none | O(1) |
| `f>=` | `( a b -- f )` ⚠ | float `>=` (≥), result `1.0`/`0.0`; no type check | 2 | none | O(1) |
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
| `fln1+` | `( a -- ln(1+a) )` ⚠ | `log1p`, in place — natural log of `1+a`, accurate for small `a` | 1 | none | O(1) |
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

```forth f+
1.5 2.5 f+ . cr
```
```output
4
```

```forth f-
5 1.5 f- . cr
```
```output
3.5
```

```forth f*
2.5 4 f* . cr
```
```output
10
```

```forth f/
7 2 f/ . cr
```
```output
3.5
```

```forth f=
2 2 f= . cr
```
```output
1
```

```forth f<
1 2 f< . cr
```
```output
1
```

```forth f>
1 2 f> . cr
```
```output
0
```

```forth f<=
2 2 f<= . cr
```
```output
1
```

```forth f>=
1 2 f>= . cr
```
```output
0
```

```forth f^
2 0.5 f^ . cr
```
```output
1.41421
```

```forth fmod
7 3 fmod . cr
```
```output
1
```

```forth f*+
2 3 10 f*+ . cr
```
```output
16
```

```forth f*-
2 3 10 f*- . cr
```
```output
4
```

```forth f1+
41 f1+ . cr
```
```output
42
```

```forth f1-
43 f1- . cr
```
```output
42
```

```forth fsq
9 fsq . cr
```
```output
81
```

```forth fnegate
5 fnegate . cr
```
```output
-5
```

```forth fabs
-3.5 fabs . cr
```
```output
3.5
```

```forth fsqrt
2 fsqrt . cr
```
```output
1.41421
```

```forth fexp
1 fexp . cr
```
```output
2.71828
```

```forth flog
1000 flog . cr
```
```output
3
```

```forth fln
E fln . cr
```
```output
1
```

```forth fln1+
0 fln1+ . 1e-15 fln1+ . cr
```
```output
0 1e-15
```

```forth fsin
PI 2 f/ fsin . cr
```
```output
1
```

```forth fcos
0 fcos . cr
```
```output
1
```

```forth ftan
PI 4 f/ ftan . cr
```
```output
1
```

```forth ftanh
0 ftanh . cr
```
```output
0
```

```forth fasin
1 fasin . cr
```
```output
1.5708
```

```forth facos
1 facos . cr
```
```output
0
```

```forth fatan
1 fatan . cr
```
```output
0.785398
```

```forth fround
2.5 fround . cr
```
```output
3
```

```forth ftruncate
2.9 ftruncate . cr
```
```output
2
```

```forth fround-up
2.1 fround-up . cr
```
```output
3
```

```forth fround-down
2.9 fround-down . cr
```
```output
2
```

---

## Constants

constants.telic, capitalized by convention. Mathematical values are computed at load;
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

```forth PI
PI . cr
```
```output
3.14159
```

```forth E
E . cr
```
```output
2.71828
```

```forth TAU
TAU . cr
```
```output
6.28319
```

```forth PHI
PHI . cr
```
```output
1.61803
```

```forth C
C . cr
```
```output
299792458 m.s^-1
```

```forth G
G . cr
```
```output
6.6743e-11 m^3.s^-2.kg^-1
```

```forth H
H . cr
```
```output
6.62607e-34 m^2.kg.s^-1
```

```forth HBAR
HBAR . cr
```
```output
1.05457e-34 m^2.kg.s^-1
```

```forth KB
KB . cr
```
```output
1.38065e-23 m^2.kg.s^-2.kelvin^-1
```

```forth NA
NA . cr
```
```output
6.02214e+23 mol^-1
```

```forth QE
QE . cr
```
```output
1.60218e-19 coulomb
```

## Unary math (polymorphic: float or matrix)

Tag-checked; safe. Float input → float; matrix input → new matrix, element-wise.
`abs`, `round`, `truncate`, `round-up`, and `round-down` also take an exact and
answer exactly (see Exact rationals); the transcendentals reject one.
All but `lgamma` and the rounding words also take a complex, answering
principal values; `abs` on one answers the modulus as a float.
A float result that would be NaN (`-1 sqrt`, `-1 ln`) is `null` — NaN-boxing
reserves NaN bit patterns for tags, so `null` is Telic's NaN, and it is falsy,
`null?`, and `= null`. Matrix buffers hold raw NaN elements untouched
(element-wise math writes them, `sort` places them last); a NaN read out of a
matrix (`@i,j`, `@e`) surfaces as `null` the same way.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `abs` | `( a -- \|a\| )` | `fabs` | 2 (float) | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `sqrt` | `( a -- √a )` | `sqrt` | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `exp` | `( a -- eᵃ )` | `exp` | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `log10` | `( a -- log₁₀ a )` | `log10` | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `ln` | `( a -- ln a )` | `log` — natural log | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `ln1+` | `( a -- ln(1+a) )` | `log1p` — natural log of `1+a`, accurate for small `a` where adding 1 before taking the logarithm would lose precision to cancellation; float or matrix element-wise, complex rejected | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `log2` | `( a -- log2 a )` | base-2 log | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `lgamma` | `( a -- ln Γ(a) )` | `lgamma` — log of the gamma function, which extends the factorial (`5 lgamma` is `ln 4!`). Γ overflows a double past 171, while `171 lgamma` is 706.57. Defined for positive arguments: it is `+inf` at zero and the negative integers, and for negative non-integers it returns ln \|Γ(a)\| and the sign is discarded | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `sin` | `( a -- sin a )` | sine (radians) | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `cos` | `( a -- cos a )` | cosine (radians) | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `tan` | `( a -- tan a )` | tangent (radians) | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `tanh` | `( a -- tanh a )` | hyperbolic tangent | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `erf` | `( a -- erf a )` | the error function | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `erfc` | `( a -- erfc a )` | the complementary error function, 1 minus the error function, computed without cancellation in the tail | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `sinh` | `( a -- sinh a )` | hyperbolic sine | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `cosh` | `( a -- cosh a )` | hyperbolic cosine | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `asin` | `( a -- asin a )` | inverse sine | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `acos` | `( a -- acos a )` | inverse cosine | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `atan` | `( a -- atan a )` | inverse tangent | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `round` | `( a -- round a )` | `round` | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `truncate` | `( a -- trunc a )` | `trunc` | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `round-up` | `( a -- ceil a )` | `ceil` | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `round-down` | `( a -- floor a )` | `floor` | 2 | matrix `1m(r×c)` | float O(1); matrix O(r×c) |
| `quotient` | `( a b -- quotient )` | core.telic: the quotient of a÷b, truncated toward zero | 9 | none | O(1) |

```forth abs
-7 abs . cr
[ -1 2 ] vector abs matrix>array . cr
```
```output
7
[ 1 2 ]
```

```forth sqrt
16 sqrt . cr
-1 sqrt . cr
```
```output
4
null
```

```forth exp
0 exp . cr
```
```output
1
```

```forth log10
100 log10 . cr
```
```output
2
```

```forth ln
E ln . cr
```
```output
1
```

```forth ln1+
0 ln1+ . 1e-15 ln1+ . cr
```
```output
0 1e-15
```

```forth log2
1024 log2 . cr
```
```output
10
```

```forth lgamma
5 lgamma . cr
```
```output
3.17805
```

```forth sin
0 sin . cr
```
```output
0
```

```forth cos
PI cos . cr
```
```output
-1
```

```forth tan
0 tan . cr
```
```output
0
```

```forth tanh
100 tanh . cr
```
```output
1
```

```forth erf
1 erf . cr
```
```output
0.842701
```

```forth erfc
2 erfc . cr
```
```output
0.00467773
```

```forth sinh
1 sinh . cr
```
```output
1.1752
```

```forth cosh
1 cosh . cr
```
```output
1.54308
```

```forth asin
0.5 asin . cr
```
```output
0.523599
```

```forth acos
0 acos . cr
```
```output
1.5708
```

```forth atan
0 atan . cr
```
```output
0
```

```forth round
2.5 round . -2.5 round . cr
```
```output
3 -3
```

```forth truncate
-2.9 truncate . cr
```
```output
-2
```

```forth round-up
2.1 round-up . cr
```
```output
3
```

```forth round-down
-2.1 round-down . cr
```
```output
-3
```

```forth quotient
7 3 quotient . -7 3 quotient . cr
```
```output
2 -2
```

---

## Comparison and logic

Result is `1.0` (true) or `0.0` (false), with a float fast path. `=` uses `val_cmp` (structural): matrices compare by shape then row-major contents, so they order for set membership. `<`/`>` are structural too, **except on matrices**, where they compare element-wise and return a 1.0/0.0 matrix (same shape, or a scalar broadcasts over the matrix). A quantity on either side of `<`/`>`/`<=`/`>=`/`eq`/`neq` compares within its dimension: the right operand rescales into the left's unit (`prices 10 $ <` works whether prices are in `$` or `¢`; a dimensioned matrix masks element-wise, the mask coming back bare), and a quantity against a plain number or a different dimension errors — including the fused compare-and-branch form. `=` stays structural: differing dimensions answer 0. An array operand masks element-wise too: each element compares by `val_cmp` against the other operand (or pairwise against an equal-length array — unequal lengths error), yielding an n×1 mask, so `names "ann" eq where` filters a text column and string order is lexicographic. Directly before `if`/`while`/`until` a comparison fuses into a compare-and-branch, which stays structural.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `=` | `( a b -- bool )` | structural equality; handles — a stream, database, C pointer, continuation, execution token or symbol — compare by identity, so two open connections are unequal and a handle equals only itself | 3 (float) | none | float O(1); string O(\|s\|); array/set O(n); frame O(n); matrix O(r×c) |
| `<` | `( a b -- bool )` or `( mat/arr x -- mat )` | less-than; element-wise 1/0 mask on matrix operands (scalar broadcast) and on array operands (compared in natural order per element, n×1) | 3 (float) | matrix `1m(r×c)` | float O(1); string O(\|s\|); array/set O(n); frame O(n); matrix O(r×c) |
| `<=` | `( a b -- bool )` or `( mat/arr x -- mat )` | less-than-or-equal (≤); element-wise 1/0 mask on matrix operands (scalar broadcast) and on array operands (compared in natural order per element, n×1) | 3 (float) | matrix `1m(r×c)` | float O(1); string O(\|s\|); array/set O(n); frame O(n); matrix O(r×c) |
| `true` | `( -- bool )` | core.telic: pushes 1 | 1 | none | O(1) |
| `false` | `( -- bool )` | core.telic: pushes 0 | 1 | none | O(1) |
| `>` | `( a b -- bool )` or `( mat/arr x -- mat )` | greater-than; element-wise 1/0 mask on matrix operands (scalar broadcast) and on array operands (compared in natural order per element, n×1) | 3 (float) | matrix `1m(r×c)` | float O(1); string O(\|s\|); array/set O(n); frame O(n); matrix O(r×c) |
| `>=` | `( a b -- bool )` or `( mat/arr x -- mat )` | greater-than-or-equal (≥); element-wise 1/0 mask on matrix operands (scalar broadcast) and on array operands (compared in natural order per element, n×1) | 3 (float) | matrix `1m(r×c)` | float O(1); string O(\|s\|); array/set O(n); frame O(n); matrix O(r×c) |
| `eq` | `( a b -- bool )` or `( mat/arr x -- mat )` | equality; element-wise 1/0 mask on matrix and array operands (scalar broadcast; per array element in natural order), structural equality on other collections and scalars. NaN elements equal nothing | 3 (float) | matrix `1m(r×c)` | float O(1); string O(\|s\|); array/set O(n); frame O(n); matrix O(r×c) |
| `neq` | `( a b -- bool )` or `( mat/arr x -- mat )` | not-equal: element-wise 1/0 mask on matrix and array operands (scalar broadcast; per array element in natural order), structural inequality on other collections and scalars. A NaN element differs from everything, so it masks 1 | 3 (float) | matrix `1m(r×c)` | float O(1); string O(\|s\|); array/set O(n); frame O(n); matrix O(r×c) |
| `nan?` | `( v -- bool )` or `( mat/arr -- mat )` | NaN test: 1/0 mask over a matrix's elements; an array answers an n×1 mask marking `null` elements (a text column's missing cells), composing with `where`/`select-rows`; `1` on `null` itself (a scalar NaN *is* `null`), `0` on any float, exact, or complex. NaNs compare false under `<`/`>`/`eq`, so this is the word that masks them | 2 | `1m(r×c)` | O(1); matrix/array O(n) |
| `0=` | `( a -- bool )` | `!truthy(a)`; any type | 2 | none | O(1) |
| `1=` | `( a -- bool )` | core.telic: `1` when the value equals 1, else `0` | 5 | none | O(1) |
| `true?` | `( a -- bool )` | core.telic: `1` when the value is truthy, else `0`. A heap value is truthy by its handle, so an empty string, array, or frame is truthy; only `0`, `null`, and an unbound handle are falsy | 3 | none | O(1) |
| `false?` | `( a -- bool )` | core.telic: `1` when the value is falsy, else `0` | 2 | none | O(1) |
| `between?` | `( v low high -- bool )` or `( mat/arr low high -- mat )` | core.telic: low ≤ v ≤ high, inclusive; a matrix or array operand answers an element-wise mask, and any ordered scalars compare (strings lexicographic, quantities rescaled within a dimension) | 9 | matrix `2m(r×c)` | O(1); matrix/array O(n) |
| `clamp` | `( x low high -- x' )` | core.telic: x limited to [low, high] — values under low answer low, over high answer high; a matrix clamps element-wise with scalar bounds, an array clamps each element (nested arrays included), and quantities clamp within their dimension | 6 | matrix `2m(r×c)`; array `1a(n)` | O(1); matrix/array O(n) |
| `type-of` | `( a -- sym )` | The value's type as a symbol: `:float` `:string` `:symbol` `:array` `:set` `:frame` `:matrix` `:quantity` `:xt` `:continuation` `:stream` `:db` `:ptr` `:segment` `:null` `:wildcard` `:lvar` `:exact` `:complex` `:rest`. A bound logic var reports its value's type; an unbound one is `:lvar` | 2 | none | O(1) |
| `float?` | `( a -- bool )` | core.telic: `1` when the value is a float, else `0` | 5 | none | O(1) |
| `string?` | `( a -- bool )` | core.telic: `1` when the value is a string, else `0` | 5 | none | O(1) |
| `symbol?` | `( a -- bool )` | core.telic: `1` when the value is a symbol, else `0` | 5 | none | O(1) |
| `array?` | `( a -- bool )` | core.telic: `1` when the value is an array, else `0` | 5 | none | O(1) |
| `set?` | `( a -- bool )` | core.telic: `1` when the value is a set, else `0` | 5 | none | O(1) |
| `frame?` | `( a -- bool )` | core.telic: `1` when the value is a frame, else `0` | 5 | none | O(1) |
| `matrix?` | `( a -- bool )` | core.telic: `1` when the value is a matrix, else `0` | 5 | none | O(1) |
| `quantity?` | `( a -- bool )` | core.telic: `1` when the value is a quantity, else `0` | 5 | none | O(1) |
| `xt?` | `( a -- bool )` | core.telic: `1` when the value is an execution token, else `0` | 5 | none | O(1) |
| `continuation?` | `( a -- bool )` | core.telic: `1` when the value is a continuation, else `0` | 5 | none | O(1) |
| `stream?` | `( a -- bool )` | core.telic: `1` when the value is a stream, else `0` | 5 | none | O(1) |
| `db?` | `( a -- bool )` | core.telic: `1` when the value is a database, else `0` | 5 | none | O(1) |
| `ptr?` | `( a -- bool )` | core.telic: `1` when the value is a C pointer, else `0` | 5 | none | O(1) |
| `segment?` | `( a -- bool )` | core.telic: `1` when the value is a segment, else `0` | 5 | none | O(1) |
| `null?` | `( a -- bool )` | True when the value is `null`; a logic variable is tested through its binding, so one bound to `null` answers 1 and an unbound one 0 | 2 | none | O(1) |
| `wildcard?` | `( a -- bool )` | core.telic: `1` when the value is a wildcard, else `0` | 5 | none | O(1) |
| `lvar?` | `( a -- bool )` | core.telic: `1` when the value is an unbound logic variable, else `0` | 5 | none | O(1) |
| `and` | `( a b -- bool )` | logical and of truthiness | 3 | none | O(1) |
| `or` | `( a b -- bool )` | logical or of truthiness | 3 | none | O(1) |
| `not` | `( a -- bool )` | logical not of truthiness | 2 | none | O(1) |

`truthy` of a float is `≠ 0.0`; of any heap value, its handle `≠ 0`.

```forth =
1 1 = . "ab" "ab" = . [ 1 2 ] [ 1 2 ] = . cr
```
```output
1 1 1
```

```forth <
1 2 < . cr
[ 1 5 3 ] vector 2 < matrix>array . cr
```
```output
1
[ 1 0 0 ]
```

```forth <=
2 2 <= . cr
```
```output
1
```

```forth true
true . false . cr
```
```output
1 0
```

```forth false
false . true . cr
```
```output
0 1
```

```forth >
3 2 > . cr
```
```output
1
```

```forth >=
2 3 >= . cr
```
```output
0
```

```forth eq
"ab" "ab" eq . cr
[ 1 2 1 ] vector 1 eq matrix>array . cr
```
```output
1
[ 1 0 1 ]
```

```forth neq
"ab" "ac" neq . cr
[ 1 2 1 ] vector 1 neq matrix>array . cr
```
```output
1
[ 0 1 0 ]
```

```forth nan?
null nan? . cr
[ 1 null 3 ] vector nan? matrix>array . cr
```
```output
1
[ 0 1 0 ]
```

```forth 0=
0 0= . 5 0= . cr
```
```output
1 0
```

```forth 1=
1 1= . cr
```
```output
1
```

```forth true?
5 true? . 0 true? . null true? . cr
```
```output
1 0 0
```

```forth false?
5 false? . 0 false? . null false? . cr
```
```output
0 1 1
```

```forth between?
5 3 8 between? . 9 3 8 between? . cr
[ 1 5 9 ] vector 2 8 between? matrix>array . cr
```
```output
1 0
[ 0 1 0 ]
```

```forth clamp
5 0 3 clamp . -1 0 3 clamp . cr
[ -5 2 9 ] vector 0 3 clamp matrix>array . cr
```
```output
3 0
[ 0 2 3 ]
```

```forth type-of
3.5 type-of . "s" type-of . [ ] type-of . cr
```
```output
:float :string :array
```

```forth float?
3 float? . "x" float? . cr
```
```output
1 0
```

```forth string?
"x" string? . cr
```
```output
1
```

```forth symbol?
:a symbol? . cr
```
```output
1
```

```forth array?
[ ] array? . cr
```
```output
1
```

```forth set?
[< 1 >] set? . cr
```
```output
1
```

```forth frame?
{ } frame? . cr
```
```output
1
```

```forth matrix?
[ 1 ] vector matrix? . cr
```
```output
1
```

```forth quantity?
3 m quantity? . cr
```
```output
1
```

```forth xt?
' dup xt? . cr
```
```output
1
```

```forth continuation?
[: 5 yield :] start-generator continuation? . drop cr
```
```output
1
```

```forth stream?
stdout stream? . cr
```
```output
1
```

```forth db?
":memory:" db-open dup db? . db-close cr
```
```output
1
```

```forth ptr?
3 ptr? . cr
```
```output
0
```

```forth segment?
4 int-segment segment? . cr
```
```output
1
```

```forth null?
null null? . 0 null? . cr
```
```output
1 0
```

```forth wildcard?
_ wildcard? . cr
```
```output
1
```

```forth lvar?
lvar lvar? . cr
```
```output
1
```

```forth and
1 0 and . 1 2 and . cr
```
```output
0 1
```

```forth or
0 0 or . 0 3 or . cr
```
```output
0 1
```

```forth not
0 not . 5 not . cr
```
```output
1 0
```

### Bitwise

Each operand is read as a two's-complement integer (exact within the double's
53-bit integer range), the operation is applied, and the result is pushed as a
float. `rshift` is arithmetic (sign-preserving).

Hex literals cover the input side: `0xff` (either case, optional sign) reads
as the float 255. Digits only — no `p` exponents — and at most 2^53, past
which the token is not a literal; `{0:x}` under `format` renders hex back out.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `bit-and` | `( a b -- f )` | bitwise AND | 2 | none | O(1) |
| `bit-or` | `( a b -- f )` | bitwise OR | 2 | none | O(1) |
| `bit-xor` | `( a b -- f )` | bitwise XOR | 2 | none | O(1) |
| `bit-not` | `( a -- f )` | two's-complement complement | 1 | none | O(1) |
| `lshift` | `( a n -- f )` | left shift `a` by `n` bits | 2 | none | O(1) |
| `rshift` | `( a n -- f )` | arithmetic right shift, = `floor(a / 2ⁿ)` | 2 | none | O(1) |
| `lowest-bit` | `( a -- i )` | 0-indexed position of the lowest set bit (`-1` if `a` is 0) | 1 | none | O(1) |

```forth bit-and
12 10 bit-and . cr
```
```output
8
```

```forth bit-or
12 10 bit-or . cr
```
```output
14
```

```forth bit-xor
12 10 bit-xor . cr
```
```output
6
```

```forth bit-not
0 bit-not . cr
```
```output
-1
```

```forth lshift
1 10 lshift . cr
```
```output
1024
```

```forth rshift
-16 2 rshift . cr
```
```output
-4
```

```forth lowest-bit
12 lowest-bit . cr
```
```output
2
```

---

## Dimensioned quantities

A quantity is a magnitude (a float, a matrix, an exact, or a complex)
carrying a unit.
Arithmetic between two exact-magnitude quantities is exact, unit rescaling
included (`1/2 $ 50/1 ¢ +` is `1 $`); an exact magnitude against a float
magnitude follows the exact/float mixing rule, and comparison crosses the two
exactly. Units are
rational-exponent vectors over user-declared base dimensions, each with a
rational scale relative to its dimension's base; arithmetic propagates and
checks them. Same-dimension units at any rational scale coexist and convert
(`$`/`¢`, `kg`/`g`, `inch`/`cm`). A unit is a pure scale factor; `°C`/`°F`,
which need an added zero as well, cannot be declared.

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
- `negate` / `abs` — keep the unit; transcendentals (`sin`, `log10`, …) reject a quantity.
- `=` `<` `>` — compare by value, normalizing scale within a dimension (`100 ¢ 1 $ = → 1`). On a dimensioned matrix, `<`/`>`/`eq` answer an element-wise bare mask with the same normalization (`prices 10 $ <`); `=` stays structural everywhere.

Printing shows magnitude then unit: a named unit prints its name (`3 newton`); an
unnamed compound prints its dimensional form with the scale folded into the
magnitude (`10 m 2 s / → 5 m.s^-1`), positive exponents first.

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
| `as` | `( q unit-xt -- q' )` | units.telic: re-express `q` in the unit `unit-xt` attaches (`' MiB`) — `2 34 ^ B ' MiB as` is `16384 MiB`, `1 hour ' s as` is `3600 s`; `magnitude` after gives the bare count. Same dimension required; a different-dimension `q` or a bare-number `q` errors | 4 | 1 pair | O(1) |

`units.telic` predeclares a standard set (names spelled out and lowercase):
length `m` (`km`), time `s` (`minute`, `hour`, `day`, `week`), mass `kg`, current `ampere`,
temperature `kelvin`, amount `mol`; derived `hertz` `newton` `pascal` `joule`
`watt` `coulomb` `volt`; three currencies, each its own dimension —
`$`/`¢`, `£`/`penny`, `€`/`eurocent`; and the IEC binary data units
`B` `KiB` `MiB` `GiB` `TiB` `PiB`, each 1024× the last.

```forth magnitude
10 km magnitude . cr
```
```output
10
```

```forth unit-of
10 km unit-of . cr
```
```output
1 km
```

```forth as
2 34 ^ B ' MiB as . cr
1 hour ' s as . cr
```
```output
16384 MiB
3600 s
```

## Exact rationals

An exact is a fraction of two arbitrarily large integers, always reduced to
lowest terms; an integer is the case with denominator 1. `1/3`, `-7/2`, and
`5/1` are literals, and a plain integer literal too large for a float to hold
exactly (a 19-digit id) reads as an exact, keeping every digit. The
same rule applies to `json>frame` integers and SQLite INTEGER columns;
`frame>json` and `db-exec`/`db-query` parameters write integer exacts back
without loss (a non-integer exact errors there).

The polymorphic words compute exactly on exact operands: `+` `-` `*` `/`
(`1/3 1/6 +` is `1/2`; dividing two exacts always yields an exact; `/` errors
on an exact zero), `%` `mod` `quotient`, `negate` `abs` `1+` `1-` `sq`, `^`
with an integer exponent (float or exact), and `round` `truncate` `round-up`
`round-down`, which answer integer exacts. Only comparison accepts an exact
and a float together: `=` `<` `>` `<=` `>=` `min2` `max2`, `sort`, and set
membership compare the two kinds numerically and without rounding
(`1/2 0.5 =` is true), so a collection holding both sorts and deduplicates
correctly. Arithmetic between an exact and a float errors — convert
explicitly. A quantity's magnitude may be an exact (see Dimensioned
quantities); `^` on one takes integer exponents, and `sqrt` and the
transcendentals reject one. Matrices, segments, the bitwise words, and
the ⚠ float words take only floats; an exact operand errors. Every
exact operation allocates its result, so exact arithmetic costs ten times
the float fast path and more; the float path itself is unchanged.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `float>exact` | `( f -- x )` | The float's exact value — every float is an integer divided by a power of two, so nothing is lost; errors on NaN or an infinity. An exact passes through unchanged | limbs | `1o` | O(limbs) |
| `exact>float` | `( x -- f )` | The nearest float, round-to-nearest-even (denormals and the overflow threshold included); a float passes through unchanged | shift + divmod | arena temps | O(bits · limbs) |
| `numerator` | `( x -- x' )` | The reduced numerator as an integer exact, carrying the sign | limbs | `1o` | O(limbs) |
| `denominator` | `( x -- x' )` | The reduced denominator as a positive integer exact | limbs | `1o` | O(limbs) |
| `rationalize` | `( f -- x )` | The simplest rational that reads back as the same float — the smallest-denominator fraction in the float's rounding interval (`0.111 rationalize` is `111/1000`); an exact passes through unchanged | cf steps | exacts per step | O(steps · limbs) |
| `exact?` | `( a -- bool )` | core.telic: `1` when the value is an exact, else `0` | 5 | none | O(1) |

```forth float>exact
0.5 float>exact . cr
0.1 float>exact denominator . cr
```
```output
1/2
36028797018963968
```

```forth exact>float
1/4 exact>float . cr
```
```output
0.25
```

```forth numerator
-6/8 numerator . cr
```
```output
-3
```

```forth denominator
6/8 denominator . cr
```
```output
4
```

```forth rationalize
0.111 rationalize . cr
1/3 exact>float rationalize . cr
```
```output
111/1000
1/3
```

```forth exact?
1/2 exact? . 0.5 exact? . cr
```
```output
1 0
```

## Complex numbers

A complex is a pair of floats, real and imaginary. `3+4i`, `-1.5-2i`, and
`4i` are literals; the imaginary part is always explicit (`1i`, never a bare
`i`), and output round-trips as input. `+` `-` `*` `/` take two complexes or
a complex and a float, which promotes losslessly (a float is a complex with
imaginary part 0); `negate` keeps the type; `abs` answers the modulus as a
float. `sqrt`, `exp`, `ln`, `log10`, the trigonometric and hyperbolic words,
`sq`, `1+`, and `1-` are closed over complexes, answering principal values
(`-1+0i sqrt` is `0+1i`, `-1+0i ln` is `0+3.14159i`); `^` takes a complex
base or exponent. A NaN part is refused at construction. Comparison and
sorting order by real part then imaginary part, and a complex equals a float
of its value (`3+0i 3 =` is true), so collections, frames, `format`, and the
parallel words take complexes like any value. A quantity's magnitude may be
a complex (`3+4i ohm` is an impedance) — quantity arithmetic combines the
parts and rescales both by the unit conversion factor, and a float magnitude
promotes. A complex operand errors in matrices, exact arithmetic, `lgamma`,
and the rounding words.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `complex` | `( re im -- z )` | Build a complex from two floats | 3 | 1 pair | O(1) |
| `real-part` | `( z -- f )` | The real part; a float answers itself | 2 | none | O(1) |
| `imaginary-part` | `( z -- f )` | The imaginary part; a float answers 0 | 2 | none | O(1) |
| `conjugate` | `( z -- z' )` | The complex conjugate — the imaginary part negated, so `z conjugate *` is \|z\|² (as a complex); a float answers itself | 2 | 1 pair | O(1) |
| `arg` | `( z -- f )` | The phase in (−π, π] — `atan2` of the imaginary over the real part; a float answers 0, or π when negative | 2 | none | O(1) |
| `complex?` | `( a -- bool )` | core.telic: `1` when the value is a complex, else `0` | 5 | none | O(1) |

```forth complex
3 4 complex . cr
```
```output
3+4i
```

```forth real-part
3+4i real-part . cr
```
```output
3
```

```forth imaginary-part
3+4i imaginary-part . 5 imaginary-part . cr
```
```output
4 0
```

```forth conjugate
3+4i conjugate . 5 conjugate . cr
```
```output
3-4i 5
```

```forth arg
0+1i arg . -1+0i arg . -2 arg . cr
```
```output
1.5708 3.14159 3.14159
```

```forth complex?
3+4i complex? . 5 complex? . cr
```
```output
1 0
```

## Return stack

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `>r` | `( a -- )` → return stack | Move top to return stack | 2 | none | O(1) |
| `r>` | return stack → `( -- a )` | Move return-stack top to data stack | 2 | none | O(1) |
| `r@` | `( -- a )` | Copy return-stack top to data stack | 2 | none | O(1) |

```forth >r
1 2 >r . r> . cr
```
```output
1 2
```

```forth r>
5 >r r> . cr
```
```output
5
```

```forth r@
7 >r r@ . r> . cr
```
```output
7 7
```

---

## Side stack

A third stack (depth 1024) for values set aside during a computation; used by `try-catch` to hold the handler.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `>side` | `( a -- )` | Push to side stack | 2 | none | O(1) |
| `side>` | `( -- a )` | Pop from side stack | 2 | none | O(1) |
| `side-drop` | `( -- )` | Discard side-stack top | 1 | none | O(1) |
| `side-peek` | `( -- a )` | Copy side-stack top to the data stack | 1 | none | O(1) |
| `side-depth` | `( -- n )` | Push side-stack depth | 1 | none | O(1) |

```forth >side
10 20 >side . side> . cr
```
```output
10 20
```

```forth side>
7 >side side> . cr
```
```output
7
```

```forth side-drop
1 >side 2 >side side-drop side> . cr
```
```output
1
```

```forth side-peek
9 >side side-peek . side-drop cr
```
```output
9
```

```forth side-depth
1 >side 2 >side side-depth . side-drop side-drop cr
```
```output
2
```

---

## Control flow (compile-time)

Immediate words that emit branch instructions into the current definition. Outside a definition or quotation there is nothing to emit into, and the openers (`if`, `?if`, `begin`) error: `only valid inside a colon definition or quotation`. A top-level conditional goes in a quotation, run on the spot: `[: … if … then :] execute`.

| Word | Runtime effect | Behavior |
|------|---------------|----------|
| `if` | `( flag -- )` | Branch past the `then`/`else` if flag is falsy |
| `?if` | `( flag -- flag )` | Branch past the `then`/`else` if flag is falsy, but peek the flag instead of consuming it — the flag stays on the stack in both branches |
| `else` | — | Separate the true and false arms |
| `then` | — | Close an `if`/`if…else`; patches the forward branch |
| `begin` | — | Mark a loop top |
| `until` | `( flag -- )` | Branch back to `begin` if flag is falsy |
| `again` | — | Unconditional branch back to `begin` |
| `while` | `( flag -- )` | Exit the loop forward if flag is falsy (`begin … while … repeat`) |
| `repeat` | — | Branch back to `begin`; patches the `while` exit |
| `do` | `( start limit delta -- )` | Take start, limit, delta from the stack and read the next token as the index name; the body runs with the index at start, start+delta, …, while the value is short of limit ⚠ |
| `loop` | — | End a `do` loop |
| `case` | `( sel -- )` | Open pattern dispatch on the selector; clauses follow, each `pattern of … endof`, then an optional default region, then `endcase` |
| `of` | — | Unify the clause pattern with the selector: on a match the bindings commit, both are dropped, and the body runs; on a mismatch the bindings roll back, the pattern is dropped, and the selector falls to the next clause |
| `endof` | — | Close a clause; jumps past `endcase` |
| `endcase` | — | Close the `case`; the default region before it runs with the unmatched selector on the stack and owns it (`drop` it or transform it) |
| `leave` | — | Branch past the innermost loop's closing word; conditional form is `if leave then` |
| `continue` | — | Branch to the innermost loop's next iteration: a `while` loop re-runs its test, an `until` loop skips its trailing test and repeats unconditionally, and a `do` loop steps its index and counts the iteration (so it terminates) |
| `exit` | `( -- )` | Return early from the current definition (this one runs at run time) |

`leave` and `continue` are plain compiled branches — zero runtime cost. Both
are compile errors outside a loop, and a quotation opens its own frame, so a
`[: leave :]` inside a loop body does not see that loop. A `begin` with no
`until`/`again`/`repeat`, or a `do` with no `loop`, is a compile error at `;`
or `:]` (an unpatched `leave` would otherwise be a wild branch); the partial
definition rolls back. In `times` / `i-times` quotations, `exit` already ends
the current iteration.

`case` dispatches by unification (`unify?`), so a clause pattern may be a
ground value, which behaves as equality; a frame, which matches as an open
record (`{ :type :quit } of` matches any frame carrying that pair); `_`; or a
structure holding logic vars, which bind for the clause body — read them with
`?`, and declare per-call fresh vars in the head (`| ?x |`). A failed arm's
bindings roll back before the next arm is tried. The pattern is computed at
run time by ordinary code, so anything may build it.

A `do` loop whose start is already at or past limit runs zero times; a zero
delta errors. Writing to the index inside the body does not change how many
times the loop runs. `do` pops its triple in `matrix-range`'s operand order.
The operands are floats, and the index step is a raw float add. The index is
still readable after the loop.
A `do` index name resolves as a `to` target does: an existing local of the
body reuses its slot, a word's name shadows it for the body, a global
variable's name errors. Each loop takes three of the body's 128 local slots —
the index, the remaining-iteration count, and the delta — and a loop of 2^53
or more iterations errors. Nested `do` loops read each index by its name;
giving an inner `do` the index name of an enclosing `do` is a compile error.

```forth if
: absolute dup 0 < if negate then ; -7 absolute . cr
```
```output
7
```

```forth ?if
: keep-if-truthy ?if "kept" . then . cr ; 5 keep-if-truthy 0 keep-if-truthy
```
```output
kept 5
0
```

```forth else
: parity 2 mod 0= if "even" else "odd" then . cr ; 7 parity
```
```output
odd
```

```forth then
: past-ten 10 > if "big" . then "done" . cr ; 42 past-ten
```
```output
big done
```

```forth begin
: countdown begin dup . 1- dup 0= until drop cr ; 3 countdown
```
```output
3 2 1
```

```forth until
: triple-count 1 begin dup . 3 + dup 9 > until drop cr ; triple-count
```
```output
1 4 7
```

```forth again
: to-five 1 begin dup . dup 5 = if leave then 1+ again drop cr ; to-five
```
```output
1 2 3 4 5
```

```forth while
: halves begin dup 0 > while dup . 2 quotient repeat drop cr ; 20 halves
```
```output
20 10 5 2 1
```

```forth repeat
: powers 1 begin dup 100 < while dup . 2 * repeat drop cr ; powers
```
```output
1 2 4 8 16 32 64
```

```forth do
: squares 0 5 1 do k k k * . loop cr ; squares
: countdown 5 0 -1 do k k . loop cr ; countdown
: count-evens 0 to n_evens 0 10 1 do k k 2 mod 0= if ++ n_evens then loop n_evens . cr ; count-evens
```
```output
0 1 4 9 16
5 4 3 2 1
5
```

```forth loop
: tenths 0 0.3 0.1 do k k . loop cr ; tenths
: skip-one 0 4 1 do k k 1 = if continue then k . loop cr ; skip-one
: find-cutoff 0 100 1 do k k k * 50 > if leave then loop k . cr ; find-cutoff
```
```output
0 0.1 0.2
0 2 3
8
```

```forth leave
: stop-at-zero begin dup . 1- dup 0 < if leave then again drop cr ; 2 stop-at-zero
```
```output
2 1 0
```

```forth continue
: odds-to-nine 0 begin 1+ dup 9 > if leave then dup 2 mod 0= if continue then dup . again drop cr ; odds-to-nine
```
```output
1 3 5 7 9
```

```forth case
: kind case 1 of "one" endof 2 of "two" endof drop "many" endcase . cr ;
1 kind 5 kind
```
```output
one
many
```

```forth of
: dispatch | ?x | case { :cmd :add :n x } of x ? 1 + . endof { :cmd :quit } of "bye" . endof drop "?" . endcase cr ;
{ :cmd :add :n 4 } dispatch
{ :cmd :quit :id 7 } dispatch
```
```output
5
bye
```

```forth endof
: parity case 0 of "zero" endof 1 of "one" endof drop "big" endcase . cr ;
1 parity
```
```output
one
```

```forth endcase
: describe case :ok of "fine" endof "unmatched: " . dup . drop "seen" endcase . cr ;
:oops describe
```
```output
unmatched:  :oops seen
```

```forth exit
: early dup 0 < if drop "neg" . cr exit then drop "pos" . cr ; -3 early
```
```output
neg
```

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
| `[<` | — | Open a set literal; `>]` closes it (both need surrounding spaces) |
| `>]` | — | Close a set literal |
| `:]` | — | Close a quotation (`[:` itself is under Defining) |
| `\|` | — | Close a body's locals head, and open it when no name precedes: `\| x y \|` and `x y \|` both receive x and y (see Locals) |

```forth [
[ 1 2 3 ] . cr
```
```output
[ 1 2 3 ]
```

```forth ]
[ 1 2 3 ] . cr
```
```output
[ 1 2 3 ]
```

```forth {
{ :a 1 :b 2 } frame>array . cr
```
```output
[ :a 1 :b 2 ]
```

```forth }
{ :x 9 } :x @ . cr
```
```output
9
```

```forth [<
[< 3 1 2 >] . cr
```
```output
[< 1 2 3 >]
```

```forth >]
[< 3 1 2 >] . cr
```
```output
[< 1 2 3 >]
```

```forth :]
5 [: 2 * :] execute . cr
```
```output
10
```

```forth |
: hyp | a b | a a * b b * + sqrt ; 3 4 hyp . cr
: discounted | price | 0.2 to rate price price rate * - ; 100 discounted . cr
: staged 10 to start-value  start-value 3 * to scaled  start-value scaled + ; staged . cr
```
```output
5
80
40
```

## Defining and compiling words

These parse following tokens and/or compile code. Costs are dominated by compilation, not by a stack effect, so no cost columns.

At `;` and `:]` the compiler rewrites tail calls: a colon-word call that
reaches `exit` becomes a jump, so tail recursion — direct, or through
`recurse` — runs at constant return-stack depth. A body using `>r`/`r>`/`r@`,
`reset`/`shift`, `fail`, or a quotation with locals keeps plain calls, and so
does a call through a word that is still `defer`red — mutual recursion via
`defer` grows the return stack.

| Word | Stack effect | Behavior |
|------|-------------|----------|
| `:` | — | Begin a colon definition; read the following name; enter compile mode |
| `;` | — | End a colon definition; emit `exit`; store the source text for `see`. Self-delimiting: `dup *;` parses |
| `recurse` | — | Compile a call to the innermost definition being compiled — the enclosing quotation, else the enclosing colon word — so an anonymous quotation can self-call. In tail position the call is eliminated (constant return-stack depth); elsewhere it grows the return stack. Compile error outside a definition |
| `variable` | — | Read the following name; declare a global variable initialized to `0.0` |
| `constant` | `( val -- )` | Pop a value and read the following name; define an inline word that pushes it as a literal, so call sites fold to the literal with no run-time fetch. Fixed at definition — `to` cannot reassign it |
| `to` | `( val -- )` | Assign to the named local (in a definition) or global. At interpreted top level — the REPL, a program file, a `load`ed file — it auto-creates an absent global. In a compiled body, a colon definition or a quotation alike, a free name declares a local in that body's frame and stores into it (see Locals), so the head names only what the body receives and a body needs no head at all. Assigning an existing global from inside a body needs `^name` in the head; without it `to` reports that the name is a global. An existing word that is not a variable is never a target, at top level or in a body — `to m` fails because `m` is the metre unit. May trigger superword store-fusion while compiling. |
| `symbol` | — | Read the following name; declare a word that pushes a specific interned symbol |
| `defer` | `( "name" -- )` | Read the following name; declare a forward-referenced word with no target. Calling it before a target is installed throws `unresolved deferred word`. Enables mutual recursion and late binding; set the target with `embodies` or `embodies!` |
| `embodies` | `( xt "name" -- )` | Pop an xt (a colon word or quotation) and read the following name; install it as the named deferred word's target. Retargetable — each later call re-reads it — so a call to the deferred word forwards through one dispatch. Top-level only |
| `embodies!` | `( xt "name" -- )` | Pop an xt (a colon word or quotation) and read the following name; install it as the named deferred word's target, finalizing: rewrite every existing call site of the deferred word to call the target directly (no forwarding cost), then turn the word into an ordinary word. Not retargetable afterward; a further `embodies`/`embodies!` reports it is no longer deferred. Top-level only |
| `base` | `( -- q )` | Push a base quantity — a fresh dimension with its base unit, magnitude `1.0`. Paired with `unit` to declare a base dimension (`base unit m`) |
| `unit` | `( q -- )` | Read the following name; pop a quantity whose magnitude is a positive whole number, and define a postfix word attaching that unit. The magnitude is the unit's integer scale relative to its dimension's base (`100 cent unit dollar`). A single unnamed base dimension gets named after the word |
| `:name` | `( -- sym )` | Symbol literal; interns the name at read time |
| `string>symbol` | `( str -- sym )` | Intern a computed string as a symbol |
| `[:` | `( -- xt )` | Open an anonymous quotation (closed by `:]`); compiles its body and pushes its xt. A body that names a local of the enclosing definition or quotation captures it by copy, and the literal then yields a curried token carrying those values (see Locals) |
| `'` | `( "name" -- xt )` | Parse the following word at compile time and push its xt (immediate; folds the xt in as a literal) |
| `lookup` | `( "name" -- xt )` | Parse the following word at run time and push its xt |
| `execute` | `( xt -- … )` | Call the word at xt |
| `curry` | `( value xt -- xt' )` | Bind a value into a curried token: a heap value carrying the target xt and the bound value, accepted wherever an xt is. At invocation the bound value is pushed, then the target runs — so currying binds a word's **trailing** parameters. Collected like any array, so a word may curry on every call; usable inside a parallel region. Applied to a quotation with a head, the bound value fills a trailing head local, so the quotation can declare more locals than the combinator supplies (see Locals). The token is the pair `(bound value, target)`: `curry` packages a witness with a body — existential introduction under Curry–Howard, the witness kept visible (`see` prints it, so it is a transparent dependent pair, not an opaque `∃`) — and invoking the token unpacks the pair, binding the witness in the body | 3 | `1o` | O(n bound) |
| `2curry` | `( a b xt -- xt' )` | Bind two values into a curried token: at invocation it pushes `a`, then `b`, then calls xt | 4 | `1o` | O(n bound) |
| `ncurry` | `( v₁ … v_N xt N -- xt' )` | Bind N values into a curried token: at invocation it pushes `v₁` … `v_N` in that order, then calls xt. `N` of 0 answers a token with the target's own bindings. The count is a value, so a caller binds as many as it has. Currying a token flattens — the outer values push first | 3 + N | `1o` | O(n bound) |
| `inline` | — | Mark the most recent definition inline; future calls splice its body. A body containing a quotation is not spliced — such calls compile as plain calls, since a copied quotation header would have no recorded span |
| `internal` | — | Mark the most recent definition internal: hidden from `words`, `apropos`, and completion, and resolvable — by name or tick — only within its own load unit (the embedded library, one `load`/`load-library` file, or the REPL session); unreachable from another unit |
| `forget` | — | Read the following name; truncate the dictionary back to before it |

```forth :
: twice 2 * ; 21 twice . cr
```
```output
42
```

```forth ;
: greet "hi" . ; greet greet cr
```
```output
hi hi
```

```forth recurse
: fact dup 1 > if dup 1- recurse * then ; 5 fact . cr
```
```output
120
```

```forth variable
variable counter 5 to counter counter . cr
```
```output
5
```

```forth constant
42 constant answer answer . cr
```
```output
42
```

```forth to
variable total 1 to total total 10 + to total total . cr
```
```output
11
```

```forth symbol
symbol blue blue . cr
```
```output
:blue
```

```forth defer
defer greeting : hello-word "hello" . cr ; ' hello-word embodies greeting greeting
```
```output
hello
```

```forth embodies
defer greeting : hello-word "hello" . cr ; ' hello-word embodies greeting greeting
```
```output
hello
```

```forth embodies!
defer farewell : bye-word "bye" . cr ; ' bye-word embodies! farewell farewell
```
```output
bye
```

```forth base
base unit point 12 point unit pica 3 pica . cr
```
```output
3 pica
```

```forth unit
base unit inch 12 inch unit foot 2 foot 6 inch + . cr
```
```output
2.5 foot
```

```forth :name
:north . cr
```
```output
:north
```

```forth string>symbol
"dyn" "amic" + string>symbol . cr
```
```output
:dynamic
```

```forth [:
5 [: 2 * :] execute . cr
: scale-all | rows factor | rows [: factor * :] map ;
[ 1 2 3 ] 10 scale-all . cr
```
```output
10
[ 10 20 30 ]
```

```forth '
16 ' sqrt execute . cr
```
```output
4
```

```forth lookup
lookup sqrt 9 swap execute . cr
```
```output
3
```

```forth execute
7 ' 1+ execute . cr
```
```output
8
```

```forth curry
3 ' + curry 4 swap execute . cr
```
```output
7
```

```forth 2curry
1 2 ' - 2curry execute . cr
```
```output
-1
```

```forth ncurry
1 2 ' swap 2 ncurry execute . . cr
```
```output
1 2
```

```forth inline
: double 2 * ; inline : quadruple double double ; 5 quadruple . cr
```
```output
20
```

```forth internal
: helper-word 3 ; internal : public-word helper-word 2 * ; public-word . cr
```
```output
6
```

```forth forget
: era 1 ; era . cr forget era : era 2 ; era . cr
```
```output
1
2
```

### Locals

The **head** of a definition or quotation body names what that body receives from the stack, rightmost from the top: `| a b c |` takes c from the top, then b, then a. The opening bar is optional, so `: hypotenuse a b | …` and `[: element index | … :]` are the same heads as `| a b |` and `| element index |`. A body has at most one head, and it comes first: a second list, or a list after code, is a compile error, and an empty `| |` is one too — a body that receives nothing omits the head entirely.

Everything else the body needs is declared where it is first assigned. A `to` on a name that is neither already a local of this body nor an existing word declares a local and stores into it. The compiler collects those names before compiling the body, so a name declared by a `to` anywhere in the body is a local of the whole body — a name means one thing throughout, whichever branch or loop assigns it, and the head list carries only what arrives on the stack. A *read* of a name nothing declares is still an unknown word. A local the body never reads, received in the head or assigned by `to`, draws a compile warning naming the definition (with its file and line when loaded); a `do` index is exempt, since a loop that only repeats has no other spelling.

A name in the head that would otherwise resolve outward carries a marker. `^name` names an enclosing **global** the body assigns rather than a local shadowing it: without it, `to name`, `++ name`, `-- name`, `f++ name` and `f-- name` on an existing global are a compile error naming the marker to add. `?name` is a slot holding a fresh logic variable per call, received from nothing. An unmarked name is a capture.

A local may shadow an ordinary word, deliberately and visibly, but never a **compile-time** word: `| source to |`, `| ?case |`, `5 to if`, and a `do` index named `to` are all compile errors — `to is a compile-time word and cannot name a local; rename it` — because the name would shadow the construct that makes the body work, leaving `to` in that body no longer assigning. This holds however the local is declared: in the head, by a `to`, or as a `do` index.

Locals live on the return stack: up to 128 names across up to 64 nested scopes. A slot reads as `null` before its first assignment. A body reads the locals **it declares itself**; a quotation that names a local of the enclosing body **captures** it: the name becomes a trailing head local of the quotation holding a copy of the enclosing slot's value, taken when the `[: … :]` literal is evaluated, and the literal's value is then a curried token rather than a bare xt — exactly what `enclosing-value [: … name | … :] curry` builds by hand. Values reach a quotation four ways: received into its own head, captured from the enclosing body, parked on the stack below the combinator's operands and read by depth with `pick`, or bound into a curried token by `curry`/`2curry`/`ncurry`.

A capture is a copy. Reading it costs the same as any local; `to name` inside the quotation declares a fresh local shadowing the enclosing one, as it does for any name, and `++`/`--`/`f++`/`f--` on a captured name is a compile error, since they would change only the copy. A quotation nested inside a capturing quotation captures through it: each level copies from the one above, so every reference stays in the innermost frame. The token is built where the literal is evaluated, so a capturing quotation written inside a `do`/`begin` loop of its definition allocates a token every iteration; the compiler warns (`warning: quotation captures x inside a loop; …`) unless a captured name is assigned in that body, since then per-iteration construction is the only correct behavior. Hoist the literal above the loop with `to` when the captured values are fixed. A non-capturing quotation compiles as before: a constant xt, no allocation.

The mechanism: a local reference compiles to the **slot index** in the frame that declares it, always the innermost locals-bearing scope, and the op reads `local_base + slot` with no frame walk. Names are discarded after compilation and no value is bound then. Because every reference is depth 0, a quotation's meaning does not depend on which frames happen to be live when it runs — it reads the same slots under `map`, under `i-times`, through `execute`, inside another word's frame, or after a continuation capture and resume.

| Syntax | Behavior |
|--------|----------|
| `\| x y z \|` | Receive x, y, z from the stack: z ← top, y ← second, x ← third; read by bare name |
| `x y z \| … ;` | The opening bar is optional: names before the closing bar, at the head of the body, are the same head |
| `\| ^g \|` | `g` is the enclosing global, which the body may assign with `to`, `++`, `--`, `f++` or `f--`; without the marker those report that `g` is a global |
| `\| ?x \|` | A slot holding a fresh logic variable per call, received from nothing; read by bare name |
| `[: x y \| … :]` | A quotation head, the same in every respect: x and y are received from the stack |

These compile-time words read a following local name and emit a single fused depth-0 instruction:

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `++` | `( -- )` | Increment the named local or global variable by 1 in place; only inside a colon definition; a global target is declared `^name` in the head; errors on an unknown or non-variable name | 1 | none | O(1) |
| `--` | `( -- )` | Decrement the named local or global variable by 1 in place; only inside a colon definition; a global target is declared `^name` in the head; errors on an unknown or non-variable name | 1 | none | O(1) |
| `f++` | `( -- )` ⚠ | Unsafe float increment: raw `.number` mutation, no tag check, for a local known to hold a float; a global target is declared `^name` in the head | 1 | none | O(1) |
| `f--` | `( -- )` ⚠ | Unsafe float decrement: raw `.number` mutation, no tag check; a global target is declared `^name` in the head | 1 | none | O(1) |

```forth ++
: count-up 0 to tally ++ tally ++ tally tally ; count-up . cr
```
```output
2
```

```forth --
: count-down 5 to tally -- tally tally ; count-down . cr
```
```output
4
```

```forth f++
: fast-up 1.5 to reading f++ reading reading ; fast-up . cr
```
```output
2.5
```

```forth f--
: fast-down 1.5 to reading f-- reading reading ; fast-down . cr
```
```output
0.5
```

---

## I/O and printing

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `.` | `( a -- )` | Print value then a space; matrices print as a grid, frames pretty-print | 1 + print | none | O(size printed) |
| `.a` | `( a -- )` | Print value then a space, showing everything: no element truncation, and floats print at full round-trip precision (`%.17g`) rather than 6 significant figures. Matrix/vector columns lose their fixed-width alignment when values render at full precision | 1 + print | none | O(size printed) |
| `render` | `( a -- str )` | The text `.` would print, returned as a string instead of printed: no truncation, no trailing separator (a matrix grid's final newline is dropped). Strings render raw, symbols by name, collections/frames/matrices in their laid-out form | 1 + size | `1o` | O(size) |
| `.s` | `( -- )` | Print every stack value, bottom to top; leaves the stack intact | print | none | O(depth) |
| `peek` | `( a -- a )` | core.telic: print the top value then a space without consuming it — a stack probe | 1 + print | none | O(size printed) |
| `,` | `( a -- a )` | core.telic: print the top value then a space without consuming it, under a one-character name for splicing probes into a pipeline | 1 + print | none | O(size printed) |
| `print` | `( x -- )` | core.telic: print value then a space; matrices print as a grid, frames pretty-print | 1 + print | none | O(size printed) |
| `print-stack` | `( -- )` | core.telic: print every stack value, bottom to top; leaves the stack intact | print | none | O(depth) |
| `cr` | `( -- )` | Print a newline | 1 | none | O(1) |
| `emit` | `( code -- )` | Print the character with codepoint `code`, UTF-8 encoded (1–4 bytes); range-checked `[0, 0x10FFFF]` | 1 | none | O(1) |
| `tty?` | `( -- bool )` | Whether stdout is a terminal (`isatty`) — printing words branch on it to emit styling only for a person at a terminal, so piped and batch output stays plain (`help` dims its prose this way) | 1 | none | O(1) |

String literals `"…"` are **raw**: bytes between the quotes are copied verbatim and an embedded newline is kept; the only escape is a doubled `""`, which yields one `"` (a lone `"` closes the string). A `{n}` in a literal is ordinary text — a regex `\d{3}` stays intact — and template-filling is the explicit word `format` (in String operations below).

```forth .
PI . cr
```
```output
3.14159
```

```forth .a
PI .a cr
```
```output
3.1415926535897931
```

```forth render
{ :a 1 } render print cr
```
```output
{
  :a 1
}
```

```forth .s
1 2 3 .s cr clear
```
```output
1 2 3
```

```forth peek
5 peek 1+ . cr
```
```output
5 6
```

```forth ,
1 2 , + . cr
```
```output
2 3
```

```forth print
"hello" print cr
```
```output
hello
```

```forth print-stack
7 8 print-stack cr clear
```
```output
7 8
```

```forth cr
"a" . cr "b" . cr
```
```output
a
b
```

```forth emit
87 emit 9731 emit cr
```
```output
W☃
```

```forth tty?
tty? . cr
```
```output
0
```

---

## String operations

Regex words run on PCRE2 with JIT-compiled patterns. Each distinct pattern is compiled once and cached (a 1024-slot hash table keyed on the pattern bytes, bounded probe window), so reusing a pattern costs a hash plus one comparison, then the match. Patterns are PCRE syntax in raw `"…"` literals — PCRE itself interprets `\n`, `\t`, `\d`, `\x22`, and the rest. Matching is multiline: `^` and `$` bind to line boundaries. Patterns and subjects are treated as **UTF-8**: `.` matches one codepoint, and `\w` `\d` `\s` `\b` use Unicode properties (accented letters, non-ASCII digits). Invalid byte sequences are tolerated — they simply fail to match rather than raising an error. Match offsets are **byte** offsets (pair them with `byte-substring`). Captures come back as strings; an optional group that didn't participate is `0.0`. Booleans are `1.0`/`0.0`. In the cost columns `n` is the subject length.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `match` | `( str pat -- [ whole cap… ] \| 0 )` | First (leftmost) match as a flat array: whole match then each capture; no match returns `0` | n | `1a` + captures | O(n) |
| `match-all` | `( str pat -- [ [whole cap…] … ] \| 0 )` | Every non-overlapping leftmost match, each a flat sub-array; a zero-width match advances one byte; no match returns `0` | n | `1a` per match + captures | O(n + m·g) |
| `replace` | `( str pat rep -- str' )` | Replace **all** matches; in `rep`, `&` or `\0` is the whole match, `\1`–`\9` a capture, `\&` and `\\` literals | n | `1o` + buffer growth | O(n) |
| `xml-escape` | `( str -- str' )` | strings.telic: `&` `<` `>` `'` to their XML entities, for element text and single-quoted attributes | 4n | 4 strings | O(n) |
| `basename` | `( path -- filename )` | strings.telic: the path's last component; a path with no `/` passes through | n | `1o` | O(n) |
| `split` | `( str pat -- [ piece… ] )` | Split `str` at each non-overlapping match of `pat`; the pieces are the gaps between matches, empty fields kept; no match → `[ str ]` | n | `1a` + pieces | O(n) |
| `substring` | `( str start end -- sub )` | Half-open **codepoint** range `[start, end)`; bounds-checked against the codepoint count | 2 + n | `1o` | O(n) |
| `byte-substring` | `( str start end -- sub )` | Half-open **byte** range `[start, end)`; bounds-checked. Pairs with byte offsets from `match`/`match-all` | 2 + k | `1o` | O(k), k = end − start |
| `char-at` | `( str index -- char )` | The one-character string at codepoint `index`; bounds-checked against the codepoint count | 2 + n | `1o` | O(n) |
| `codepoint-at` | `( str index -- code )` | The integer codepoint at codepoint `index`; bounds-checked | 2 + n | none | O(n) |
| `string>chars` | `( str -- [ char… ] )` | Array of one-character strings, one per codepoint | n | `1a` + `1o`/char | O(n) |
| `string>codepoints` | `( str -- [ code… ] )` | Array of integer codepoints, one per codepoint | n | `1a` | O(n) |
| `codepoint>char` | `( code -- char )` | One-character string for codepoint `code`; range-checked `[0, 0x10FFFF]` | 1 | `1o` | O(1) |
| `codepoints>string` | `( [ code… ] -- str )` | Encode each codepoint to UTF-8 and concatenate; per-element type- and range-checked | n | `1o` | O(n) |
| `trim` | `( str -- str' )` | Strip leading and trailing ASCII whitespace (`' ' \t \n \v \f \r`) | n | `1o` | O(n) |
| `upper-case` | `( str -- str' )` | A copy with the ASCII letters a–z raised to A–Z; every other byte passes through unchanged, UTF-8 sequences included, so non-ASCII letters (`é`, `ω`) keep their case — Unicode folding needs tables the runtime does not carry | n | `1o` | O(n) |
| `lower-case` | `( str -- str' )` | A copy with the ASCII letters A–Z lowered to a–z; every other byte passes through unchanged, UTF-8 sequences included, so non-ASCII letters keep their case | n | `1o` | O(n) |
| `join` | `( arr sep -- str )` | Concatenate the string elements of `arr` separated by `sep`; errors on a non-string element | 2 + total | `1o` | O(total) |
| `index-of` | `( str pat -- i )` | strings.telic: codepoint index of `pat`'s first regex match in `str`, or `-1` if none | n | `1a` + pieces | O(n) |
| `spaces` | `( k -- str )` | strings.telic: a string of k spaces | k | `1a` + `1o` | O(k) |
| `pad-left` | `( str width -- str' )` | strings.telic: s left-padded with spaces to width (unchanged when already that wide; codepoint widths) | n | `1a` + `1o` | O(n) |
| `pad-right` | `( str width -- str' )` | strings.telic: s right-padded with spaces to width (unchanged when already that wide; codepoint widths) | n | `1a` + `1o` | O(n) |
| `string>number` | `( str -- n \| null )` | Parse a decimal/float string to a float, ignoring surrounding whitespace; `null` if `str` is not entirely a number | n | none | O(n) |
| `edit-distance` | `( a b -- n )` | Edit distance between two strings over codepoints: insertions, deletions, substitutions, and adjacent transpositions each cost 1 (Levenshtein with transpositions — optimal string alignment); symmetric | n·m | none | O(n·m) |
| `format` | `( … template -- str )` | Fill `template`'s `{n}` (or `{n:spec}`) placeholders with the nth-from-top stack value, then drop exactly the referenced positions (unreferenced values stay); renders floats/strings/symbols/exacts/quantities. `{nl}` and `{tab}` emit a newline and a tab — string literals have no escapes, so format is where control characters come from. When stdout is a terminal, the ink directives `{black}` `{red}` `{green}` `{yellow}` `{blue}` `{magenta}` `{cyan}` `{white}` and `{bold}` `{dim}` emit the SGR escape styling the following text until `{plain}` reverts to plain ink; when it is not (piped, batch), they vanish, so redirected output carries no escape bytes. Only these directives substitute; other brace content is left literal | len + refs | `1o` | O(len) |

A placeholder may carry a format spec after a colon — `{n:spec}` — a printf-style mini-language controlling how the value renders. `spec` is optional flags (`-`, `+`, space, `#`, `0`), an optional field width, an optional `.precision`, and an optional conversion letter:

- `f` `e` `g` (and `F` `E` `G`) render the value as a float: `{0:.2f}` fixes the precision, `{0:8.2f}` also pads to a field width.
- `d` / `i` render it as an integer, truncated toward zero: `{0:04d}`.
- `x` / `X` render it as lowercase/uppercase hex, truncated toward zero: `{0:x}`, `{0:#06x}` (the `#` flag adds `0x`); a negative value errors.
- `s`, or no conversion letter, places the value's default rendering in a field: `{0:8}` right-justifies, `{0:-8}` left-justifies, `{0:.3}` truncates to three characters.

A float or integer conversion requires a float operand; a non-float operand, an unknown conversion letter, or trailing characters in the spec is an error. With no colon, `{n}` renders the value in its default form.

`match` finds the leftmost match anywhere in the subject; anchor with `^`/`$` (or `\A`/`\z`).

```forth match
"x=42" "(\w+)=(\d+)" match . cr
```
```output
[ "x=42" "x" "42" ]
```

```forth match-all
"a1 b2" "\w(\d)" match-all . cr
```
```output
[ [ "a1" "1" ]
  [ "b2" "2" ] ]
```

```forth replace
"hello world" "o" "0" replace . cr
```
```output
hell0 w0rld
```

```forth xml-escape
"a < b & c" xml-escape . cr
```
```output
a &lt; b &amp; c
```

```forth basename
"/usr/local/bin/telic" basename . cr
```
```output
telic
```

```forth split
"a,b,,c" "," split . cr
```
```output
[ "a" "b" "" "c" ]
```

```forth substring
"telic" 1 3 substring . cr
```
```output
el
```

```forth byte-substring
"héllo" 0 3 byte-substring . cr
```
```output
hé
```

```forth char-at
"héllo" 1 char-at . cr
```
```output
é
```

```forth codepoint-at
"A" 0 codepoint-at . cr
```
```output
65
```

```forth string>chars
"abc" string>chars . cr
```
```output
[ "a" "b" "c" ]
```

```forth string>codepoints
"AB" string>codepoints . cr
```
```output
[ 65 66 ]
```

```forth codepoint>char
9731 codepoint>char . cr
```
```output
☃
```

```forth codepoints>string
[ 87 111 87 ] codepoints>string . cr
```
```output
WoW
```

```forth trim
"  pad  " trim "|" + . cr
```
```output
pad|
```

```forth upper-case
"Hello, World!" upper-case . cr
"héllo" upper-case . cr
```
```output
HELLO, WORLD!
HéLLO
```

```forth lower-case
"Hello, World!" lower-case . cr
[ "Ann" "ANN" "ann" ] [: lower-case :] map array>set size . cr
```
```output
hello, world!
1
```

```forth join
[ "a" "b" "c" ] "-" join . cr
```
```output
a-b-c
```

```forth index-of
"hello" "l+" index-of . cr
```
```output
2
```

```forth spaces
3 spaces byte-size . cr
```
```output
3
```

```forth pad-left
"7" 3 pad-left "|" + . cr
```
```output
  7|
```

```forth pad-right
"7" 3 pad-right "|" + . cr
```
```output
7  |
```

```forth string>number
"3.5" string>number . "x" string>number null? . cr
```
```output
3.5 1
```

```forth edit-distance
"kitten" "sitting" edit-distance . cr
```
```output
3
```

```forth format
1 2 "{1} then {0}" format . cr
PI "{0:.2f}" format . cr
255 "{0:#x}" format . cr
"{bold}{red}alert{plain} normal" format . cr
```
```output
1 then 2
3.14
0xff
alert normal
```

---

## Sets

Sorted `Val` arrays with binary-search insertion; equality is structural. `+`/`*`/`-` on two sets are union/intersection/difference.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `[< v… ]` | `( -- set )` | Set literal; `[<` pushes a mark, `>]` gathers everything above it in one sort-and-dedup pass | n log n | `1o` + realloc | O(n log n) |
| `set` | `( v₀ … vₙ₋₁ n -- set )` | Gather the top n values into a new set | 2 + n log n | `1o` + reallocs | O(n log n) |
| `union` | `( set₁ set₂ -- set₃ )` | Union into a new set, merging the two sorted arrays | m+n | `1o` + reallocs | O(m+n) |
| `intersection` | `( set₁ set₂ -- set₃ )` | Intersection into a new set, merging the two sorted arrays | m+n | `1o` + reallocs | O(m+n) |
| `difference` | `( set₁ set₂ -- set₃ )` | set₁ − set₂ into a new set, merging the two sorted arrays | m+n | `1o` + reallocs | O(m+n) |
| `set-add!` | `( set v -- set )` | Insert v in sorted position if absent (dedups); leaves set on the stack | log n + n | reallocs | O(n) |
| `set-remove!` | `( set v -- set )` | Remove v if present (no-op if absent); leaves set on the stack | log n + n | none | O(n) |
| `in?` | `( members values -- mask/binary )` | datasets.telic: membership by binary search — a scalar `values` answers 1 when it is a member of `members` (a set, array, or vector; a dimensioned vector contributes quantities, so units reconcile); an array answers an n×1 mask, a vector a mask of its shape, each element 1 when a member; a NaN or `null` answers 0. `[ 10 20 ] vector prices in? where select-rows` keeps the rows at listed prices | log m per element | `1m(n)`; a non-set `members` adds `1a(m)` + `1o`; a dimensioned `values` adds `2a(n)` | O(n log m), plus O(m log m) to build the set |
| `array>set` | `( array -- set )` | Sort a copy of the array and dedup into a set; the source array is unchanged | n log n | `1o` + realloc | O(n log n) |
| `set>array` | `( set -- arr )` | arrays.telic: the elements as an array in sorted order | 1 | `1o` | O(n) |
| `group-by` | `( array key -- frame )` | arrays.telic: group elements into a frame from each symbol to the set of elements under it. The key's type chooses the path: a symbol names a field read from each element frame, grouped in one sorted pass in C; an execution token `( element -- sym )` computes each element's group symbol | symbol n log n; xt n·(xt + log n) | frame + sets | symbol O(n log n); xt O(n·xt + n log n) |
| `size` | `( coll -- n )` | Element count: set/array members, **codepoints** of a string, pair count of a frame; a string's codepoint count is computed on first use and memoized on the object | 2 | none | O(1); a string's first `size` is O(n) |
| `byte-size` | `( str -- n )` | Byte length of a string | 2 | none | O(1) |

```forth set
10 20 20 3 set . cr
```
```output
[< 10 20 >]
```

```forth union
[< 1 2 >] [< 2 3 >] union . cr
```
```output
[< 1 2 3 >]
```

```forth intersection
[< 1 2 3 >] [< 2 3 4 >] intersection . cr
```
```output
[< 2 3 >]
```

```forth difference
[< 1 2 3 >] [< 2 >] difference . cr
```
```output
[< 1 3 >]
```

```forth set-add!
[< 1 3 >] 2 set-add! . cr
```
```output
[< 1 2 3 >]
```

```forth set-remove!
[< 1 2 3 >] 2 set-remove! . cr
```
```output
[< 1 3 >]
```

```forth in?
[< 1 2 3 >] 2 in? . [< 1 2 3 >] 9 in? . cr
[ 2 4 ] vector [ 1 2 3 4 ] vector in? matrix>array . cr
[< "b" "c" >] [ "a" "b" ] in? matrix>array . cr
```
```output
1 0
[ 0 1 0 1 ]
[ 0 1 ]
```

```forth array>set
[ 3 1 3 2 ] array>set . cr
```
```output
[< 1 2 3 >]
```

```forth set>array
[< 3 1 2 >] set>array . cr
```
```output
[ 1 2 3 ]
```

```forth group-by
[ { :name "ann" :team :red } { :name "bo" :team :blue } { :name "cy" :team :red } ] :team group-by /red @ size . cr
[ 1 2 3 4 ] [: 2 mod 0= if :even else :odd then :] group-by frame>array . cr
```
```output
2
[ :even [< 2 4 >] :odd [< 1 3 >] ]
```

```forth size
[ 1 2 3 ] size . "héllo" size . { :a 1 } size . cr
```
```output
3 5 1
```

```forth byte-size
"héllo" byte-size . cr
```
```output
6
```

---

## Arrays

0-indexed, elements of any type. Grows at the end in amortized O(1) over a backing buffer that doubles on demand; indexing stays O(1).

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `[ v… ]` | `( -- arr )` | Array literal; `[` marks, `]` gathers above the mark | n | `1a(n)` | O(n) |
| `array` | `( v₀ … vₙ₋₁ n -- arr )` | Gather the top n values into an array | 2 + n | `1a(n)` | O(n) |
| `array-of` | `( val n -- arr )` | New n-element array, every slot = val | 3 + n | `1a(n)` | O(n) |
| `@i` | `( arr i -- val )` | Array element; on a matrix returns row i as a 1×c matrix | 3 (array) | matrix `1m(1×c)` | O(1) array; O(c) matrix |
| `!i` | `( arr val i -- arr )` | Store val at index i in place; leaves arr on the stack | 4 | none | O(1) |
| `add-last!` | `( arr v -- arr )` | Append v at the end, doubling the backing buffer when full; leaves arr on the stack | 2 | `≤1a` on grow | amortized O(1) |
| `remove-last!` | `( arr -- v )` | Remove and return the last element; errors on an empty array | 2 | none | O(1) |
| `take` | `( arr/set n -- arr )` | First n elements (clamped) | 2 + n | `1a(n)` | O(n) |
| `reverse` | `( arr/set -- arr )` | Reversed copy | 1 + n | `1a(n)` | O(n) |
| `concat` | `( arr/set arr/set -- arr )` | Concatenated copy | 2 + m + n | `1a(m+n)` | O(m+n) |
| `range` | `( from to -- arr )` | Inclusive integer range, step ±1 | 3 + n | `1a(n)` | O(n) |
| `spread` | `( arr/set/fr -- v… )` | Spread the elements onto the stack; a frame spreads alternating sym/value | 1 + n | none | O(n) |
| `slice!` | `( src sstart sstep slen arr tstart -- arr )` | Copy `slen` elements `src[sstart], src[sstart+sstep], …` into `arr[tstart…]` in place | 6 + slen | self-overlap may malloc slen | O(slen) |
| `to-slice!` | `( v₀ … vₙ₋₁ n arr offset -- arr )` | Store the n values under their count into `arr[offset…offset+n)`; leaves arr | 2 + n | none | O(n) |
| `nlast` | `( arr/matrix n -- arr/vector )` | arrays.telic: the last n elements, n clamped to [0, length]; a matrix answers its last n row-major elements as an n×1 vector | 3n | 3×`1a(n)`; matrix `2m` | O(n) |
| `first` | `( arr/matrix -- v )` | core.telic: element 0 of an array, or the first row-major element of a matrix (as a float) | 9 | none | O(1) |
| `last` | `( arr/matrix -- v )` | core.telic: the final element of an array, or the last row-major element of a matrix (as a float); empty array errors | 9 | none | O(1) |
| `second` | `( arr -- v )` | core.telic: element 1 of an array | 3 | none | O(1) |
| `assoc` | `( entries key -- value )` | core.telic: the value paired with `key`, the first match by structural equality (so tuple arrays, frames and pairs serve as keys, `1` and `"1"` are distinct, quantities match within a dimension, `null` is a key). The entries are `[ key value ]` arrays, `first` the key and `second` the value, as `count`, `group-indices` and `split-by` answer them. A missing key answers `null`, as a stored `null` value does | 3 + entries scanned | none | O(n) |
| `skip` | `( arr n -- arr )` | arrays.telic: all but the first n elements | 3n | 3×`1a(n)` | O(n) |
| `sort` | `( arr/set/v -- arr/v )` | Sorted copy: an array orders ascending in natural order; a set projects its already-ordered elements to an array; an nx1 or 1xn vector sorts ascending with NaNs last (other matrix shapes error) | 1 + n log n | `1a(n)` / `1m(n)` | O(n log n); vectors above 8k elements O(n) radix |
| `flatten` | `( arr/mat -- arr/mat )` | arrays.telic: one dimension. An array answers its elements with every nested array spliced in, recursively; a flat array answers itself unchanged. A matrix answers the same elements as a 1×(r·c) row | 1 + m | `1a(m)`; matrix `1m(1×r·c)` | O(m) |
| `sample` | `( arr/set count repl -- arr )` | Draw `count` elements; `repl` truthy = with replacement, else without (count ≤ len) | 3 + n | `1a(count)` (+ `malloc(n)` without replacement) | O(n) |
| `shuffle` | `( arr -- arr )` | datasets.telic: new array, elements uniformly permuted; input untouched | 3 + n | as `sample` | O(n) |
| `resample` | `( arr/set -- arr )` | datasets.telic: same-size draw with replacement (the bootstrap draw); input untouched | 3 + n | `1a(n)` | O(n) |
| `iota` | `( n -- arr )` | arrays.telic: `[0…n−1]`, empty when n ≤ 0 | 3 + n | `1a(n)` | O(n) |

```forth array
1 2 3 3 array . cr
```
```output
[ 1 2 3 ]
```

```forth array-of
0 4 array-of . cr
```
```output
[ 0 0 0 0 ]
```

```forth @i
[ 10 20 30 ] 1 @i . cr
```
```output
20
```

```forth !i
[ 1 2 3 ] 99 1 !i . cr
```
```output
[ 1 99 3 ]
```

```forth add-last!
[ 1 2 ] 3 add-last! . cr
```
```output
[ 1 2 3 ]
```

```forth remove-last!
[ 1 2 3 ] remove-last! . cr
```
```output
3
```

```forth take
[ 1 2 3 4 ] 2 take . cr
```
```output
[ 1 2 ]
```

```forth reverse
[ 1 2 3 ] reverse . cr
```
```output
[ 3 2 1 ]
```

```forth concat
[ 1 2 ] [ 3 4 ] concat . cr
```
```output
[ 1 2 3 4 ]
```

```forth range
3 7 range . cr
```
```output
[ 3 4 5 6 7 ]
```

```forth spread
[ 1 2 3 ] spread . . . cr
```
```output
3 2 1
```

```forth slice!
[ 10 20 30 ] 0 1 3 [ 0 0 0 0 0 ] 1 slice! . cr
```
```output
[ 0 10 20 30 0 ]
```

```forth to-slice!
7 8 2 [ 0 0 0 0 ] 1 to-slice! . cr
```
```output
[ 0 7 8 0 ]
```

```forth nlast
[ 1 2 3 4 ] 2 nlast . [ 1 2 3 4 ] 2 2 matrix 3 nlast transpose matrix>array . cr
```
```output
[ 3 4 ] [ 2 3 4 ]
```

```forth first
[ 7 8 9 ] first . [ 1 2 3 4 ] 2 2 matrix first . cr
```
```output
7 1
```

```forth last
[ 7 8 9 ] last . [ 1 2 3 4 ] 2 2 matrix last . cr
```
```output
9 4
```

```forth second
[ 7 8 9 ] second . cr
```
```output
8
```

```forth assoc
[ [ "a" 1 ] [ "b" 2 ] ] "b" assoc . cr
[ [ :x 10 ] [ :y 20 ] ] :z assoc . cr
```
```output
2
null
```

```forth skip
[ 1 2 3 4 ] 1 skip . cr
```
```output
[ 2 3 4 ]
```

```forth sort
[ 3 1 2 ] sort . cr
```
```output
[ 1 2 3 ]
```

```forth flatten
[ [ 1 2 ] [ 3 [ 4 ] ] ] flatten . cr
[ 1 2 3 4 ] 2 2 matrix flatten dim swap . . cr
```
```output
[ 1 2 3 4 ]
1 4
```

```forth sample
42 seed [ 1 2 3 4 5 ] 3 false sample . cr
```
```output
[ 3 4 5 ]
```

```forth shuffle
42 seed [ 1 2 3 4 5 ] shuffle . cr
```
```output
[ 3 4 5 1 2 ]
```

```forth resample
42 seed [ 1 2 3 ] resample . cr
```
```output
[ 1 1 3 ]
```

```forth iota
4 iota . cr
```
```output
[ 0 1 2 3 ]
```

---

## Frames

Symbol-keyed sorted maps; binary-search lookup. A **path** is an array of steps; a plain *locator* is all symbols, and the literal `/a/b/c` is a compile-time constant array that allocates nothing at run time. A path may instead be a **search path** matching a set of nodes (see Path queries below). The single-target words (`@`, `!`, `delete-at`, `update-at`) require a locator and reject a search path (the error names `select-keys`/`select-values`); `has?` accepts either. `d` = path depth, `n` = frame size.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `{ :k v … }` | `( -- fr )` | Frame literal from alternating key/value pairs above the `{` mark; a path key (`/a/b/c`) vivifies nested frames | n·(log n + n) | `1o` + reallocs | O(n²) |
| `frame` | `( keys values -- fr )` | Build from parallel key and value arrays of equal length | 2 + n log n | `1o` + reallocs | O(n log n) |
| `array>frame` | `( arr -- fr )` | Build from an even-length alternating-kv array; a path key (`/a/b/c`) vivifies nested frames | 1 + n log n | `1o` + reallocs | O(n log n) |
| `frame>array` | `( fr -- arr )` | Flatten to an alternating-kv array in the frame's own key order (symbol id, i.e. interning order, not alphabetical) | 1 + n | `1o` | O(n) |
| `@` | `( fr sym/path -- val )` | Get by key or path; errors if absent or if the path is a search path | 3 + d log n | none | O(d log n) |
| `name@key` | `( -- val )` | Get in one token: the part left of `@` is a local, else a defined word, and it supplies the frame; `key` is interned at read time and compiled as the operand of one op, so `row@price` is a local fetch plus `(@key)` with no symbol on the stack. Chains left to right — `row@address@city` walks two frames. An empty left part takes the frame from the stack, `( fr -- val )`, so `@price` is the postfix form. Errors when the key is absent or the value is not a frame. The rule applies only to tokens that are not defined words, so `@i`, `@or` and any word named with an `@` keep their meanings, and defining `row@total` shadows the token form for that spelling | 2 + log n | none | O(log n) |
| `name!key` | `( val -- )` | Set in one token, dropping the frame `!` returns: `99 row!price` stores 99 at `:price` in `row`'s frame and leaves the stack empty. The left part resolves as a local, else a defined word, supplying the frame; an empty left part takes the frame from above the value, `( val fr -- )`. A chain may end in a set — `row@address!city` — but only its last step may, since a set leaves no frame to walk | 2 + log n | none | O(log n) |
| `@or` | `( fr sym/path fallback -- val )` | Get by key or path, the fallback when absent in one probe, no error on miss; the fallback is already evaluated, so it suits values, not expensive computations | 4 + d log n | none | O(d log n) |
| `!` | `( fr val sym/path -- fr )` | Set by key or path, vivifying intermediates; mutates fr; errors on a search path. Writes take the address last, so the value is computed first and the destination named beside the word | d log n | realloc on growth; `1o` per vivified frame | O(d log n) amortized |
| `has?` | `( fr sym/path -- bool )` | Existence test for a frame key or path, no error on miss; a search path is true if any node matches (short-circuits at the first); on a set `( set v -- bool )`, membership by binary search in natural order (`in?` is the mask-producing form); on a string `( str pat -- bool )`, true if regex `pat` matches anywhere | 3 + d log n | none | O(d log n) |
| `delete-at` | `( fr sym/path -- fr )` | Remove a key (errors if absent or on a search path); mutates fr | n | none | O(n) |
| `rename-key!` | `( fr old new -- fr )` | core.telic: move the value at key `old` to key `new` in place and leave fr — a dataset column renames the same way (`ds :price :cost rename-key!`); `old` absent errors, an existing `new` is overwritten; keys stay in symbol-id order | 2n | none | O(n) |
| `update-at` | `( fr xt sym/path -- fr )` | Apply xt to the value at the key, store the result back; errors on a search path | d log n + xt | none | O(d log n + xt) |
| `keys` | `( fr -- arr )` | The keys as an array of symbols, in the frame's storage order (by symbol id, the order of first interning); parallel to `values`, so `frame` rebuilds the frame from the two | 1 + n | `1a(n)` | O(n) |
| `key-set` | `( fr -- set )` | The keys as a set of symbols, for membership tests and set algebra against other key sets | 1 + n log n | `1o` | O(n log n) |
| `values` | `( fr -- arr )` | Values in key order | 1 + n | `1a(n)` | O(n) |
| `merge` | `( fr₁ fr₂ -- fr )` | New frame with all keys; fr₂ wins collisions | m+n | `1o` | O(m+n) |
| `copy` | `( a -- a' )` | Deep copy of any value: dereferences bound logic vars to their values and gives each unbound var a fresh shared var; recurses into frames, arrays, matrices, strings, sets, continuations; identity for scalars. Defined generally, not frame-specific. | tree size | one object per node | O(tree size) |
| `reify` | `( a -- a' )` | Deep copy of any value, dereferencing bound logic vars and recursing into frames, arrays, matrices, strings, sets, continuations, but each unbound var becomes a canonical inert symbol `:_0`, `:_1`, … numbered by first appearance — a ground, storable, comparable snapshot. | tree size | one object per node | O(tree size) |

```forth frame
[ :a :b ] [ 1 2 ] frame frame>array . cr
```
```output
[ :a 1 :b 2 ]
```

```forth array>frame
[ :x 1 :y 2 ] array>frame frame>array . cr
```
```output
[ :x 1 :y 2 ]
```

```forth frame>array
{ :a 1 :b 2 } frame>array . cr
```
```output
[ :a 1 :b 2 ]
```

```forth @
{ :a { :b 5 } } /a/b @ . cr
```
```output
5
```

```forth name@key
: price-of row | row@price ; { :price 9 } price-of . cr
```
```output
9
```

```forth name!key
: mark-sold row | 0 row!price row ; { :price 9 } mark-sold frame>array . cr
```
```output
[ :price 0 ]
```

```forth @or
{ :a 1 } :b 99 @or . cr
```
```output
99
```

```forth !
{ } 5 /a/b ! /a/b @ . cr
```
```output
5
```

```forth has?
{ :a 1 } :a has? . { :a 1 } :b has? . [< 1 2 >] 2 has? . "abc" "b+" has? . cr
```
```output
1 0 1 1
```

```forth delete-at
{ :a 1 :b 2 } :a delete-at frame>array . cr
```
```output
[ :b 2 ]
```

```forth rename-key!
{ :a 1 :b 2 } :a :x rename-key! frame>array . cr
```
```output
[ :b 2 :x 1 ]
```

```forth update-at
{ :n 10 } ' 1+ :n update-at frame>array . cr
```
```output
[ :n 11 ]
```

```forth keys
{ :a 1 :b 2 } keys . cr
```
```output
[ :a :b ]
```

```forth key-set
{ :b 2 :a 1 } key-set dup :a in? . [< :a :c >] intersection . cr
```
```output
1 [< :a >]
```

```forth values
{ :a 1 :b 2 } values . cr
```
```output
[ 1 2 ]
```

```forth merge
{ :a 1 :b 2 } { :b 20 :c 30 } merge frame>array . cr
```
```output
[ :a 1 :b 20 :c 30 ]
```

```forth copy
{ :a 1 } dup copy 2 :a ! drop :a @ . cr
```
```output
1
```

```forth reify
[ lvar lvar 5 ] reify . cr
```
```output
[ :_0 :_1 5 ]
```

### Path queries

A search path generalizes a locator with three step kinds, matching a set of nodes instead of one. Descent is through nested frames only; an array, set, or scalar is a leaf, and `//` is depth-capped against cycles.

- `*` — any one child at this level.
- `//` — descendant-or-self: any depth at or below the current node.
- `[…]` — a predicate filtering the current node: `[k]` (key `k` exists), `[k=v]`, `[k<v]`, `[k>v]` (compare key `k`'s value to `v`), `[.=v]`/`[.<v]`/`[.>v]` (compare the node itself, via `.`), or `[a/b op v]` (a sub-path subject). Several predicates on one step chain: `[role=admin][age>45]`.

So `/users/*/name` is the `:name` of every child of `:users`, `/root//city` is every `:city` at any depth, and `/people/*[age>30]` filters by predicate. `s` = nodes visited.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `select-values` | `( fr path -- arr )` | Every matched value, in document (pre-order) order, duplicates kept | s | `1a` + reallocs | O(s) |
| `select-keys` | `( fr path -- arr )` | The full root-to-match path (a symbol array) for every match, document order | s | `1a` + `1a` per match | O(s + total path length) |

`select-values` is the cheaper word (it captures the node directly, no per-match path array); `array>set` the result when distinct values are wanted, or hand the array to `choose` as backtracking choice points.

```forth select-values
{ :a { :n 1 } :b { :n 2 } } /*/n select-values . cr
```
```output
[ 1 2 ]
```

```forth select-keys
{ :a { :n 1 } :b { :n 2 } } //n select-keys . cr
```
```output
[ [ :a :n ]
  [ :b :n ] ]
```

---

## JSON

Objects ↔ frames (keys interned as symbols), arrays ↔ arrays, strings ↔ strings, numbers ↔ floats, except that an integer too large for a float to hold exactly reads as an integer exact and an integer exact writes as its digits (a non-integer exact errors on write). JSON `true`/`false` ↔ the reserved `:1`/`:0` symbols; `null` ↔ `null`.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `json>frame` | `( str -- val )` | Parse a JSON string. Escapes and `\uXXXX` (with surrogate pairs) decode to UTF-8; depth-guarded; rejects trailing non-whitespace. Each object's keys are sorted after collection | scan + build | one object per node | O(\|s\| log \|s\|) |
| `frame>json` | `( val -- str )` | Serialize a value to JSON. Floats use the shortest round-trip form; strings are escaped (non-ASCII emitted raw); object keys are the symbol names | walk + build | `1o` string | O(tree size) |
| `null` | `( -- null )` | Push `null` — what JSON `null` parses to, and what an unset `env` returns | 1 | none | O(1) |

```forth json>frame
"{""a"": [1, 2]}" json>frame /a @ . cr
```
```output
[ 1 2 ]
```

```forth frame>json
{ :a 1 } frame>json . cr
```
```output
{"a": 1}
```

```forth null
null . null null? . cr
```
```output
null 1
```

---

## Value serialization

A whole value graph as bytes and back: matrices, datasets, regression
models — anything built from floats, strings, symbols, `null`, `_`,
arrays, sets, frames, pairs, matrices, segments, exacts, complexes, and
quantities, nested to any depth. Shared substructure is written once and
referenced, so an object reachable twice comes back as one object and a cyclic
frame round-trips. A bound logic variable travels as its value.

Refused, each naming the type: an execution token or curried token (compiled
code, and an anonymous quotation has no name to resolve), a continuation, a
stream, a database handle, a C pointer, and an unbound logic variable. A
model held behind the FFI is refused for the same reason — `xgb-save` writes
those in XGBoost's own format.

A quantity travels by unit name and base-dimension names, so it reloads in a
session that never declared the unit: missing dimensions and the unit are
declared on the spot. A name already taken by an ordinary word errors rather
than redefining it, so loading data cannot change what a word means.

The bytes carry a magic and a version; unknown bytes and truncated input error
rather than answering a damaged value. Doubles and counts are little-endian.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `value>bytes` | `( v -- str )` | Serialize a value graph to a byte string | n | `1o` + buffer growth | O(n) |
| `bytes>value` | `( str -- v )` | Rebuild the value a `value>bytes` string holds; errors on damaged or truncated data | n | one object per node | O(n) |
| `save-value` | `( v path -- )` | io.telic: serialize a value graph and write it to the file at `path` | n + file write | as `value>bytes` | O(n) |
| `load-value` | `( path -- v )` | io.telic: read the file at `path` and rebuild the value it holds | file read + n | as `bytes>value` | O(n) |

```forth value>bytes
[ 1 2 3 4 ] 2 2 matrix value>bytes bytes>value matrix>array . cr
```
```output
[ 1 2 3 4 ]
```

```forth bytes>value
{ :name "ann" :scores [ 1 2 3 ] vector } value>bytes bytes>value :scores @ mean . cr
```
```output
2
```

```forth save-value
{ :estimates [ 1.5 -0.25 ] vector :meta { :n-rows 4 } } "/tmp/docs-model.bin" save-value
"/tmp/docs-model.bin" load-value :estimates @ matrix>array . cr
```
```output
[ 1.5 -0.25 ]
```

```forth load-value
[< 1 2 3 >] "/tmp/docs-set.bin" save-value "/tmp/docs-set.bin" load-value . cr
```
```output
[< 1 2 3 >]
```

---

## Matrices

Row-major `double` storage. `r` rows, `c` columns.

### Construction

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `0-matrix` | `( r c -- mat )` | r×c zero matrix (calloc) | 3 | `1m(r×c)` | O(1)+ |
| `matrix` | `( arr r c -- mat )` or `( arr r -- mat )` | Build from a float array; two-arg form takes r = rows and infers columns | 3 + r×c | `1m(r×c)` | O(r×c) |
| `vector` | `( arr -- v )` | matrix.telic: the array as an nx1 matrix, length inferred | 3 + n | `1m(n)` | O(n) |
| `diagonal-matrix` | `( fill n -- mat )` | n×n matrix with `fill` on the diagonal | 2 + n | `1m(n×n)` | O(n) |
| `identity-matrix` | `( n -- mat )` | matrix.telic: n×n matrix with 1 on the diagonal | n | `1m(n×n)` | O(n) |
| `matrix-range` | `( start end step -- mat )` | 1×N row of evenly spaced values | 3 + N | `1m(1×N)` | O(N) |

```forth 0-matrix
2 3 0-matrix dim swap . . cr
```
```output
2 3
```

```forth matrix
[ 1 2 3 4 ] 2 2 matrix render print cr
```
```output
<matrix 2x2>
          1          2
          3          4
```

```forth vector
[ 1 2 3 ] vector dim swap . . cr
```
```output
3 1
```

```forth diagonal-matrix
7 2 diagonal-matrix render print cr
```
```output
<matrix 2x2>
          7          0
          0          7
```

```forth identity-matrix
2 identity-matrix render print cr
```
```output
<matrix 2x2>
          1          0
          0          1
```

```forth matrix-range
0 1 0.25 matrix-range matrix>array . cr
```
```output
[ 0 0.25 0.5 0.75 1 ]
```

### Shape and indexing

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `@j` | `( mat j -- col )` | Column j as an r×1 matrix (copy) | 2 + r | `1m(r×1)` | O(r) |
| `@i,j` | `( mat i j -- f )` | Single element as a float | 4 | none | O(1) |
| `@e` | `( mat i -- f )` | Element at flat row-major index i as a float; the same access on n×1 and 1×n vectors | 3 | none | O(1) |
| `!e` | `( mat v i -- mat )` | Store v (a float, or `null` for NaN) at flat row-major index i, in place | 4 | none | O(1) |
| `!i,j` | `( mat v i j -- mat )` | Store v (a float, or `null` for NaN) at row i, column j, in place | 5 | none | O(1) |
| `dim` | `( mat/dataset -- r c )` | Push rows then columns; datasets.telic extends it to a dataset — rows from the first column's length, columns from the key count | 3 | none | O(1) |
| `reshape` | `( mat r c -- mat' )` | Same elements, new shape (must match); memcpy | 3 + r×c | `1m(r×c)` | O(r×c) |
| `transpose` | `( mat -- mat' )` | Rows/columns swapped | 1 + r×c | `1m(c×r)` | O(r×c) |
| `diagonal` | `( mat -- mat' )` | Diagonal as a 1×min(r,c) matrix | 1 + min(r,c) | `1m(1×min)` | O(min(r,c)) |
| `as-column` | `( v -- v' )` | matrix.telic: any vector shape as n×1 | r×c | `1m(n×1)` | O(n) |
| `matrix>array` | `( mat -- arr )` | The elements as an array in row-major order: floats from a bare matrix; a dimensioned matrix yields one quantity per element in its unit; a NaN element becomes `null` either way | 1 + r×c | `1a(r×c)`; dimensioned + 1 pair per non-NaN element | O(r×c) |
| `num-elements` | `( mat -- n )` | matrix.telic: the element count, rows × columns | 5 | none | O(1) |
| `n-rows` | `( mat/dataset -- n )` | datasets.telic: the row count | 6 | none | O(1) |
| `n-columns` | `( mat/dataset -- n )` | datasets.telic: the column count | 8 | none | O(1) |

```forth @j
[ 1 2 3 4 ] 2 2 matrix 1 @j matrix>array . cr
```
```output
[ 2 4 ]
```

```forth @i,j
[ 1 2 3 4 ] 2 2 matrix 1 0 @i,j . cr
```
```output
3
```

```forth @e
[ 1 2 3 4 ] 2 2 matrix 2 @e . cr
```
```output
3
```

```forth !e
[ 1 2 3 4 ] 2 2 matrix 99 0 !e matrix>array . cr
```
```output
[ 99 2 3 4 ]
```

```forth !i,j
[ 1 2 3 4 ] 2 2 matrix 99 1 1 !i,j matrix>array . cr
```
```output
[ 1 2 3 99 ]
```

```forth dim
[ 1 2 3 4 5 6 ] 2 3 matrix dim swap . . cr
```
```output
2 3
```

```forth reshape
[ 1 2 3 4 5 6 ] 2 3 matrix 3 2 reshape dim swap . . cr
```
```output
3 2
```

```forth transpose
[ 1 2 3 4 ] 2 2 matrix transpose matrix>array . cr
```
```output
[ 1 3 2 4 ]
```

```forth diagonal
[ 1 2 3 4 ] 2 2 matrix diagonal matrix>array . cr
```
```output
[ 1 4 ]
```

```forth as-column
[ 1 2 3 ] 1 3 matrix as-column dim swap . . cr
```
```output
3 1
```

```forth matrix>array
[ 1 2 3 4 ] 2 2 matrix matrix>array . cr
```
```output
[ 1 2 3 4 ]
```

```forth num-elements
[ 1 2 3 4 5 6 ] 2 3 matrix num-elements . cr
```
```output
6
```

```forth n-rows
[ 1 2 3 4 5 6 ] 2 3 matrix n-rows . cr
```
```output
2
```

```forth n-columns
[ 1 2 3 4 5 6 ] 2 3 matrix n-columns . cr
```
```output
3
```

### Multiplication and reductions

Matrix multiply is `matmul`; `*` on matrices is element-wise. The general
`αAB + βC` form is `dgemm`, which the statistics library supplies through the
platform BLAS (`"statistics" load-library`).

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `matmul` | `( A B -- A·B )` | The matrix product; errors unless A's columns match B's rows. Loading the statistics library replaces it with a version that computes small operands directly and hands anything above 24×24 of work (rows × inner × columns) to cblas dgemm, which is faster there and slower below it | m·k·n | `1m(m×n)` | O(m·k·n) |
| `sum` | `( mat -- f )` | Sum of all elements | 1 + r×c | none | O(r×c) |
| `max` | `( mat -- f )` | Maximum element | 1 + r×c | none | O(r×c) |
| `min` | `( mat -- f )` | Minimum element | 1 + r×c | none | O(r×c) |
| `argmax` | `( mat -- f )` | Flat row-major index of the maximum element (first on ties) | 1 + r×c | none | O(r×c) |
| `argmin` | `( mat -- f )` | Flat row-major index of the minimum element (first on ties) | 1 + r×c | none | O(r×c) |
| `row-sums` | `( mat -- mat' )` | r×1 of per-row sums | 1 + r×c | `1m(r×1)` | O(r×c) |
| `row-maxes` | `( mat -- mat' )` | r×1 of per-row maxima | 1 + r×c | `1m(r×1)` | O(r×c) |
| `row-mins` | `( mat -- mat' )` | r×1 of per-row minima | 1 + r×c | `1m(r×1)` | O(r×c) |
| `column-sums` | `( mat -- mat' )` | 1×c of per-column sums | 1 + r×c | `1m(1×c)` | O(r×c) |
| `column-maxes` | `( mat -- mat' )` | 1×c of per-column maxima | 1 + r×c | `1m(1×c)` | O(r×c) |
| `column-mins` | `( mat -- mat' )` | 1×c of per-column minima | 1 + r×c | `1m(1×c)` | O(r×c) |
| `mean` | `( mat -- f )` | matrix.telic: the arithmetic mean of the elements, NaNs skipped | r×c | none | O(r×c) |
| `row-means` | `( mat -- mat' )` | matrix.telic: r×1 of per-row means | r×c | 2×`1m(r×1)` | O(r×c) |
| `column-means` | `( mat -- mat' )` | matrix.telic: 1×c of per-column means | r×c | 2×`1m(1×c)` | O(r×c) |
| `successive-differences` | `( v -- differences )` | matrix.telic: `v[i+1] − v[i]` over an n×1 vector as an (n−1)×1 vector; one element answers 0×1; a matrix with more than one column errors | 2n | 3×`1m(n×1)` | O(n) |

```forth matmul
[ 1 2 3 4 5 6 ] 2 3 matrix [ 7 8 9 10 11 12 ] 3 2 matrix matmul matrix>array . cr
```
```output
[ 58 64 139 154 ]
```

```forth sum
[ 1 2 3 4 ] 2 2 matrix sum . cr
```
```output
10
```

```forth max
[ 1 2 3 4 ] 2 2 matrix max . cr
```
```output
4
```

```forth min
[ 1 2 3 4 ] 2 2 matrix min . cr
```
```output
1
```

```forth argmax
[ 3 9 4 1 ] vector argmax . cr
```
```output
1
```

```forth argmin
[ 3 9 4 1 ] vector argmin . cr
```
```output
3
```

```forth row-sums
[ 1 2 3 4 ] 2 2 matrix row-sums matrix>array . cr
```
```output
[ 3 7 ]
```

```forth row-maxes
[ 1 2 3 4 ] 2 2 matrix row-maxes matrix>array . cr
```
```output
[ 2 4 ]
```

```forth row-mins
[ 1 2 3 4 ] 2 2 matrix row-mins matrix>array . cr
```
```output
[ 1 3 ]
```

```forth column-sums
[ 1 2 3 4 ] 2 2 matrix column-sums matrix>array . cr
```
```output
[ 4 6 ]
```

```forth column-maxes
[ 1 2 3 4 ] 2 2 matrix column-maxes matrix>array . cr
```
```output
[ 3 4 ]
```

```forth column-mins
[ 1 2 3 4 ] 2 2 matrix column-mins matrix>array . cr
```
```output
[ 1 2 ]
```

```forth mean
[ 2 4 6 ] vector mean . cr
```
```output
4
```

```forth row-means
[ 1 2 3 4 ] 2 2 matrix row-means matrix>array . cr
```
```output
[ 1.5 3.5 ]
```

```forth column-means
[ 1 2 3 4 ] 2 2 matrix column-means matrix>array . cr
```
```output
[ 2 3 ]
```

```forth successive-differences
[ 1 4 9 16 ] vector successive-differences matrix>array . cr
```
```output
[ 3 5 7 ]
```

### Reshaping, selection, statistics

The statistics treat NaN elements as missing values and skip them: `sum` and
`norm` count nothing missing as 0; `mean` `var` `std` `se` `min` `max`
`quantile` `median` `percentile` `iqr` `ci` `argmax` `argmin` error with "all
elements are NaN (missing)" (`var`: "needs at least 2 non-NaN elements") when
too little remains; the correlations delete row i from both vectors when
either element i is NaN, and error below 2 complete pairs; `regress-with`
deletes incomplete rows of its design matrix before fitting. The positional
operations (`cumulative-sum`, the row/column reductions, `dot`) keep NaN in
place.

`c` below is the output column count; `k` the index count; `n = r×c`.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `augment` | `( a b -- mat )` | Concatenate two matrices column-wise; errors unless row counts match | 2 + r·c | `1m(r×c)` | O(r·c) |
| `vstack` | `( a b -- mat/dataset )` | Stack two matrices row-wise (a on top of b); errors unless column counts match. datasets.telic extends it to two datasets: identical column sets required, each column concatenated top-then-bottom and re-inferred (a numeric column stays a vector); differing columns error. A `null` operand answers the other, so `blocks null ' vstack reduce` folds a list of blocks | 2 + r·c | `1m(r×c)`; dataset one column each | O(r·c) |
| `hstack` | `( a b -- mat/dataset )` | matrix.telic: concatenate two matrices column-wise; errors unless row counts match. datasets.telic extends it to two datasets set side by side: equal row counts and disjoint column names required (both error otherwise), rows aligned by position, the columns of both retained. A `null` operand answers the other, so `columns null ' hstack reduce` folds a list of columns | 2 + r·c | `1m(r×c)`; dataset shares columns | O(r·c) |
| `submatrix` | `( mat rs re cs ce -- mat )` | Copy the half-open block rows [rs,re) × cols [cs,ce); errors out of bounds or start > end | 5 + r·c | `1m(r×c)` | O(r·c) |
| `select-rows` | `( mat/dataset/arr idx -- same )` | New matrix of the rows named by `idx` — a float index array or an index vector (nx1 or 1xn); a dimensioned matrix keeps its unit; errors on a non-float or out-of-range index. datasets.telic extends it to a dataset (every column gathered by the same indices, matrix and array columns alike) and to a bare array (elements gathered by index) | 2 + k·c | `1m(k×c)`; dataset one column each; array `1a(k)` | O(k·c) |
| `mesh` | `( v mask b -- v' )` | Masked substitution: element i of the result is `b`'s where `mask[i]` is a definite nonzero, `v`'s where it is 0 **or NaN** (an unknown mask cell changes nothing). `v` is a matrix, dimensioned matrix, or array; the mask a bare matrix of `v`'s shape (element count, for an array). `b` is shape-matched same-representation, or broadcasts: a float, `null` (→ NaN), a quantity, or — for an array subject — any single value. Units reconcile as `+`: `b` rescales into `v`'s unit, which the result keeps; a quantity against a bare number errors. Conditional-mutate idioms: `dup nan? 0 mesh` fills NaNs, `dup -1 eq null mesh` turns a sentinel into NaN, `dup 100 > 100 mesh` caps at 100 | 3 + n | `1m(r×c)` / `1a(n)` | O(n) |
| `argsort` | `( v -- v' )` or `( arr -- arr )` | The sorting permutation of a vector, shape preserved: element i is the source index of the i-th smallest value; ties keep index order, NaNs go last in index order. An array operand answers the permutation under natural (structural) order, so mixed types order, ties in index order, as a float-index array | 1 + n log n | `1m(n)` + `malloc(16n)`; array `1a(n)` + `malloc(4n)` | O(n log n); vectors above 8k elements O(n) radix |
| `ranks` | `( v -- v' )` | statistics.telic: 0-based midranks as nx1 — tied values share the mean of their sorted positions, NaNs rank last in index order | n log n + 2n | `3m(n)` + `malloc(16n)` | O(n log n) |
| `where` | `( mat -- v )` | Flat row-major indices of the nonzero elements, as a k×1 index vector (1×k for a 1×n mask) | 1 + n | `1m(k)` | O(n) |
| `drop-nans` | `( v -- v' )` | matrix.telic: the finite elements of a vector, NaNs dropped | 4n | mask + index + `1m(k)` | O(n) |
| `cumulative-sum` | `( mat -- mat' )` | Running sum over the elements in row-major order, shape preserved — a vector's prefix sums | 1 + n | `1m(r×c)` | O(n) |
| `var` | `( mat -- f )` | Sample variance (÷ n−1) over all elements; errors with fewer than 2 | 1 + n | none | O(n) |
| `quantile` | `( mat p -- f )` | Linearly-interpolated quantile at p ∈ [0,1] over all elements (sorts a copy); errors if p out of range or empty | 2 + n log n | `malloc(n)` | O(n log n) |
| `quantiles` | `( mat probs -- v )` | statistics.telic: the linearly-interpolated quantile at each probability in the `probs` array, as a vector in that order | k·(2 + n log n) | `1a(k)` + `1m(k)` | O(k·n log n) |
| `histogram-table` | `( v n-bins -- fr )` | statistics.telic: equal-width bin counts over a vector's value range, as `{ :counts (n-bins×1) :low :bin-width }`. NaNs dropped, the maximum value is counted in the last bin, a constant vector takes the range value ± 1; errors on n-bins < 1 or no finite values | n + n-bins | `1m(n-bins)` + `1fr` | O(n + n-bins) |
| `ecdf` | `( v -- xs ys )` | statistics.telic: the empirical CDF as two n×1 vectors — the finite elements sorted ascending, and the cumulative fractions (i+1)/n, so `ys` at index i is F(`xs` at i). Ties stay as consecutive points; NaNs are excluded from the points and from n; errors when no finite values remain | 2n log n | `2m(n)` + `1a(n)` | O(n log n) |
| `binomial-deviance` | `( y p -- dev )` | statistics.telic: −2 Σ[y ln p + (1−y) ln(1−p)] over n×1 vectors — the proper scoring rule for probability models; p is clamped to [1e-12, 1−1e-12], so an overconfident prediction scores a large finite penalty rather than infinity | 10n | clamp + term vectors | O(n) |
| `brier` | `( outcomes probabilities -- f )` | statistics.telic: Brier score — mean of (probability − outcome)² over n×1 vectors; NaN elements are skipped | 3n | `2m(n)` | O(n) |
| `auc` | `( outcomes scores -- f )` | statistics.telic: area under the ROC curve = P(a random positive scores above a random negative), ties counted half (Mann–Whitney). outcomes n×1 in {0,1}, scores real; a NaN score drops its row (outcomes stay aligned); throws if either class is absent. Ties are handled exactly, so the result is independent of row order | n_pos·n_neg | index vectors + per-positive masks | O(n_pos·n_neg) |
| `cv-folds` | `( units n-folds -- folds )` | statistics.telic: deal `units` round-robin into `n-folds` `[ train test ]` index-array pairs in the given order; errors on n-folds < 2 or fewer units than folds | n | fold index arrays | O(n) |
| `cross-validate` | `( units n-folds fit-xt score-xt -- fold-losses )` | statistics.telic: k-fold cross-validation — units deal round-robin into folds in the order given (shuffle first when order matters; reuse one shuffle to compare configurations on the same folds); per fold, `fit` `( train-units -- model )` then `score` `( model test-units -- loss )`, both taking everything from the stack since they run in this word's frame; answers the per-fold losses as an n-folds vector (`mean` it for the CV estimate, `std` for the standard error). A unit is whatever the array holds — rows, or per-cluster index arrays for cluster CV | k·(fit + score) + n·k | fold index arrays | O(k·(fit + score)) |
| `ks-distance` | `( a b -- d )` | Two-sample Kolmogorov–Smirnov statistic: the largest absolute gap between the two samples' ECDFs, both advanced past each pooled value before measuring (ties). Symmetric; d ∈ [0, 1]; NaNs excluded per sample, each sample's own n; dimensioned inputs are computed over their magnitudes; errors when either sample has no finite values | (n+m) log(n+m) | `malloc(n)` + `malloc(m)` | O((n+m) log(n+m)); above 8k elements the sorts are O(n) radix |
| `std` | `( mat -- f )` | statistics.telic: the sample standard deviation of all elements | n | none | O(n) |
| `se` | `( mat -- f )` | statistics.telic: standard error of the mean (standard deviation ÷ √n) | n | none | O(n) |
| `median` | `( mat -- f )` | statistics.telic: the median (the 0.5 quantile) of all elements | n log n | `malloc(n)` | O(n log n) |
| `percentile` | `( mat pct -- f )` | statistics.telic: the linearly-interpolated value at percentile pct ∈ [0,100] over all elements | n log n | `malloc(n)` | O(n log n) |
| `iqr` | `( mat -- f )` | statistics.telic: interquartile range, Q3 − Q1 | 2n log n | `malloc(n)` ×2 | O(n log n) |
| `nonmissing-count` | `( mat -- n )` | The number of non-NaN elements | 1 + n | none | O(n) |
| `summary` | `( v/dataset -- fr )` | statistics.telic: a vector answers `{ :min :q1 :median :mean :q3 :max }` over its finite elements — a dimensioned vector in its unit, an instant vector (unit exactly `s`) with each statistic rendered as an ISO time string — plus `:missing` with the NaN count when any; an all-missing vector answers `{ :missing n }`, an empty one `{ }`. A dataset answers that frame per numeric column and `{ :distinct }` (distinct non-missing cells, plus `:missing`) per text column, keyed by column name; any other column value errors naming the column | 4n log n (per column) | `malloc(n)` ×4 + `1fr` (per column) | O(n log n) |
| `ci` | `( mat level -- low high )` | statistics.telic: percentile confidence interval — level 0.95 gives the 0.025 and 0.975 quantiles | 2n log n | `malloc(n)` ×2 | O(n log n) |
| `complete-cases` | `( xs ys -- xs' ys' )` | statistics.telic: drop the paired rows where either vector is NaN, keeping the two aligned and returned as n×1; magnitudes are taken, so dimensioned inputs come back unitless | 4n | index vector + 2 gathers | O(n) |
| `covariance` | `( xs ys -- f )` | statistics.telic: sample covariance over the complete pairs (n−1 denominator); errors below 2 complete pairs | 6n | `4m(n)` | O(n) |
| `correlation-pearson` | `( xs ys -- f )` | statistics.telic: Pearson correlation coefficient over the complete pairs; accepts nx1 or 1xn; `null` when either vector is constant | 12n | `6m(n)` | O(n) |
| `correlation-spearman` | `( xs ys -- f )` | statistics.telic: Spearman rank correlation — the correlation of the two vectors' midranks; `null` when either vector is constant | 2n log n | `6m(n)` + `malloc(16n)` ×2 | O(n log n) |
| `correlation-kendall` | `( xs ys -- f )` | Kendall tau-b: concordant minus discordant pairs over sqrt of tie-corrected pair counts, via one (x,y) sort and a merge-sort exchange count; NaN when all x or all y are tied; errors on length mismatch or fewer than 2 elements | 2n log n | `malloc(16n)` ×2–3 | O(n log n); above 8k elements the pair sort is O(n) radix |
| `correlate-with` | `( xs ys xt B -- fr )` | statistics.telic: bootstrap 95% CI for the correlation word at xt — resamples (x, y) pairs jointly, B refits, as `{ :estimate :se :bias :ci-low :ci-high }`; deterministic under a fixed seed | B·(n + xt) | pairs matrix + per-worker resample + `1fr` | O(B·(n + xt) / cores) |
| `cor` | `( xs ys -- fr )` | statistics.telic: Kendall tau-b with a 500-replicate bootstrap CI, as `{ :estimate :se :bias :ci-low :ci-high }` | as `correlate-with` | as `correlate-with` | as `correlate-with` |
| `qnorm` | `( p -- z )` | statistics.telic: standard normal quantile (inverse CDF) — relative error below 1.15e-9; errors unless p strictly inside (0, 1) | 30 | none | O(1) |
| `pnorm` | `( z -- p )` | statistics.telic: standard normal CDF, machine-accurate; float or matrix element-wise | 5 | matrix `1m(r×c)` | O(1); matrix O(n) |
| `random-normal` | `( -- z )` | statistics.telic: one standard normal deviate drawn from the global RNG stream | 32 | none | O(1) |
| `sample-without-replacement` | `( arr n -- arr )` | statistics.telic: draw n elements from the array without replacement (n ≤ length) | n | as `sample` | O(n) |
| `sample-with-replacement` | `( arr n -- arr )` | statistics.telic: draw n elements from the array with replacement | n | as `sample` | O(n) |
| `bootstrap` | `( data fit-xt B -- arr )` | statistics.telic: B refits of fit-xt over resamples of data — dataset/matrix rows, or an array's elements. One serial draw sets the run seed; replicate i draws its indices at run-seed + i, so no resample outlives its fit and results don't depend on scheduling — deterministic under a fixed seed | B(n + fit) | per-fit resample + `1a(B)` | O(B·(n + fit)) |
| `pbootstrap` | `( data fit-xt B -- arr )` | statistics.telic: B refits of fit-xt over resamples of data (dataset/matrix rows, or an array's elements), the fits run in parallel; identical results to a serial run through per-replicate seeding | as `bootstrap` | as `bootstrap` | O(B·(n + fit) / cores) |
| `bootstrap-with` | `( data fit-xt B mapper-xt -- arr )` | statistics.telic: B refits of fit-xt over resamples of data, the refits driven by the supplied `map`-shaped mapper-xt | as `bootstrap` | as `bootstrap` | as `bootstrap` |
| `sigmoid` | `( mat -- mat' )` | statistics.telic: elementwise logistic 1/(1+e⁻ˣ), mapping reals to (0,1) | 4n | `1m(r×c)` | O(n) |
| `norm` | `( mat -- f )` | matrix.telic: Euclidean (L2) norm, √(Σ elements²) — a vector's length, a matrix's Frobenius norm | 1 + n | none | O(n) |
| `dot` | `( v w -- f )` | matrix.telic: inner product; shapes must broadcast, so match the vectors | 2 + 2n | `1m(n)` | O(n) |
| `frobenius-norm` | `( mat -- f )` | Euclidean (L2) norm: √(Σ aᵢⱼ²) over all elements — a vector's length; for a matrix the Frobenius (entrywise 2-)norm, not the spectral norm | 1 + n | none | O(n) |

```forth augment
[ 1 2 ] vector [ 3 4 ] vector augment matrix>array . cr
```
```output
[ 1 3 2 4 ]
```

```forth vstack
[ 1 2 ] vector [ 3 4 ] vector vstack matrix>array . cr
{ :a [ 1 2 ] :b [ :x :y ] } { :a [ 3 ] :b [ :z ] } vstack :a @ transpose matrix>array . cr
null [ 5 ] vector vstack matrix>array . cr
```
```output
[ 1 2 3 4 ]
[ 1 2 3 ]
[ 5 ]
```

```forth hstack
[ 1 2 ] vector [ 3 4 ] vector hstack matrix>array . cr
{ :a [ 1 2 ] } { :b [ 3 4 ] } hstack keys . cr
[ [ 1 ] vector [ 2 ] vector [ 3 ] vector ] null ' hstack reduce dim . . cr
```
```output
[ 1 3 2 4 ]
[ :a :b ]
3 1
```

```forth submatrix
[ 1 2 3 4 5 6 7 8 9 ] 3 3 matrix 0 2 1 3 submatrix matrix>array . cr
```
```output
[ 2 3 5 6 ]
```

```forth select-rows
[ 10 20 30 40 ] vector [ 2 0 ] select-rows matrix>array . cr
```
```output
[ 30 10 ]
```

```forth mesh
[ 1 -1 3 ] vector dup -1 eq null mesh matrix>array . cr
```
```output
[ 1 null 3 ]
```

```forth argsort
[ 30 10 20 ] vector argsort matrix>array . cr
```
```output
[ 1 2 0 ]
```

```forth ranks
[ 10 20 20 30 ] vector ranks matrix>array . cr
```
```output
[ 0 1.5 1.5 3 ]
```

```forth where
[ 5 0 7 ] vector where matrix>array . cr
```
```output
[ 0 2 ]
```

```forth drop-nans
[ 1 null 3 ] vector drop-nans matrix>array . cr
```
```output
[ 1 3 ]
```

```forth cumulative-sum
[ 1 2 3 ] vector cumulative-sum matrix>array . cr
```
```output
[ 1 3 6 ]
```

```forth var
[ 2 4 4 4 5 5 7 9 ] vector var . cr
```
```output
4.57143
```

```forth quantile
[ 1 2 3 4 ] vector 0.5 quantile . cr
```
```output
2.5
```

```forth quantiles
[ 1 2 3 4 ] vector [ 0 0.5 1 ] quantiles matrix>array . cr
```
```output
[ 1 2.5 4 ]
```

```forth histogram-table
[ 1 1 2 3 3 3 ] vector 3 histogram-table :counts @ matrix>array . cr
```
```output
[ 2 1 3 ]
```

```forth ecdf
[ 3 1 2 ] vector ecdf matrix>array . matrix>array . cr
```
```output
[ 0.333333 0.666667 1 ] [ 1 2 3 ]
```

```forth binomial-deviance
[ 1 0 1 ] vector [ 0.9 0.1 0.8 ] vector binomial-deviance . cr
```
```output
0.867729
```

```forth brier
[ 1 0 ] vector [ 0.8 0.3 ] vector brier . cr
```
```output
0.065
```

```forth auc
[ 1 0 1 0 ] vector [ 0.9 0.2 0.7 0.4 ] vector auc . cr
```
```output
1
```

```forth cv-folds
[ 0 1 2 3 ] 2 cv-folds . cr
```
```output
[ [ [ 1 3 ]
    [ 0 2 ] ]
  [ [ 0 2 ]
    [ 1 3 ] ] ]
```

```forth cross-validate
[ 1 2 3 4 ] 2 [: vector mean :] [: vector mean swap - abs :] cross-validate matrix>array . cr
```
```output
[ 1 1 ]
```

```forth ks-distance
[ 1 2 3 ] vector [ 1 2 3 ] vector ks-distance . cr
[ 1 2 3 ] vector [ 4 5 6 ] vector ks-distance . cr
```
```output
0
1
```

```forth std
[ 2 4 4 4 5 5 7 9 ] vector std . cr
```
```output
2.13809
```

```forth se
[ 2 4 4 4 5 5 7 9 ] vector se . cr
```
```output
0.755929
```

```forth median
[ 1 2 3 4 ] vector median . cr
```
```output
2.5
```

```forth percentile
[ 1 2 3 4 ] vector 25 percentile . cr
```
```output
1.75
```

```forth iqr
[ 1 2 3 4 ] vector iqr . cr
```
```output
1.5
```

```forth nonmissing-count
[ 1 null 3 ] vector nonmissing-count . cr
```
```output
2
```

```forth summary
[ 1 2 3 4 ] vector summary /median @ . cr
```
```output
2.5
```

```forth ci
[ 1 2 3 4 5 6 7 8 9 10 ] vector 0.8 ci . . cr
```
```output
9.1 1.9
```

```forth complete-cases
[ 1 null 3 ] vector [ 4 5 null ] vector complete-cases matrix>array . matrix>array . cr
```
```output
[ 4 ] [ 1 ]
```

```forth correlation-pearson
[ 1 2 3 4 5 ] vector [ 2 4 6 8 10 ] vector correlation-pearson . cr
```
```output
1
```

```forth correlation-spearman
[ 1 2 2 3 ] vector [ 1 3 2 4 ] vector correlation-spearman . cr
```
```output
0.948683
```

```forth correlation-kendall
[ 1 2 3 4 5 ] vector [ 2 1 4 3 5 ] vector correlation-kendall . cr
```
```output
0.6
```

```forth correlate-with
7 seed [ 1 2 3 4 5 6 7 8 9 10 ] vector [ 2 1 4 3 5 7 6 9 8 10 ] vector ' correlation-pearson 100 correlate-with :estimate @ . cr
```
```output
0.951515
```

```forth cor
7 seed [ 1 2 3 4 5 6 7 8 9 10 ] vector [ 2 1 4 3 5 7 6 9 8 10 ] vector cor :estimate @ . cr
```
```output
0.822222
```

```forth qnorm
0.975 qnorm . cr
```
```output
1.95996
```

```forth pnorm
1.959963984540054 pnorm . cr
```
```output
0.975
```

```forth random-normal
42 seed random-normal . cr
```
```output
-1.37955
```

```forth covariance
[ 1 2 3 4 ] vector [ 2 4 6 8 ] vector covariance . cr
```
```output
3.33333
```

```forth sample-without-replacement
42 seed [ 1 2 3 4 5 ] 2 sample-without-replacement . cr
```
```output
[ 3 4 ]
```

```forth sample-with-replacement
42 seed [ 1 2 3 ] 4 sample-with-replacement . cr
```
```output
[ 1 1 3 3 ]
```

```forth bootstrap
42 seed [ 1 2 3 4 5 ] [: vector mean :] 3 bootstrap . cr
```
```output
[ 2.2 2.4 2.6 ]
```

```forth pbootstrap
42 seed [ 1 2 3 4 5 ] [: vector mean :] 3 pbootstrap . cr
```
```output
[ 2.2 2.4 2.6 ]
```

```forth bootstrap-with
42 seed [ 1 2 3 ] [: vector mean :] 2 ' map bootstrap-with . cr
```
```output
[ 1.66667 1 ]
```

```forth sigmoid
[ 0 ] vector sigmoid matrix>array . cr
```
```output
[ 0.5 ]
```

```forth norm
[ 3 4 ] vector norm . cr
```
```output
5
```

```forth dot
[ 1 2 3 ] vector [ 4 5 6 ] vector dot . cr
```
```output
32
```

```forth frobenius-norm
[ 3 4 ] vector frobenius-norm . cr
```
```output
5
```

---

## Segments

A flat, fixed-length buffer of C `int`s stored off the arena (one `calloc`, freed by GC): the out-parameter and index buffer a C library takes as `int *`, reached through `segment>pointer`. `@i` reads an element as a float and `!i` stores a float truncated toward zero, sharing the array index ops. Dense double storage is a matrix (`n 1 0-matrix`, `@e`/`!e`, `matrix>pointer`), and the fusions cover both alike.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `int-segment` | `( n -- seg )` | n-element int segment, zero-filled; errors if n < 0 | 1 | `1seg(n)` | O(n) |
| `@i` | `( seg i -- f )` | Read element i as a float; bounds-checked | 3 | none | O(1) |
| `!i` | `( seg f i -- seg )` | Store float f at index i in place, truncated toward zero to a C int; bounds-checked; leaves seg | 4 | none | O(1) |
| `segment>pointer` | `( seg -- ptr )` | Intern the backing buffer and return an FFI pointer handle (no copy) | 1 | none | O(1)† |

`†` amortized; the pointer-intern table grows occasionally.

```forth int-segment
2 int-segment 7 1 !i dup 0 @i . 1 @i . cr
```
```output
0 7
```

```forth @i
3 int-segment 0 @i . cr
```
```output
0
```

```forth-noexec segment>pointer
4 int-segment segment>pointer ptr? . cr
```
```output
1
```

---

## Random

A thread-local xoshiro256\*\* stream; each worker derives its own stream from the shared base seed, so parallel runs are deterministic per worker.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `seed` | `( n -- )` | Set the global base seed and reset the stream counter; per-thread streams derive from it | 1 | none | O(1) |
| `random` | `( -- f )` | Uniform float in [0,1) | 1 | none | O(1) |
| `random-int` | `( bound -- f )` | Uniform integer in [0,bound) as a float; errors if bound ≤ 0 | 1 | none | O(1)† |

`†` expected O(1); rejection sampling may retry. `sample` (Arrays) and `resample-indices` (Datasets and TSV) draw on this stream.

```forth seed
42 seed random . cr
```
```output
0.083863
```

```forth random
42 seed random . random . cr
```
```output
0.083863 0.37898
```

```forth random-int
42 seed 10 random-int . 10 random-int . cr
```
```output
2 2
```

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
identical on every platform; the `-local` words go through libc in the
process's timezone, re-reading `TZ` on every call. WASI has no timezone
machinery, so there the `-local` words behave as UTC and `parse-time` lacks
`%z`.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `wall-now` | `( -- instant )` | units.telic: current epoch seconds as a quantity in `s`; steps when the system clock is adjusted | 2 | 1 pair | O(1) |
| `epoch>date` | `( instant -- date )` | Decompose an instant into a date frame, UTC | 40 | `1o` | O(1) |
| `epoch>date-local` | `( instant -- date )` | Decompose in the process's timezone | 40 | `1o` | O(1) |
| `date>epoch` | `( date -- instant )` | Compose an instant from a date frame, UTC; `:year` required, absent fields default, out-of-range fields carry | 30 | 1 pair | O(1) |
| `date>epoch-local` | `( date -- instant )` | Compose an instant from a date frame in the process's timezone; the zone rules resolve DST | 30 | 1 pair | O(1) |
| `format-time` | `( instant format -- string )` | Render with strftime, UTC | len | `1s` | O(len) |
| `format-time-local` | `( instant format -- string )` | Render with strftime in the process's timezone | len | `1s` | O(len) |
| `parse-time` | `( string format -- instant )` | Parse with strptime; uncaptured fields default to 1970-01-01 00:00:00, read as UTC unless the format captures an offset with `%z`; errors on a mismatch | len | 1 pair | O(len) |
| `time>iso` | `( instant -- string )` | units.telic: render an instant as an ISO 8601 UTC string (`YYYY-MM-DDTHH:MM:SSZ`) | len | `1s` | O(1) |
| `iso>time` | `( string -- instant )` | units.telic: parse an ISO 8601 UTC string (`YYYY-MM-DDTHH:MM:SSZ`) to an instant | len | 1 pair | O(1) |
| `days-in-month` | `( year month -- days )` | units.telic: length of the month, leap-aware | 60 | frames | O(1) |
| `date-shift` | `( instant delta -- instant )` | units.telic: calendar shift, UTC. `:years`/`:months` step the calendar with the day clamped to the target month (Jan 31 + 1 month = Feb 28/29); `:weeks` `:days` `:hours` `:minutes` `:seconds` add exact durations. Components combine and may be negative | 200 | frames + pairs | O(1) |

```forth-noexec wall-now
wall-now time>iso . cr
```
```output
2026-08-06T23:14:09Z
```

```forth epoch>date
0 s epoch>date :year @ . cr
```
```output
1970
```

```forth epoch>date-local
"UTC" "TZ" env! 0 s epoch>date-local :year @ . cr
```
```output
1970
```

```forth date>epoch
{ :year 2000 } date>epoch 1 day / round . cr
```
```output
10957
```

```forth date>epoch-local
"UTC" "TZ" env! { :year 2000 } date>epoch-local { :year 2000 } date>epoch = . cr
```
```output
1
```

```forth format-time
0 s "%Y-%m-%d" format-time . cr
```
```output
1970-01-01
```

```forth format-time-local
"UTC" "TZ" env! 0 s "%H:%M" format-time-local . cr
```
```output
00:00
```

```forth parse-time
"2001-02-03" "%Y-%m-%d" parse-time time>iso . cr
```
```output
2001-02-03T00:00:00Z
```

```forth time>iso
0 s time>iso . cr
```
```output
1970-01-01T00:00:00Z
```

```forth iso>time
"2020-01-02T03:04:05Z" iso>time epoch>date :day @ . cr
```
```output
2
```

```forth days-in-month
2024 2 days-in-month . cr
```
```output
29
```

```forth date-shift
"2026-01-31T09:00:00Z" iso>time { :months 1 } date-shift time>iso . cr
```
```output
2026-02-28T09:00:00Z
```

---

## Datasets and TSV

*Rows* are an array of row-arrays (as `load-tsv` returns) — the raw I/O interchange, the only form preserving a file's physical column order. A *dataset* is a column-oriented frame; `query` selects its rows by a pattern frame, `query-rows` answers them as frames for the logic words. `r`/`c` are rows/columns, `n`/`k` observations/selected columns. `select-rows` (under Matrices) accepts a dataset, gathering every column by one index array or vector — with `where` masks and `argsort` that is filtering and sorting.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `load-tsv` | `( path -- rows )` | Read a TSV file into an array of row-arrays; an empty cell → `null`, a numeric cell → float, else a string. No header handling | 1 + bytes | `1a(r)` + one array per row + a string per text cell | O(bytes) |
| `read-tsv` | `( path -- dataset )` | datasets.telic: read a TSV file with a header row into a column-oriented dataset; columns are typed — uniformly float-or-`null` cells become an n×1 vector (`null` → NaN), uniform-unit quantity cells a dimensioned vector, anything else a cell array | bytes + 2·r·c | rows + one array per column + `1m` per numeric column + `1fr` | O(bytes + r·c) |
| `write-tsv` | `( dataset path -- )` | datasets.telic: write a dataset as a TSV with a header row of the column names; a dimensioned column errors (strip its unit with `magnitude` first) | 2·r·c | transient rows | O(r·c) |
| `save-tsv` | `( rows path -- )` | Write an array of row-arrays as TSV; `null` → empty, a whole-number float → integer, strings raw; errors on a tab/newline inside a string or a non-array row | 2 + r·c | none (to file) | O(r·c) |
| `rows>dataset` | `( rows header? -- dataset )` | datasets.telic: column-oriented frame from rows with typed columns — uniformly float-or-`null` cells become an n×1 vector (`null` → NaN), uniform-unit quantity cells a dimensioned vector, anything else stays the cell array; keys come from row 0 when header? is true, else `:col1…` are synthesized | 2·r·c | `k×1a(r)` + `1m` per numeric column + `1fr` | O(r·c) |
| `dataset>rows` | `( dataset -- rows )` | datasets.telic: an array of row-arrays led by a header row of the column names as strings, columns in key order; each cell is its column's value (NaN → `null`, dimensioned cells as quantities) | r·c | header + one array per row + `1a(r·c)` cells | O(r·c) |
| `headn` | `( dataset n leading-columns -- )` | datasets.telic: print the first min(n, rows) rows as an aligned table — the `leading-columns` symbols appear first in the given order, the remaining columns alphabetical by name (an empty `leading-columns` orders every column alphabetically); column names as the header line, two-space gutter, numeric/quantity columns right-aligned, text left, `:datetime` columns as ISO time strings, other cells in their default rendering; empty dataset prints nothing | r·c | rendered cells | O(r·c) |
| `head` | `( dataset -- )` | datasets.telic: print the first 10 rows as an aligned table, columns alphabetical by name | r·c | rendered cells | O(r·c) |
| `dataset>matrix` | `( dataset cols -- mat )` | datasets.telic: build an n×k matrix from the named numeric columns (rows are observations) | n·k | flat `1a(n·k)` + `2m(n×k)` | O(n·k) |
| `column-type` | `( dataset sym -- sym )` | datasets.telic: the named column's type from its representation — matrix `:numeric`, quantity in exactly `s` `:datetime`, other quantity `:quantity`, array `:text`; a missing key errors | 8 | 1 pair | O(log c) |
| `column>array` | `( column -- arr )` | datasets.telic: a column in any representation as an array of its values — array columns pass through unchanged, matrix and quantity columns become per-element values (NaN → `null`, dimensioned elements become quantities) | n | `1a(n)` for matrix columns, none for arrays | O(n) |
| `column>set` | `( column -- set )` | datasets.telic: the set of the column's distinct values | 2n log n | `1a(n)` + `1o` | O(n log n) |
| `row-at` | `( dataset i -- fr )` | datasets.telic: row i as a frame of scalars, one key per column, each cell the column's value (NaN → `null`, dimensioned cells as quantities); errors when i is outside [0, rows) | r·c | `1a(r)` per numeric column + `1fr` | O(r·c) |
| `select-columns` | `( dataset cols -- dataset )` | datasets.telic: the named columns as a new dataset (a fresh frame sharing the column values); a missing name errors | k log c | `1a(k)` + `1o` | O(k log c) |
| `filter-columns` | `( dataset pred -- dataset )` | datasets.telic: the columns whose name and values satisfy `pred` `( column-name column -- binary )`, in dataset order, as a fresh frame sharing the column values; every column is offered, so selecting by name is the predicate's job. Non-mutating | c·(pred + log c) | kept-key array + `1o` | O(c·(pred + log c)) |
| `select-eq` | `( dataset sym value -- dataset )` | datasets.telic: the rows whose `sym` column equals `value`; other rows drop | n + n·c | mask + index vector + one column each | O(n·c) |
| `select-neq` | `( dataset sym value -- dataset )` | datasets.telic: the rows whose `sym` column differs from `value`; other rows drop | n + n·c | mask + index vector + one column each | O(n·c) |
| `query` | `( dataset pattern -- dataset )` | datasets.telic: the rows whose columns equal the pattern frame's ground values — each key names a column (a key that is not a column errors), each value is dereferenced and compared with `eq` (so symbols, strings, floats and quantities within their dimension all serve; `1` and `"1"` differ), and a value that is an unbound logic variable or `_` constrains nothing and takes no binding; the masks multiply, and `{ }` keeps every row. Rows keep their order and every column | n·k + n·c | k masks + index vector + one column each | O(n·(k + c)) |
| `query-rows` | `( dataset pattern -- rows )` | datasets.telic: `query`'s rows as frames, one per row keyed by column, for binding the pattern's logic variables row by row — `[: row | pattern row ~ drop … :]` under `each`, or `choose` over them | query + n·c | query + `1o` per row | O(n·(k + c)) |
| `complete-rows` | `( dataset cols -- dataset )` | datasets.telic: the rows where none of the named columns is missing — NaN in a vector or dimensioned vector, `null` in a text column; `cols` is a symbol, a symbol array, or `[ ]` for every column; a missing name errors. Every column reorders together and keeps its representation | n·k + n·c | one array and mask per named column, index vector, one column each | O(n·(k + c)) |
| `sort-rows` | `( dataset sym -- dataset )` | datasets.telic: rows sorted ascending by the named column, so every column reorders together and keeps its representation; a missing key errors | n log n + n·c | permutation + one column each | O(n log n + n·c) |
| `sort-rows-descending` | `( dataset sym -- dataset )` | datasets.telic: rows sorted descending by the named column in natural order (so text columns descend too); equal keys keep dataset order, so chained sorts compose — minor key first, major key last — and missing cells sort last; a missing key errors | n log n + n·c | permutation ×3 + one column each | O(n log n + n·c) |
| `count` | `( arr/v/dataset -- pairs )` | datasets.telic: occurrences of each distinct value as `[ [ value n ] … ]`, most frequent first, ties in value order; a vector counts its elements (a dimensioned one counts quantities), a dataset counts whole rows, each a frame keyed by column name | 2n log n | rows + pairs + 3×`1a` | O(n log n) |
| `zero-variance?` | `( column -- bool )` | datasets.telic: 1 when the column (array or vector) has fewer than two distinct values — a constant or empty column; `null`/NaN counts as a value, so `[ 1 null ] vector` answers 0. `[: nip zero-variance? not :] filter-columns` drops constant columns from a dataset | 2n log n | `count`'s | O(n log n) |
| `group-indices` | `( column -- pairs )` | datasets.telic: `[ [ value [indices] ] … ]` per distinct value in natural order — each index array holds the value's row positions, ascending; a numeric column's NaN group orders last, a text column's `null` group first | 2n log n | permutation + one pair and array per value | O(n log n) 
| `split-by` | `( dataset by -- pairs )` | datasets.telic: `[ [ key sub-dataset ] … ]`, one pair per distinct value of the `by` column, or per value tuple of a `by`-symbol array (the key then an array), in `group-indices` order; each sub-dataset is the group's rows with every column and representation kept | n log n + per-group select | one sub-dataset per group | O(n log n + n·c) |
| `frames>dataset` | `( rows -- dataset )` | datasets.telic: an array of row frames as a column-oriented dataset, keys from row 0 — differing keys throw. Each column's representation is inferred: all-float cells (`null` → NaN) become an n×1 vector, uniform-unit quantities a dimensioned vector, anything else stays an array | n·k log k | one column per key + `1o` | O(n·k log k) |
| `aggregate` | `( dataset by xt -- dataset )` | datasets.telic: split-apply-combine — rows group by the `by` column's distinct values, or by the value tuple of a `by`-symbol array (tuples group and order in natural order); for each group `xt` `( group-dataset -- frame )` answers one row frame, the group's key values are stored into it under their own keys (overwriting any the xt set), and the rows reassemble into a dataset | n log n + per-group xt | one sub-dataset and frame per group + result columns; array `by` adds one tuple array per row | O(n log n + n·c) |
| `within-groups` | `( dataset by xt -- column )` | datasets.telic: the grouped transform, `aggregate`'s non-collapsing twin — rows group as `aggregate` groups them; for each group `xt` `( group-dataset -- column/scalar )` answers one value per group row (a column or array of the group's length) or one scalar that fills the group; the values return to their original row positions and the assembled n-row column is answered, in the representation `column-from-cells` infers, for the caller to store (`… within-groups :share !`) or use as a mask. A quotation answering the wrong count errors naming the group | n log n + per-group xt | one sub-dataset per group + `2a(n)` + one column | O(n log n + n·c) |
| `expand-grid` | `( grid-frame -- dataset )` | datasets.telic: the cartesian product of the per-key value lists as a dataset — `grid-frame` maps each column key to its values (array, vector, or set), the result has one column per key and `∏ lengths` rows, every combination present. Columns follow the frame's key order with the last key varying fastest, so rows come out sorted left to right (odometer order). Numeric columns come back as vectors, text as arrays; a zero-length value list yields 0 rows, an empty frame `{ }` | ∏·k | one column each + index arrays | O(∏·k) |
| `merge-by` | `( left right key join-type -- dataset )` | datasets.telic: join on `key` (a symbol or symbol array); `join-type` is `:inner` (matched pairs only), `:left` (every left row, right's columns `null` on a miss), `:right` (every right row, left's columns `null`), or `:outer` (left rows then unmatched right rows, missing side `null`). The probed side's key must be **unique** — right for `:inner`/`:left`, left for `:right`, both for `:outer` — a duplicate on the probed side errors; a non-key column in both datasets errors naming it (no `.x`/`.y` suffixing). Columns are the union with key once; a null-filled numeric column re-infers as a vector with NaN | L·R | row frames + merged columns | O(L·R) |
| `repeat-column!` | `( dataset value sym -- dataset )` | datasets.telic: store `value` repeated over the rows as the named column, in place, and leave the dataset: a scalar fills every row; an array or vector of length k repeats n-rows/k times, and n-rows not a multiple of k errors. The column takes the representation `column-from-cells` infers — floats and `null` a vector, one-unit quantities a dimensioned vector, else an array; an existing key is overwritten | 4n | `2a(n)` + one column | O(n) |
| `set-unit!` | `( dataset unit-xt sym -- dataset )` | datasets.telic: set the named column's unit to the one `unit-xt` attaches (`' m`), replacing any existing unit, so the column becomes a dimensioned vector and the stats keep the unit. In place, returns the dataset; a non-numeric (text) column errors naming it | log c + n | one column | O(n) |
| `replace-where!` | `( dataset pred replacement sym -- )` | datasets.telic: replace the named column's cells passing `pred` `( column -- mask )`, in place, so the replacement broadcasts and units reconcile: `pipeline [: -1 eq :] null :rep_touches replace-where!` nulls a sentinel, `[: nan? :] 0` fills missing, `[: 10 $ < :] 5 $` floors prices | pred + n | mask + one column | O(n) |
| `resample-indices` | `( n -- arr )` | datasets.telic: n indices drawn from [0,n) with replacement (bootstrap), from the global stream | 2n | `2×1a(n)` | O(n) |
| `resample-indices-ext` | `( n seed -- arr )` | n indices drawn from [0,n) with replacement by a private generator seeded from `seed` — same draw for the same seed regardless of thread or stream position | n | `1a(n)` | O(n)† |

```forth load-tsv
[ [ "a" "b" ] [ 1 2 ] ] "/tmp/docs-example.tsv" save-tsv "/tmp/docs-example.tsv" load-tsv . cr
```
```output
[ [ "a" "b" ]
  [ 1 2 ] ]
```

```forth read-tsv
[ [ "x" ] [ 1 ] [ 2 ] ] true rows>dataset "/tmp/docs-example2.tsv" write-tsv "/tmp/docs-example2.tsv" read-tsv :x @ mean . cr
```
```output
1.5
```

```forth write-tsv
[ [ "x" ] [ 1 ] [ 2 ] ] true rows>dataset "/tmp/docs-example2.tsv" write-tsv "/tmp/docs-example2.tsv" read-tsv :x @ mean . cr
```
```output
1.5
```

```forth save-tsv
[ [ "a" "b" ] [ 1 2 ] ] "/tmp/docs-example.tsv" save-tsv "/tmp/docs-example.tsv" load-tsv . cr
```
```output
[ [ "a" "b" ]
  [ 1 2 ] ]
```

```forth rows>dataset
[ [ "n" "v" ] [ "a" 1 ] [ "b" 2 ] ] true rows>dataset :v @ matrix>array . cr
```
```output
[ 1 2 ]
```

```forth dataset>rows
[ [ "x" ] [ 7 ] ] true rows>dataset dataset>rows . cr
```
```output
[ [ "x" ]
  [ 7 ] ]
```

```forth head
[ [ "name" "age" ] [ "ann" 34 ] [ "bo" 25 ] ] true rows>dataset head
```
```output
age  name
 34  ann
 25  bo
```

```forth headn
[ [ "name" "age" ] [ "ann" 34 ] [ "bo" 25 ] ] true rows>dataset 1 [ :name ] headn
```
```output
name  age
ann    34
```

```forth dataset>matrix
[ [ "x" "y" ] [ 1 10 ] [ 2 20 ] ] true rows>dataset [ :x :y ] dataset>matrix matrix>array . cr
```
```output
[ 1 10 2 20 ]
```

```forth column-type
[ [ "x" ] [ 1 ] ] true rows>dataset :x column-type . cr
```
```output
:numeric
```

```forth column>array
[ [ "x" ] [ 1 ] [ 2 ] ] true rows>dataset :x @ column>array . cr
```
```output
[ 1 2 ]
```

```forth column>set
[ "b" "a" "b" ] column>set . cr
```
```output
[< "a" "b" >]
```

```forth row-at
[ [ "name" "age" ] [ "ann" 34 ] [ "bo" 25 ] ] true rows>dataset 1 row-at frame>array . cr
```
```output
[ :name "bo" :age 25 ]
```

```forth select-columns
[ [ "a" "b" ] [ 1 2 ] ] true rows>dataset [ :b ] select-columns keys . cr
```
```output
[ :b ]
```

```forth filter-columns
{ :a [ 1 2 ] vector :b [ 0 0 ] vector :c [ 3 4 ] vector :d [ "p" "q" ] } to cols
cols [: nip dup matrix? if sum 0 eq not else drop 1 then :] filter-columns keys . cr
cols [: drop dup :b = swap :d = or :] filter-columns keys . cr
```
```output
[ :a :c :d ]
[ :b :d ]
```

```forth select-eq
{ :program [ :a :b :a ] :n [ 1 2 3 ] vector } :program :a select-eq :n @ transpose matrix>array . cr
```
```output
[ 1 3 ]
```

```forth select-neq
{ :program [ :a :b :a ] :n [ 1 2 3 ] vector } :program :a select-neq :n @ transpose matrix>array . cr
```
```output
[ 2 ]
```

```forth query
{ :parent [ :john :john :anne ] :child [ :django :bob :django ] } to family
family { :child :django } query :parent @ . cr
lvar to Who family { :parent :john :child Who } query n-rows . cr
```
```output
[ :john :anne ]
2
```

```forth query-rows
{ :parent [ :john :john :anne ] :child [ :django :bob :django ] } to family
family { :parent :john } query-rows [: row | lvar to Kid { :child Kid } row ~ drop Kid ? . :] each cr
```
```output
:django :bob
```

```forth complete-rows
{ :x [ 1 null 3 ] vector :y [ "a" "b" null ] } :x complete-rows :y @ . cr
{ :x [ 1 null 3 ] vector :y [ "a" "b" null ] } [ ] complete-rows :y @ . cr
```
```output
[ "a" null ]
[ "a" ]
```

```forth sort-rows
{ :name [ "b" "a" "c" ] :score [ 2 3 1 ] vector } :score sort-rows :name @ . cr
```
```output
[ "c" "b" "a" ]
```

```forth sort-rows-descending
{ :name [ "b" "a" "c" ] :score [ 2 3 1 ] vector } :score sort-rows-descending :name @ . cr
```
```output
[ "a" "b" "c" ]
```

```forth count
[ :b :a :b ] count . cr
```
```output
[ [ :b 2 ]
  [ :a 1 ] ]
```

```forth zero-variance?
[ 1 1 1 ] vector zero-variance? . [ 1 2 ] vector zero-variance? . cr
```
```output
1 0
```

```forth group-indices
[ :x :y :x ] group-indices . cr
```
```output
[ [ :x
    [ 0 2 ] ]
  [ :y
    [ 1 ] ] ]
```

```forth split-by
{ :g [ "b" "a" "b" ] :x [ 1 2 3 ] vector } :g split-by
[: key-group | key-group 0 @i . key-group 1 @i :x @ transpose matrix>array . cr :] each
```
```output
a [ 2 ]
b [ 1 3 ]
```

```forth frames>dataset
[ { :a 1 } { :a 2 } ] frames>dataset :a @ matrix>array . cr
```
```output
[ 1 2 ]
```

```forth aggregate
{ :g [ "a" "b" "a" ] :x [ 1 2 3 ] vector } :g [: group | { :sum group :x @ sum } :] aggregate :sum @ matrix>array . cr
{ :g [ "a" "b" "a" ] :h [ 1 1 2 ] vector :x [ 1 2 3 ] vector }
[ :g :h ] [: group | { :sum group :x @ sum } :] aggregate :sum @ matrix>array . cr
```
```output
[ 4 2 ]
[ 1 3 2 ]
```

```forth within-groups
{ :g [ "a" "b" "a" ] :x [ 1 2 3 ] vector } to grouped
grouped :g [: group | group :x @ dup sum / :] within-groups transpose matrix>array . cr
grouped :g [: group | group :x @ sum :] within-groups transpose matrix>array . cr
```
```output
[ 0.25 1 0.75 ]
[ 4 2 4 ]
```

```forth expand-grid
{ :a [ 1 2 ] :b [ :x :y :z ] } expand-grid dup :a @ matrix>array . :b @ . cr
```
```output
[ 1 1 1 2 2 2 ] [ :x :y :z :x :y :z ]
```

```forth merge-by
{ :id [ 1 2 3 ] :x [ :a :b :c ] } { :id [ 1 3 ] :y [ 10 30 ] } :id :left merge-by dup :x @ . :y @ column>array . cr
```
```output
[ :a :b :c ] [ 10 null 30 ]
```

```forth repeat-column!
{ :x [ 1 2 3 4 ] vector } 7 :seven repeat-column! [ 1 2 ] :cycle repeat-column!
dup :seven @ transpose matrix>array . :cycle @ transpose matrix>array . cr
```
```output
[ 7 7 7 7 ] [ 1 2 1 2 ]
```

```forth set-unit!
{ :height [ 170 180 ] vector } ' m :height set-unit! :height @ mean . cr
```
```output
175 m
```

```forth replace-where!
[ [ "v" ] [ -1 ] [ 5 ] ] true rows>dataset dup [: -1 eq :] null :v replace-where! :v @ nonmissing-count . cr
```
```output
1
```

```forth resample-indices
42 seed 4 resample-indices . cr
```
```output
[ 2 2 1 1 ]
```

```forth resample-indices-ext
5 99 resample-indices-ext . cr
```
```output
[ 3 1 2 1 0 ]
```

---

## Higher-order

The quotation/predicate cost dominates; `xt` denotes one call.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `map` | `( arr/set xt -- arr )` or `( dataset xt -- dataset )` | Apply xt to each element; xt must leave exactly one value. datasets.telic extends it to a dataset: xt maps each row frame to a new row frame — derive, rename, or drop fields — and the returned frames rebuild into a dataset, so all rows must share keys and columns re-infer their representation | 2 + n·xt | `1a(n)`; dataset rows + new columns | O(n·xt); dataset O(n·(xt + k log k)) |
| `nmap` | `( arr₁ … arr_N xt N -- arr )` | N-ary zip-map over equal-length arrays | rows·(N+xt) | `1a(rows)` | O(rows·xt) |
| `filter` | `( arr/set xt -- arr )` or `( dataset xt -- dataset )` | Keep elements where xt is truthy. datasets.telic extends it to a dataset: xt sees each row as a frame keyed by column name and answers a bool (1.0/0.0); the kept rows come back as a dataset, so every column keeps its representation | 2 + n·xt | malloc(n) flags + `1a(k)`; dataset rows + mask + one column each | O(n·xt) |
| `reduce` | `( arr/set init xt -- val )` | Left fold; xt is `( acc elem -- acc )` | 3 + n·xt | none | O(n·xt) |
| `times` | `( xt n -- )` | Run xt n times, no index pushed | 2 + n·xt | none | O(n·xt) |
| `sum-times` | `( xt n -- total )` | arrays.telic: the sum of `xt` `( i -- term )` over i in 0..n-1 | 3 + n·(1+xt) | none | O(n·xt) |
| `product-times` | `( xt n -- product )` | arrays.telic: the product of `xt` `( i -- term )` over i in 0..n-1 | 3 + n·(1+xt) | none | O(n·xt) |
| `i-times` | `( xt n -- )` | Run xt n times, pushing index 0..n-1 first | 2 + n·(1+xt) | none | O(n·xt) |
| `fold-times` | `( acc map-xt combine-xt n -- acc' )` | Counted map-fold: for i in 0..n-1 push i, run `map-xt` `( i -- term )`, then combine the accumulator with the term. The accumulator never appears on the data stack — with `' f+`, `' f-`, `' f*`, `' f/` or their polymorphic counterparts the arithmetic runs inside the loop with no dispatch, and any other combiner is invoked as `( acc term -- acc' )`. `0 [: dup f* :] ' f+ 5 fold-times` answers 30; values the body needs beyond the index are parked below and read with `pick` | 4 + n·(1+xt) | none | O(n·xt) |
| `find-first` | `( items pred -- element )` | The first element for which pred is truthy, or `null`; short-circuits at the first hit (does not run pred over the rest) | n·xt | none | O(n·xt) |
| `any?` | `( items pred -- bool )` | arrays.telic: true when pred is truthy for some element, false otherwise; short-circuits at the first hit | n·xt | none | O(n·xt) |
| `all?` | `( items pred -- bool )` | arrays.telic: true when every element satisfies pred, vacuously true on empty. Runs pred over **every** element, so it does not short-circuit and a side-effecting pred runs n times | 2n·xt | `1a(n)` | O(n·xt) |
| `each` | `( items xt -- )` | Run xt `( element -- )` on every element for its side effects; the element is the only thing the quotation may consume, and it must leave nothing. No result, no allocation | 2 + n·xt | none | O(n·xt) |
| `flat-map` | `( items xt -- arr )` | arrays.telic: xt returns an array per element, results concatenated | n·xt + total | `1a(n)` + `1a(total)` | O(n·xt + total) |
| `sort-by` | `( items xt -- arr )` | arrays.telic: sorted by the key xt `( element -- key )` extracts, one evaluation per element; equal keys keep index order | n·xt + n log n | 3×`1a(n)` + `malloc(4n)` | O(n·xt + n log n) |
| `partition` | `( items pred -- matches rest )` | arrays.telic: the elements satisfying pred and the others, one pass, input order kept | n·xt | 2 arrays + the curried predicate token | O(n·xt) |

```forth map
[ 1 2 3 ] [: dup * :] map . cr
```
```output
[ 1 4 9 ]
```

```forth nmap
[ 1 2 ] [ 10 20 ] ' + 2 nmap . cr
```
```output
[ 11 22 ]
```

```forth filter
[ 1 2 3 4 ] [: 2 mod 0= :] filter . cr
```
```output
[ 2 4 ]
```

```forth reduce
[ 1 2 3 4 ] 0 ' + reduce . cr
```
```output
10
```

```forth times
[: "ho" . :] 3 times cr
```
```output
ho ho ho
```

```forth sum-times
[: dup * :] 4 sum-times . cr
```
```output
14
```

```forth product-times
' 1+ 4 product-times . cr
```
```output
24
```

```forth i-times
[: . :] 3 i-times cr
```
```output
0 1 2
```

```forth fold-times
0 [: dup f* :] ' f+ 5 fold-times . cr
```
```output
30
```

```forth find-first
[ 3 8 5 ] [: 4 > :] find-first . cr
```
```output
8
```

```forth any?
[ 1 3 5 ] [: 2 mod 0= :] any? . cr
```
```output
0
```

```forth all?
[ 2 4 ] [: 2 mod 0= :] all? . cr
```
```output
1
```

```forth each
[ 1 2 3 ] [: . :] each cr
```
```output
1 2 3
```

```forth flat-map
[ 1 2 ] [: dup 1 + 2 array :] flat-map . cr
```
```output
[ 1 2 2 3 ]
```

```forth sort-by
[ "bb" "a" "ccc" ] ' size sort-by . cr
```
```output
[ "a" "bb" "ccc" ]
```

```forth partition
[ 1 2 3 4 ] [: 2 mod :] partition . . cr
```
```output
[ 2 4 ] [ 1 3 ]
```

### Parallel

Run the xt across worker threads over the shared heap; `w` worker threads, `c` items per claim. The bare forms default to `num-cores` workers and claim 1. The worker contract: the xt produces fresh values — reading shared inputs is fine, writing into them from several workers is an unguarded race — and it must not print (workers share one stdout; print from the coordinator after the join), nor may it start another parallel region — `pmap`/`pfilter`/`pmap-reduce` called inside a worker raise a clean error, so a worker uses `map`/`filter`/`reduce` for inner iteration. Allocation of any type inside a worker is unrestricted — strings, arrays, sets, frames, matrices, segments, interned symbols all take the per-thread path. A faulting xt (a throw, wrong arity, out-allocating the region) aborts the whole region and raises a clean error, never a partial result. Each worker's logic-variable bindings are private, so unification inside a parallel quotation is local to its worker and does not compose into a shared search.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `pmap` | `( arr xt -- arr )` | Apply xt to each element across worker threads, results in input order (num-cores workers, claim 1) | 2 + n·xt | `1a(n)` | O(n·xt / w) |
| `pmap-ext` | `( arr w c xt -- arr )` | Apply xt to each element across worker threads, results in input order, with explicit worker count `w` and items-per-claim `c` | 2 + n·xt | `1a(n)` | O(n·xt / w) |
| `pfilter` | `( arr pred -- arr )` | Keep elements where pred is truthy, run across worker threads, order preserved (num-cores workers, claim 1) | 2 + n·xt | malloc(n) flags + `1a(k)` | O(n·xt / w) |
| `pfilter-ext` | `( arr w c pred -- arr )` | Keep elements where pred is truthy, run across worker threads, order preserved, with explicit worker count `w` and items-per-claim `c` | 2 + n·xt | malloc(n) flags + `1a(k)` | O(n·xt / w) |
| `pmap-reduce` | `( arr id map-xt combine-xt -- val )` | Fused parallel map+fold; `combine-xt` must be associative with `id` as neutral element | 2 + n·xt | per-worker partials | O(n·xt / w) |
| `pmap-reduce-ext` | `( arr w c id map-xt combine-xt -- val )` | Fused parallel map+fold with explicit worker count `w` and items-per-claim `c`; `combine-xt` must be associative with `id` as neutral element | 2 + n·xt | per-worker partials | O(n·xt / w) |
| `num-cores` | `( -- n )` | Online CPU count | 1 | none | O(1) |

```forth pmap
[ 1 2 3 4 ] [: dup * :] pmap . cr
```
```output
[ 1 4 9 16 ]
```

```forth pmap-ext
[ 1 2 3 4 ] 2 1 [: 10 * :] pmap-ext . cr
```
```output
[ 10 20 30 40 ]
```

```forth pfilter
[ 1 2 3 4 5 ] [: 2 mod 0= :] pfilter . cr
```
```output
[ 2 4 ]
```

```forth pfilter-ext
[ 1 2 3 4 5 ] 2 1 [: 3 > :] pfilter-ext . cr
```
```output
[ 4 5 ]
```

```forth pmap-reduce
[ 1 2 3 4 ] 0 [: dup * :] ' + pmap-reduce . cr
```
```output
30
```

```forth pmap-reduce-ext
[ 1 2 3 4 ] 2 1 0 ' identity ' + pmap-reduce-ext . cr
```
```output
10
```

```forth num-cores
num-cores 0 > . cr
```
```output
1
```

---

## Delimited continuations

The substrate for exceptions, coroutines, generators. See `docs/continuations.md`. `L` = captured return-stack length.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `reset` | `( -- )` | Push a unique mark on the return stack, delimiting the captured region | 1 | none | O(1) |
| `shift` | `( -- k )` | Capture the return-stack slice up to the nearest `reset`, push k, and unwind to the `reset`'s caller the way `shift-with` does, so the slice may span nested `execute` calls and combinator bodies. The rest of the shifting word's body belongs to k and runs on `resume`, not at capture | L | `1o` (cont) | O(L) |
| `shift-with` | `( xt -- )` | Capture as `shift`, then run xt in the outer context with k on the stack before unwinding | L + xt | `1o` (cont) | O(L + xt) |
| `resume` | `( k -- … )` | Pop k and re-enter it (multi-shot — the continuation object survives, so a retained copy can be resumed again); pushes whatever the resumed code yields | L + resumed | none | O(L + resumed) |
| `throw` | `( exc -- )` | Unwind to the nearest exception prompt, leaving `exc 1` (what `catch` consumes); with no enclosing prompt it is an interpreter error, `uncaught exception: <value>`, the trace captured at the throw site. The prompt search skips locals regions, so local slots are never read as prompts | L | none | O(L) |
| `catch` | `( xt -- result 0 \| exc 1 )` | exceptions.telic: run xt; `(result 0)` on success, `(exc 1)` on a `throw` **or** an interpreter error (an error frame `{ :message :trace }` becomes the exception value) | — | cont if thrown; `1f` + `2s` on a caught interpreter error | O(xt) |
| `try-catch` | `( normal-xt err-xt -- … )` | exceptions.telic: run normal-xt; on a `throw` or interpreter error, run err-xt with the exception (the `{ :message :trace }` error frame, for an interpreter error) on the stack | — | cont if thrown; `1f` + `2s` on a caught interpreter error | O(normal-xt) |
| `ensure` | `( body-xt cleanup-xt -- … )` | exceptions.telic: run cleanup-xt (stack-neutral) whether body-xt returns normally or throws/errors, then re-raise on the throw path | — | cont if thrown | O(body-xt) |
| `expect` | `( flag -- )` | test.telic: pass silently when flag is truthy; else throw `expectation was false` | — | `1s` on fail | O(1) |
| `expect=` | `( actual expected -- )` | test.telic: pass when `actual = expected` (deep structural `=`); else throw `expected <expected>, got <actual>` | — | `1s` on fail | O(n) |
| `expect-near` | `( actual expected tolerance -- )` | test.telic: pass when `\|actual − expected\| <= tolerance`; else throw the expected-range message | — | `1s` on fail | O(1) |
| `expect-throws` | `( xt -- )` | test.telic: run xt under `catch`; pass iff it throws, else throw `expected a throw` | — | cont if thrown | O(xt) |
| `test` | `( name xt -- )` | test.telic: run xt under `catch`; print `ok <name>` or `FAIL <name>: <reason>` (a runtime error's `:message`, else the thrown value), tally it, restore the stack, continue past a failure | — | prints | O(xt) |
| `test-report` | `( -- )` | test.telic: print `<n> passed, <m> failed`; throw when any failed so a program-file run exits nonzero | — | prints | O(1) |
| `new-tests` | `( -- )` | test.telic: zero the passed and failed counters `test` tallies into, so the next `test-report` covers only the tests run after it — one file's independent groups, or a re-run suite in a session | — | none | O(1) |
| `with-db` | `( path body-xt -- … )` | exceptions.telic: `db-open` the path, run body-xt `( db -- … )` with the handle, `db-close` on either exit | — | 1 db + cont if thrown | O(body-xt) |
| `with-stream` | `( stream body-xt -- … )` | exceptions.telic: run body-xt `( stream -- … )` over an already-open stream, `close` it on either exit | — | cont if thrown | O(body-xt) |

```forth reset
: two-step reset 1 . shift 2 . cr ;
two-step "mid" . cr resume "end" . cr
```
```output
1 mid
2
end
```

```forth shift
: two-step reset 1 . shift 2 . cr ;
two-step "mid" . cr resume "end" . cr
```
```output
1 mid
2
end
```

```forth shift-with
: risky reset "a" . [: drop "b" . cr :] shift-with "c" . cr ;
risky
```
```output
a b
```

```forth resume
: two-step reset 1 . shift 2 . cr ;
two-step "mid" . cr resume "end" . cr
```
```output
1 mid
2
end
```

```forth throw
: catch-demo [: "boom" throw :] catch if "caught" . . cr then ; catch-demo
```
```output
caught boom
```

```forth catch
[: 42 :] catch . . cr
```
```output
0 42
```

```forth try-catch
[: 1 0 / :] [: :message @ . cr :] try-catch
```
```output
division by zero
```

```forth ensure
[: "body" . :] [: "cleanup" . :] ensure cr
```
```output
body cleanup
```

```forth expect
1 expect "ok" . cr
```
```output
ok
```

```forth expect=
2 2 expect= "same" . cr
```
```output
same
```

```forth expect-near
3.14 PI 0.01 expect-near "near" . cr
```
```output
near
```

```forth expect-throws
[: "x" throw :] expect-throws "threw" . cr
```
```output
threw
```

```forth test
new-tests "adds" [: 3 4 + 7 expect= :] test test-report
```
```output
ok adds
1 passed, 0 failed
```

```forth test-report
new-tests "adds" [: 3 4 + 7 expect= :] test test-report
```
```output
ok adds
1 passed, 0 failed
```

```forth new-tests
new-tests "adds" [: 3 4 + 7 expect= :] test test-report
```
```output
ok adds
1 passed, 0 failed
```

```forth with-db
":memory:" [: "create table t(x)" [ ] db-exec . :] with-db cr
```
```output
0
```

```forth-noexec with-stream
"echo hi" run :out @ [: read print :] with-stream
```
```output
hi
```

---

## Generators

Coroutines over the continuation substrate: a producer `yield`s values one at a time and a driver `resume`s it for the next. All generators.telic on `shift`/`reset`/`resume`. `L` = captured return-stack length per step.

A producer drops nothing after `yield`. The word does not consume the value it emits, and what is on the stack when the producer resumes is whatever the driver left there: under `gen-take` the emitted value itself, since that word gathers the run of them into an array at the end; under `gen-each` the `:gen-end` sentinel, the consumer having already taken the value. A `drop` after `yield` therefore removes a collected value under `gen-take` (`count N out of range`) and removes the sentinel under `gen-each`, which ends the iteration early and silently.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `yield` | `( v -- … )` | generators.telic: emit v to the driver and suspend. The driver decides what is on the stack when the producer resumes, so a producer loop drops nothing after `yield` (see the note above the table) | L | `1o` (cont) | O(L) |
| `start-generator` | `( producer -- value generator )` | generators.telic: run producer to its first `yield`; leaves the yielded value and a resumable continuation | L | `1o` (cont) | O(producer to first yield) |
| `gen-take` | `( producer count -- array )` | generators.telic: the first `count` values the producer yields, collected into an array | — | `1a(count)` + cont/step | O(count · L) |
| `gen-each` | `( producer consumer -- )` | generators.telic: run consumer on each value the producer yields until the producer finishes (a `:gen-end` sentinel marks exhaustion) | — | cont/step | O(values · consumer) |

```forth yield
: nums 1 yield 2 yield ; ' nums 2 gen-take . cr
: countdown-gen 3 to remaining begin remaining 0 > while remaining yield remaining 1- to remaining repeat ;
' countdown-gen 3 gen-take . cr
: naturals 0 to natural begin natural yield natural 1+ to natural again ;
' naturals 4 gen-take . cr
```
```output
[ 1 2 ]
[ 3 2 1 ]
[ 0 1 2 3 ]
```

```forth start-generator
[: 5 yield drop :] start-generator drop . cr
```
```output
5
```

```forth gen-take
: odds 1 yield 3 yield 5 yield ; ' odds 3 gen-take . cr
```
```output
[ 1 3 5 ]
```

```forth gen-each
: pair-gen 10 yield 20 yield ; ' pair-gen [: . :] gen-each cr
```
```output
10 20
```

---

## Logic

Logic variables, unification, and committed choice, built on the trail and a `PROMPT_CHOICE` prompt. The primer behind these words — unknowns and substitutions, unification, the trail, search, reification, and the fact database as a worked application — is docs/logic.md. A logic var is always created explicitly: `lvar` pushes a fresh one, `lvar to x` names a persistent global (`to` auto-creates the global at the top level), and a `?` prefix in a locals list (`| ?x |`) declares a fresh per-call local. Capitalizing logic-var names (`X`, `Hs`) is stylistic convention; case carries no meaning. `unify` records every binding on the trail; a `unify` mismatch or an explicit `fail` backtracks to the nearest `amb`. A trailing `rest` in an array pattern, `[ H T rest ]`, is the `[H|T]` head/tail split under `unify`. To keep a result past backtracking, snapshot it with `copy` (fresh vars) or `reify` (canonical `:_N`). A **goal** is any quotation run for success or failure: it either completes, leaving its results, or fails — through an explicit `fail` or a `unify` mismatch — and failure is a control transfer to the nearest choice point, not a returned boolean. That is the contract `choose`, `solutions`, and `take-solutions` expect, and what separates a goal from `filter`'s predicate, which must always return and answer 1/0. A logic var prints by the name of a variable that holds it — `?x` while free (the `?` marks it unbound, matching the `| ?x |` declaration form), `x=value` once bound — or `_N` when anonymous; an anonymous bound var prints its value.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `lvar` | `( -- v )` | Push a fresh, unbound logic variable | 2 | `1 lvar` | O(1) |
| `_` | `( -- wild )` | The anonymous wildcard — unifies with anything, binds nothing, allocates nothing (a constant, not a fresh var) | 2 | none | O(1) |
| `unify` | `( a b -- term )` | Unify a and b, binding logic vars (recorded on the trail) so the two match, then leave the dereffed left term. Atoms by value; arrays element-wise, a trailing `rest` pattern taking the remaining elements; frames as open records — shared keys must unify, extra keys on either side allowed. A `_` on either side matches anything and binds nothing. On a mismatch, `fail`s. | n | none | O(n) |
| `~` | `( a b -- term )` | Unify a and b, binding logic vars (recorded on the trail) so the two match, then leave the dereffed left term; atoms by value, arrays element-wise with a trailing `rest` pattern taking the remaining elements, frames as open records; `_` on either side matches anything and binds nothing; on a mismatch, `fail`s | n | none | O(n) |
| `deref` | `( v -- val )` | Follow a logic var's binding chain to the first non-variable value (v itself if unbound). Shallow — a returned structure still has bound vars inside; for a fully resolved snapshot use `reify` or `copy` | d | none | O(d) |
| `rest` | `( var -- pattern )` | Wrap a logic variable or `_` as a rest pattern. As the last element of an array pattern under `unify`, `matches?`, `unify?`, or `case`/`of`, it takes the other side's remaining elements as a fresh array: `[ H T rest ]` binds H to the first element and T to the rest, `[ _ _ rest ]` matches any nonempty array, `[ _ rest ]` any array at all. Prefix elements unify pairwise; a subject shorter than the prefix is a mismatch. A rest anywhere but last, or on both sides, errors; the pattern is not serializable; `copy` gives it a fresh variable | 2 | `1a(n − prefix)` on binding | O(n) |
| `?` | `( v -- val )` | logic.telic: follow a logic var's binding chain to the first non-variable value (v itself if unbound); shallow | d | none | O(d) |
| `amb` | `( xt1 xt2 -- … )` | Run xt1; if it fails (a `unify` mismatch or `fail`), roll its bindings back through the trail and run xt2. Commits to the first branch that succeeds. | xt1 | none | O(xt1 + xt2) |
| `fail` | `( -- )` | Backtrack to the nearest enclosing `amb`, failing the current branch; with no enclosing `amb`, an error | 1 | none | O(L) |
| `choose` | `( items goal -- )` | logic.telic: run the goal with each element of the array in turn, committing to the first for which it succeeds; `fail` if none do, and on an empty array | n·goal | one curried token per element tried | O(n·goal) |
| `solutions` | `( items goal -- arr )` | logic.telic: the goal's value for every element of the array it succeeds on, in order. Each answer is snapshotted (fresh vars) before the search moves on, and every binding rolls back, so nothing stays bound afterward | n·goal | `1a` + a copy per answer | O(n·goal) |
| `take-solutions` | `( items goal n -- arr )` | logic.telic: the first n of the goal's values for the array's elements it succeeds on, in order; once n are collected the goal no longer runs (the remaining elements are walked only to unwind), and every binding rolls back so nothing stays bound afterward | n·goal | `1a` + a copy per answer | O(n·goal) |
| `matches?` | `( a b -- flag )` | Non-destructive unify test: mark the trail, unify a and b, roll the trail back, push whether they unified. Leaves no bindings and never backtracks, so it composes in straight-line code | n | none | O(n) |
| `unify?` | `( a b -- flag )` | Committed unify test: on success the bindings stay and it answers true; on a mismatch the trail rolls back and it answers false, never backtracking. `case`/`of` dispatch through it | n | none | O(n) |

```forth lvar
lvar dup 5 ~ drop ? . cr
```
```output
5
```

```forth _
[ 1 2 ] [ _ 2 ] matches? . cr
```
```output
1
```

```forth unify
lvar to Q [ 1 Q ] [ 1 2 ] unify . cr
```
```output
[ 1 Q=2 ]
```

```forth ~
[ 1 2 ] [ 1 2 ] ~ . cr
```
```output
[ 1 2 ]
```

```forth deref
lvar dup 7 ~ drop deref . cr
```
```output
7
```

```forth rest
lvar to Head  lvar to Tail
[ 1 2 3 ] [ Head Tail rest ] ~ drop Head ? . Tail ? . cr
[ 4 5 ] [ _ _ rest ] matches? . [ ] [ _ _ rest ] matches? . cr
```
```output
1 [ 2 3 ]
1 0
```

```forth ?
lvar dup 9 ~ drop ? . cr
```
```output
9
```

```forth amb
[: 1 :] [: 2 :] amb . cr
```
```output
1
```

```forth fail
[: fail :] [: "fallback" :] amb . cr
```
```output
fallback
```

```forth choose
[ 1 2 3 ] [: dup 2 < if fail then . cr :] choose
```
```output
2
```

```forth solutions
[ 1 2 3 4 ] [: dup 2 mod if fail then dup * :] solutions . cr
[ { :n 1 :ok :y } { :n 2 :ok :n } { :n 3 :ok :y } ]
[: { :ok :y } ~ :n @ :] solutions . cr
```
```output
[ 4 16 ]
[ 1 3 ]
```

```forth take-solutions
[ 1 2 3 4 5 ] [: dup * :] 3 take-solutions . cr
```
```output
[ 1 4 9 ]
```

```forth matches?
[ 1 _ ] [ 1 5 ] matches? . cr
```
```output
1
```

```forth unify?
lvar to W
[ 1 W ] [ 1 5 ] unify? . W ? . cr
```
```output
1 5
```

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

The auto-fuser also collapses a comparison immediately before a branch — `= if`, `> while`, `0= until` — into a single compare-and-branch instruction (shown by `see-compiled` as `(=0branch)`, `(>0branch)`, and the like). These are internal and never typed; the source stays the plain comparison followed by the control word.

Stack reads fuse too, so a body reading parked values costs the same as one reading locals. A literal depth before `pick` becomes one op (`(pick.n) 2`), and a literal count before `array` becomes one op that gathers without pushing the count (`2 array` → `(array.lit) 2`); a `pick` immediately before an unsafe float op becomes a depth-addressed arithmetic op that reads the slot in place (`2 pick f+` → `(f+.d) 2`, and `over f+` → `(f+.d) 1`); and two picks feeding `@i` become a single indexed read (`3 pick 2 pick @i` → `(@i.dd) 3 1`, the index operand adjusted for the copy the first pick would have pushed). `@e` fuses in every shape `@i` does (`(@e.lit.l0)`, `(@e.swap.l0)`, `(@e.d)`, `(@e.dd)`, `(gather.e.l0)`, and the in-place step ops), so a matrix read and written as a flat buffer costs what an array does. `see-compiled` shows all three, and they are what a `times` / `i-times` / `fold-times` body compiles to when it reads values a caller parked below the combinator's operands.

Word-locals fuse the same way. A float op over two locals, or a local and a float literal, becomes one instruction that reads the slots directly (`(ll*0)`, `(ll.lit+0)`); a following `to name` fuses into it, so `zr zr f* to zr2` is a single instruction that reads two slots and writes a third (`(ll*0!)`). An op taking one operand from the stack and one from a local fuses with its store the same way — `ci f+ to zi` is one instruction (`(sl+!0)`) — and when the destination is also the operand, `total x f+ to total` becomes an accumulate (`(acc+0)`). `++ name` / `f++ name` are the one-instruction forms of incrementing a local, so `iter 1+ to iter` written as `f++ iter` compiles to `(local f+!0)`. Sources are read before the destination is written, so a slot may be both.

```forth vvf+
variable a 3 to a variable b 4 to b
: sum-ab vvf+ a b ; sum-ab . cr
```
```output
7
```

```forth vvf-
variable a 3 to a variable b 4 to b
: diff-ab vvf- a b ; diff-ab . cr
```
```output
-1
```

```forth vvf*
variable a 3 to a variable b 4 to b
: prod-ab vvf* a b ; prod-ab . cr
```
```output
12
```

```forth vvf/
variable a 3 to a variable b 4 to b
: quot-ab vvf/ a b ; quot-ab . cr
```
```output
0.75
```

```forth vf+
variable a 3 to a
: plus-a vf+ a ; 10 plus-a . cr
```
```output
13
```

```forth vf-
variable a 3 to a
: minus-a vf- a ; 10 minus-a . cr
```
```output
7
```

```forth vf*
variable a 3 to a
: times-a vf* a ; 10 times-a . cr
```
```output
30
```

```forth vf/
variable a 3 to a
: over-a vf/ a ; 12 over-a . cr
```
```output
4
```

```forth vfsq
variable a 3 to a
: sq-a vfsq a ; sq-a . cr
```
```output
9
```

```forth vfneg
variable a 3 to a
: neg-a vfneg a ; neg-a . cr
```
```output
-3
```

```forth vfabs
variable a -5 to a
: abs-a vfabs a ; abs-a . cr
```
```output
5
```

```forth vfsqrt
variable a 9 to a
: root-a vfsqrt a ; root-a . cr
```
```output
3
```

```forth vfexp
variable a 0 to a
: exp-a vfexp a ; exp-a . cr
```
```output
1
```

```forth vflog
variable a 100 to a
: log-a vflog a ; log-a . cr
```
```output
2
```

```forth vfsin
variable a 0 to a
: sin-a vfsin a ; sin-a . cr
```
```output
0
```

```forth vfcos
variable a 0 to a
: cos-a vfcos a ; cos-a . cr
```
```output
1
```

```forth vftan
variable a 0 to a
: tan-a vftan a ; tan-a . cr
```
```output
0
```

```forth vftanh
variable a 0 to a
: tanh-a vftanh a ; tanh-a . cr
```
```output
0
```

```forth vvf*+
variable b 4 to b variable c 10 to c
: fma-bc vvf*+ b c ; 2 fma-bc . cr
```
```output
18
```

```forth vvf*-
variable b 4 to b variable c 10 to c
: fms-bc vvf*- b c ; 2 fms-bc . cr
```
```output
2
```

---

## REPL and introspection

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `words` | `( -- )` | List all non-internal words in aligned columns, grouped by section, alphabetical within a group: words defined this session first, then words loaded from a library file, then the reference sections in alphabetical order by section name, then units and undocumented | dict scan | none | O(\|dict\| log \|dict\|) |
| `variables` | `( -- arr )` | core.telic: one `{ :name :value :type }` frame per global (`variable`-declared or `to`-auto-created), oldest first — the name symbol, the live value (shared reference for collections), and its type as a symbol | dict scan | `1a` + one frame per global | O(\|dict\|) |
| `vars` | `( -- )` | repl.telic: pretty-print every global, one `{ :name :value :type }` frame per block | dict scan + print | `1a` + frames | O(\|dict\|) |
| `telic` | `( -- )` | Print the telic logo and the interpreter version | print | none | O(1) |
| `telic-version` | `( -- str )` | The interpreter version as a string — for a program that reports its runtime | 1 | `1s` | O(1) |
| `apropos` | `( str -- )` | Print every word whose name or reference summary contains s (case-insensitive): name, stack effect, summary per line; a word defined by a `load` matches by name or by the summary of the comment above its definition and prints that effect and summary, and other session words match by name | table scan | none | O(entries) |
| `see` | `( xt -- )` | Print a word's source (`: name … ;`), a quotation's `[: … :]` text from its recorded span, or `variable`/`symbol`/primitive form; a curried token prints its bound values, then its target | dict scan | none | O(\|dict\|) |
| `see>string` | `( xt -- str )` | A word's source (`: name … ;`), a quotation's `[: … :]` text, or a `variable`/`symbol`/primitive form (a curried token's bound values then its target), returned as a string (trailing newline stripped) | dict scan | `1o` | O(\|dict\|) |
| `callers` | `( xt -- arr )` | The names, as strings in definition order, of the colon definitions whose compiled bodies reference the word: a call or tail call, an `' word` literal, a primitive's op, a variable's read or store, a unit or deferred word's use. A quotation's body counts for the definition enclosing it; a recursive word lists itself. `xt` must name a dictionary word — a quotation or curried token errors | dict scan × body | `1a` + strings | O(\|dict\|²) |
| `edit` | `( "name" -- )` | Parse the following word and open its source — what `see` prints — in `$EDITOR` (`vi` when unset) on a temporary `.telic` file, waiting until the editor exits; a saved change runs the edited text as a `load`, so the definition is replaced, and an unchanged file leaves the word as it was. A name not yet defined opens as `: name` on one line and `;` on the next. Needs a terminal on stdin and stdout and an editor exiting with status 0; errors otherwise | editor | temp file | — |
| `see-compiled` | `( xt -- )` | Disassemble a colon definition's compiled cells; a curried token prints its bound values, then disassembles its target | body scan | none | O(body) |
| `see-compiled>string` | `( xt -- str )` | The disassembly of a colon definition's compiled cells (a curried token's bound values then its target), returned as a string (trailing newline stripped) | body scan | `1o` | O(body) |
| `see-tree` | `( xt -- )` | Disassemble a colon definition's compiled cells, but each colon-word call is expanded inline, indented two spaces, recursively down to primitives; recursive calls print as `name ...` | body scan | none | O(expanded body) |
| `see-tree>string` | `( xt -- str )` | The disassembly with each colon-word call expanded inline, indented two spaces, recursively down to primitives (recursive calls as `name ...`), returned as a string (trailing newline stripped) | body scan | `1o` | O(expanded body) |
| `man` | `( xt -- fr )` | Frame of a word's reference entry (`:word :effect :summary`, plus `:ops :alloc :order` for runtime words); a unit word synthesizes its entry from the unit's definition (`unit: m × 1000`); a word a `load` defined under a `( a b -- c ) \\ summary` comment answers `{ :word :effect :summary }` from that comment, continuation lines starting with `\\` extending the summary; `null` otherwise | dict scan + log n | `1o` + strings | O(\|dict\|) |
| `help` | `( "name" -- )` | repl.telic: parse the next word and print its reference entry, or the entry `man` builds from the comment above a loaded definition; bare `help` (no name on the line) prints a starter cheat sheet, and an unknown name prints `unknown word: <name>` without erroring | dict scan + log n | `1o` + strings + print | O(\|dict\|) |
| `gc` | `( -- )` | Force a mark-sweep now | walks stacks + dict + roots, frees unmarked | none | O(objects + dict) |
| `gauges` | `( -- fr )` | repl.telic: the interpreter's resource readings as a frame of six frames; memory is in MiB (dictionary pools in KiB), processor time in s, counts are floats, and a `[ used capacity ]` pair is an array. `:dictionary`: `:cells`, `:name-pool`, `:source-pool`, `:symbol-pool`, `:quotations`, `:word-locations`, `:cell-lines`, `:loaded-files` as pairs, plus `:words`, `:session-words` (defined since the embedded library), `:symbols`. `:heap`: `:arena` `[ used reserved ]`, `:live` (matrix, segment and continuation payload bytes, the ones that drive the collection trigger), `:gc-threshold`, `:memory-headroom` (until the next collection), `:objects` `[ live table-size ]` (live is claimed handles minus the free list), `:handles-claimed` `[ claimed table-size ]` (the high-water mark; a collection refills the free list rather than lowering it), `:max-objects`, `:free-handles`, `:handle-headroom` `[ unclaimed trigger ]` (a collection is requested when unclaimed falls under trigger), `:pairs` `[ live table-size ]`, `:collections`. `:stacks`: `:data`, `:return`, `:side`, `:calls`, `:trail`, `:logic-vars`, `:roots`, each `[ depth capacity ]`. `:resources`: `:databases`, `:regex-cache`, `:workers`, each `[ in-use capacity ]`. `:computer`: `:cpu-count`, `:physical-memory`, the load averages `:load-1` `:load-5` `:load-15`, and this process's `:user-time`, `:system-time`, `:max-rss` (peak resident memory), `:minor-faults`, `:major-faults`, `:voluntary-switches`, `:involuntary-switches` — `null` under wasm, which has no such calls. `:session`: `:line`, `:interactive`, `:load-depth`, `:gc-disabled`, `:tracing` | dict walk + symbol scan | `1fr` × 6 + pairs | O(words + symbols) |
| `print-gauges` | `( -- )` | repl.telic: print the readings of `gauges` that move during a run as a six-row table: live memory, objects, pairs, and arena against their capacities with a percentage; collections with their rate; data, return, and call depth; trail and logic-variable depth; CPU percentage, peak resident memory, major faults per second, load average against the core count, involuntary switches per second; then the interval since the previous call and the clock. Rates are computed against the previous `print-gauges` call, so a first call shows them as 0. On a terminal the labels are dim and a percentage or rate is yellow above 60% of its capacity, red above 85% | gauges + format | strings | O(words + symbols) |
| `gauges>rows` | `( g -- rows )` | repl.telic: the six row strings `print-gauges` prints, from a `gauges` frame — live or saved: the rates and the interval compare against the frame given on the previous call, the sample clock is the frame's `:sampled-at` (monotonic seconds) when present, else `now`, and the footer's clock its `:published-at` when present, else `wall-now`; ink directives as on a tty | gauges>rows + format | strings | O(words + symbols) |
| `after-entry` | `( -- )` | repl.telic: the REPL hook — a deferred word the interactive loop runs after every entry's report, with the data stack as the entry left it; the default target does nothing, `' xt embodies after-entry` installs another (lib/gauges-view.telic installs its publisher). An error in the hook prints `after-entry: <message>` and leaves the stack as it was | target | target | target |
| `tick-every` | `( seconds -- )` | repl.telic: arm a periodic timer (`setitimer`; 0 disarms, a negative interval errors): every `seconds` the running code is interrupted at its next op boundary — or inside a `sleep`, which then resumes for its remaining time — to run `on-tick` once. Nothing ticks until armed; wasm has no timer, so the word does nothing there | 2 | none | O(1) |
| `on-tick` | `( -- )` | repl.telic: the timer hook — a deferred word run once per `tick-every` tick, with the data stack as the running code left it, so a target must leave the stack as it found it; the default target does nothing, `' xt embodies on-tick` installs another. An error in the hook prints `on-tick: <message>` to stderr and the running code continues | target | target | target |
| `alloc-stats` | `( -- )` | Print and reset the allocation counters (`lvars=… arrays=…`) since the last call, or since the embedded library finished loading — its own allocations are not counted | 2 | none | O(1) |
| `bye` | `( -- )` | Exit the process with status 0 | — | — | — |
| `halt` | `( code -- )` | Exit the process with the given code as its exit status | — | — | — |
| `now` | `( -- f )` | Monotonic-clock seconds as a float | 1 | none | O(1) |
| `sleep` | `( seconds -- )` | Block for the given float seconds (sub-second supported) | blocks | none | O(1) |
| `timed` | `( xt -- … )` | Run xt, print its elapsed monotonic-clock seconds, then pass through whatever it left on the stack | 2 + xt + print | none | O(xt) |
| `trace` | `( xt patterns -- … )` | Run xt printing one line per op to stderr before it executes: the op as `see-compiled` shows it, then `\|` and the data stack (its top four values, deepest first, `…` when deeper). `patterns` is an array of regex strings: the first selects ops by their op text (`^sort`); any further ones keep a selected op only when one matches the whole line (`[ "^sort" "\\| .*nan" ]`); `[ ]` prints every line. Patterns see every stack value in full; the printed line shows a value longer than 48 characters cut to that width with `…`, except the value holding the match. Filtered lines print set apart by blank lines, the matched text highlighted on a terminal. Every op the run reaches is covered, and a word or quotation a combinator runs is announced by name before its body. The traced code's own output interleaves on stdout. A non-string element or an invalid pattern errors before xt runs | per op: render + match + print | none | O(ops × patterns) |

```forth-noexec words
words
```
```output
Stack manipulation:
  -rot      2drop     2dup      clear     depth     drop      dup
  identity  nip       over      pick      roll      rot       swap
Arithmetic:
...
```

```forth variables
42 to answer-var variables [: :name @ :] map dup size 1- @i . cr
```
```output
:answer-var
```

```forth-noexec vars
7 to speed vars
```
```output
{
  :name :speed
  :value 7
  :type :float
}
```

```forth-noexec telic
telic
```
```output
                                          telic 0.26.0
                              https://github.com/free-variation/telic
```

```forth telic-version
telic-version "\d+\.\d+\.\d+" has? . cr
```
```output
1
```

```forth-noexec apropos
"kendall" apropos
```
```output
cor              ( xs ys -- fr )          statistics.telic: Kendall tau-b with a 500-replicate bootstrap CI, as { :estimate :se :bias :ci-low :ci-high }
correlation-kendall ( xs ys -- f )           Kendall tau-b: concordant minus discordant pairs over sqrt of tie-corrected pair counts, via one (x,y) sort and a merge-sort exchange count; NaN when all x or all y are tied; errors on length mismatch or fewer than 2 elements
```

```forth see
: sq-see dup * ; ' sq-see see
```
```output
: sq-see dup * ;
```

```forth see>string
: sq-see2 dup * ; ' sq-see2 see>string print cr
```
```output
: sq-see2 dup * ;
```

```forth callers
: sq-called dup * ;
: uses-sq-called 3 sq-called ;
: maps-sq-called [ 1 2 ] [: sq-called :] map drop ;
' sq-called callers . cr
```
```output
[ "uses-sq-called" "maps-sq-called" ]
```

```forth-noexec edit
edit sq
```
```output
```

```forth see-compiled
: sc-demo 1.5 2.5 f+ ; ' sc-demo see-compiled
```
```output
: sc-demo   \ 5 cells
 0: (lit) 1.5
 2: (lf+) 2.5
 4: exit
;
```

```forth see-compiled>string
: sc-demo2 1.5 2.5 f+ ; ' sc-demo2 see-compiled>string print cr
```
```output
: sc-demo2   \ 5 cells
 0: (lit) 1.5
 2: (lf+) 2.5
 4: exit
;
```

```forth see-tree
: st-inner 1 ; : st-outer st-inner 2 * ; ' st-outer see-tree
```
```output
: st-outer
  0: st-inner:
    0: (lit) 1
    2: exit
  2: (lit) 2
  4: *
  5: exit
;
```

```forth see-tree>string
: st-inner 1 ; : st-outer2 st-inner 3 * ; ' st-outer2 see-tree>string print cr
```
```output
: st-outer2
  0: st-inner:
    0: (lit) 1
    2: exit
  2: (lit) 3
  4: *
  5: exit
;
```

```forth man
' dup man :effect @ . cr
```
```output
( a -- a a )
```

```forth help
help nip
```
```output
nip ( a b -- b )
  Drop the second item, keeping the top
  ops 1, alloc none, O(1)

  > 1 2 nip . cr
  2
```

```forth gc
gc "collected" . cr
```
```output
collected
```

```forth gauges
gauges dup keys [: "{0}" format :] sort-by . :stacks @ :data @ . cr
```
```output
[ :computer :dictionary :heap :resources :session :stacks ] [ 0 65536 ]
```

```forth gauges>rows
gauges gauges>rows size . cr
```
```output
6
```

```forth-noexec after-entry
: report-depth depth "depth {0}" format . cr ;
' report-depth embodies after-entry
```
```output
```

```forth-noexec tick-every
variable ticks 0 to ticks
: count-tick | ^ticks | ++ ticks ;
' count-tick embodies on-tick
1 tick-every 2.5 sleep 0 tick-every
ticks . cr
```
```output
2
```

```forth-noexec on-tick
' publish-gauges-entry embodies on-tick
```
```output
```

```forth-noexec print-gauges
print-gauges
```
```output
live     33.7 / 256 MiB       13% │ data     2        cpu      81%
objects  1.9M / 2.1M          89% │ return   35       rss      612 MiB
pairs    660.2k / 1.0M        63% │ calls    1 / 4096 faults   0/s
gc       3  0.1/s                 │ trail    0        load     2.6 / 16
arena    464.1 / 16384 MiB     3% │ lvars    0        switches 4/s
interval 16.9 s at 02:26:34
```

```forth-noexec alloc-stats
alloc-stats
```
```output
lvars=0 arrays=0
```

```forth-noexec bye
bye
```
```output
```

```forth-noexec halt
3 halt
```
```output
```

```forth-noexec now
now . cr
```
```output
1.48012e+06
```

```forth sleep
0 sleep "woke" . cr
```
```output
woke
```

```forth-noexec timed
[: [ 1 2 3 ] ' 1+ map :] timed . cr
```
```output
2.1e-06
[ 2 3 4 ]
```

```forth trace
[: 3 4 + :] [ ] trace . cr
: sq-traced | x | x x * ;
[: 5 sq-traced 2 + :] [ "^sq" ] trace . cr
```
```output
> (lit) 3                 |
> (lit) 4                 | 3
> +                       | 3 4
> exit                    | 7
7

> sq-traced               | 5

27
```

---

## Persistence

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `evaluate` | `( str -- )` | Run the string's characters as source, as if they had been typed at that point: the reader takes its tokens, compile-time words act, definitions enter the dictionary, and the code runs against the same stacks. Unlike loading a file, nothing is recorded for `reload` and an error names only the failing word instead of carrying a `file:line:` prefix. A colon definition works here, where one inside a quotation does not. Errors on an unterminated string literal or definition in the text, and on text at or above the input buffer size | text + run | input buffer copy | O(text + run) |
| `load` | `( str -- )` | Run a source file as if typed; record it for `reload`. Resolves the path as given (relative to the current directory, or absolute); if that open fails, retries relative to the directory of the file that ran the `load`. An error raised while loading is prefixed `file:line: ` (the line of the failing token); a nested `load` locates to the innermost file. Every colon word a load defines remembers its file and the line of each call it compiles, and a runtime error trace prints after each word's name the file and the line that was executing in it, the failing op's line for the innermost frame (`in / ← faulty (/tmp/docs-trace.telic:1)`); words typed at the REPL or given with `-e` carry no location | file read + run | input buffer | O(file) |
| `load-library` | `( name -- )` | core.telic: run `lib/<name>` from beside the telic binary as a source file, so `"plot" load-library` works from any cwd; a name without `.telic` gains it | file read + run | input buffer | O(file) |
| `reload` | `( -- )` | Truncate user state, re-run every loaded file in order | forget + N loads | — | O(Σ files) |
| `save` | `( str -- )` | Write all user words as re-loadable `.telic` source | dict scan + write | file I/O | O(\|user dict\|) |

```forth evaluate
": doubled 2 * ;" evaluate
21 doubled . cr
```
```output
42
```

```forth load
": loaded-word 11 ;" "/tmp/docs-load.telic" write-file "/tmp/docs-load.telic" load loaded-word . cr
```
```output
11
```

```forth load-library
"plot" load-library ' scatter xt? . cr
```
```output
1
```

```forth save
: keep-me 5 ; "/tmp/docs-save.telic" save "/tmp/docs-save.telic" read-file ": keep-me" has? . cr
```
```output
1
```

```forth-noexec reload
reload
```
```output
```

---

## Files and environment

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `read-file` | `( path -- str )` | Read a whole file as one string (byte-safe); errors if it can't be opened | file read | `1o` + buffer | O(file) |
| `write-file` | `( str path -- )` | Create or truncate the file, then write the string's bytes | file write | none | O(\|s\|) |
| `append-file` | `( str path -- )` | Open in append mode, write the string's bytes | file write | none | O(\|s\|) |
| `open-file` | `( path -- stream )` | The file as a read-only stream, so `read-line` / `read` / `read-available` / `wait-readable` / `close` walk a file too large to read whole line by line; errors if it can't be opened | 1 | none | O(1) |
| `file-exists?` | `( path -- bool )` | Whether the path exists; follows symlinks, tests any file type, not just regular files | 1 | none | O(1) |
| `args` | `( -- arr )` | The command-line arguments after the program file, as a string array — everything after the file belongs to the program, options included. A leading `#!` line is skipped | 1 + n | `1a(n)` + `1o` per argument | O(bytes) |
| `env` | `( name -- val )` | Environment variable as a string, or `null` if unset (so set-empty `""` and unset stay distinct) | 1 | `1o` on hit | O(\|val\|) |
| `env!` | `( value name -- )` | Set an environment variable (overwriting); process-wide, so subsequent `start-process` children inherit it — value first, name last | 1 | none | O(1) |
| `cwd` | `( -- path )` | The interpreter's current working directory as a string | 1 | `1o` | O(\|path\|) |
| `binary-dir` | `( -- str )` | The directory holding the running telic binary, symlinks resolved, so an installation's resources are reachable from any cwd; errors on the wasm build (no executable path) | 1 | `1o` | O(\|path\|) |
| `cd` | `( path -- )` | Change the interpreter's working directory; process-wide, so it moves the base for relative file I/O and is inherited by subsequent `start-process` children | 1 | none | O(1) |
| `find-executable` | `( name -- path\|null )` | `io.telic`: the absolute path of `name` on `$PATH` (first directory holding it), or `null` if unset or not found; a name containing `/` matches no bare `PATH` entry, so it answers `null` | split + probe | `1o` per candidate | O(dirs) |
| `list-directory` | `( path -- arr )` | The directory's entry names as an array of strings, `.` and `..` excluded, sorted in ascending order so a listing is the same on every filesystem. Names only, not paths — joining them to the parent is the caller's; errors when the path is missing or is not a directory | n log n | `1a(n)` + `1o` per name | O(n log n) |
| `file-info` | `( path -- fr\|null )` | `io.telic`: `{ :kind :size :modified }` for one path — `:kind` is `:file`, `:directory`, or `:other`, `:size` the byte count, `:modified` the modification time as an instant (a quantity in `s`, whole seconds). Symlinks are followed, so a link to a directory reports `:directory`; a missing path — a dangling symlink included — answers `null` rather than erroring, so `dup null? if … then` guards it | 20 | `1o` + 1 pair | O(1) |
| `make-directory` | `( path -- )` | Create the directory, intermediate components included; silent when it already exists, so it is idempotent. Errors when a component exists as a non-directory or the path is unwritable | d | none | O(d), d = path components |
| `delete-file` | `( path -- )` | Remove the file; errors when it is missing or is a directory | 1 | none | O(1) |
| `delete-directory` | `( path -- )` | Remove the directory; errors unless it exists and is empty. There is no recursive form | 1 | none | O(1) |
| `rename-file` | `( from to -- )` | Rename `from` to `to`, replacing an existing `to` — a move within one filesystem, files and directories alike; errors across filesystems | 1 | none | O(1) |
| `copy-file` | `( from to -- )` | `io.telic`: copy `from`'s bytes to `to`, creating or truncating it; the whole file passes through memory and the mode and timestamps are not carried over. Throws unless `from` is a regular file | file read + write | `1o` + buffer | O(bytes) |
| `touch-file` | `( path -- )` | Set the path's modification time to now, creating an empty file when nothing is there; an existing file keeps its contents — the word never truncates — and a directory is touched in place. Errors when the path is unwritable | 1 | none | O(1) |
| `ls` | `( path -- )` or `( -- )` | repl.telic: print the directory's entries in aligned columns to an 80-column width, a trailing `/` marking each directory, in sorted order; an empty directory prints nothing. Bare `ls` on an empty data stack lists the working directory; with anything on the stack the top value is the path, so `ls` after other work reads that value rather than the cwd | n log n + n stats | `1a(n)` + `1o` per name | O(n log n) |
| `mkdir` | `( path -- )` | `io.telic`: create the directory, intermediate components included; idempotent | d | none | O(d) |
| `rm` | `( path -- )` | `io.telic`: remove the file; one file, never a tree; errors when it is missing or is a directory | 1 | none | O(1) |
| `rmdir` | `( path -- )` | `io.telic`: remove the directory; errors unless it exists and is empty | 1 | none | O(1) |
| `mv` | `( from to -- )` | `io.telic`: rename `from` to `to`, replacing an existing `to`; a move within one filesystem | 1 | none | O(1) |
| `pwd` | `( -- )` | `io.telic`: print the working directory and a newline; symlinks are resolved, so the real path is printed | 1 | `1o` | O(\|path\|) |
| `cat` | `( path -- )` | `io.telic`: write the file's bytes to stdout. Byte-exact: nothing is added, so a file with no closing newline leaves the cursor mid-line | file read + write | `1o` + buffer | O(file) |
| `cp` | `( from to -- )` | `io.telic`: copy `from`'s bytes to `to`, creating or truncating it | file read + write | `1o` + buffer | O(bytes) |
| `touch` | `( path -- )` | `io.telic`: set the path's modification time to now, creating an empty file when nothing is there | 1 | none | O(1) |

`read-file` and `write-file` carry bytes, NUL and invalid UTF-8 included, so
they hold binary as well as text; `save-value` and `load-value` (see Value
serialization) put a whole value graph through them.

```forth read-file
"hello" "/tmp/docs-file.txt" write-file "/tmp/docs-file.txt" read-file . cr
```
```output
hello
```

```forth open-file
"one
two" "/tmp/docs-file.txt" write-file
"/tmp/docs-file.txt" open-file dup read-line . dup read-line . close cr
```
```output
one two
```

```forth write-file
"hello" "/tmp/docs-file.txt" write-file "/tmp/docs-file.txt" read-file . cr
```
```output
hello
```

```forth append-file
"a" "/tmp/docs-app.txt" write-file "b" "/tmp/docs-app.txt" append-file "/tmp/docs-app.txt" read-file . cr
```
```output
ab
```

```forth file-exists?
"/tmp" file-exists? . "/no/such/path" file-exists? . cr
```
```output
1 0
```

```forth args
args . cr
```
```output
[ ]
```

```forth env
"42" "DOCS_VAR" env! "DOCS_VAR" env . cr
"NO_SUCH_VAR_XYZ" env null? . cr
```
```output
42
1
```

```forth env!
"42" "DOCS_VAR" env! "DOCS_VAR" env . cr
```
```output
42
```

```forth cwd
cwd file-exists? . cr
```
```output
1
```

```forth binary-dir
binary-dir file-exists? . cr
```
```output
1
```

```forth cd
cwd "/tmp" cd cwd "tmp" has? . cd cr
```
```output
1
```

```forth find-executable
"sh" find-executable null? 0= . cr
```
```output
1
```

```forth list-directory
"/tmp/docs-fs" make-directory
"a" "/tmp/docs-fs/one.txt" write-file
"bb" "/tmp/docs-fs/two.txt" write-file
"/tmp/docs-fs" list-directory . cr
```
```output
[ "one.txt" "two.txt" ]
```

```forth file-info
"hello" "/tmp/docs-info.txt" write-file
"/tmp/docs-info.txt" file-info dup :kind @ . :size @ . cr
"/tmp/docs-info.txt" file-info@modified 1 s / 0 > . cr
"/tmp/docs-no-such-file" file-info null? . cr
```
```output
:file 5
1
1
```

```forth make-directory
"/tmp/docs-mk/inner" make-directory
"/tmp/docs-mk/inner" make-directory
"/tmp/docs-mk" list-directory . cr
```
```output
[ "inner" ]
```

```forth delete-file
"x" "/tmp/docs-delete.txt" write-file
"/tmp/docs-delete.txt" delete-file
"/tmp/docs-delete.txt" file-exists? . cr
```
```output
0
```

```forth delete-directory
"/tmp/docs-rmdir" make-directory
"/tmp/docs-rmdir" delete-directory
"/tmp/docs-rmdir" file-exists? . cr
```
```output
0
```

```forth rename-file
"moved" "/tmp/docs-from.txt" write-file
"/tmp/docs-from.txt" "/tmp/docs-to.txt" rename-file
"/tmp/docs-to.txt" read-file . cr
"/tmp/docs-from.txt" file-exists? . cr
```
```output
moved
0
```

```forth copy-file
"contents" "/tmp/docs-copy-from.txt" write-file
"/tmp/docs-copy-from.txt" "/tmp/docs-copy-to.txt" copy-file
"/tmp/docs-copy-to.txt" read-file . cr
```
```output
contents
```

```forth touch-file
"/tmp/docs-touch" touch-file
"/tmp/docs-touch" file-exists? . cr
"kept" "/tmp/docs-touch" write-file
"/tmp/docs-touch" touch-file
"/tmp/docs-touch" read-file . cr
```
```output
1
kept
```

```forth ls
"/tmp/docs-ls/inner" mkdir
"note" "/tmp/docs-ls/note.txt" write-file
"a table" "/tmp/docs-ls/data.tsv" write-file
"/tmp/docs-ls" ls
```
```output
data.tsv  inner/    note.txt
```

```forth mkdir
"/tmp/docs-mkdir/inner" mkdir
"/tmp/docs-mkdir" list-directory . cr
```
```output
[ "inner" ]
```

```forth rm
"x" "/tmp/docs-rm.txt" write-file
"/tmp/docs-rm.txt" rm
"/tmp/docs-rm.txt" file-exists? . cr
```
```output
0
```

```forth rmdir
"/tmp/docs-rmdir" mkdir
"/tmp/docs-rmdir" rmdir
"/tmp/docs-rmdir" file-exists? . cr
```
```output
0
```

```forth mv
"carried" "/tmp/docs-mv-from.txt" write-file
"/tmp/docs-mv-from.txt" "/tmp/docs-mv-to.txt" mv
"/tmp/docs-mv-to.txt" read-file . cr
```
```output
carried
```

```forth-noexec pwd
pwd
```
```output
/Users/you/work/telic
```

```forth cat
"two{nl}lines{nl}" format "/tmp/docs-cat.txt" write-file
"/tmp/docs-cat.txt" cat
```
```output
two
lines
```

```forth cp
"duplicated" "/tmp/docs-cp-from.txt" write-file
"/tmp/docs-cp-from.txt" "/tmp/docs-cp-to.txt" cp
"/tmp/docs-cp-to.txt" cat cr
```
```output
duplicated
```

```forth touch
"/tmp/docs-touched" touch
"/tmp/docs-touched" file-exists? . cr
```
```output
1
```

---

## Logging

One word. Filtering by level is the reader's job, so the writer carries no
threshold: every call emits one line, `<iso time> <level> <message>`, the level
in the second field for a reader to cut on. Configuration is environment
variables, as for every adjustable setting: `TELIC_LOG_DIR` names a directory
for a per-run file; unset, lines go to stderr. The first `log` call under
`TELIC_LOG_DIR` derives `dir/telic-log-<datetime>` from that write's time
(`telic-log-2026-09-06T09-14-02Z`, hyphens so the name is filesystem-safe and
sorts), creates the directory, stores the path in `TELIC_LOG_FILE`, and every
later call appends to it. Setting `TELIC_LOG_FILE` yourself before the first
write names the file directly. The library holds no state of its own.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `log` | `( str level -- )` | io.telic: write one line, the UTC stamp, the level symbol by name and the message, to the log file under `TELIC_LOG_DIR`/`TELIC_LOG_FILE`, else to stderr; `level` is any symbol | 20 | `1s` per line | O(\|s\|) |

```forth log
"/tmp/docs-log" "TELIC_LOG_DIR" env!
"fit converged" :info log
"TELIC_LOG_FILE" env read-file "^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ info fit converged$" has? . cr
```
```output
1
```

---

## Subprocesses and streams

A stream (`T_STREAM`) wraps an OS file descriptor — a pipe to a child process, or a file from `open-file` (see Files and environment). `start-process` launches a program directly from an argv array (no shell, so no quoting or injection surface) and returns a frame `{ :pid :in :out :err }` whose `:in`/`:out`/`:err` are streams. The lifecycle is: `write` input → `close` `:in` (sends EOF) → `read` the output → `wait`. `SIGPIPE` is ignored process-wide, so a `write` to a child that has exited returns an error rather than killing the interpreter. Bytes are raw and length-counted, so streams are binary-safe.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `start-process` | `( argv -- proc )` | fork/exec `argv[0]` with `argv` as its arguments; return `{ :pid :in :out :err }` (the three streams are `T_STREAM`) | fork + 3 pipes | `1o` frame + 3 streams | O(argc) |
| `run-result` | `( argv -- frame )` | subprocess.telic: run `argv` to completion and return `{ :out :err :status }`, closing the streams and reaping the child | fork + drain | `1fr` + output strings | O(output) |
| `write` | `( str stream -- )` | Write the string's bytes to the stream; loops over partial writes, retries `EINTR` | write syscalls | none | O(\|s\|) |
| `read` | `( stream -- str )` | Read the stream to EOF into one string | read syscalls | `1o` + buffer growth | O(bytes) |
| `read-line` | `( stream -- str \| null )` | Read up to and including the next `\n` and answer the line without that terminator; a `\r` before it is content and stays, as it does under `"\n" split`. Bytes after the terminator are left in the stream, so `read` on the same stream answers the rest — the word holds no buffer and costs one `read` syscall per byte, for line protocols rather than bulk input. At end of input with nothing accumulated it answers `null`; a final unterminated run of bytes answers as a line, and the call after it answers `null`. Retries `EINTR` | bytes | `1o` + buffer growth | O(bytes) |
| `read-available` | `( stream -- str )` | The bytes already waiting on the stream, without blocking: up to 65536 of them as a string, `""` when none are waiting, `null` at end of input. A zero-timeout `poll` decides, then one `read`. Used with `wait-readable` when one thread serves several streams: `read-line` blocks until its newline arrives, so a writer that flushes a partial line and then computes would stall every other stream, while this word takes what is there and leaves the caller to assemble lines | 1 + bytes | `1o` | O(bytes) |
| `wait-readable` | `( streams seconds -- ready )` | Wait until at least one of `streams` has bytes to read, and answer a new array of those that do — empty when the wait expires first. `seconds` is a float with sub-second granularity: `0` polls without waiting, a negative value waits indefinitely. End of input counts as readable, so a stream whose writer has exited comes back and the read that follows answers `null` instead of blocking. `poll(2)` underneath, retrying `EINTR`; errors on a non-stream element, a closed stream, or more than 256 streams | 1 + n | `1a(k)` | O(n) |
| `close` | `( stream -- )` | Close the fd; closing a child's `:in` sends it EOF. Idempotent; a closed handle stays stale — reading or writing it reports `stream is closed` even after the descriptor number is reissued to another stream. A dropped handle holds its descriptor until process exit; `with-stream` scopes one | 1 syscall | none | O(1) |
| `stdin` | `( -- stream )` | Standard input as a `T_STREAM` over fd 0; `stdin read` reads it whole. (The REPL reads its own program from stdin, so this word suits file-loaded programs.) | 1 | none | O(1) |
| `stdout` | `( -- stream )` | Standard output as a `T_STREAM` over fd 1; `s stdout write` emits | 1 | none | O(1) |
| `stderr` | `( -- stream )` | Standard error as a `T_STREAM` over fd 2; composes with `write`/`close` like any stream | 1 | none | O(1) |
| `stdout>string` | `( xt -- str )` | Run xt with descriptor 1 redirected to an unlinked temporary file, restore the descriptor, and answer everything xt wrote — a raw `stdout write` included, the redirect being at the descriptor rather than in the printing words. Whatever xt leaves on the stack stays, the string on top; `stderr` is untouched, and a child from `start-process` writes to its own pipe, not this capture. Captures nest, each call saving its own descriptor. An error or `throw` out of xt restores the descriptor, discards the captured text, and propagates. Native-only: the wasm build errors, WASI having no temporary files | 2 + xt + bytes | `1o` + the temporary file | O(xt + bytes) |
| `wait` | `( pid -- status )` | Block until the child exits; return its exit code, or `128 + signo` if it was killed by a signal | blocks | none | O(1) |
| `stop` | `( pid -- status )` | `SIGKILL` the child then reap it (137 = 128+9, or its code if it had already exited) | 2 syscalls | none | O(1) |
| `running?` | `( pid -- bool )` | Non-blocking liveness via `waitid`+`WNOHANG`+`WNOWAIT`; true while running, false once exited. Non-reaping, so a later `wait` still returns the status | 1 syscall | none | O(1) |
| `open-app-window` | `( path -- )` | browser.telic: open `path` in a detached browser application window (Chromium `--app`), falling back to the system `open` / `xdg-open` | fork | none | O(1) |
| `run` | `( str -- proc )` | subprocess.telic: split a command string on runs of spaces and launch it as a subprocess, returning `{ :pid :in :out :err }` | split + fork | `1a` + `1o` frame + 3 streams | O(\|s\| + argc) |
| `write-in` | `( str proc -- )` | subprocess.telic: write the string to the child's `:in` stream | write syscalls | none | O(\|s\|) |
| `read-out` | `( proc -- str )` | subprocess.telic: read the child's `:out` stream to EOF | read syscalls | `1o` + buffer growth | O(bytes) |
| `read-err` | `( proc -- str )` | subprocess.telic: read the child's `:err` stream to EOF | read syscalls | `1o` + buffer growth | O(bytes) |
| `end-process` | `( proc -- )` | subprocess.telic: tear down a subprocess frame — close `:in`/`:out`/`:err` and wait on `:pid` (graceful, blocks until exit) | 3 closes + wait | none | O(1) |
| `parallel-run` | `( commands width -- results )` | subprocess.telic: run each argv array in `commands` as a subprocess, at most `width` at once; collect `{ :out :err :status }` per command in input order, refilling a slot as each child finishes | fork per command + poll | `1a` + per-child frames/streams | O(critical path) |

Line access is `read-line` one line at a time, or `read "\n" split` for a
stream already in hand; both cut at `\n` and leave a `\r` before it in the
line.

```forth start-process
[ "echo" "hi" ] start-process dup read-out trim . :pid @ wait . cr
```
```output
hi 0
```

```forth run-result
[ "echo" "ok" ] run-result :out @ trim . cr
```
```output
ok
```

```forth write
[ "cat" ] start-process dup :in @ "ping" swap write dup :in @ close dup read-out trim . :pid @ wait drop cr
```
```output
ping
```

```forth read
[ "echo" "data" ] start-process :out @ read trim . cr
```
```output
data
```

```forth read-line
[ "printf" "first\nsecond\n" ] start-process :out @
dup read-line . dup read-line . read-line . cr
```
```output
first second null
```

```forth read-available
[ "printf" "chunk" ] start-process to talker
[ talker :out @ ] 5 wait-readable drop
talker :out @ read-available . cr
talker end-process
[ "sleep" "5" ] start-process to quiet
quiet :out @ read-available byte-size . cr
quiet :pid @ stop drop
quiet :in @ close  quiet :out @ close  quiet :err @ close
```
```output
chunk
0
```

```forth wait-readable
[ "printf" "now" ] start-process to source
[ source :out @ ] 5 wait-readable size . cr
source :out @ read-line . cr
source end-process
```
```output
1
now
```

```forth close
[ "cat" ] start-process dup :in @ "ping" swap write dup :in @ close dup read-out trim . :pid @ wait drop cr
```
```output
ping
```

```forth-noexec stdin
stdin read size . cr
```
```output
42
```

```forth stdout
"direct" stdout write cr
```
```output
direct
```

```forth stderr
stderr stream? . cr
```
```output
1
```

```forth stdout>string
[: "quiet" . :] stdout>string "|" + . cr
```
```output
quiet |
```

```forth wait
[ "true" ] start-process :pid @ wait . cr
```
```output
0
```

```forth stop
[ "sleep" "5" ] start-process :pid @ stop . cr
```
```output
137
```

```forth running?
[ "true" ] start-process :pid @ dup wait drop running? . cr
```
```output
0
```

```forth-noexec open-app-window
"figures/plot.svg" open-app-window
```
```output
```

```forth run
"echo hi" run read-out trim . cr
```
```output
hi
```

```forth write-in
"cat" run dup "ping" swap write-in dup :in @ close dup read-out trim . end-process cr
```
```output
ping
```

```forth read-out
"echo hi" run read-out trim . cr
```
```output
hi
```

```forth read-err
[ "sh" "-c" "echo oops >&2" ] start-process read-err trim . cr
```
```output
oops
```

```forth end-process
"cat" run dup "ping" swap write-in dup :in @ close dup read-out trim . end-process cr
```
```output
ping
```

```forth parallel-run
[ [ "echo" "a" ] [ "echo" "b" ] ] 2 parallel-run [: :out @ trim . :] each cr
```
```output
a b
```

---

## SQLite

Embedded relational storage via the vendored SQLite amalgamation, built into the binary. A database is a `T_DB` value — an inline handle into a per-interpreter registry of open connections, like a stream. `db-exec` and `db-query` take a `params` array bound positionally to the statement's `?` placeholders (`[ ]` for none): a float binds as a double, a string or symbol as text, `null` as NULL, anything else errors — so string parameters need no hand-escaping. A `db-query` result is a dataset, so every dataset word — `query`, `merge-by`, `aggregate`, `select-rows` — applies to it unchanged. `n` = rows returned, `c` = columns.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `db-open` | `( path -- db )` | Open (creating if absent) the database file at `path` and push a handle; `":memory:"` is a private in-memory database. Errors if it can't be opened | open | 1 connection (not GC'd) | O(1)+ |
| `db-close` | `( db -- )` | Close the connection and free its registry slot. Idempotent — closing an already-closed handle is a no-op. A closed handle stays stale: using it reports `database is closed` even after the slot is reissued to another database. A handle that is dropped without closing holds the connection until process exit; `with-db` scopes one | 1 syscall | none | O(1) |
| `db-exec` | `( db statement params -- n )` | Bind `params` to the statement's `?` placeholders and run it with no result set (INSERT / UPDATE / DELETE / CREATE / …); return the affected-row count as a float (0 for DDL). One statement per call. On a bad statement, errors with SQLite's message | per statement | none | O(statement) |
| `db-query` | `( db query params -- dataset )` | database.telic: bind `params` to the query's `?` placeholders and run it, answering the result as a column-oriented dataset with **typed columns**: a column whose every cell is INTEGER, REAL or NULL becomes an n×1 vector (NULL → NaN; an integer beyond 2⁵³ stays an exact in an array column), a column declared DATE/DATETIME/TIMESTAMP becomes a vector of instants in `s` (numeric cells read as epoch seconds, text cells parsed as ISO Z), and anything else stays an array with TEXT → string, BLOB → string of raw bytes, NULL → `null`. Rows keep result order, duplicates included. An empty column declared numeric stays an empty vector, so the type survives an empty result; a repeated column name keeps its last occurrence. On a bad query, errors with SQLite's message | n·c | `1o` frame + `1a`/column + `1m` per numeric column + a string per text cell | O(n·c) |
| `tsv>db` | `( tsv-path db table -- info )` | database.telic: import a TSV file into a new table. The header row names the columns (identifiers quoted, so any header text works); a column whose every non-empty cell is numeric is REAL, else TEXT; empty cells insert as NULL; all rows go in one transaction. `info` is `{ :n-rows N :columns [ … ] }` — a `:real` column carries `{ :name :type :summary }` with a `:summary` statistics frame of its distribution, a `:text` column `{ :name :type :distinct }` with `COUNT(DISTINCT)` (NULLs uncounted). Errors before creating anything on a missing or ragged file; an existing table errors on the CREATE, leaving it untouched | r·c | rows + dataset + `1s`/statement | O(r·c) |

Using a closed handle errors (`database is closed`). Do selection, projection, and joins in the SQL itself where the table is large; Telic materializes the result as a dataset.

```forth db-open
":memory:" db-open db? . cr
```
```output
1
```

```forth db-close
":memory:" db-open dup db-close db-close "closed twice" . cr
```
```output
closed twice
```

```forth db-exec
":memory:" db-open dup "create table t(x)" [ ] db-exec . dup "insert into t values (?)" [ 5 ] db-exec . db-close cr
```
```output
0 1
```

```forth db-query
":memory:" db-open dup "select 2 as v" [ ] db-query :v @ matrix>array . db-close cr
```
```output
[ 2 ]
```

```forth tsv>db
[ [ "x" ] [ 1 ] [ 2 ] ] "/tmp/docs-db.tsv" save-tsv ":memory:" db-open dup "/tmp/docs-db.tsv" swap "t" tsv>db :n-rows @ . db-close cr
```
```output
2
```

---

## Foreign function interface

Call C functions in any shared library at runtime via `libdl` + `libffi` — no per-library glue. `ffi-open` loads a library; `ffi-function` / `ffi-variadic` resolve a symbol and define a Telic word that marshals its arguments and result. Types are symbols: `:void :int :long :double :ptr :string` — Telic floats marshal to/from C `int`/`long`/`double`, strings pass as `const char*` (a returned `char*` is copied into a Telic string), and `:ptr` is an opaque C pointer held as a `T_PTR` handle (a registry index, since a 64-bit pointer doesn't fit a Val's 44-bit payload). FFI is unsafe: a wrong signature corrupts or crashes — argument *count* is checked, types are the caller's responsibility.

| Word | Stack effect | Behavior | Ops | Alloc | O |
|------|-------------|----------|-----|-------|---|
| `ffi-open` | `( path -- lib )` | `dlopen` the library at `path` and push a `T_PTR` handle; `""` opens the running process itself (`dlopen(NULL)`) for already-linked symbols. Errors if not found | dlopen | 1 handle (not GC'd) | O(1) |
| `ffi-function` | `( lib symbol arg-types ret-type -- ) <name>` | Resolve `symbol` in `lib`, build a libffi call interface, and define the following word `<name>` to call it. `arg-types` is an array of type symbols, `ret-type` a single symbol. The interface is prepared once; calls are ~30–100 ns | dlsym + prep_cif | 1 binding | O(argc) |
| `matrix>pointer` | `( mat -- ptr )` | Intern the matrix's row-major element buffer and return a `T_PTR` handle to pass as a `:ptr` argument; no copy — aliases the live buffer (amortized intern) | 1 | none | O(1) |
| `segment>pointer` | `( seg -- ptr )` | Intern a segment's data buffer and return a `T_PTR` handle (no copy) | 1 | none | O(1) |
| `pointer-cell` | `( -- ptr )` | Allocate a zeroed pointer-sized cell and return a `T_PTR` handle to it, for use as a C out-parameter slot (`&out`) or a one-element handle array; a callee writes a pointer or integer into it, read back with `pointer-deref`. Freed by `ffi-free` | malloc | 1 cell (not GC'd) | O(1) |
| `pointer-deref` | `( ptr -- ptr' )` | Load the pointer stored at cell `ptr` (`*(void**)ptr`) and return it as a `T_PTR` handle — reads a handle a C call wrote into an out-parameter cell, or steps through a `T**` | 1 | 1 handle | O(1) |
| `pointer-long` | `( ptr -- n )` | Load the 64-bit integer stored at cell `ptr` (`*(int64_t*)ptr`) as a float — reads a `bst_ulong`/`long` out-value a C call wrote into a cell; errors above 2^53 (not float-exact) | 1 | none | O(1) |
| `pointer-string-at` | `( ptr i -- str )` | Copy the C string at index `i` of a `char**` at `ptr` (`ptr[i]`, NUL-terminated) into a Telic string — reads one entry of a returned string array (e.g. `XGBoosterFeatureScore`'s feature names) | 1 + \|s\| | `1o` | O(\|s\|) |
| `pointer>address` | `( ptr -- n )` | The pointer's numeric address as a float, for embedding in an `__array_interface__` JSON string; errors if the address exceeds 2^53 (not float-exact — macOS arm64 user addresses are well under it) | 1 | none | O(1) |
| `floats>matrix` | `( ptr n -- mat )` | Copy `n` 32-bit floats from foreign memory at `ptr` into a fresh n×1 double matrix — the read-back for a C call that returns a `float const*` result buffer (e.g. predictions); errors if `n < 1` | n | `1m(n×1)` | O(n) |
| `ffi-variadic` | `( lib symbol arg-types ret-type n-fixed -- ) <name>` | Like `ffi-function` for a variadic C function: `n-fixed` leading arguments use the fixed convention, the rest the variadic one (`ffi_prep_cif_var`). Variadic argument types are fixed per binding, so declare one word per type combination (e.g. a `:string` `setopt` and a `:long` `setopt`) | dlsym + prep_cif_var | 1 binding | O(argc) |
| `ffi-free` | `( ptr -- )` | `free` a C buffer held as a `T_PTR` (e.g. from `malloc`) and clear its registry slot. Not for library handles | free | none | O(1) |

A defined FFI word pops its arguments, marshals each per the declared signature, calls through libffi, and pushes the marshalled return (`:void` pushes nothing). The build links `-lffi`; `dlopen` is in libSystem.

```forth ffi-open
"" ffi-open "cos" [ :double ] :double ffi-function c-cos 0 c-cos . cr
```
```output
1
```

```forth ffi-function
"" ffi-open "cos" [ :double ] :double ffi-function c-cos 0 c-cos . cr
```
```output
1
```

```forth matrix>pointer
[ 1 2 ] vector matrix>pointer ptr? . cr
```
```output
1
```

```forth pointer-cell
pointer-cell ptr? . cr
```
```output
1
```

```forth pointer-deref
pointer-cell pointer-deref ptr? . cr
```
```output
1
```

```forth pointer-long
pointer-cell pointer-long . cr
```
```output
0
```

```forth-noexec pointer-string-at
names-cell pointer-deref 0 pointer-string-at . cr
```
```output
age
```

```forth pointer>address
pointer-cell pointer>address 0 > . cr
```
```output
1
```

```forth-noexec floats>matrix
predictions-pointer 3 floats>matrix matrix>array . cr
```
```output
[ 0.12 0.87 0.44 ]
```

```forth-noexec ffi-variadic
"" ffi-open "printf" [ :string :double ] :int 1 ffi-variadic c-printf
```
```output
```

```forth ffi-free
pointer-cell ffi-free "freed" . cr
```
```output
freed
```

---

## Type tags

| Tag | Description |
|-----|-------------|
| `T_FLOAT` | 64-bit double; any bit pattern that is not a boxed NaN |
| `T_STRING` | heap object; NUL-terminated UTF-8 bytes, `len` = byte count |
| `T_SYMBOL` | symbol-pool offset; equal names share one offset |
| `T_ARRAY` | heap object; `Val[]` |
| `T_SET` | heap object; sorted `Val[]`, binary-search membership |
| `T_FRAME` | heap object; parallel keys (`cell[]`) and values (`Val[]`) ordered by symbol id — interning order, not alphabetical — for binary-search lookup |
| `T_MATRIX` | heap object; r×c row-major `double[]` |
| `T_SEGMENT` | heap object; flat fixed-length `int[]` buffer, calloc'd off the arena; see Segments |
| `T_QUANTITY` | a magnitude (float or matrix) plus a unit id, in a pair-table slot `{magnitude, unit}`; see Dimensioned quantities. Dimensionless results collapse away, so a live quantity always carries a real unit |
| `T_EXACT` | heap object; a reduced fraction — sign plus numerator and denominator as 32-bit-limb magnitudes in one buffer; see Exact rationals |
| `T_COMPLEX` | a pair-table slot `{real, imaginary}`, both floats; see Complex numbers |
| `T_XT` | execution token (dict index); first-class callable |
| `T_CURRIED` | heap object; a curried token — `items[0]` is the target xt, `items[1..]` the bound values pushed at invocation. Accepted wherever `T_XT` is, and `type-of` calls both `:xt` |
| `T_ADDR` | dict index; used internally for return-stack frames |
| `T_STREAM` | OS file descriptor (a pipe end to a child process); an inline `int`, like `T_ADDR` |
| `T_DB` | inline handle into the per-interpreter registry of open SQLite connections; not GC'd (closed with `db-close`) |
| `T_PTR` | opaque C pointer from the FFI (library handle or data pointer); a registry index, not the raw 64-bit address; not GC'd |
| `T_CONT` | heap object; a captured return-stack slice plus a resume IP |
| `T_MARK` | ephemeral sentinel from `[<`, `[`, `{`, `reset`; not user-visible |
| `T_LOGIC_VAR` | index into the logic-var stack; unbound, or bound to a Val (resolve with `deref`) |
| `T_UNBOUND` | binding sentinel for an unbound logic var; also the `_` wildcard value when on the stack |
| `T_REST` | a rest pattern: the logic-var index it binds, or the wildcard sentinel; see `rest` under Logic |
| `T_NONE` | uninitialized / sentinel; the empty list and `null` |

Boolean convention: `1.0` true, `0.0` false.

---

## Object allocation

Most heap values use one slot in the `objects[]` table (recycled from the free list after a collection, otherwise a pointer bump into a table that doubles up to a 64M-entry ceiling) plus an arena `Object` struct plus one payload allocation. A collection runs when the ceiling is reached, on an explicit `gc`, or when matrix, segment and continuation payload bytes pass the threshold (twice the survivors of the last collection, at least 256 MiB); arena objects alone never trigger one, so array, string and frame churn grows the heap until one of those events. Two kinds are exceptions: **quantities and complexes** live in a separate dense, GC'd pair table (`{head, tail}` inline, no payload), and **logic vars** on a bump-allocated stack reclaimed by truncation on backtrack.

| Type | Payload |
|------|---------|
| String | `len + 1` bytes (NUL-terminated) |
| Array | `n × sizeof(Val)`; two or fewer elements live inside the `Object` struct with no payload allocation |
| Set | 4 × `sizeof(Val)` initial, doubles on overflow |
| Frame | 4 × (`sizeof(cell)` keys + `sizeof(Val)` values), doubles on overflow |
| Matrix | `r × c × sizeof(double)` (calloc, zero-filled) |
| Segment | `n × sizeof(int)` (calloc, zero-filled) |
| Exact | `(numerator limbs + denominator limbs) × 4` bytes |
| Continuation | `max(L,1) × sizeof(Val)` |
