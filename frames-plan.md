# Frames — symbol-keyed nested maps

## Context

logicforth needs an associative structure. It can't be `dict` (collides with Forth's word dictionary) or `map` (collides with the higher-order `map`). The design is narrower than a generic hashmap: a **frame** (Minsky/knowledge-representation sense — named slots) whose keys are **interned symbols** mapping to **arbitrary values**, where a value may itself be a frame, forming a tree. It's deliberately the **compound-term type the planned unification layer will build on** (the analog of a Prolog functor), so it must support structural comparison and structural unification, with keys always ground symbols and only values ever becoming logic variables.

Access is path-style: a `/a/b/c` literal is an array of symbols that walks down the tree. This first cut covers **deterministic navigation + mutation**; XPath-style node-set queries (`//`, `*`, predicates) are a later phase.

## Final design

- **Name:** `frame`. Type `T_FRAME`, kind `OBJECT_FRAME`.
- **Keys:** interned symbols only (never logic vars). Equality is symbol-id equality — the id is the `symbol_pool` byte offset, an integer, so no string work.
- **Values:** any `Val`, including nested frames, arrays, logic vars (later).
- **Unordered.** No positional/sibling semantics.
- **Representation: sorted parallel arrays, no hashing.** `keys[]` holds symbol-ids kept sorted; `values[]` is parallel; `len`/`capacity` like sets. Lookup is binary search over densely-packed ints. A hash table is the wrong tool: frames are record-sized with integer keys (flat ordered scan beats hashing), and the dominant op is structural compare/unify, which wants canonical order — a sorted key array makes compare/unify a single linear merge. **Mutable in place / reference semantics.** Costs unification nothing because it binds logic-var *leaf* objects via the trail, never restructuring a frame; discipline (as in Prolog): don't imperatively mutate a live term — `copy` gives an independent one.
- **Bracket scheme (settled):** `[ … ]` arrays, **`{ … }` frames** (`{ :a 1 :b 2 }`), **`< … >` sets** (`< 1 2 3 >`). All balanced and dedicated — each closer (`]`/`}`/`>`) builds exactly one type from the nearest mark, so **no mark discriminator** is needed.
- **Comparisons renamed `lt` / `gt`** (formerly `<` / `>`), to free `< >` for sets. `=` is unchanged.
- **Prompt** is `count|top` with a background color — green normally, red on error — `isatty`-gated so piped/test output is plain text. No brackets: they collided with the `[ ]` array literal that `.s` prints, so a trailing `[3|[2]]` read as another list on the stack. The colored block now distinguishes the prompt from stack output (and `suppress_depth_bg` keeps it a solid color even when the top value is a shaded collection).
- **Accessors reuse the dead `@`/`!`.** `dovar` auto-derefs and no `T_ADDR` reaches the data stack, so `@`(`p_fetch`)/`!`(`p_store`) are unreachable — repurpose:
  - `@` `( frame path -- value )` — get at path; **errors on a missing key**.
  - `!` `( frame path value -- frame )` — set at path; **auto-vivifies** intermediate frames; leaves the frame for chaining.
- **Other ops:** `delete-at` `( frame path -- frame )` (errors if absent), `has?` `( frame path -- bool )` (non-erroring), `update-at` `( frame path xt -- frame )`, `keys`/`values` `( frame -- array )` (top level), `size`, `merge` `( f1 f2 -- f3 )` (new frame, right wins), `copy` (**deep**).
- **Printing:** `print_val` (used by `.s`, embedding, interpolation) renders the compact single-line `{ :a 1 :b 2 }`; the **`.` word pretty-prints a top-level frame** multi-line/indented (expanding nested frames, `print_val` for leaves) — `.s` stays compact. Frames print with `{ }` so they round-trip to the literal.
- **Path literal:** `/a/b/c` → an array of symbols `[ :a :b :c ]` (dynamically constructible; ordinary array). `:foo` symbols and bare `/` (division) are unaffected — a glued `/a/b` token doesn't collide.

## Status

**Done (built, `make test` green at 67/67):**
- `T_FRAME`/`OBJECT_FRAME` type; sorted parallel `keys`/`values`; `make_frame`, `object_new_frame`.
- `object_new` dedup helper + `NEW_OBJECT`/`NEW_FRAME` macros; `cap` field renamed `capacity` throughout.
- `frame_find` (lower-bound binary search), `frame_put`, `frame_delete`; shared `frame_path` (validate) + `frame_walk` (descend; `WALK_ERROR`/`WALK_VIVIFY`/`WALK_PROBE` modes with a `found` out-param).
- `>frame` and `frame` ( keys values -- frame ) builders; `{ … }` frame literal; `< … >` set literal (the `{`↔`<` swap).
- `val_cmp`, `tag_name`, GC (`mark_val` + both free paths), `print_val`/`print_val_compact` frame cases, pretty-printer wired into `.`. Symbols print with a leading `:` everywhere, so values round-trip.
- Access / query ops: `@` (path get), `!` (path set, auto-vivify), `delete-at`, `has?` (non-erroring probe), `update-at` (apply a quotation to a leaf), `keys`, `values`, `size` (polymorphic count; `cardinality` renamed).
- `/a/b/c` path literal in `run_outer` — compiles to a symbol array built **once** (a constant), GC-kept via `mark_body`, so paths cost no per-use allocation.
- Operand-prelude macros: `PEEK_AT(var, depth, op, type)`, `PEEK_FRAME_PATH` / `PEEK_FRAME_PATH_VALUE`, `FRAME_LOOKUP`, `REQUIRE_NONEMPTY_PATH`.
- `lt`/`gt` rename (+ comparison test migration); set→`< >` migration; `count|top` prompt — green, red on error, isatty-gated, no brackets.
- Tests: `tests/67_frames.l4` covers literal, `>frame`/`frame`, sorted keys, nesting, `@`/`!`/`has?`/`delete-at`/`update-at`, `keys`/`values`/`size`, path literals, equality, set dedup.
- Tangent done: `to` auto-creates a global at the REPL (errors inside a definition); shared via `create_variable`.

**Performance (frames lean on the allocator, so it was tuned):**
- Free-list allocator: `gc` repopulates a slot free-list, `object_alloc_slot` pops O(1). Killed an O(n)-per-allocation scan that was 95% of the frame benchmark. `MAX_OBJECTS` raised to 256K; `object_mark` → `uint8_t`.
- `bench/frames.l4` (heavy nested build / long-path read / `has?` / `delete-at`) with a faithful `bench/frames.py` reference. ~47× faster after the above + the constant path literals; now competitive with CPython native dicts. Profile hotspot is now `frame_walk`/`frame_put` (the sorted-array ops), not allocation.

**Remaining:**
- Ops: `merge` ( f1 f2 -- f3 ), deep `copy`.
- Frame **image save/load** — add the `OBJECT_FRAME` case to `p_save_image`/`p_load_image` (serialize `len` then `(symbol-id, val)` pairs); currently the one remaining `-Wswitch` warning.
- Tests for `merge` / deep-`copy` + an image round-trip.

## Verification

- `make test` from the project root.
- `tests/67_frames.l4` covers the implemented ops above; add `merge` / deep-`copy` and an image round-trip as those land.
- Hand-compute expected values before capturing `.expected`, then confirm via the binary.

## JSON interop (planned)

Native conversion between JSON text and logicforth values — `json>` `( string -- value )` to parse and `>json` `( value -- string )` to serialize (names TBD). Frames are the natural home for JSON objects, which is what makes this worth doing now: object keys are strings and frame keys are interned symbols, so they round-trip directly.

Type mapping:
- JSON object ↔ **frame** (string key ↔ interned symbol; symbols hold arbitrary bytes, so keys with spaces/punctuation survive).
- JSON array ↔ **array**.
- JSON string ↔ `T_STRING`; JSON number ↔ `T_FLOAT` (all numbers are f64).
- JSON `true`/`false`/`null` — **open question:** logicforth has no native boolean or null. Candidates: booleans → the `-1.0`/`0.0` float convention, `null` → `T_NONE`; or symbols `:true`/`:false`/`:null` for a lossless round-trip. Decide before implementing.

Notes:
- This revisits the earlier "no JSON" stance in `PLAN.md` — the original objection was the lack of a clean object mapping; frames remove it.
- Hand-rolled recursive-descent parser in C, zero dependencies (same ethos as the POSIX-regex/TSV choices); UTF-8 stored as-is.
- Frames are unordered, so object key order isn't preserved across a round-trip; duplicate keys resolve last-wins (`frame_put` overwrites). Acceptable.

## Deferred (design leaves room, not in this cut)
- Node-set queries: `//` recursive descent, `*` wildcard, predicate filters, returning collections.
- Unification integration: structural unify over frames (equal key set, recurse on values), logic-var values bound via the trail. This cut only ensures frames are value-comparable and structurally inspectable so that layer sits on top cleanly.
