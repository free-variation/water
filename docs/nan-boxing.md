# NaN-boxing in Water

This is a primer on how Water packs a tagged value into 8 bytes by
exploiting unused bit patterns in the IEEE 754 double format. By the end you
should understand:

- Why a tagged-value system needs a tag, and what the naive layout costs
- What an IEEE 754 double looks like at the bit level, and which patterns it
  leaves unused
- How those unused patterns become storage for tagged non-float values
- The one place care is required — telling a real float NaN from a boxed value
- How the single-word representation shapes the surrounding code

It's a conceptual tour; the encoding lives in `src/c/water.h`, behind the
`Val` type and its accessor and constructor helpers. Every site that creates or
inspects a value goes through those helpers, so only they need to know the bit
layout. The aim is to make that layout feel obvious in retrospect.

---

## Part 1: Why a value needs a tag

Water is dynamically typed. A value on the stack might be a float, a symbol,
an array handle, an execution token, or any of about a dozen kinds, and the
interpreter has to know which to do anything: adding two floats is one machine
instruction, adding two arrays is concatenation, adding a float to an array is an
error. So every value carries both *what it is* (a tag) and *the value itself* (a
payload). The pair is what Water calls a `Val`.

The obvious layout is a struct with a tag field and a 64-bit payload field. With
alignment that comes to 16 bytes per value — a few tag bits, padding, and eight
payload bytes. Every stack slot, every array element, every value passed to or
from a function is then 16 bytes. NaN-boxing shrinks that to 8 — a single machine
word — by storing the tag *inside* the same eight bytes as the payload.

---

## Part 2: What an IEEE 754 double looks like

To pack other values into a double's eight bytes, we need to know what a double's
bits already mean. The format has three fields — a 1-bit sign, an 11-bit exponent,
and a 52-bit mantissa — and the value is `(-1)^sign × 1.mantissa × 2^(exponent −
bias)`. Two exponent values are reserved:

- **Exponent all zeros** is zero (mantissa zero) or a subnormal (very small)
  number.
- **Exponent all ones** is infinity (mantissa zero) or, with any non-zero
  mantissa, **NaN** — Not a Number.

The NaN case is where the surplus lives.

---

## Part 3: The NaN surplus

The IEEE rule for NaN is: exponent all ones, mantissa non-zero. That leaves
*billions* of distinct bit patterns that are all, technically, valid NaNs —
roughly 2⁵³ of them, counting the sign bit. But software almost never produces
more than one NaN: `0.0/0.0`, `sqrt(-1)`, `inf − inf` all yield a single canonical
NaN, and that's the one programs care about. The other ~2⁵² patterns are real
estate the standard reserved for diagnostic payloads that no real ecosystem ever
used.

That's the loophole. We pick one NaN pattern to mean "this really is a float NaN"
and reinterpret all the others as tagged non-float values. By convention a *quiet*
NaN — the ordinary kind hardware produces — has the top mantissa bit set, so any
pattern with the exponent all ones *and* the top mantissa bit set looks like a
quiet NaN to the floating-point unit. Water keys on exactly that signature,
which leaves the remaining low bits (and the unused sign bit) free to claim for a
tag and a payload.

---

## Part 4: The encoding

A boxed (non-float) value is laid out within the eight bytes like this:

```
bit  63:     unused (the sign bit) — free, not part of the box test
bits 62..51: the quiet-NaN signature (exponent all ones + top mantissa bit)
bits 50..44: the tag (7 bits — room for up to 128 distinct kinds)
bits 43..0:  the payload (44 bits — a handle, an index, an offset, a small int)
```

The signature bits are the gate. If they hold the quiet-NaN pattern, the value is
boxed: read the tag and payload from the bits below. If they hold *anything else*
— any other exponent, any finite number — the entire 64-bit pattern is an ordinary
double, used as-is. So float arithmetic touches the bits directly with zero
overhead, and only non-floats pay any decoding.

Four operations sit on this layout. The gatekeeper asks "is this a float?" with
one mask and one compare against the signature. Given a boxed value, one accessor
extracts its tag and another its payload; for a float, "the value" is just the
double, and "the payload" is the full 64-bit pattern (so a value's bits survive a
round-trip to and from the dictionary verbatim). The 44-bit payload is wide enough
for every non-float kind Water has — object handles, symbol-pool offsets,
dictionary positions, file descriptors, small markers — because none of those
needs a full 64 bits; a 32-bit handle has headroom to spare.

A value is never assembled field by field. Because the representation is opaque
bits, every value is *built* through a constructor — a general one that takes a
tag and a payload and ORs in the signature, plus a thin wrapper per kind — and the
float case is the one that needs care.

---

## Part 5: The float-NaN corner

Real arithmetic occasionally produces a NaN, and the pattern it produces — often
the hardware's canonical one — can fall inside the signature region that means
"boxed." Left alone, the gatekeeper would look at a genuine float NaN, see the
signature, and decode a bogus tag and payload from it.

Water's handling is partial but sufficient. The float constructor
canonicalizes any signature-region NaN to one fixed pattern, so a float built from
*any* NaN-producing computation always ends up with the same bits — behavior is
deterministic. It does not *resolve* the collision: that canonical pattern still
carries the signature, so a value holding it would still read as boxed. The scheme
is sound in practice because Water programs essentially never produce float
NaNs — ordinary arithmetic doesn't hit the cases, and no primitive lets a user
construct a specific NaN bit pattern. This is the one place NaN-boxing demands care
that a plain tagged struct wouldn't; the payoff — half the bytes per value, half
the stack bandwidth, single-register passing — is worth that corner.

---

## Part 6: How the single-word value shapes the code

The 8-byte value shows up in a few places beyond arithmetic:

- **Values are built, not mutated.** There's no tag field to write; every value
  comes from a constructor. Code reads a value's kind through the accessor, never
  by projecting a struct field.
- **Variable storage is one cell.** A variable in the dictionary is its handler
  cell plus a single cell holding the packed value, so reading or writing a
  variable is one dictionary access — which is also why the GC's dictionary walk
  can treat a variable's value as one cell (see `threading.md` and `gc.md`).
- **Stack slots are 8 bytes.** The data, return, and side stacks and frame value
  arrays are all arrays of values, so each slot is one word, not two — half the
  footprint, and the hot top-of-stack working set stays in cache.
- **Calls are cheaper.** An 8-byte value passes in a single register; functions
  taking or returning values get tighter code than a 16-byte struct spanning two.

The cost is qualitative: a tag check is a mask-and-compare on bits already in a
register rather than a field read — cheap, but a touch more to read in the source
— and the float-NaN corner above is sharp. The runtime infers float-versus-boxed
from the bit pattern alone, so a program that could manufacture arbitrary NaN
patterns could confuse it; Water exposes no way to do that, so in practice
the inference always holds.

---

For broader context: `gc.md` reads a value's tag to decide what to trace, and
`threading.md` relies on a value fitting in one dictionary cell — but both are
independent of *how* the tag is packed, which is the point of routing every
access through the helpers.
