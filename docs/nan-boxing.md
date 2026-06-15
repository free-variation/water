# NaN-Boxing in logicforth

This document is a primer on how logicforth packs a tagged value into 8 bytes by exploiting unused bit patterns in the IEEE 754 double-precision floating-point format. By the end you should understand:

- Why a tagged-value system needs a tag at all, and what the naive layout costs
- What an IEEE 754 double looks like at the bit level, and which bit patterns it leaves unused
- How those unused patterns become storage for tagged non-float values
- The encoding logicforth uses, with the accessor macros that hide the bit manipulation
- The one place where care is required — distinguishing a real float NaN from a boxed value — and how `make_float` handles it
- How the single-word representation shapes the code that builds, stores, and inspects Vals

The implementation is in `src/c/logicforth.h`, in the `Val` union and the surrounding `VAL_*` macros and `make_*` constructors. Every site that creates or inspects a Val goes through those helpers; only they know the bit layout. The aim of this document is to make that bit layout feel obvious in retrospect, rather than clever.

---

## Part 1: Why a Val needs a tag

logicforth is dynamically typed. A value on the stack might be a float, a symbol, an array handle, an execution token, or any of about a dozen kinds. The interpreter has to know which kind it's looking at to do anything useful — adding two floats is one machine instruction; adding two arrays is array concatenation; adding a float to an array is an error.

So every stack value carries both *what it is* (a tag) and *the value itself* (the payload). The pair together is what logicforth calls a `Val`.

The simplest layout is a struct:

```c
typedef struct {
    Tag tag;        // 4 bytes (enum)
    int64_t data;   // 8 bytes (payload)
} Val;
```

Tags are small integers; the tag field is 7 bits, so there can be up to 128 of them. The payload is a 64-bit field that's either the bit pattern of a double (when the tag is `T_FLOAT`) or a 32-bit handle/index/cfa (for everything else).

With 8-byte alignment for the 64-bit data field, that struct comes out to 16 bytes per Val: 4 tag bytes, 4 bytes of padding, 8 payload bytes.

The data stack is an array of Vals. Each push or pop moves 16 bytes. Each function that takes or returns a Val passes 16 bytes. Each `Val[]` slot — in the data stack, the return stack, frames, array storage — costs 16 bytes.

NaN-boxing shrinks that 16-byte struct to 8 — a single machine word.

---

## Part 2: What an IEEE 754 double looks like

Before we can pack other values into the same 8 bytes as a double, we need to know what a double's bits already mean. The format has three fields:

```
| sign | exponent | mantissa |
| 1    | 11       | 52       |
```

The value a double represents is:

```
(-1)^sign × 1.mantissa × 2^(exponent - 1023)
```

A few concrete examples:

| value | sign | exponent | mantissa | hex |
|---|---:|---:|---:|---|
| 1.0 | 0 | 1023 (0x3FF) | 0 | 0x3FF0000000000000 |
| 2.0 | 0 | 1024 (0x400) | 0 | 0x4000000000000000 |
| 3.0 | 0 | 1024 | top bit set | 0x4008000000000000 |
| -1.0 | 1 | 1023 | 0 | 0xBFF0000000000000 |
| 0.0 | 0 | 0 | 0 | 0x0000000000000000 |

Two exponent values are reserved as special cases:

- **Exponent = 0** with mantissa = 0 is zero. With non-zero mantissa, it's a "subnormal" (very small) number.
- **Exponent = 0x7FF** with mantissa = 0 is infinity (sign bit picks ±). With non-zero mantissa, it's NaN — Not a Number.

The reserved-exponent cases are where the surplus lives.

---

## Part 3: The NaN surplus

The IEEE rule for NaN is: any bit pattern with exponent = 0x7FF and mantissa ≠ 0 is a NaN. That definition leaves **2^52 − 1** distinct bit patterns that are all valid NaNs. Multiply by two for the sign bit and you have nearly 2^53 different "NaNs."

But software almost never produces or uses more than one NaN. Operations like `0.0/0.0`, `sqrt(-1)`, or `inf - inf` produce a canonical NaN; that's the one programs care about. The other ~2^52 patterns are real estate the standard reserved for diagnostic payloads that no real ecosystem ever used.

That's the loophole NaN-boxing exploits. We pick one NaN pattern as "this is really a float NaN" and reinterpret all the others as tagged non-float values.

By convention, a *quiet NaN* (the standard kind, produced by `0.0/0.0`) has the top mantissa bit set. So any bit pattern whose exponent bits are all ones with the top mantissa bit set — the 12-bit signature `0x7FF8` fills in bits 62..51 — is a quiet NaN as far as floating-point hardware is concerned. logicforth keys on exactly that signature, which leaves the low 51 bits (plus the unused sign bit) free to claim.

Within those bits we have plenty of room for a tag and a payload.

---

## Part 4: The encoding

logicforth uses this layout for boxed (non-float) values:

```
bit  63:     unused (sign) — free, not part of the box check
bits 62..51: 0x7FF8 quiet-NaN signature (exponent all ones + top mantissa bit)
bits 50..44: tag (7 bits, up to 128 tags)
bits 43..0:  payload (44 bits, holding the int64_t value)
```

If the signature bits (62..51) are *anything other than* the quiet-NaN pattern, the value is a regular double — exponent is some other value, or it's a finite number — and the entire 64-bit pattern is interpreted as a double.

The constants in `logicforth.h`:

```c
#define NAN_BOX_PREFIX 0x7FF8000000000000ULL
#define NAN_BOX_MASK   0x7FF8000000000000ULL
#define VAL_TAG_SHIFT  44
#define VAL_TAG_MASK   0x0007F00000000000ULL
#define VAL_DATA_MASK  0x00000FFFFFFFFFFFULL
```

The `Val` type itself is a union:

```c
typedef union {
    uint64_t bits;
    double   number;
} Val;
```

The same 8 bytes can be viewed either as raw bits (for tag/payload inspection) or as a double (for float arithmetic). The union makes the reinterpretation explicit at the language level without runtime cost.

The four accessor macros do the inspection:

```c
#define VAL_IS_FLOAT(v) (((v).bits & NAN_BOX_MASK) != NAN_BOX_PREFIX)
#define VAL_TAG(v)      (VAL_IS_FLOAT(v) ? T_FLOAT : (Tag)(((v).bits >> VAL_TAG_SHIFT) & (VAL_TAG_MASK >> VAL_TAG_SHIFT)))
#define VAL_NUMBER(v)   ((v).number)
#define VAL_DATA(v)     ((int64_t)(VAL_IS_FLOAT(v) ? (v).bits : ((v).bits & VAL_DATA_MASK)))
```

`VAL_IS_FLOAT` is the gatekeeper: one mask, one compare. If the signature bits don't match the prefix, it's a float; otherwise it's boxed.

`VAL_TAG` returns `T_FLOAT` for floats and decodes the embedded tag for boxed values.

`VAL_NUMBER` returns the union's `number` field directly. For a Val that holds a float, this gives the double value with zero work. For a boxed value, this would return the double interpretation of the prefix-and-payload bits — not meaningful, and only called when the caller already knows the Val is a float.

`VAL_DATA` returns the payload — masked to 44 bits for boxed values, full 64 bits for floats. The float case is what makes save/restore round-trips work: when we serialize a Val's "data" to the dict (e.g., as a variable's stored value), the full bit pattern needs to survive.

---

## Part 5: Constructing boxed values

The constructor side is `make_tagged` (the general form) plus `make_float` (the special case):

```c
static inline Val make_tagged(Tag tag, int64_t data) {
    Val value;
    if (tag == T_FLOAT) {
        value.bits = (uint64_t)data;
    } else {
        value.bits = NAN_BOX_PREFIX
            | ((uint64_t)tag << VAL_TAG_SHIFT)
            | ((uint64_t)data & VAL_DATA_MASK);
    }
    return value;
}
```

For non-float tags, we OR together the prefix, the tag (shifted into bits 44-50), and the payload (masked to 44 bits). For T_FLOAT, the "data" is already the bit pattern of a double — just copy it across.

The typed wrappers are one-liners:

```c
static inline Val make_symbol(int cfa) { return make_tagged(T_SYMBOL, cfa); }
static inline Val make_xt(int cfa) { return make_tagged(T_XT, cfa); }
static inline Val make_string(int handle) { return make_tagged(T_STRING, handle); }
/* ... etc, one per non-float tag ... */
```

`make_float` needs special handling because of a corner case that's the heart of NaN-boxing's correctness.

---

## Part 6: The float-NaN corner

Real float arithmetic occasionally produces NaN — `0.0/0.0`, `sqrt(-1)`, `inf - inf`. The resulting bit pattern is *some* NaN, most commonly the FPU's canonical one (often `0x7FF8000000000000`, which is the box prefix itself with zero payload).

A NaN that happens to fall in the `0x7FF8...` region collides with the boxed-value space. `VAL_IS_FLOAT` would see the prefix, conclude this Val is boxed, and decode a bogus tag and payload from the NaN's bits.

logicforth's handling is partial. The `make_float` helper canonicalizes any prefix-region NaN to a single fixed pattern:

```c
static inline Val make_float(double number) {
    Val value;
    value.number = number;
    if ((value.bits & NAN_BOX_MASK) == NAN_BOX_PREFIX) {
        value.bits = NAN_BOX_PREFIX | VAL_DATA_MASK;
    }
    return value;
}
```

The canonicalization guarantees deterministic behavior — a Val constructed by `make_float` from any NaN-producing double ends up with the same bit pattern. But it doesn't *resolve* the collision: that canonical pattern still has the prefix, so `VAL_IS_FLOAT` will read it as boxed (tag = `T_NONE`, garbage payload).

The scheme is sound in practice because lforth programs rarely produce float NaNs. Standard arithmetic doesn't hit the failure modes, and there's no user-level primitive that constructs a specific NaN bit pattern. One way to resolve the collision fully is to reserve an extra bit (say, the sign bit at position 63) to distinguish "real float NaN" from "boxed value" and have `VAL_IS_FLOAT` check both. The code doesn't, because that cost would apply on every tag inspection in every hot loop, and the partial scheme suffices in practice.

This is the one place where NaN-boxing requires care that a plain tagged struct wouldn't. The win — half the bytes per Val, halved stack bandwidth, single-register passing — is paid for by this corner.

---

## Part 7: How Val is used elsewhere

The single-word Val shapes the surrounding code in a few places.

**Vals are built, not assembled.** There is no `v.tag` field to write — with the union representation, a Val is opaque bits. Every site that builds a Val calls `make_tagged` or one of its wrappers (`make_float`, `make_array`, …); a Val is never set field-by-field.

**Variable storage.** A variable in the dictionary occupies two cells: the `dovar` handler at cfa+0 and `Val.bits` at cfa+1. Reading a variable's value is a single dict access; writing stores the Val's bits in one cell. The two-cell layout is handled in:

- `create_variable` (allocates the cells)
- `push_variable` (reads a variable's value)
- `p_to_var` (writes a variable's value)
- `destruct-to` (bulk-writes multiple variables)
- `p_to`'s interpret-mode branch (REPL `to var`)
- `mark_body`'s `dovar` arm (GC walks variable cells)

**Stack arrays.** The data stack, return stack, side stack, and frame value arrays are all `Val[]`, so each slot is 8 bytes rather than 16 — half the footprint, and the hot top-of-stack working set stays comfortably in cache.

**Function calling convention.** An 8-byte Val passes by value in a single register; functions taking or returning Val (notably `pop`, the `make_*` helpers) get tighter code than a 16-byte struct that occupies two registers. The effect on per-op cost is small but real where the same Val passes through several helpers.

---

## Part 8: Where it helps

The gain isn't uniform. Code where the data stack is the hot data structure — fannkuch and nqueens both shuffle Vals through the stack heavily — wins the most from halving stack bandwidth. Code structured to keep working data in global variables rather than on the stack (n-body's style) benefits less from the stack savings, though the two-cell variable layout from Part 7 helps it a little.

The other gain is qualitative: with 8-byte Vals, function signatures involving Vals are cheaper and the compiler keeps more Vals in registers across operations. Hard to quantify without instrumentation, but visible in the assembly for hot primitives.

---

## Part 9: What it costs

The cost has two pieces.

**Tag inspection is a mask-and-compare, not a field read.** Code paths like generic `+` that check `VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT` mask-and-compare on the bits rather than projecting a field. On modern CPUs this is essentially free — the bits are already in a register, the compare is one cycle, the branch well-predicted — but the source-level reading is more complex, and the macro computes a conditional rather than just reading a field.

**The corner around float NaN is sharp.** The runtime infers float vs. boxed from the bit pattern alone. The canonicalization in `make_float` papers over the most common collisions, but a hostile program that constructs specific NaN bit patterns could still confuse the runtime. logicforth doesn't expose a primitive that lets users do this directly, so the scheme is sound in practice.

---

## Part 10: Where to look in the source

The whole bit-layout story is in `src/c/logicforth.h`:

- **`Val` typedef** — the 8-byte union. `bits` for raw access, `number` for the float view.
- **The `NAN_BOX_*` and `VAL_*` constants** — prefix, mask, shift, tag mask, data mask.
- **`VAL_IS_FLOAT`, `VAL_TAG`, `VAL_NUMBER`, `VAL_DATA` macros** — every read of a Val's tag, payload, or float value goes through these.
- **`make_tagged`** — the general constructor. Handles both the boxed case and the float pass-through.
- **`make_float`** — the float constructor with NaN-prefix canonicalization.
- **`make_symbol`, `make_string`, `make_xt`, etc.** — one-line wrappers around `make_tagged` for each non-float tag.

The compact variable layout (one cell per variable's value) shows up in:

- `create_variable` in `src/c/words.c` — emits the handler and a single `Val.bits` cell, initialized to 0.0 via `make_float(0.0).bits`.
- `push_variable` in `src/c/core.c` — reads the cell and casts it to `Val.bits`.
- `p_to_var` and the interpret-mode branch of `p_to` in `src/c/words.c` — both write `v.bits` to the cell.
- `destruct-to` in `src/c/collections.c` — bulk-writes Vals into variables.
- `mark_body`'s `dovar` branch in `src/c/core.c` — reconstructs the Val from the single cell for GC marking.

For broader context:

- **`docs/gc.md`** — the `mark_value` walker inspects a Val's tag through `VAL_TAG`.
- **`docs/threading.md`** — the Val representation is independent of how handlers are dispatched.
