# The symbol hash index in water

This is a primer on how water interns symbols — turning a name like
`first_name` into a small integer id — and on the hash index that makes the
lookup constant-time. By the end you should understand:

- What a symbol is, where symbols come from, and why interning sits on a hot path
- How the symbol pool stores names, and why an id is just a byte offset into it
- Why recognizing a name needs an index, not a scan
- The open-addressing index that provides it: the `offset + 1` slot encoding, the
  hash, and why the table never rehashes
- How interning stays correct when several workers intern at once
- The one corner — keeping the index consistent when the pool is truncated

It's a conceptual tour; the interner lives in `src/c/core.c`, over the symbol pool
and hash table held on the single global vocabulary.

---

## Part 1: What a symbol is, and why interning

A symbol is an interned string: a name assigned a stable integer id, so two
occurrences of the same name share one id and compare in a single integer compare
instead of a string compare. Symbols show up in two main places — symbol literals
(`:first_name` in source pushes one) and frame keys (a frame stores its keys as
symbol ids, kept sorted for binary-search lookup). Both funnel through one
operation: take a name, return its id, creating the id on first sight and
returning the existing one thereafter.

Because frame construction interns *every key of every object*, a workload like
parsing thousands of JSON objects re-interns the same key names over and over. So
interning must be cheap not just to add a new name but — more importantly — to
*recognize* a name already seen.

---

## Part 2: The pool — names as offsets

The names live in one flat buffer, stored back to back, each NUL-terminated:

```
offset:  0           2     6        ...
pool:    '0' '\0'    '1' '\0'    'a' 'g' 'e' '\0'    ...
         └ id 0 ┘    └ id 2 ┘    └──── id 4 ────┘
```

**A symbol's id is its byte offset into this pool.** To recover a name's text, you
index the pool at its id. (The first two entries, `"0"` and `"1"`, are reserved on
purpose: they're the boolean symbols `:0` and `:1`, interned first at startup,
followed by a few path symbols like `*` and `//`. That post-startup high-water
mark matters in Part 6.)

The representation is compact and makes the text trivially recoverable, but it has
no structure that helps you *find* a name. Given a name, the only way to ask "is it
already here, and where?" is to walk the pool — `strcmp` against each stored name
in turn. That's O(n) per lookup, O(n²) to intern n distinct names, and the cost
bites exactly when the same names are interned repeatedly in a loop, which is what
frame-heavy code does. So the pool carries a side table that maps a name straight
to its offset, turning recognition into a hash probe.

---

## Part 3: The index over the pool

The pool stays exactly as it is; the index is a separate fixed array of slots.
It's **open-addressed** — the array *is* the storage, no per-entry allocation, no
pointer chasing. A slot holds a pool offset, but encoded as `offset + 1`, so that
the value 0 can mean "empty." That encoding earns its keep: offset 0 is a real,
valid id (the boolean `:0`), so 0 can't double as the empty marker directly;
storing `offset + 1` lets a freshly zeroed table read as all-empty while still
letting offset 0 live in it.

A slot is chosen by hashing the name (an FNV-1a hash over the bytes) and folding
the result to the table size. The table size is a power of two, so the fold is a
single mask and probing wraps with the same mask.

---

## Part 4: Interning — lock-free probe, locked insert

Interning is a **probe** that recognizes a name already present, and an **insert**
that appends a new name under a lock.

The probe hashes the name to a starting slot and walks forward (linear probing):
an empty slot means the name isn't present; a non-empty slot is decoded to an
offset and the name there is compared, returning the id on a match and probing on
otherwise. A `strcmp` runs only on a hash *collision* — a different name landing on
the same slot — so with a well-spread hash and a half-empty table the common case
(re-interning a known key) is: hash, land on the slot, one compare, return.
Constant time.

Interning probes first with no lock and, on a hit, returns immediately — the
overwhelmingly common path, and the only path a single-threaded program takes for
a known name. A miss has to insert, and concurrency makes that the delicate part.
Three things keep it correct when several workers intern at once:

- **The insert is serialized.** Appending a name bumps the pool's high-water mark
  and writes the bytes; two threads doing that at once would corrupt the pool. A
  mutex serializes inserters.
- **It re-probes under the lock (double-checked).** Between the first lock-free
  probe and taking the lock, another thread may have interned the *same* new name.
  The second probe catches that and returns the existing id, so two threads racing
  to intern one new name dedup to a single offset.
- **The publish is ordered.** The writer fills the pool bytes, then publishes the
  slot with a release-store; a reader's acquire-load of that slot pairs with the
  release, so any thread that sees the published slot also sees the name bytes
  already in place — no reader dereferences a half-written entry.

The lock is taken only inside a parallel region; an ordinary single-threaded run
never touches it, and the atomics compile to plain loads and stores. The worker
model that makes concurrent interning possible is `multicore.md`'s subject.

---

## Part 5: Sizing the table

Open-addressing tables usually grow and rehash as they fill, because probe chains
lengthen badly past a moderate load. water's table never grows — it's sized
once and that's the whole lifecycle. Two constants set the headroom: the pool's
byte size and the table's slot count.

The invariant that matters is **slots ≥ pool-bytes / 2**: the smallest symbol is a
one-character name plus its terminator (two bytes), so the pool holds at most
pool-bytes/2 distinct names ever; with at least that many slots the table can
never be more than half full, an empty slot always exists, and the linear-probe
loop always terminates. If the table is sized *below* that invariant it can fill
before the pool does (interning enough distinct symbols — JSON ingests strings as
symbols, so high counts are reachable), so the probe caps its scan at the table
size and interning fails cleanly with "symbol table full" rather than looping
forever. Either way the table never rehashes. The trade is memory against headroom:
more slots than names are always allocated and mostly empty, and shrinking the
table buys memory at the cost of that overflow-impossible guarantee, with the probe
cap as the backstop.

---

## Part 6: The corner — staying consistent on truncation

The index is *redundant* state: it duplicates information the pool already implies,
so it has to be kept in step with the pool, and there's one place the pool changes
out from under it. water can **truncate the pool** — `forget_user` (the path
behind the `reset` family) rolls it back to the post-startup snapshot, dropping
every symbol interned since; loading an image resets it to the snapshot plus the
image's own names. After either, the pool's contents have changed but the index
still points at the old layout, with slots referring to offsets now beyond the
pool or to overwritten names. A lookup could match a stale entry and return garbage.

The fix is to **rebuild the index from the surviving pool** whenever the pool is
truncated: clear the table and re-insert every name still in it, walking the pool
entry by entry. This runs only at reset or image load — rare, and always
single-threaded with no worker in flight, so it needs no locking. It's also why
the reserved boolean symbols survive a reset cleanly: they live below the snapshot
watermark, so the rebuild always re-scans and re-inserts them, and the `offset + 1`
encoding lets the false symbol at offset 0 round-trip through both the live table
and a rebuilt one.

---

For broader context: `nan-boxing.md` is how a symbol id rides inside an 8-byte
value; `gc.md` notes that frames hold symbol ids as keys (plain integers, not heap
references, so the collector doesn't trace them); `multicore.md` is the worker
model and the in-region flag that gate the interning lock.
