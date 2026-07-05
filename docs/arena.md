# The arena allocator

water runs its heap through a custom allocator instead of calling
`malloc`/`free` per object. The design buys two things at once: allocation that
costs little more than a pointer increment, and reclamation that recycles freed
memory in constant time with no search. This document builds the idea up from the
bottom — the mechanism, not the exact data structures, which live in `core.c`.

## The shape of the heap

Every heap value in water is a small fixed-size header struct (an `Object`)
plus, usually, a separately allocated payload — a string's bytes, an array's
slots, a frame's parallel key and value arrays. The headers and most of those
payloads come from the arena. A few payloads deliberately sit outside it; the
last section says which and why.

The arena is one contiguous region of memory, reserved once and carved up by
hand.

## Reserve big, page in lazily

At startup the arena reserves a very large virtual address range — `ARENA_RESERVE`
bytes (a double-digit number of gigabytes) — in a single anonymous `mmap`.
Reserving that much sounds extravagant, but anonymous memory is *demand-paged*:
the kernel hands back a range of addresses and commits a physical page only when
you first touch it. Reserving the range costs almost nothing; a program that
allocates a few megabytes uses a few megabytes of RAM.

The point of reserving so much up front is that the region never has to move or
grow. Its base address is fixed for the life of the process, so a pointer into
the arena stays valid forever — there is no realloc-and-relocate that could pull
memory out from under a value still in use. A single high-water mark records how
far into the reservation the allocator has claimed so far.

## Bump allocation

Allocations don't touch that shared high-water mark directly. Each one advances a
cursor within a *slab* — a contiguous window the allocator has already claimed
from the reserved region. Handing out memory is then just:

```
allocation = cursor
cursor += rounded_up_request
```

— a pointer read and an add. When the window doesn't have room for the request,
the allocator refills it by claiming a fresh slab (`SLAB_BYTES`, tens of
kilobytes) from the reserved region with a single atomic bump of the shared
high-water mark. So the shared write happens once per *slab*, not once per
allocation; everything in between is the local cursor.

That cursor, and the rest of an allocating thread's private bookkeeping, lives in
an **allocation context**. A single-threaded program uses one context; inside a
parallel region each worker gets its own, so workers bump their own slabs and
contend on the shared region only at slab boundaries (see `multicore.md`). The
sequential path is one predicted branch to pick the context, then the bump.

Requests are rounded up to a 16-byte alignment (`ARENA_ALIGNMENT`). That earns
its keep twice: every block is aligned for any type the language stores,
including `double`s and SIMD-friendly matrix buffers; and, as the next layers
rely on, there is always room to stash a pointer-sized word at the start of a
block.

Bump allocation has one limitation: it only moves forward. It can never take
memory back. Reclamation is the job of the layers built on top.

## Reuse without a general heap: size-class free lists

To recycle freed memory without a `malloc`-style best-fit search, the arena sorts
allocations into **size classes** — one class per power of two. A request is
rounded up to the next power of two (with a small floor), and the exponent *is*
the class index. Each class keeps its own free list of blocks of exactly that
size:

- **Freeing** pushes the block onto its class's list.
- **Allocating** pops the class's list if it's non-empty, and otherwise bump-
  allocates a fresh block of the class's size.

Both are O(1) — no search, no coalescing — and a freed block is instantly
available to the next request of the same class. The lists need no side table,
because they are *intrusive*: while a block sits free, its own first bytes hold
the "next free block" pointer, so the list threads through the dead blocks and
costs no extra memory. The lists are per-context, so a worker recycles into and
out of its own lists without touching another thread's.

The trade is internal fragmentation: a request just over a power of two occupies
the next class up, wasting up to (almost) half a block. In exchange, allocation
and freeing are constant-time, and growing a buffer is usually free — see below.

## A malloc-shaped layer: the size header

Finding a block's class needs its size, but a caller freeing a string or array
holds only a pointer. The arena solves this the way a system allocator does: it
stores the size next to the block. The allocation is widened by one alignment
unit; the size is written into that leading header, and the pointer handed back
points just past it. To free, step back to the header, read the recorded size,
and recycle the block into the matching class. The header is a full 16 bytes
rather than 8 so the payload after it stays 16-aligned.

This buys a cheap grow: because each class spans a factor of two, a buffer can
often grow — or shrink — without moving. If the new size lands in the same class,
realloc just rewrites the header and returns the same pointer; only a class
change triggers an allocate-copy-free. That is what makes a growing collection —
a set absorbing elements, a string being appended to — cheap in the common case.

This `malloc`/`free`/`realloc`-shaped trio backs every variable-size payload in
the language.

## Fixed-size objects: a dedicated free list

`Object` headers are all the same size and are created and destroyed constantly.
They don't need the size-class machinery — a single free list for one fixed size
is simpler, and skips both the class computation and the header word. The same
intrusive trick applies: a dead struct's first bytes link it onto the list. A
struct is bump-allocated the first time it's needed and recycled forever after,
zeroed before each reuse so it starts clean. Like the size-class lists, this list
is per-context.

## What lives in the arena, and what doesn't

The arena holds the two kinds of memory that churn with the program's data: the
`Object` headers, and the variable-size payloads (string bytes, set/array slots,
frame key and value arrays).

A few buffers deliberately stay outside it. Matrix element arrays, segment data
buffers, and captured-continuation slices use the system `calloc`/`free` — they
are large, long-lived, and allocated rarely, so the size-class scheme would only
add overhead. The object handle table and a handful of interpreter side-tables
grow with the system `realloc`, since they are resized wholesale rather than
churned block by block. Keeping those out keeps the arena's tenants uniform and
its free lists tight. Those `calloc`'d buffers are also the ones the collector
meters by *byte* size, because they're where a program's memory footprint
actually concentrates — see `gc.md`.

## Who refills the free lists

Nothing above frees by hand — the program never frees its own data. The reclaimer
is the garbage collector: when a sweep finds an unreachable value it returns the
payload and the header to the arena, which is exactly what pushes those blocks
back onto the free lists for the next allocation to pop. The arena supplies the
mechanism; `gc.md` covers the collection policy that drives it.
