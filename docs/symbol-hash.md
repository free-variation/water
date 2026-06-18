# The Symbol Hash Index in logicforth

This document is a primer on how logicforth interns symbols — turning a name like `first_name` into a small integer id — and on the hash index that makes that lookup constant-time. By the end you should understand:

- What a symbol is in logicforth, where symbols come from, and why interning needs to be fast
- How the symbol pool stores names, and why an id is just a byte offset into it
- Why recognizing a name needs an index rather than a scan of the pool
- The open-addressing hash index that provides it: the `offset + 1` slot encoding, the FNV-1a hash, and why the table never needs to rehash
- How interning probes the index, and how it stays correct when several worker threads intern at once — lock-free reads, inserts under a lock
- The one corner that needs care — keeping the index consistent when the pool is truncated on `forget_user` (the reset path) or image load
- How the table is sized, and what that costs in memory

The implementation is in `src/c/core.c` (`symbol_hash_index`, `probe_symbol`, `intern_symbol`, `rebuild_symbol_hash`, and the `intern_lock` mutex) and `src/c/logicforth.h` (the `symbol_pool` / `symbol_hash` fields on the single global `Vocabulary` and the `SYMBOL_POOL` / `SYMBOL_HASH_SIZE` constants). The aim of this document is to make the index feel like an obvious bookkeeping trick — a side table that answers "have I seen this name?" — rather than a clever optimization bolted onto the interner.

---

## Part 1: What a symbol is, and why interning

A symbol in logicforth is an interned string: a name that has been assigned a stable integer id, so that two occurrences of the same name share one id and compare in a single integer compare instead of a string compare.

Symbols show up in two main places:

- **Symbol literals.** Writing `:first_name` in source pushes a symbol value. The parser interns the text after the colon and tags the result `T_SYMBOL` (see `docs/nan-boxing.md` for how the id rides inside an 8-byte `Val`).
- **Frame keys.** A frame — logicforth's key/value map, and the target of `json>frame` — stores its keys as symbol ids. `{ :a 1 :b 2 }` holds two `cell` keys, each an interned id, kept sorted so lookup is a binary search.

Both paths funnel through one function, `intern_symbol(interp, name)`, which takes a NUL-terminated C string and returns its id — creating the id on first sight, returning the existing one on every sight after.

Because frame construction interns *every key of every object*, and a workload like parsing a few thousand JSON objects re-interns the same key names over and over, interning sits squarely on a hot path. It needs to be cheap not just to add a new name but — more importantly — to recognize a name already seen.

---

## Part 2: The symbol pool — names as offsets

The names themselves live in one flat buffer on the single global `Vocabulary` (`extern Vocabulary vocab;` — there is exactly one, shared across the program; interpreter state lives elsewhere):

```c
char symbol_pool[SYMBOL_POOL];   // contiguous NUL-terminated names
int  symbol_pool_here;           // bytes used so far
```

Names are stored back to back, each NUL-terminated:

```
offset:  0           2     6        ...
pool:    '0' '\0'    '1' '\0'    'a' 'g' 'e' '\0'    ...
         └ id 0 ┘    └ id 2 ┘    └──── id 4 ────┘
```

**A symbol's id is its byte offset into this pool.** Id 0 is the name stored at `symbol_pool[0]`, id 4 is the name at `symbol_pool[4]`, and so on. To recover a symbol's text, index the pool: `&vocab.symbol_pool[id]` is a C string. (The two entries above — `"0"` at offset 0 and `"1"` at offset 2 — are reserved on purpose; they are the boolean symbols `:0` and `:1`, interned first at startup. A handful of other names — `*`, `//`, `.` (the wildcard, descendant, and self path symbols) — are interned right after, so the post-startup pool already holds several reserved entries before any user code runs; that startup high-water mark is what `init_symbol_pool_here` records. More on that in Part 7.)

This representation is compact and makes the text trivially recoverable, but it has no structure that helps you find a name. Given a name, the only way to ask "is it already here, and at what offset?" is to walk the pool.

---

## Part 3: Why recognition needs an index

Walking the pool is a linear search: to find or add one name, `strcmp` the candidate against *every* name already stored, advancing entry by entry with a `strlen` each step. If there are `n` symbols, interning one name is O(n) — and interning a run of `n` distinct names is O(n²).

For a program that interns a few dozen symbols once, that is invisible. The cost bites when the same names are interned repeatedly in a loop — exactly what frame-heavy code does. Each `{ ... }` built at runtime, each object produced by `json>frame`, re-walks the pool once per key, and the dominant work is *re-recognizing* names already present — the case a search should make cheap.

So the pool carries a side table that maps a name straight to its offset. Recognition becomes a hash probe instead of a walk.

---

## Part 4: The hash index over the pool

The pool stays exactly as it is. The side table is a fixed array of slots on the `Vocabulary`:

```c
char        symbol_pool[SYMBOL_POOL];        // interned name bytes
int         symbol_pool_here;
_Atomic int symbol_hash[SYMBOL_HASH_SIZE];   // open-addressing slots: offset+1, or 0 for empty
```

It is **open-addressed**: the table is the storage, with no per-entry allocation and no pointer chasing. A slot holds an offset into the pool — but encoded as `offset + 1`, so that the value `0` can mean "empty." That detail matters: offset 0 is a real, valid symbol id (`:0`, the boolean false), so we cannot use 0 as the empty marker directly. Storing `offset + 1` lets a freshly zeroed table read as all-empty while still letting offset 0 live in it.

The slots are `_Atomic` because several worker threads can intern concurrently; Part 5 covers the access protocol. A single thread sees them as ordinary ints.

The hash is FNV-1a over the name bytes, folded into the table size:

```c
static unsigned int symbol_hash_index(const char *name) {
    unsigned int hash = 2166136261u;          // FNV offset basis
    for (const unsigned char *byte = (const unsigned char *)name; *byte; byte++) {
        hash ^= *byte;
        hash *= 16777619u;                     // FNV prime
    }
    return hash & (SYMBOL_HASH_SIZE - 1);      // SIZE is a power of two
}
```

Because `SYMBOL_HASH_SIZE` is a power of two, the fold is a single mask, and probing wraps with the same mask.

---

## Part 5: Interning — lock-free probe, locked insert

Interning splits in two: a lock-free **probe** that recognizes a name already in the table, and an **insert** that appends a new name to the pool under a lock.

`probe_symbol` hashes the name to a starting slot and walks forward (linear probing) until it finds the name, hits an empty slot, or exhausts the table. Each slot is read with an acquire-load:

```c
static int probe_symbol(const char *name, unsigned int *empty_slot) {
    unsigned int index = symbol_hash_index(name);
    for (int probe = 0; probe < SYMBOL_HASH_SIZE; probe++) {
        int slot = atomic_load_explicit(&vocab.symbol_hash[index], memory_order_acquire);
        if (slot == 0) {                              // empty: name is not present
            *empty_slot = index;
            return -1;
        }
        if (strcmp(&vocab.symbol_pool[slot - 1], name) == 0)
            return slot - 1;                          // found: decode the id
        index = (index + 1) & (SYMBOL_HASH_SIZE - 1); // probe on
    }
    return -2;                                        // scanned every slot, none empty
}
```

`strcmp` runs once per **hash collision** — only when a different name happens to land on the same slot. With a well-spread hash and a half-empty table, that is rarely more than one compare. The common case — re-interning a key seen thousands of times — is: hash the name, land on its slot, one `strcmp` to confirm, return. Constant time.

`intern_symbol` probes first without any lock. On a hit it returns immediately — the overwhelmingly common path, and the only path a single-threaded program ever takes for a known name. On a miss it has to insert:

```c
int intern_symbol(Interpreter *interp, const char *name) {
    unsigned int index;
    int symbol_offset = probe_symbol(name, &index);
    if (symbol_offset >= 0)
        return symbol_offset;                         // already interned

    if (in_parallel)
        pthread_mutex_lock(&intern_lock);

    symbol_offset = probe_symbol(name, &index);       // re-probe under the lock
    if (symbol_offset == -1) {
        int name_bytes = (int)strlen(name) + 1;
        if (vocab.symbol_pool_here + name_bytes > SYMBOL_POOL) {
            fail(interp, "symbol pool full");
            symbol_offset = 0;
        } else {
            symbol_offset = vocab.symbol_pool_here;
            memcpy(&vocab.symbol_pool[symbol_offset], name, (size_t)name_bytes);
            vocab.symbol_pool_here += name_bytes;
            atomic_store_explicit(&vocab.symbol_hash[index], symbol_offset + 1,
                                  memory_order_release);
        }
    } else if (symbol_offset == -2) {
        fail(interp, "symbol table full");
        symbol_offset = 0;
    }

    if (in_parallel)
        pthread_mutex_unlock(&intern_lock);
    return symbol_offset;
}
```

Three things make this correct:

- **The insert is serialized.** Appending to the pool bumps `symbol_pool_here` and writes the name bytes; two threads doing that at once would corrupt the pool. The `intern_lock` mutex serializes inserts, so the append is atomic with respect to other inserters.
- **It re-probes under the lock (double-checked).** Between the first lock-free probe and acquiring the lock, another thread may have interned the *same* new name. The second `probe_symbol` catches that: if the name is now present, intern returns its id and no second copy is made. Two threads racing to intern the same new name dedup to one offset.
- **Reads and the publish are ordered.** The writer fills the pool bytes, then publishes the slot with a release-store. A reader's acquire-load of that slot pairs with the release, so any thread that sees the published `offset + 1` also sees the name bytes already in the pool — no reader can dereference a half-written entry.

The lock is taken only when `in_parallel` is set. Outside a parallel region — every ordinary REPL or script run — interning never touches the mutex; the atomics compile to plain loads and stores on the single thread. `docs/multicore.md` covers `in_parallel` and the worker model that makes concurrent interning possible.

A worked collision: suppose `age` hashes to slot 5, and that slot is occupied. The probe reads its stored value, subtracts 1 to get the pool offset, and `strcmp`s the name there against `age`. If it matches, done. If not (some other name owns slot 5), it moves to slot 6 and tries again, wrapping at the end of the table. An insert stops at the first empty slot along that same probe path, so a name is always found on the exact path its insertion would have taken.

---

## Part 6: Sizing the table

Open-addressing tables usually grow and rehash when they get too full, because probe chains lengthen badly past ~70% load. logicforth's table never grows — it is sized once, zeroed with the rest of the `Vocabulary`, and that is the whole lifecycle (no rehash code, no growth check, no resize). What that buys in headroom comes down to two constants: `SYMBOL_POOL` (the pool's size in bytes) and `SYMBOL_HASH_SIZE` (the table's slot count).

The smallest possible symbol is a one-character name plus its NUL terminator — 2 bytes — so the pool can hold at most `SYMBOL_POOL / 2` distinct symbols, ever.

The invariant that matters is `SYMBOL_HASH_SIZE ≥ SYMBOL_POOL / 2`: with at least that many slots, the table can never be more than half full, an empty slot always exists, and the linear-probe loop always terminates. logicforth sizes the table below that, so it can fill *before* the pool does — a program interning enough distinct symbols (JSON ingests strings as symbols, so high counts are reachable) exhausts the slots. To keep that from becoming an infinite probe, `probe_symbol` caps its scan at `SYMBOL_HASH_SIZE` steps and returns `-2`, and `intern_symbol` fails with `"symbol table full"` when no empty slot remains. Either way the table never rehashes: at or above the threshold it cannot overflow; below it, interning fails cleanly when the table is full.

The cost is memory: `SYMBOL_HASH_SIZE` ints, always allocated, mostly empty for a typical program that interns a few hundred symbols. Keeping `SYMBOL_HASH_SIZE ≥ SYMBOL_POOL / 2` preserves the overflow-impossible guarantee; sizing it smaller trades that headroom for less memory, with the probe cap as the backstop.

---

## Part 7: The corner — keeping the index consistent on truncation

The index is redundant state: it duplicates information already implied by the pool. Redundant state has to be kept in step with its source, and there is one place where the source changes out from under it.

logicforth can **truncate the symbol pool**. Two places do it:

- **`forget_user`** rolls state back to its post-startup snapshot: `symbol_pool_here = init_symbol_pool_here`, dropping every symbol interned since startup. This is the block the `reset` word triggers (along with `reload` and the image error-recovery path), so the reset family of operations all funnel through here.
- **`p_load_image`** (load-image) restores a saved image, setting `symbol_pool_here` to the startup snapshot plus the image's own symbol bytes.

After either, the pool's logical contents have changed, but the hash table still points at the old layout — with slots referring to offsets that are now beyond `symbol_pool_here`, or to names that have been overwritten. A lookup could match a stale entry and return a garbage id.

The fix is to rebuild the index from the surviving pool whenever the pool is truncated:

```c
void rebuild_symbol_hash(void) {
    memset(vocab.symbol_hash, 0, sizeof(vocab.symbol_hash));      // all empty
    for (int offset = 0; offset < vocab.symbol_pool_here; ) {
        const char *name = &vocab.symbol_pool[offset];
        unsigned int index = symbol_hash_index(name);
        while (vocab.symbol_hash[index] != 0)
            index = (index + 1) & (SYMBOL_HASH_SIZE - 1);
        vocab.symbol_hash[index] = offset + 1;
        offset += (int)strlen(name) + 1;
    }
}
```

It clears the table and re-inserts every name still in the pool, walking the pool entry by entry — but only once, at reset or image load, which is rare and always single-threaded (no worker is in flight), so it needs no locking or atomics. `rebuild_symbol_hash` is called right after each of the two `symbol_pool_here = ...` truncations.

This is why the reserved boolean symbols survive a reset cleanly. `:0` and `:1` are interned first at startup, landing at offsets 0 and 2 — *below* the reset snapshot watermark. A rebuild re-scans `[0, symbol_pool_here)`, which always includes them, so they keep their ids and their `offset + 1` slots. The `offset + 1` encoding and the rebuild work together: the false symbol at offset 0 round-trips through both the live table and a rebuilt one.

---

## Part 8: Where to look in the source

In `src/c/logicforth.h`:

- **`SYMBOL_POOL` and `SYMBOL_HASH_SIZE`** — when `SYMBOL_HASH_SIZE ≥ SYMBOL_POOL / 2` the table can't overflow and never rehashes; the current config sizes the hash below that, so the probe caps at `SYMBOL_HASH_SIZE` steps and interning fails with "symbol table full" when the table fills (Part 6).
- **`symbol_pool` / `symbol_pool_here` / `symbol_hash`** on the `Vocabulary` struct — the name buffer, its high-water mark, and the `_Atomic` open-addressing index.

In `src/c/core.c`:

- **`symbol_hash_index`** — FNV-1a over the name, masked to the table size.
- **`probe_symbol`** — the lock-free probe: hash, acquire-load each slot, compare on collision; returns the id, the empty slot index, or `-2` for a full table.
- **`intern_symbol`** — probe, and on a miss insert under `intern_lock` with a re-probe (double-check) and a release-store publish; the lock is taken only when `in_parallel`.
- **`rebuild_symbol_hash`** — re-inserts every pooled name; takes no argument (it reads the global `vocab` directly); called after the pool is truncated, single-threaded.
- The two truncation sites — `forget_user` (which the `reset` word, `reload`, and image error-recovery all route through) and `p_load_image` — each set `symbol_pool_here` back and then call `rebuild_symbol_hash`.

For broader context:

- **`docs/nan-boxing.md`** — how a symbol id is carried inside an 8-byte `Val` (the `T_SYMBOL` tag and the payload that holds the offset).
- **`docs/gc.md`** — frames hold symbol ids as keys; the collector walks frame values but the keys are plain integer ids, not heap references.
- **`docs/multicore.md`** — the worker model and the `in_parallel` flag that gate the interning lock.
