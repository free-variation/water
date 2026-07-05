# Garbage collection in water

This is a primer on how water reclaims memory. By the end you should
understand:

- What problem garbage collection solves
- How a heap value is represented, and why user code never holds a raw pointer
- Mark-and-sweep: what each phase does and why it's correct
- Where the *roots* live and why each one matters
- `gc_roots` — a small but subtle mechanism that fixes a class of bug C
  programmers will recognize
- What makes a collection run — both triggers — and what GC deliberately leaves
  alone

It's a conceptual tour. The implementation is in `src/c/core.c`; this document
explains the machinery, not its field names.

---

## Part 1: Why GC exists

The problem shows up the moment a program creates data at runtime. Concatenate
two strings:

```forth
"hello " "world" +
```

`+` produces a *new* string that didn't exist before; it has to live somewhere
while the program runs, and once nothing refers to it the memory can be reused.

A program that allocates but never frees leaks until it dies. A program that
frees too eagerly frees something another part is still using — a use-after-free,
usually a crash, sometimes a security hole. The traditional fix is to make the
programmer match every allocation with exactly one free; it is error-prone,
composes badly across libraries, and is very hard to get right for shared or
cyclic structures.

Garbage collection automates the decision. The runtime tracks which objects are
still *reachable* — some chain of references leads from live program state to the
object — and frees the rest. The programmer never frees; they don't think about
it. The cost is periodic scanning and some loss of control over *when* memory is
reclaimed; the benefit is simpler, safer programs and the freedom to build shared
structures without ownership puzzles.

---

## Part 2: What gets collected

The defining choice is that user code never holds a raw pointer to a heap value.
A value sitting on a stack is a small tagged word (a `Val`); for a heap type, its
payload is a **handle** — an index into a global table — not an address. The
indirection is what makes the rest of the system possible: garbage collection
reuses table slots in place, and threads share heap values by sharing handles
(see `multicore.md`).

There are two collectable tables:

- The **object table** holds the composite values: strings, sets, arrays,
  frames, matrices, segments, and continuations.
- A separate **pair table** holds cons cells, the building block of lists.

A heap value is a header plus, usually, one or two payloads — a string's bytes, a
set's or array's slots, a frame's parallel keys and values, a matrix's or
segment's flat numeric buffer, a continuation's captured stack slice. Freeing a
value means releasing each piece with the allocator that produced it: the arena
for headers and the small payloads, the system allocator for the large numeric
buffers (`arena.md` draws that line).

Not every `Val` is a reference. The tag says how to read the payload:

- A **float** *is* the number; a **symbol** is an offset into the symbol pool; an
  **execution token** or **address** is a position in the dictionary; a
  **stream** is a file descriptor; a **pointer** is an FFI registry index. None
  of these point at collectable storage — the GC skips them.
- An **object handle** or a **cons cell** points directly at collectable storage,
  and the GC must trace it.
- A **logic variable** is an index into the binding store; the GC follows it to
  whatever the variable is bound to.

So the GC's job, in slogan form: starting from the roots, find every live cell —
object slot and cons cell alike — and free the dead ones.

---

## Part 3: Mark and sweep

water uses *mark-and-sweep*, one of the oldest collection algorithms. It
runs in two phases.

**Mark.** Starting from the *roots* — references the runtime knows are live —
traverse every reachable value and record that it's marked for this cycle.

**Sweep.** Walk both tables; free everything that isn't marked.

The correctness argument is short: a value is reachable iff a chain of references
leads to it from a root; after marking, a value is marked iff reachable; sweep
frees the unmarked, so the live survive and the dead are freed.

The trickiness is all in the details:

- How do you enumerate every root?
- How do you walk a value's outbound references?
- How do you keep the mark phase from looping forever on a cycle?
- How do you find references buried inside compiled code?
- How do you guarantee no live reference is hidden somewhere the GC never looks?

The rest of this document answers each.

---

## Part 4: Roots — where reachability starts

A *root* is a reference the GC trusts as live; anything reachable from a root is
live, everything else dead. In water the roots come from five places:

1. The **data stack** — the values user code is computing with.
2. The **return stack** — saved instruction pointers, and any values parked there
   with `>r`, some of which may be heap references.
3. The **side stack** — a third stack for stashing values out of the way; same
   story.
4. The **dictionary** — `variable` storage and the literal values compiled into
   word bodies live here.
5. The **`gc_roots` array** — in-flight values that C-level code is holding but
   hasn't yet put anywhere the first four would find. Part 7 is about this one.

These are the complete set of places the running program keeps a `Val`. If a
value isn't reachable from one of them, nothing can reach it — it's dead.

The first four are scanned by walking an array (or, for the dictionary, by the
machinery in Part 6) and tracing each `Val`. The fifth is the subtle one.

---

## Part 5: The mark phase

Marking is a single recursive idea: given a `Val`, if it isn't a heap reference,
stop; if it's a logic variable, follow it to its binding and continue; if it's a
cons cell or an object, record the mark and descend into its children. Leaves —
strings, matrices, segments — hold only flat bytes or numbers, so there's nothing
to descend into. Sets, arrays, frames, and continuations hold `Val`s, so the
marker recurses through them. (Frames store their keys as plain symbol offsets,
not `Val`s, so only the values are traced.) The recursion terminates because
every step either reaches a leaf, reaches an already-marked value (the cycle
guard), or reaches a non-reference.

The one design choice worth dwelling on is **how a mark is recorded**, because
the obvious approach — a mark bit per value, cleared at the start of every cycle
— costs an O(n) clear pass over the whole heap each time. water instead uses
an **epoch**: a single counter the collector bumps by one at the start of each
collection. Every value carries the epoch at which it was last marked, and a
value counts as marked for *this* cycle iff its stamp equals the current epoch.
Bumping the counter therefore invalidates every leftover mark at once — no clear
pass — and the same scheme covers both tables: object slots and cons cells each
carry an epoch stamp, so neither needs a separate cleared-each-cycle bit array.

### A worked example

Suppose the data stack holds one array of three items: a float, a second array,
and a string — and the inner array's only item is *the same* string handle as the
outer's third item (a shared subobject).

Marking from the data stack: mark the outer array, descend. The float is not a
reference — skip. The inner array — mark it, descend, mark the shared string (a
leaf, nothing below). Back in the outer array, reach the shared string again —
its stamp already equals this epoch, so stop immediately. Every reachable handle
is marked; the shared string was visited once, not twice; and had the outer array
contained *itself*, the already-marked check would have caught it the same way.
That repeated-handle check is what makes cycles safe.

---

## Part 6: The dictionary as a special root

The three stacks and `gc_roots` hold `Val`s directly — walk the array, trace each
one. The dictionary is different: it's one long array of cells holding compiled
word bodies, and only *some* of those cells are `Val`s. A cell may be a handler
index (a compiled instruction), an operand to the previous instruction (a branch
target, a local slot number), an inline literal `Val`, or — for a `variable` —
the variable's current value. The literals and variable values can be heap
references; if the GC misses them, the compiled code that will later use them
finds its target freed.

So the GC walks every word body cell by cell, and for each instruction it has to
know *how many cells that instruction occupies* — its handler plus any operands —
so it can step over operands without mistaking one for the next instruction, and
recognize the two instruction kinds that carry a `Val` (an inline literal, and a
string-literal handle) to trace them. That per-instruction width is the load-
bearing fact: it must be right for every compiled op, branches and fused
superwords included, or the walk either runs off the end of a body or reads an
operand as if it were an instruction. It is the first thing to get right when a
new operand-carrying op is added.

To find each word's body, the collector recovers the bounds of every compiled
definition from the dictionary's word list and traces the cells between them; a
`variable`'s single value cell is traced directly.

---

## Part 7: gc_roots — temporary roots for in-flight work

This is the subtlest piece of the collector. It exists because of a quiet bug
that would otherwise be very hard to find.

### The problem

Consider how `map` is implemented in C: allocate a fresh result array, then loop
over the source, run the user's quotation on each element, and store each result
into the array. The result array is built incrementally, and in the middle of the
loop the code runs *arbitrary user code* — the quotation — which can allocate
freely, and may allocate enough to trigger a collection.

What does that collection see? The data stack, return stack, side stack, and
dictionary — none of which mention the half-built result array. Its handle is
sitting in a C local variable inside the `map` implementation, invisible to the
GC. The collector finds an unreferenced slot, frees it, and the next loop
iteration writes a result into freed memory: a use-after-free, then a crash or
silent corruption.

This is the C-level analogue of a rooted-but-unrooted bug familiar from any
GC'd language with a foreign-function interface: the collector must know about
*every* live reference, including ones held momentarily outside the structures it
normally scans.

### The fix

A small array — `gc_roots` — that C code pushes an in-flight value onto and pops
when done, and that the root scan includes. `map` pushes the result array's
handle before the loop and pops it after; while the quotation runs and possibly
collects, the result is reachable through `gc_roots`, gets marked, survives. It's
unrooted only once it's safely on the data stack.

The pattern recurs wherever a C primitive holds a freshly allocated handle in a
local and is about to do something that might allocate: building a result
collection while a user quotation runs, holding a filename string across file
I/O. Push before, pop after.

The array is deliberately small: these references are transient, each push
matched by a pop a few lines later, and the nesting depth is one or two in
practice. If it ever overflowed, the push fails loudly rather than dropping a
root silently — dropping one would risk freeing a live value on the next
collection.

---

## Part 8: The sweep phase

After marking, the sweep is mechanical. Walk each table; a slot whose value's
stamp equals the current epoch is live — keep it. Otherwise the slot is dead or
already empty: if a value is there, free it — release each payload with the
allocator that made it, recycle the header, and (for the large `calloc`'d
buffers) decrement the live-byte meter Part 9 relies on — then record the now-
empty handle on a free list.

Rebuilding that free list is the real point of the sweep. It's a stack of
reusable handles, emptied at the start of the sweep and refilled with every dead
or empty slot; the allocator pops from it, so reusing a freed handle is O(1) with
no scan. The pair table is swept the same way into its own free list.

### Why not compact?

After a sweep the tables have holes — freed slots scattered among live ones.
water doesn't slide live values down to close them, because a value is named
by its slot index: moving it would mean finding and rewriting every handle that
points at it. So holes are reused in place. The tables stay sparse and never
shrink below their high-water mark, and allocation stays O(1) regardless — the
sweep leaves a free list, and the allocator just pops.

---

## Part 9: What makes a collection run

A collection fires on either of two conditions.

**Handle exhaustion (the lazy trigger).** A table has handed out every slot up to
its ceiling and its free list is empty. There is no other way to get a slot, so
the allocator collects, then reuses what the sweep frees. This is the common
case, and it's why steady-state allocation has zero collection overhead: nothing
runs until the slots run out, and then one mark-and-sweep pauses execution for as
long as the live set is large.

**Byte pressure (the size trigger).** Handle exhaustion alone is blind to *size*:
a few thousand giant matrices exhaust RAM long before they exhaust the (millions
of) handle slots. So the large `calloc`'d payloads — matrix, segment, and
continuation buffers — are metered, and when the live-byte total crosses a
threshold, a collection is requested. The threshold is not a fixed number: after
each collection it's reset to a multiple of whatever survived, with a floor
(`HEAP_GC_FLOOR`). A program that genuinely needs a large live heap collects
rarely; a churny one that allocates and drops big buffers is kept in check. The
floor keeps small programs from collecting constantly.

The byte trigger does **not** collect at the allocation site. A primitive in the
middle of its work may have popped operands into C locals that aren't yet on any
stack or in `gc_roots`; collecting right then could free them — the same hazard
Part 7 describes, but from a different direction. So the allocator only *requests*
a collection by setting a pending flag; the interpreter's instruction dispatch
notices the flag and breaks out to a **safepoint between words**, and the
collection runs there, where every live value is on a stack or rooted. Deferring
to a word boundary is what makes the byte trigger safe to fire from deep inside an
allocation.

One routine suspends collection entirely: deep copy (`copy` / `reify`) builds a
new structure whose intermediate pieces aren't reachable from any root until it's
finished, so it disables collection for the duration and accepts that an
allocation may fail rather than risk a sweep freeing the half-built copy.

The `gc` word forces a collection by hand — useful for measuring or debugging,
rarely needed otherwise.

---

## Part 10: What GC doesn't manage

It's worth being explicit about things that look like memory but aren't collected:

- **The dictionary, and the name / source / symbol pools.** Fixed-size arrays
  written by the compiler; everything in them lasts until program exit or until
  `forget` rewinds the dictionary.
- **The loaded-files list, input and token buffers.** Manually managed or reused
  in place; they persist for the program's life.

The boundary is simple: anything reached through a handle is collected; anything
allocated another way is manually managed or permanent. `forget` rewinds compiled
state, but objects referenced only by forgotten code aren't freed immediately —
the next collection notices they're unreachable and reclaims them.

**Cycles** are handled correctly by the epoch guard, though they're hard to build
from user code (the collections aren't mutated in place to point at themselves).
**Finalizers** don't exist — a heap value wraps only memory, never an OS resource,
so there's nothing to run at death. **Weak references** don't exist either; every
reference is strong.

---

## Part 11: Under parallelism

Inside a parallel region the global collector is suspended. Running it would mean
growing or sweeping tables that several threads are reading and writing at once,
and a table that grows can move under a concurrent reader. Instead each worker
collects its *own* garbage: a generational sweep that marks from that worker's
roots, traverses only values the region created, and reclaims only the slots that
worker allocated — no global pause, no lock. `multicore.md` covers it.

---

## Part 12: The big picture

The collector, in slogan form: find the live values by walking from the roots;
free the rest. The details that make it work:

- A small, complete set of roots — three stacks, the dictionary, and a temporary-
  roots array for C-level in-flight values.
- A type-aware marker that descends into composite values, follows logic
  variables, marks cons cells, and stops on already-marked values so cycles
  terminate — with the mark recorded as an epoch stamp, so old marks clear for
  free.
- A dictionary walker that knows compiled-code layout well enough to find
  literals and variable values without misreading operands.
- A sweep that frees each payload with the right allocator and rebuilds the
  free lists both tables allocate from.
- Two triggers — handle exhaustion and metered byte pressure — the second
  deferred to a safepoint between words so no in-flight operand is freed under a
  running primitive.

For broader context: `arena.md` is the allocator the sweep returns memory to;
`continuations.md` covers the captured stack slices the marker walks; and
`multicore.md` covers the per-worker collector that replaces this one inside a
parallel region.
