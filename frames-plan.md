# Frames — symbol-keyed nested maps

## Context

logicforth needs an associative structure. It can't be called `dict` (collides with Forth's word dictionary) or `map` (collides with the higher-order `map`). The design that emerged is narrower and more purposeful than a generic hashmap: a **frame** (Minsky/knowledge-representation sense — named slots) whose keys are **interned symbols** mapping to **arbitrary values**, where a value may itself be a frame, forming a tree. This is deliberately the **compound-term type the planned unification layer will build on** (the analog of a Prolog functor), so it must support structural comparison and structural unification, with keys always ground symbols and only values ever becoming logic variables.

Access is path-style: a `/a/b/c` literal is an array of symbols that walks down the tree. This first cut covers **deterministic navigation + mutation**; the broader XPath-style node-set queries (`//` descendant, `*` wildcard, predicate filters) are a later phase the design leaves room for.

## Locked design decisions

- **Name:** `frame`. Type `T_FRAME`, kind `OBJECT_FRAME`.
- **Keys:** interned symbols only (never logic vars). Key equality is symbol-id equality — the id is the `symbol_pool` byte offset, an integer, so no string work.
- **Values:** any `Val`, including nested frames, arrays, logic vars (later).
- **Unordered.** No positional/sibling semantics.
- **Representation:** **sorted parallel arrays — no hashing.** `keys[]` holds symbol-ids kept in sorted order; `values[]` is parallel. Lookup is binary search (or a linear scan for tiny `n`) over densely-packed ints; values are touched only on a hit. This is the proven set representation (sorted `items[]` + binary search) applied to key/value pairs. A hash table is the wrong tool here: frames are small (record-sized) with integer keys, so hashing/probing overhead loses to a flat ordered scan; and the dominant operation is structural compare/unify (frames are the logic layer's terms), which wants a canonical order — a sorted key array makes compare/unify a single linear merge with no per-key search, whereas a hash has no order and forces per-key probing. **Mutable in place / reference semantics** (like arrays/sets). Costs unification nothing because it binds logic-var *leaf* objects through the trail, never restructuring a frame. Discipline (same as Prolog): don't imperatively mutate a live term; `copy` produces an independent one.
- **Literal swap:** frames take `{ :a 1 :b 2 }`; **sets move to `#{ 1 2 3 }`**. Forced because `{ :a :b }` is irresolvably ambiguous between a 2-symbol set and a one-entry frame; matches Clojure (`{ }` maps, `#{ }` sets). The printed form of sets also changes to `#{ … }` for round-trip consistency.
- **Path literal:** `/a/b/c` → an array of symbols `[ :a :b :c ]` (dynamically constructible; ordinary array, no new tag). `:foo` symbol literals are untouched.
- **Accessors reuse the now-dead `@`/`!`.** `dovar` auto-derefs and no `T_ADDR` ever reaches the data stack, so `@`(`p_fetch`)/`!`(`p_store`) are unreachable today — repurpose them:
  - `@`  `( frame path -- value )` — get at path; **error on any missing key**.
  - `!`  `( frame path value -- frame )` — set at path; **auto-vivifies** missing intermediate frames; leaves the frame for chaining.
- **Other operations:** `delete-at` `( frame path -- frame )`, `has?` `( frame path -- bool )` (non-erroring), `update-at` `( frame path xt -- frame )` (apply quotation to leaf), `keys`/`values` `( frame -- array )` (top level), `size` `( frame -- n )`, `merge` `( frame1 frame2 -- frame3 )` (new frame, right wins), `copy` `( frame -- frame' )` (**deep**).
- **Recommended defaults:** `delete-at` on a missing leaf **errors** (consistent with `@`); `copy` is **deep**; `update-at` is **included**.

## Implementation

Critical files: `src/c/logicforth.h`, `src/c/core.c`, `src/c/collections.c`, plus the test corpus.

### 1. The `T_FRAME` / `OBJECT_FRAME` type (`logicforth.h`, `core.c`)
Wire a new tagged heap type, mirroring `T_SET`/`T_ARRAY`:
- `Tag` enum (after `T_MATRIX`) and `ObjectKind` enum (after `OBJECT_MATRIX`).
- `Object` union: a `struct { cell *keys; Val *values; } frame;` — `keys[i]` holds a symbol-id, kept **sorted ascending**; `values[i]` parallel; reuse `len` (entry count) and `cap` (allocated slots, grown by doubling like sets). No sentinels, no hash table.
- `make_frame(handle)` inline (after `make_matrix`); `object_new_frame(interp)` allocator + prototype. Mirror `object_new_set`.
- Internal helpers in `collections.c` next to the set helpers (mirroring `set_member`/`set_add`): `frame_find(obj, sym_id) -> index-or-negative` (binary search), `frame_put(interp, handle, sym_id, val)` (binary search; overwrite on hit, else insert keeping sorted order, growing as needed), `frame_delete`.
- `val_cmp` T_FRAME case: since keys are sorted, structural compare is a **direct linear merge** — compare by `len`, then walk both key arrays in lockstep comparing key symbol-ids, and on equal keys recursively compare the parallel values. Order-canonical for free; serves set membership and unification's atomic-equality path.
- `print_val` / `print_val_compact` T_FRAME cases, wrapped in the existing `print_depth_enter`/`print_depth_leave` so frames get the depth shading too. Full form `{ :a 1 :b 2 }`; compact form e.g. `{N}`.
- GC: add `T_FRAME` to `mark_val`'s tag gate and mark each stored value; free `keys`+`values` in the sweep and in `free_one_object`.
- Image save/load: serialize `len` then `(symbol-id, val)` pairs; the symbol-id is a pool offset that survives because the symbol pool is part of the saved image. Add the `OBJECT_FRAME` case to both `p_save_image` and `p_load_image`.
- `tag_name`: `case T_FRAME: return "a frame";`.

### 2. Literals & reader (`collections.c`, `core.c`)
- Repurpose the brace words: `{` → push a **frame-mark**, `#{` → push a **set-mark**, `}` → a single closer that finds the nearest mark and builds a set (flat elements) or a frame (alternating `symbol value` pairs, erroring if a key isn't a symbol) per the mark's discriminator. Implement the discriminator by storing a small constant in the mark `Val`'s `data` field; `[`/`]` (arrays) are untouched. Register `#{` (a glued token resolves via `find`).
- **Path literal:** add a branch in `run_outer` after the `find()` miss, alongside the `:foo` branch — if `tok[0]=='/'` and `tok[1]!='\0'`, split on `/`, `intern_symbol` each segment, build a symbol array via `object_new_array`, and `compile_or_push` it. (Bare `/` is whitespace-delimited, so no collision.)
- **Builder:** `>frame` `( kv-array -- frame )` consumes an alternating key/value array (keys must be symbols), in `collections.c`. `frame` `( -- frame )` makes an empty one.

### 3. Frame operations (`collections.c`, registrations in `core.c`)
- Rewrite `p_fetch`(`@`) and `p_store`(`!`) for the path semantics above (the old `T_ADDR` paths are dead). Path traversal walks the symbol array; `@` errors on miss, `!` auto-vivifies intermediate frames.
- New `p_frame_delete_at`, `p_frame_has`, `p_frame_update_at`, `p_frame_keys`, `p_frame_values`, `p_frame_size`, `p_frame_merge`, `p_frame_copy` (deep). Register all alongside the set words near `core.c:1567`.
- Consider folding `size` to be polymorphic over frames/arrays/sets later; for now a frame-specific `size` is fine.

### 4. Set → `#{` migration (mechanical, sizable)
- Change set print form (`print_val`/`print_val_compact` set cases and the `save` text emitter) from `{ … }` to `#{ … }`.
- Update every set **literal** in the test corpus from `{ … }` to `#{ … }` (tests `02,03,04,06,10,11,16,20,41,42,43,48,54,56,57,58,59,60`, `lib_for_12`) and regenerate the affected `.expected` files for the new printed form. `lib.l4` has no set literals.

## Sequencing

- **Stage A** — add all frame machinery (type, helpers, ops, `>frame`, `/a/b` path literal, print, GC, image, `val_cmp`) **without** touching the set literal; sets keep working. Frames are testable via `[ :a 1 :b 2 ] >frame`, `frame`, `!`. `@`/`!` repurposed (the dead `T_ADDR` paths). Build + `make test` stays green.
- **Stage B** — the breaking `{`→frame / `#{`→set swap, the set print-form change, and the test-corpus migration, as one coherent step; regenerate `.expected` and re-run `make test`.

## Verification

- `make test` from the project root after each stage; the set-migration stage regenerates `.expected` for set-bearing tests, so re-run and diff intentionally.
- New `tests/NN_frames.l4` (+ `.expected`) covering: literal `{ :a 1 :b 2 }`; `>frame` from a kv array; `/a/b` get with `@` and miss-error; `!` set with auto-vivified nesting; `delete-at`, `has?`, `update-at`, `keys`, `values`, `size`, `merge`, deep `copy` independence; nested-frame printing with depth shading; `val_cmp` equality of two same-content frames (e.g. as set members under `#{ }`); and image `save-image`/`load-image` round-trip of a nested frame.
- Hand-compute every expected value before capturing `.expected` (per house practice), then confirm via the binary.

## Deferred (design leaves room, not in this cut)
- Node-set queries: `//` recursive descent, `*` wildcard, predicate filters, returning collections.
- Unification integration: structural unify over frames (equal key set, recurse on values), with logic-var values bound via the trail — this cut only ensures frames are value-comparable and structurally inspectable so that layer can sit on top cleanly.
