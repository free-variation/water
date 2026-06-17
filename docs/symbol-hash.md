# The Symbol Hash Index in logicforth

This document is a primer on how logicforth interns symbols — turning a name like `first_name` into a small integer id — and how a hash index over the symbol pool turned that operation from a linear scan into a constant-time lookup. By the end you should understand:

- What a symbol is in logicforth, where symbols come from, and why interning needs to be fast
- How the symbol pool stores names, and why an id is just a byte offset into it
- What the original `intern_symbol` did, and why it was O(n) per call — quadratic over a run of distinct names
- How a JSON benchmark made that cost visible, and what the profile actually showed
- The open-addressing hash index that fixes it: the `offset + 1` slot encoding, the FNV-1a hash, and why the table never needs to rehash
- The one corner that needs care — keeping the index consistent when the pool is truncated on `forget_user` (the reset path) or image load
- What the change cost in memory, and what it bought in time

The implementation is in `src/c/core.c` (`symbol_hash_index`, `intern_symbol`, `rebuild_symbol_hash`) and `src/c/logicforth.h` (the `symbol_pool` / `symbol_hash` fields on the single global `Vocabulary` and the `SYMBOL_POOL` / `SYMBOL_HASH_SIZE` constants). The aim of this document is to make the index feel like an obvious bookkeeping trick — a side table that answers "have I seen this name?" — rather than a clever optimization bolted onto the interner.

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

## Part 3: The naive interner and its cost

The original `intern_symbol` did exactly that walk:

```c
int intern_symbol(Interpreter *interp, const char *name) {
    for (int i = 0; i < vocab.symbol_pool_here; ) {
        if (strcmp(&vocab.symbol_pool[i], name) == 0)
            return i;                                   // found
        i += (int)strlen(&vocab.symbol_pool[i]) + 1;    // next entry
    }
    /* not found: append name to the pool, return its offset */
}
```

Read it as a linear search. To find or add one name, it `strcmp`s the candidate against *every* name already in the pool, advancing entry by entry with a `strlen` each step. If there are `n` symbols, interning one name is O(n) — and interning a run of `n` distinct names is O(n²).

For a small program that interns a few dozen symbols once, this is invisible. The cost only bites when the same names are interned repeatedly in a loop — which is exactly what frame-heavy code does. Each `{ ... }` built at runtime, each object produced by `json>frame`, re-walks the pool once per key.

---

## Part 4: How the cost became visible

The `bm_json_loads` benchmark parses the same set of JSON objects tens of thousands of times. Each object has ~30 string keys, and each key is interned on every parse. Against CPython's C `json` module, logicforth's `json>frame` was about 2.4× slower — the first benchmark it lost.

A sampling profile (`sample` on a heavy parse loop) made the reason unambiguous. Sorted by where samples landed:

| function | samples | what |
|---|---|---|
| `strcmp` (+ stubs) | ~2285 | the pool walk's per-entry compare |
| `strlen` (+ stubs) | ~430 | the pool walk advancing entry to entry |
| `json_parse_string` | 605 | the string-decode loop |
| `malloc` / `free` / `calloc` | ~1500 | allocation churn |
| `intern_symbol` (own frame) | 192 | the loop itself |

(These sample counts are historical figures from the original profiling run, kept here for illustration; there is no profile artifact checked into the repo to reproduce them exactly.)

About **half the samples were the `strcmp`/`strlen` scan inside `intern_symbol`.** Every key the parser interned compared against the whole pool. The pool stayed small (the ~30 key names repeat across objects, so they dedup to ~30 entries), but the scan re-walked all ~30 on every one of the millions of interns.

This is the giveaway: the work was not interning new names, it was *re-recognizing* names already present — the case a search should make cheap.

---

## Part 5: The fix — a hash index over the pool

The pool stays exactly as it is. We add a side table that maps a name to its pool offset, so recognition is a hash probe instead of a walk.

The table is a fixed array of ints on the `Vocabulary`:

```c
char symbol_pool[SYMBOL_POOL];        // interned name bytes
int  symbol_pool_here;
int  symbol_hash[SYMBOL_HASH_SIZE];   // open-addressing slots: offset+1, or 0 for empty
```

It is **open-addressed**: the table is the storage, with no per-entry allocation and no pointer chasing. A slot holds an offset into the pool — but encoded as `offset + 1`, so that the value `0` can mean "empty." That detail matters: offset 0 is a real, valid symbol id (`:0`, the boolean false), so we cannot use 0 as the empty marker directly. Storing `offset + 1` lets a freshly `calloc`-zeroed table read as all-empty while still letting offset 0 live in it.

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

## Part 6: Interning with the index

`intern_symbol` becomes a probe. Hash the name to a starting slot and walk forward (linear probing) until we either find the name or hit an empty slot:

```c
int intern_symbol(Interpreter *interp, const char *name) {
    unsigned int index = symbol_hash_index(name);
    while (vocab.symbol_hash[index] != 0) {           // slot occupied
        int offset = vocab.symbol_hash[index] - 1;    // decode the id
        if (strcmp(&vocab.symbol_pool[offset], name) == 0)
            return offset;                            // found
        index = (index + 1) & (SYMBOL_HASH_SIZE - 1); // probe on
    }

    /* empty slot reached: name is new. Append to the pool... */
    int length = (int)strlen(name) + 1;
    if (vocab.symbol_pool_here + length > SYMBOL_POOL) {
        fail(interp, "symbol pool full");
        return 0;
    }
    int offset = vocab.symbol_pool_here;
    memcpy(&vocab.symbol_pool[offset], name, (size_t)length);
    vocab.symbol_pool_here += length;
    vocab.symbol_hash[index] = offset + 1;            // ...and record it here
    return offset;
}
```

The crucial change is *when* `strcmp` runs. In the linear interner it ran once per existing symbol. Here it runs once per **hash collision** — only when a different name happens to land on the same slot. With a well-spread hash and a half-empty table, that is rarely more than one compare. The common case — re-interning a key seen thousands of times before — is: hash the name, land on its slot, one `strcmp` to confirm, return. Constant time.

A worked collision: suppose `age` hashes to slot 5, and that slot is occupied. We read its stored value, subtract 1 to get the pool offset, and `strcmp` the name there against `age`. If it matches, done. If not (some other name owns slot 5), we move to slot 6 and try again, wrapping at the end of the table. Insertion stops at the first empty slot along that same probe path, so a name is always found on the exact path its insertion would have taken.

---

## Part 7: Sizing the table

Open-addressing tables usually grow and rehash when they get too full, because probe chains lengthen badly past ~70% load. logicforth's table never grows — it is sized once, `calloc`-zeroed with the rest of the `Vocabulary`, and that is the whole lifecycle (no rehash code, no growth check, no resize). What that buys in headroom comes down to two constants: `SYMBOL_POOL` (the pool's size in bytes) and `SYMBOL_HASH_SIZE` (the table's slot count).

The smallest possible symbol is a one-character name plus its NUL terminator — 2 bytes — so the pool can hold at most `SYMBOL_POOL / 2` distinct symbols, ever.

The invariant that matters is `SYMBOL_HASH_SIZE ≥ SYMBOL_POOL / 2`: with at least that many slots, the table can never be more than half full, an empty slot always exists, and the linear-probe loop always terminates. logicforth sizes the table below that, so it can fill *before* the pool does — a program interning enough distinct symbols (JSON ingests strings as symbols, so high counts are reachable) exhausts the slots. To keep that from becoming an infinite probe, `intern_symbol` caps its scan at `SYMBOL_HASH_SIZE` steps and fails with `"symbol table full"` when no empty slot remains. Either way the table never rehashes: at or above the threshold it cannot overflow; below it, interning fails cleanly when the table is full.

The cost is memory: `SYMBOL_HASH_SIZE` ints, always allocated, mostly empty for a typical program that interns a few hundred symbols. Keeping `SYMBOL_HASH_SIZE ≥ SYMBOL_POOL / 2` preserves the overflow-impossible guarantee; sizing it smaller trades that headroom for less memory, with the probe cap as the backstop.

---

## Part 8: The corner — keeping the index consistent on truncation

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

It clears the table and re-inserts every name still in the pool, walking the pool exactly the way the old linear interner did — but only once, at reset, which is rare. `rebuild_symbol_hash` is called right after each of the two `symbol_pool_here = ...` truncations.

This is why the reserved boolean symbols survive a reset cleanly. `:0` and `:1` are interned first at startup, landing at offsets 0 and 2 — *below* the reset snapshot watermark. A rebuild re-scans `[0, symbol_pool_here)`, which always includes them, so they keep their ids and their `offset + 1` slots. The `offset + 1` encoding and the rebuild work together: the false symbol at offset 0 round-trips through both the live table and a rebuilt one.

---

## Part 9: What it cost, and what it bought

**Memory.** `SYMBOL_HASH_SIZE` ints added to the global `Vocabulary` (Part 7), always allocated. For a process that interns a few hundred symbols, that table is nearly all empty. This is the price of a fixed-size, never-rehashing table.

**A second copy of the truth.** The index restates what the pool already encodes, so any code that mutates the pool out of band has to keep the index in step. Today that is exactly the two truncation sites, both calling `rebuild_symbol_hash`. A future operation that edits the pool would need the same discipline.

The payoff was direct. On `bm_json_loads`, swapping the linear interner for the hash index roughly halved parse time:

| json-loads | elapsed |
|---|---|
| linear `intern_symbol` | 2.38 s |
| hash index | 1.17 s |

(These elapsed figures are historical/illustrative numbers from the original change; no benchmark artifact backing them is checked into the repo, so treat the ~2× as the durable takeaway rather than the exact seconds.)

That confirmed the profile's claim that interning was about half the work. And the win is not JSON-specific: every symbol literal, every frame key, every dictionary-of-symbols operation in the interpreter goes through `intern_symbol`, so all of them got the same constant-time recognition. JSON parsing was merely the workload that made a long-standing O(n) scan loud enough to fix.

---

## Part 10: Where to look in the source

In `src/c/logicforth.h`:

- **`SYMBOL_POOL` and `SYMBOL_HASH_SIZE`** — when `SYMBOL_HASH_SIZE ≥ SYMBOL_POOL / 2` the table can't overflow and never rehashes; the current config sizes the hash below that, so `intern_symbol` caps its probe and fails with "symbol table full" when the table fills (Part 7).
- **`symbol_pool` / `symbol_pool_here` / `symbol_hash`** on the `Vocabulary` struct — the name buffer, its high-water mark, and the open-addressing index.

In `src/c/core.c`:

- **`symbol_hash_index`** — FNV-1a over the name, masked to the table size.
- **`intern_symbol`** — the probe: hash, compare on collision, insert at the first empty slot.
- **`rebuild_symbol_hash`** — re-inserts every pooled name; takes no argument (it reads the global `vocab` directly); called after the pool is truncated.
- The two truncation sites — `forget_user` (which the `reset` word, `reload`, and image error-recovery all route through) and `p_load_image` — each set `symbol_pool_here` back and then call `rebuild_symbol_hash`.

For broader context:

- **`docs/nan-boxing.md`** — how a symbol id is carried inside an 8-byte `Val` (the `T_SYMBOL` tag and the payload that holds the offset).
- **`docs/gc.md`** — frames hold symbol ids as keys; the collector walks frame values but the keys are plain integer ids, not heap references.
