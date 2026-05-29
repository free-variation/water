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
- **Prompt** is bare `count|top` — `0` empty, `2|99` non-empty, `0|error` on error — with a light-grey background on a terminal only (`isatty`-gated). Dropping the old `<…>` brackets avoids collision with set/frame syntax.
- **Accessors reuse the dead `@`/`!`.** `dovar` auto-derefs and no `T_ADDR` reaches the data stack, so `@`(`p_fetch`)/`!`(`p_store`) are unreachable — repurpose:
  - `@` `( frame path -- value )` — get at path; **errors on a missing key**.
  - `!` `( frame path value -- frame )` — set at path; **auto-vivifies** intermediate frames; leaves the frame for chaining.
- **Other ops:** `delete-at` `( frame path -- frame )` (errors if absent), `has?` `( frame path -- bool )` (non-erroring), `update-at` `( frame path xt -- frame )`, `keys`/`values` `( frame -- array )` (top level), `size`, `merge` `( f1 f2 -- f3 )` (new frame, right wins), `copy` (**deep**).
- **Printing:** `print_val` (used by `.s`, embedding, interpolation) renders the compact single-line `{ :a 1 :b 2 }`; the **`.` word pretty-prints a top-level frame** multi-line/indented (expanding nested frames, `print_val` for leaves) — `.s` stays compact. Frames print with `{ }` so they round-trip to the literal.
- **Path literal:** `/a/b/c` → an array of symbols `[ :a :b :c ]` (dynamically constructible; ordinary array). `:foo` symbols and bare `/` (division) are unaffected — a glued `/a/b` token doesn't collide.

## Status

**Done (built, `make test` green at 66/66):**
- `T_FRAME`/`OBJECT_FRAME` type; sorted parallel `keys`/`values`; `make_frame`, `object_new_frame`.
- `object_new` dedup helper + `NEW_OBJECT`/`NEW_FRAME` macros; `cap` field renamed `capacity` throughout.
- `frame_find` (lower-bound binary search), `frame_put`, `frame_delete`.
- `>frame` builder; `{ … }` frame literal (`p_frameopen`/`p_frameclose`); `< … >` set literal (the `{`↔`<` swap).
- `val_cmp`, `tag_name`, GC (`mark_val` + both free paths), `print_val`/`print_val_compact` frame cases, pretty-printer wired into `.`.
- `lt`/`gt` rename (+ migration of 5 comparison test files); set→`< >` migration (+ all `.expected` regenerated); prompt change.
- Tangent done: `to` auto-creates a global at the REPL (errors inside a definition); shared via `create_variable`.

**Remaining:**
- Access ops: `@` (path get), `!` (path set), `delete-at`, `has?`, `update-at`, `keys`, `values`, `size`, `merge`, `copy`. (These need the path-traversal logic over a symbol array; testable with an explicit `[ :a ]` path before the `/a/b` literal.)
- `/a/b/c` path literal in `run_outer`.
- Frame **image save/load** — add the `OBJECT_FRAME` case to `p_save_image`/`p_load_image` (serialize `len` then `(symbol-id, val)` pairs).
- Frame tests (`tests/NN_frames.l4`).

## Verification

- `make test` from the project root.
- New `tests/NN_frames.l4` (+ `.expected`) covering: literal `{ :a 1 :b 2 }` and nested `{ :a { :b 1 } }`; `>frame` from a kv array; `@` get + miss-error; `!` set with auto-vivified nesting; `delete-at`/`has?`/`update-at`/`keys`/`values`/`size`/`merge`/deep-`copy`; pretty `.` vs compact `.s`; `val_cmp` equality of same-content frames (e.g. as `< … >` set members); image round-trip of a nested frame.
- Hand-compute expected values before capturing `.expected`, then confirm via the binary.

## Deferred (design leaves room, not in this cut)
- Node-set queries: `//` recursive descent, `*` wildcard, predicate filters, returning collections.
- Unification integration: structural unify over frames (equal key set, recurse on values), logic-var values bound via the trail. This cut only ensures frames are value-comparable and structurally inspectable so that layer sits on top cleanly.
