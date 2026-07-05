# Units / dimensions — an algebra of dimensioned floats (and matrices)

## Context

Water has a single numeric type (NaN-boxed `double`) plus raw-`double` matrices. We're adding **dimensioned quantities**: a magnitude carrying a unit, with arithmetic that propagates units automatically (`10 m 2 s / → 5 m/s`). The goal is dimensional *checking and algebra*, not unit *conversion*: units are rational-exponent vectors over user-declared base dimensions, magnitudes just compute, no scale factors or affine offsets.

Two hard requirements from review:
1. **Zero performance impact on dimensionless processing.** Plain floats and raw matrices are the hot paths; they must run byte-for-byte the same code they do today. Dimensioned values are opt-in and boxed.
2. **Units extend to matrices**, where a matrix carries a single unit.

Settled: no conversions; user-definable base dimensions identified by symbols; rational exponents (num/den); dimensionless results collapse back to a plain `T_FLOAT`/raw `T_MATRIX`; unified value-first declaration (`base unit m`, `… unit newton`); unit/dimension tables pinned (no GC for now).

Target surface:
```
base unit m   base unit s   base unit kg
10 m 2 s / .                              \ 5 m/s
1 kg 1 m * 1 s / 1 s /  unit newton
3 newton .                                \ 3 kg m/s^2
6 m 2 m / 3 = .                           \ 1  (collapses to float)
[ 1 2 3 ] 1 3 matrix m .                  \ a matrix in metres (dimensioned matrix)
```

## Performance guarantee (requirement #1)

Dimensionless processing means plain `T_FLOAT` and raw `T_MATRIX` values — these never carry any unit machinery, and their code paths stay untouched:

- In every arithmetic word (`p_add`/`p_sub`/`p_mul`/`p_div`, words.c) the float×float branch is **first** and the raw-matrix branches follow; the new `T_QUANTITY` arms are **appended after** them, so a float (or raw-matrix) op dispatches exactly as today — no extra comparison before it.
- `truthy` (water.h) returns on the `T_FLOAT` branch first; the quantity arm sits after it.
- `val_cmp_depth` (core.c) handles `T_FLOAT` early; the quantity arm is reached only when both tags are `T_QUANTITY`.
- `unary_op`/`binary_op` and the `f+`/`f*`/fused superwords are not touched on their float paths; quantities are intercepted in the three specific unary words and `p_power` *before* delegating.
- The quantity magnitude combinators call the existing `matrix_add`/`matrix_mul`/broadcast macros directly — **raw-matrix arithmetic is not refactored**; its inline arms stay as-is. No shared extraction that could de-inline the hot matrix path.
- The only float-touching addition is one extra disjunct (`!= T_QUANTITY`) in the `mark_value` heap-type guard, evaluated only during GC marking, not arithmetic.

**Release gate:** `make bench` shows no regression on the dimensionless float and matrix benchmarks vs. baseline.

## Representation

NaN-boxing gives a 7-bit tag at shift 44 and a 44-bit payload (water.h). A `double` magnitude doesn't fit 44 bits, so a quantity is stored indirectly in **one pair-table slot**:

- New tag `T_QUANTITY` appended to the `Tag` enum and `static inline Val make_quantity(int slot)` beside the other `make_*` constructors (water.h). Payload = pair-slot index.
- The pair slot (`Pair{head,tail}`) holds: **`head` = the magnitude `Val` — a `T_FLOAT` or a `T_MATRIX`**; `tail.bits = (uint64_t)unit_id`. A small int's bit pattern is outside the NaN-box region, so `VAL_IS_FLOAT(tail)` is true and existing pair/image machinery treats the tail as an inert float, never a heap reference. Read via `(int)pairs.table[slot].tail.bits`.
- Allocation reuses `object_new_pair` + `INIT_PAIR`.
- **Collapse rule, centralized** in `void push_quantity(Interpreter*, Val magnitude, int unit_id)`: if `unit_id` is the empty/dimensionless descriptor → `push(interp, magnitude)` unchanged (dimensionless result re-enters as a bare float or matrix); else allocate a slot, store `head=magnitude`/`tail.bits=unit_id`, `push(make_quantity(slot))`. Every `*`/`/`/`^`/`sqrt`/`dounit` result funnels through this, so `m/m → 3` and dimensionless-matrix collapse fall out for free.

```
T_QUANTITY Val ──payload──▶ pairs.table[slot]
                              ├─ head = T_FLOAT magnitude  OR  T_MATRIX handle
                              └─ tail.bits = unit_id ─▶ units table ─▶ [(dim-id,num,den)…]
                                                                          dim-id ─▶ dims table ─▶ name (symbol offset)
```

### New global tables (pinned; new file src/c/units.c)
- **Dimensions table**: growable array; entry = name as symbol-pool offset (or a sentinel for "unnamed"). `dim-id` = index. `base` appends an unnamed dim; `unit name` back-patches the name.
- **Units table**: interned **unit descriptors**, each a sorted list of `(dim-id, num, den)` with `num≠0, den>0, gcd=1`. Interning dedups so unit-equality and every `*`/`/` result are a single `unit-id` (O(1) identity; linear-scan intern is fine — user units are few). A reserved id = the **empty/dimensionless** descriptor; never stored in a quantity.
- Helpers (units.c): `gcd`, rational add/normalize; `unit_multiply`/`unit_divide`(=multiply with negated exps)/`unit_pow(unit_id,num,den)` (`sqrt`=pow 1/2) that merge descriptors, combine exponents, drop zeros, re-intern; `render_unit(unit_id)` → string.
- Declared via structs + `extern` + prototypes in water.h; defined in units.c. Add `src/c/units.c` to `SRCS` in **Makefile**.

## Implementation order

```
1. Representation + tables   water.h + units.c
2. Declaration words         units.c: p_base, p_unit, dounit; + full handler wiring (core.c)
3. Arithmetic dispatch       words.c: quantity arms; magnitude combinators; neg/abs/sqrt/power
4. Core value machinery      core.c: tag_name, val_cmp_depth, mark_value, print_val(_compact)
5. Image round-trip          core.c: serialize the two tables; loaded_handle_ok; image_op_cells
6. Region check              functional.c: references_region_depth arm
7. Docs + tests
```

## Step 2 — declaration words (units.c) + handler wiring (core.c)

Model the defining word on **`dosym`** (lean `[handler][data-cell]`), not on a colon word:

- `p_base` — `dim_id = dims_append_unnamed()`; `unit_id = intern_unit({dim_id:1/1})`; `push_quantity(interp, make_float(1.0), unit_id)`.
- `p_unit` — `next_token()` (fail if absent); pop TOS; require a `T_QUANTITY` whose magnitude is a `T_FLOAT == 1.0` (fail otherwise — a bare float here means it was dimensionless); if its descriptor is a single `(dim_id,1,1)` over an *unnamed* dim, set that dim's name to `intern_symbol(interp,name)`; then `create_header(interp,name,0)`, `emit(interp,(cell)&dounit)`, `emit(interp,(cell)unit_id)`.
- `dounit` handler (compiled form `[dounit][cfa]`) — `int cfa=(int)vocab.dict[interp->ip++]; int unit_id=(int)vocab.dict[cfa+1]; POP(x);` require `VAL_TAG(x)==T_FLOAT || T_MATRIX` (else fail "unit: expected a float or matrix"); `push_quantity(interp, x, unit_id)`; `DISPATCH`. (Accepting a matrix is what mints a **dimensioned matrix**, e.g. `M m`.)

### Handler-integration sites — `dounit` mirrors `dovar`/`dosym` everywhere they are special-cased

A 2-cell `[handler][cfa]` word is handled specially in several spots; missing any breaks subtly. Verified against the current source — `dounit` must be added to **all** of these:

1. **`construct_vocabulary`** — `define_primitive(interp,"base",p_base,0)` and `…"unit",p_unit,0`.
2. **`compiler.handler_registry`** — register `dounit` next to `docol`/`dovar`/`dosym` (so `handler_to_id` finds it for image save).
3. **`emit_call`** — add `dounit` to the `docol||dovar||dosym` list so a compiled reference to a unit word also emits its target cfa (`[dounit][cfa]`). Without this the operand cell is never emitted and the word reads the next instruction as its cfa.
4. **`execute_cfa`** — add `if (handler == dounit) { POP(x); require float/matrix; push_quantity(x, (int)vocab.dict[cfa+1]); return; }`, mirroring the `dovar`/`dosym` special cases. **Critical:** top-level `10 m` runs through `execute_cfa`; without this it takes the docol/trampoline path and reads a garbage operand.
5. **`call_open`** — add `|| handler == dounit` to the `if (handler == dovar || handler == dosym) { context->fast = 0; return; }` guard, so a unit word passed as an xt to `map`/`times`/etc. falls back to `execute_cfa`.
6. **`see_compiled_body`** and **`see_tree_body`** — add a `dounit` case (print the unit-word name, `cursor += 2`) alongside the `docol`/`dovar`/`dosym` cases, or disassembly desyncs on any unit-using word.
7. **`image_op_cells`** — `if (handler == (cell)dounit) return 2;` alongside `dovar`/`dosym`, or `save-image` of a word referencing a unit mistranslates the operand and errors.

**Verified to need NO change** (they don't special-case `dovar`/`dosym` either; `op_cell_count` returns 1 for them and these walkers stay correct because they only key on literal/dostr/branch/docol/exit and copy/skip operand cells verbatim): `op_cell_count`, `mark_body`, `inline_word_body`, and `gc()`'s per-word marking loop. The GC story for *quantity values* is the `mark_value` guard + arm in Step 4.

## Step 3 — arithmetic dispatch (words.c)

Magnitudes may be float **or** matrix, so introduce small combinators that reuse the existing numeric machinery without disturbing hot paths: `Val q_mag_mul/q_mag_div/q_mag_add/q_mag_sub(Interpreter*, Val a, Val b)` — each dispatches on `a`/`b` ∈ {float, matrix} and calls the existing `matrix_mul`/`matrix_add`/`matrix_sub`/`matrix_div`, the `BROADCAST_*` macros, or scalar arithmetic, returning the result `Val` (or setting `error_flag`). Invoked **only** from quantity arms, so raw float/matrix dispatch is unchanged.

Append `T_QUANTITY` arms (after the existing branches) in `p_add`/`p_sub`/`p_mul`/`p_div`:
- **`*`/`/`**: q·q → `q_mag_mul/div` on heads + `unit_multiply`/`unit_divide` → `push_quantity` (empty unit ⇒ bare float/matrix automatically). q·(float|matrix) / (float|matrix)·q → combine magnitudes, keep unit. (float|matrix)/q → invert unit via `unit_divide(dimensionless,u)`.
- **`+`/`-`**: q±q → require identical `unit_id` else `fail("+ : unit mismatch")`, then `q_mag_add/sub` (matrix shape mismatch surfaces from the inner matrix op). q ± bare (float|matrix) → `fail`.

Unary math — only **three** words need quantity handling. `abs`/`sqrt` are macro-generated (`UNARY_MATH_OP`) and `unary_op` has two call sites: **pull `abs` and `sqrt` out of the macro into hand-written functions** and add quantity arms to them and to `p_neg`:
- `negate`/`abs` → transform magnitude (`-x`/`fabs`, float or matrix), unit unchanged.
- `sqrt` → `unit_pow(unit,1,2)` + `sqrt` of magnitude.
- All other transcendentals keep calling `unary_op`; its existing `else` already `fail`s — with the new `tag_name` arm it reads "got a quantity". No parameter threading, no float-path change.

**`^`** (`p_power`): before `binary_op`, if base is `T_QUANTITY` require the exponent be a float equal to an exact small rational (search denominators `1..MAX_DEN`≈64, tight epsilon) → `unit_pow(unit,num,den)` + `pow` of magnitude via `push_quantity` (exponent 0 ⇒ collapse); non-rational exponent → `fail`. Otherwise unchanged `binary_op(...,pow,"^")`.

`%` (`p_divmod`) and matrix in-place ops need **no** arm — their float/matrix-only checks already `fail` on a quantity.

## Step 4 — core value machinery (core.c)

- **`tag_name`** → `case T_QUANTITY: return "a quantity";`.
- **`val_cmp_depth`** → arm (tag equality checked first): compare `unit_id`, then heads via `val_cmp_depth(head,head,depth+1)` (handles float or matrix) — total order, consistent with pairs/matrices. `= lt gt`/fused-compare route non-floats through `val_cmp`; `unify`/`copy`/`reify` fall through to atomic / share-by-handle (quantities immutable) — no arms.
- **`mark_value`** — **two edits**: (1) add `T_QUANTITY` to the heap-type guard — omitting it skips marking and GC reclaims the live slot (and, for a dimensioned matrix, its backing Object → use-after-free); (2) arm mirroring `T_PAIR`: bounds-check vs `gc_pair_base`, set `pairs.mark_epoch[slot]`, then `mark_value(interp, pairs.table[slot].head)` (float ⇒ noop, matrix ⇒ marks the Object), `return` (tail inert).
- **`print_val`** → arm: render the head (float via `print_double`; matrix via `print_matrix_grid`, matching `p_dot`) + space + `render_unit(unit_id)`. `render_unit`: positive-exponent dims first, then one `/`, then negatives; `^n` for `|exp|≠1`, `^a/b` for non-unit denominators (`5 m/s`, `9.8 m/s^2`, `3 kg m^2/s^2`, `2 m^1/2`). Dim names from the dims table → `vocab.symbol_pool`.
- **`print_val_compact`** → analogous compact arm.
- **`p_dot`** and **`render`** need **no** arm: `p_dot`'s `else` and `p_render` (non-matrix/frame/array) both route through `print_val`. *Optional polish:* a `T_QUANTITY` case in `interp_render_val` so `format` avoids `<?>` (not needed for tests).

## Step 5 — image round-trip (core.c)

Quantities live in pair slots, already serialized wholesale by `p_save_image`/`p_load_image`: the head (float or matrix `Val`) and tail (unit_id bits) round-trip as raw bits; a dimensioned matrix's backing Object rides the existing object-table serialization. To keep each `unit_id` valid:
- Serialize the **dimensions and units tables** alongside the symbol pool using the **Watermark pattern** (`init_symbol_pool_here`): add `init_n_dims`/`init_n_units`, write only user-added entries, and add them to the bootstrap-mismatch check. Dim names are symbol offsets valid against the restored (append-only) pool; ids keep their indices.
- **`image_op_cells`** → add `dounit` → return 2 (Step 2, item 7) so colon words that reference units translate correctly.
- **`loaded_handle_ok`** → `case T_QUANTITY:` validate slot ∈ `[0,pairs.space.n)` **and** `unit_id=(int)pairs.table[slot].tail.bits` ∈ `[0,n_units)`. (The pair-walk in `validate_loaded` validates the head matrix via its own `T_MATRIX` case and ignores the float-looking tail, so the `unit_id` must be checked here, where the `T_QUANTITY` Val is seen.)

## Step 6 — region check (functional.c)

**`references_region_depth`** → `case T_QUANTITY:` — if `(int)VAL_DATA(value) >= snapshot->n_pairs` return 1; else recurse into the head (`pairs.table[slot].head`) so a dimensioned matrix created in-region is detected; tail inert.

## Step 7 — docs & tests

- Add `base`/`unit` rows + a units section to **docs/reference.md**, then regen: `python3 tools/gen-help.py` and `python3 tools/gen-editors.py`. Update/remove the **PLAN.md** entry.
- New golden test **tests/NNN_units.h2o** + **.expected** (batch: `water -b < in`, exact stdout):
  - base + arithmetic: `base unit m base unit s  10 m 2 s / render "5 m/s" = .`
  - add/sub same unit; cross-unit add errors; quantity ± bare-float errors.
  - dimensionless collapse: `6 m 2 m / 3 = .` (bare float `1`).
  - derived: `1 kg 1 m * 1 s / 1 s / unit newton  3 newton render "3 kg m/s^2" = .`
  - rational: `sqrt` of `m^2 → m`, `sqrt` of `m → m^1/2`; `^` with `2` and `1/2`; non-rational power errors.
  - transcendental on a quantity errors; quantities as set members / sorted; `=`/`lt` ordering; `copy`/`reify` of a structure holding a quantity.
  - **top-level unit application**: `10 m .` (exercises the `execute_cfa` `dounit` case), and a *compiled* use inside a colon def (`: dist 10 m ; dist .`) exercising the `emit_call`/`image_op_cells` path; `' m see-compiled` / `see-tree` of a unit-using word (exercises the `see_*` cases).
  - **dimensioned matrices**: build a matrix, attach a unit (`M m`), matrix+matrix same-unit ok, cross-unit errors, scalar-quantity × matrix-quantity propagates units, `m/m` collapses to a raw matrix; print/render shows grid + unit.
  - image round-trip: define custom units (incl. a dimensioned matrix) and a colon word that uses a unit, `save-image`/`load-image`, values still print and run correctly.

## Files touched

- **src/c/water.h** — `T_QUANTITY`; `make_quantity`; `truthy` arm (`return truthy(pairs.table[VAL_DATA].head)`); dims/units structs + externs; `init_n_dims`/`init_n_units`; prototypes (`p_base`, `p_unit`, `dounit`, `push_quantity`, `q_mag_*`, unit helpers, `render_unit`).
- **src/c/units.c** (new; add to Makefile `SRCS`) — tables, rational/`gcd`, intern, `unit_multiply`/`unit_divide`/`unit_pow`, `render_unit`, `push_quantity`, `p_base`, `p_unit`, `dounit`.
- **src/c/words.c** — quantity arms in `p_add`/`p_sub`/`p_mul`/`p_div` (appended after existing branches); `q_mag_*` combinators; `p_neg`; `abs`/`sqrt` un-macro'd with quantity arms; `p_power`.
- **src/c/core.c** — `tag_name`, `val_cmp_depth`, `mark_value` (guard + arm), `print_val`, `print_val_compact`; image save/load of the two tables + Watermarks; `loaded_handle_ok`; **`execute_cfa`** (`dounit` case), **`call_open`** (guard), **`see_compiled_body`/`see_tree_body`** (`dounit` case), **`image_op_cells`** (`dounit` → 2); `construct_vocabulary` registers `base`/`unit`; `handler_registry` + `emit_call` learn `dounit`; init Watermarks.
- **src/c/functional.c** — `references_region_depth` arm.
- **docs/reference.md** (+ regen), **PLAN.md**, **tests/NNN_units.h2o** + `.expected`.

Reuse: `intern_symbol`, `object_new_pair`/`INIT_PAIR`, `create_header`/`emit`/`emit_call`/`next_token`, `define_primitive`, the `dosym` template, `matrix_add`/`matrix_mul`/`matrix_sub`/`matrix_div` + `BROADCAST_*` macros, `val_cmp`, `print_double`/`print_matrix_grid`, `fail`, the symbol-pool Watermark machinery.

## Verification

1. `make && sh tests/run.sh` — full golden suite green, including tests/NNN_units.h2o.
2. **`make bench`** — dimensionless float **and** raw-matrix benchmarks show no regression vs. baseline (requirement #1 gate).
3. **ASan/UBSan build over the units test** (new tag in `mark_value`/image + dimensioned-matrix Objects are the risk areas): build with `-fsanitize=address,undefined -g -O1` over all SRCS incl. units.c, run `lf_asan -b < tests/NNN_units.h2o` — clean, output matches `.expected`. Stress GC (allocate many dimensioned floats and matrices in a loop, force `gc`) and a `save-image`/`load-image` cycle under ASan.

## Out of scope (future)

Unit conversion (scale/affine, °C). GC of the unit/dimension tables (pinned now; collect alongside the planned symbol-pool collection). Per-element heterogeneous units within a matrix (a matrix carries one unit for all elements).
