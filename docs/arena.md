# The arena allocator

logicforth runs its heap through a custom allocator instead of calling
`malloc`/`free` per object. The design buys two things at once: allocation that
costs little more than a pointer increment, and reclamation that recycles freed
memory in constant time with no search. This document builds it up from the
bottom.

## The shape of the heap

Every heap value in logicforth is an `Object` struct plus, usually, a separately
allocated payload — a string's bytes, an array's `Val` slots, a frame's key and
value arrays. The structs and most of those payloads come from the arena. (Two
kinds of buffer sit outside it; they're noted at the end.)

The arena is one contiguous region of memory, held in a global `arena` struct
and carved up by hand.

## Reserve big, page in lazily

At startup the arena reserves a large virtual address range in a single `mmap`:

```c
arena.base = mmap(NULL, ARENA_RESERVE, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANON, -1, 0);
arena.used     = 0;
arena.reserved = ARENA_RESERVE;
```

`ARENA_RESERVE` is 16 GiB. Reserving that much sounds extravagant, but `MAP_ANON`
memory is *demand-paged*: the kernel hands back a range of addresses and commits
physical pages only when you first touch them. Reserving 16 GiB of address space
costs almost nothing; a program that allocates a few megabytes uses a few
megabytes of RAM. The point of the large reservation is that the region never
has to move or grow — `arena.base` is fixed for the life of the process, so every
pointer into the arena stays valid.

`arena.used` is the high-water mark: the offset of the next free byte.

## Bump allocation

The base layer hands out memory by advancing `used`:

```c
static inline void *arena_alloc(size_t bytes) {
    size_t advance = (bytes + (ARENA_ALIGNMENT - 1)) & ~(size_t)(ARENA_ALIGNMENT - 1);
    if (arena.used + advance > arena.reserved) { /* exhausted: report and exit */ }
    void *p = arena.base + arena.used;
    arena.used += advance;
    return p;
}
```

That is the whole fast path: round the request up to a 16-byte boundary, return
the current top, advance the top. No free lists to consult, no header to write —
a bump allocation is a handful of instructions.

The 16-byte alignment (`ARENA_ALIGNMENT`) earns its keep twice over: every block
is suitable for any type the language stores, including `double`s and
SIMD-friendly matrix buffers; and, as the layers above rely on, there is always
room to stash a pointer-sized word at the start of a block.

Bump allocation has one limitation: it only moves forward. `arena_alloc` cannot
take memory back. Reclamation is the job of the layers built on top of it.

## Reuse without a general heap: size-class free lists

To recycle freed memory without a `malloc`-style best-fit search, the arena
sorts allocations into **size classes** — one class per power of two:

```c
static inline int size_class_index(size_t bytes) {
    if (bytes <= 16) return 4;                 // smallest class is 2^4 = 16 bytes
    return 64 - __builtin_clzll(bytes - 1);    // ceil(log2(bytes))
}
```

The index *is* the exponent: class `k` holds blocks of `1 << k` bytes, and a
request is rounded up to the next power of two (never below 16). Each class keeps
its own free list:

```c
static void *arena_alloc_sized(size_t bytes) {
    int cls = size_class_index(bytes);
    void *block = arena.size_class_free[cls];
    if (block) {                               // reuse a freed block
        arena.size_class_free[cls] = *(void **)block;
        return block;
    }
    return arena_alloc((size_t)1 << cls);      // none free: bump a fresh one
}

static void arena_free_sized(void *block, size_t bytes) {
    int cls = size_class_index(bytes);
    *(void **)block = arena.size_class_free[cls];
    arena.size_class_free[cls] = block;
}
```

Freeing pushes the block onto its class's list; allocating pops it. Both are
O(1), and there is no side bookkeeping: the free list is *intrusive* — while a
block sits free, its own first 8 bytes hold the "next free block" pointer. The
block stores the list, so the list costs no extra memory. `size_class_free` is
just an array of list heads, one per class.

The trade is internal fragmentation: a 17-byte request occupies a 32-byte block,
so power-of-two rounding can waste up to half of a block. In exchange, allocation
and freeing are constant-time — no search, no coalescing — and a freed block is
instantly available to the next request of the same class.

## A malloc-shaped layer: the size header

`arena_free_sized` needs a block's size to find its class — but a caller freeing
a string or an array has only a pointer, not a length. The `arena_malloc` family
solves this the way a system allocator does: it stores the size next to the
block.

```c
void *arena_malloc(size_t bytes) {
    void *block = arena_alloc_sized(bytes + ARENA_ALIGNMENT);
    *(size_t *)block = bytes + ARENA_ALIGNMENT;     // header: total size
    return (char *)block + ARENA_ALIGNMENT;         // payload starts after it
}

void arena_free(void *payload) {
    if (!payload) return;
    void *block = (char *)payload - ARENA_ALIGNMENT;
    arena_free_sized(block, *(size_t *)block);      // read the size back, recycle
}
```

Each allocation is widened by one `ARENA_ALIGNMENT` (16-byte) header. The total
size is written into the header; the pointer handed to the caller points *past*
it. To free, step back 16 bytes, read the recorded size, and hand the block to
`arena_free_sized`. The header is 16 bytes rather than 8 so the payload after it
stays 16-aligned.

Growing a block is cheap when it stays in the same class:

```c
void *arena_realloc(void *payload, size_t bytes) {
    if (!payload) return arena_malloc(bytes);
    void *block = (char *)payload - ARENA_ALIGNMENT;
    size_t old_total = *(size_t *)block;
    size_t new_total = bytes + ARENA_ALIGNMENT;
    if (size_class_index(new_total) == size_class_index(old_total)) {
        *(size_t *)block = new_total;       // same class: just adjust the header
        return payload;
    }
    void *grown = arena_malloc(bytes);      // new class: allocate, copy, free
    memcpy(grown, payload, min(old_payload_bytes, bytes));
    arena_free(payload);
    return grown;
}
```

Because each class spans a factor of two, a buffer can often grow — or shrink —
without moving: if the new size lands in the same power-of-two class, `realloc`
rewrites the header and returns the same pointer. Only a class change triggers an
allocate-copy-free. This is what makes a growing collection — a set absorbing
elements, a string being appended to — cheap.

`arena_malloc` / `arena_free` / `arena_realloc` are drop-in stand-ins for the C
trio, and they back every variable-size payload in the language.

## Fixed-size objects: a dedicated free list

`Object` structs are all the same size and are created and destroyed constantly.
They don't need the size-class machinery — a single free list for one fixed size
is simpler, and skips both the class computation and the header word:

```c
static Object *arena_alloc_object(void) {
    Object *fresh;
    if (arena.freed_object_structs) {            // pop a recycled struct
        fresh = arena.freed_object_structs;
        arena.freed_object_structs = *(void **)fresh;
    } else {
        fresh = arena_alloc(sizeof(Object));     // none free: bump a fresh one
    }
    memset(fresh, 0, sizeof(Object));
    return fresh;
}

static void arena_free_object(Object *obj) {     // push onto the free list
    *(void **)obj = arena.freed_object_structs;
    arena.freed_object_structs = obj;
}
```

Same intrusive trick as the size classes — the list threads through the first 8
bytes of each dead struct — but one list (`freed_object_structs`) instead of one
per class. A struct is bump-allocated the first time it's needed and recycled
forever after; `arena_alloc_object` zeroes it before returning, so a reused
struct starts clean.

## What lives in the arena, and what doesn't

The arena holds the two kinds of memory that churn with the program's data:

- **`Object` structs** — via `arena_alloc_object` / `arena_free_object`.
- **Variable-size payloads** — a string's bytes, a set's or array's `Val`
  items, a frame's key and value arrays — via `arena_malloc` / `arena_free` /
  `arena_realloc`.

A few buffers deliberately stay outside it. Matrix element arrays and
captured-continuation slices use the system `calloc`/`free` — they are large,
long-lived, and allocated rarely, so the size-class scheme would only add
overhead. The object *handle table* and a handful of interpreter side-tables grow
with the system `realloc`, since they are resized wholesale rather than churned
block by block. Keeping those out keeps the arena's tenants uniform and its free
lists tight.

## Who refills the free lists

Nothing above frees by hand — the program never calls `arena_free` on its own.
The reclaimer is the garbage collector: when a sweep finds an unreachable object
it returns the payload with `arena_free` and the struct with `arena_free_object`,
which is exactly what pushes those blocks back onto the free lists for the next
allocation to pop. The arena supplies the mechanism; `docs/gc.md` covers the
collection policy that drives it.
