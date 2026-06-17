# Garbage Collection in logicforth

This document is a primer on how logicforth's garbage collector works. By the end you should understand:

- What problem garbage collection solves and why we need it
- How logicforth represents heap-allocated objects, and what a "tagged value" is
- The mark-and-sweep algorithm: what each phase does and why it's correct
- Where the *roots* live and why each one matters
- The role of the `gc_roots[]` stack — a small but subtle piece of machinery that fixes a class of bug C programmers will recognize
- When GC runs, and what it deliberately doesn't manage

The implementation is in `src/c/core.c`, mostly in three functions: `mark_value`, `mark_body`, and `gc`. The whole collector is under 100 lines of C. The conceptual machinery, once you've seen it, is small. But several of its design choices need motivation to feel inevitable rather than arbitrary, and that's what this document tries to provide.

---

## Part 1: Why does GC exist?

The problem GC solves shows up the moment a program needs to create new data at runtime. Imagine writing a simple Forth-like calculator that lets the user push strings on a stack and concatenate them:

```forth
"hello " "world" +
```

The `+` operator, when given two strings, needs to produce a new string holding the concatenation. That new string didn't exist before; it has to live somewhere in memory while the calculator runs. Once the user drops it from the stack and stops referring to it, that memory can be reused.

A program that calls `malloc` for every new string but never calls `free` will leak memory until it runs out and crashes. A program that calls `free` aggressively can free a string while another part of the program is still holding a pointer to it, causing a use-after-free bug — usually a crash, sometimes a security hole.

The traditional answer is to make the human programmer responsible for matching every `malloc` with exactly one `free`. C does this. The problems are well known: it's error-prone, it doesn't compose well across libraries with different ownership conventions, and complex data structures (graphs, cyclic references, shared subobjects) are very hard to manage correctly by hand.

Garbage collection automates the decision. The runtime tracks which allocated objects are still *reachable* — meaning some chain of references can lead from "live program state" to the object — and frees the rest. The programmer never writes `free`; they don't even think about it.

The cost is some runtime overhead (the collector has to scan periodically) and some loss of control over exactly when memory is reclaimed. The benefit is large: simpler programs, fewer bugs, and the ability to build complex shared data structures without ownership puzzles.

---

## Part 2: The heap in logicforth

Before we can talk about GC, we need to understand what's being collected. In logicforth, the answer is: anything stored in the `Object **objects` registry.

The registry is **global**, a member of the `Arena` struct, not of `Interpreter`:

```c
typedef struct {
    ...
    Object **objects;               // the object registry (grown on demand)
    cell current_epoch;             // the mark counter (Part 5)
    int n_objects;                  // highest slot used so far + 1
    int max_objects;                // logical ceiling
    int objects_cap;                // current allocated length of objects[]
    int *free_slots;                // stack of reusable handles
    int n_free_slots;
} Arena;
extern Arena arena;
```

`arena.objects` is allocated small and doubled on demand up to a `max_objects`
ceiling (Part 9), so a fresh interpreter costs a few MB rather than reserving
the whole ceiling up front. Each slot is either `NULL` (free) or a pointer to a heap-allocated `Object`. An `Object` is a tagged variant — it can be a string, a set, an array, a frame, a matrix, or a continuation:

```c
typedef enum {
    OBJECT_STRING = 0,
    OBJECT_SET,
    OBJECT_ARRAY,
    OBJECT_FRAME,
    OBJECT_MATRIX,
    OBJECT_CONTINUATION
} ObjectKind;

typedef struct {
    ObjectKind kind;
    int len, capacity;
    union {
        char *bytes;                    // for strings
        Val  *items;                    // for sets and arrays
        struct {                        // for frames
            cell *keys;                 //   symbol-pool offsets, parallel to values[]
            Val *values;                //   the Vals at each key
        } frame;
        struct {                        // for matrices
            int rows;
            int columns;
            double *elements;
        } matrix;
        struct {                        // for continuations
            Val *return_slice;
            int return_len;
            int resume_ip;
            int local_base_offset;
        } continuation;
    };

    cell mark_epoch;                    // the GC mark (Part 5)
} Object;
```

So an `Object` has its own outer allocation (the struct) plus an inner allocation (or two, for frames) for its payload — the `bytes`, `items`, the frame's parallel `keys` and `values`, the matrix `elements`, or the continuation's `return_slice`. Freeing an Object requires freeing each piece.

The outer struct is *not* `calloc`'d and `free`'d. It comes from the arena via `arena_alloc_object()`, which pops the `arena.freed_object_structs` free-list if a dead struct is available there, or bump-allocates a fresh one with `arena_alloc(sizeof(Object))`; either way it memsets the struct before returning it. Tearing an Object down (`free_one_object`) frees the payload and then calls `arena_free_object(obj)`, which **pushes** the struct back onto `freed_object_structs` — the struct is recycled, never returned to the OS. The free-list is threaded through the first 8 bytes of each dead struct:

```c
static Object *arena_alloc_object(void) {
    Object *fresh;
    if (arena.freed_object_structs) {
        fresh = arena.freed_object_structs;
        arena.freed_object_structs = *(void **)fresh;   // unlink
    } else {
        fresh = arena_alloc(sizeof(Object));             // bump-allocate
    }
    memset(fresh, 0, sizeof(Object));
    return fresh;
}

static void arena_free_object(Object *obj) {
    *(void **)obj = arena.freed_object_structs;          // link
    arena.freed_object_structs = obj;
}
```

The payloads divide by allocator. String `bytes`, set/array `items`, and frame `keys`+`values` come from the arena's size-class allocator (`arena_malloc`/`arena_free`). Matrix `elements` and continuation `return_slice` use real `calloc`/`free`. The sweep has to free each piece with the matching allocator. The arena allocator itself — its reserved bump region, size classes, and free lists — is described in `docs/arena.md`.

User code never sees an `Object*` directly. It sees a *handle* — the index into `objects[]`. Handles are wrapped in `Val`s, the interpreter's universal tagged-value type:

```c
typedef enum {
    T_NONE = 0,
    T_SYMBOL,
    T_FLOAT,
    T_STRING,
    T_SET,
    T_ARRAY,
    T_PAIR,
    T_FRAME,
    T_MATRIX,
    T_XT,
    T_ADDR,
    T_CONT,
    T_MARK,
    T_STREAM,
    T_LOGIC_VAR,
    T_UNBOUND
} Tag;

typedef union {
    uint64_t bits;
    double   number;
} Val;
```

A `Val` is 8 bytes, NaN-boxed: a `T_FLOAT` is the `double` stored directly, and every other value packs a tag and a payload into the unused bits of a quiet NaN (the bit layout is nan-boxing.md's subject). For GC, what matters is the tag — read with `VAL_TAG(v)` — and what it says the payload `VAL_DATA(v)` means:

- `T_FLOAT` — the Val *is* the IEEE 754 double.
- `T_SYMBOL` — `data` is an offset into the symbol pool (a static string area).
- `T_XT` / `T_ADDR` — `data` is a position in the dictionary.
- `T_STREAM` — `data` is an open file descriptor.
- `T_MARK` / `T_NONE` / `T_UNBOUND` — `data` is unused or a small integer (a mark id, or the unbound-variable marker).
- `T_STRING`, `T_SET`, `T_ARRAY`, `T_FRAME`, `T_MATRIX`, `T_CONT` — `data` is a *handle*: an index into `objects[]`.
- `T_PAIR` — `data` is a slot in the separate `pairs` table; cons cells live there, not in `objects[]`.
- `T_LOGIC_VAR` — `data` is an index into the `lvar_stack`, where the variable's binding lives.

The last three groups are the ones the GC must trace. The `objects[]` handles and `T_PAIR` cells point at collectable storage directly; a `T_LOGIC_VAR` is followed to whatever it's bound to. Everything else (floats, symbols, execution tokens, dictionary addresses, marks, file descriptors) is self-contained in the `Val` and refers to no managed storage.

So the GC's job, in slogan form: find every live cell — `objects[]` slot and pair alike — reachable from the roots, and free the dead ones.

---

## Part 3: Mark-and-sweep — the algorithm

logicforth uses *mark-and-sweep*, one of the oldest GC algorithms. It runs in two phases.

**Phase 1: Mark.** Starting from the *roots* (a set of pointers the runtime knows are definitely live), traverse every reachable object. For each object you visit, record that it's "marked" for this cycle. By the end of this phase, every reachable object is marked; every unreachable object is unmarked.

**Phase 2: Sweep.** Walk through every slot in `arena.objects`. Free the ones that aren't marked. (No separate "clear the marks" step is needed for objects — see Part 5: the mark is an epoch stamp, and the next cycle simply bumps the epoch.)

The correctness argument is short. An object is *reachable* iff there's a chain of references from a root to it. After the mark phase, an object is *marked* iff it's reachable. After sweep, unmarked objects are freed; marked objects survive. So the live objects survive, the dead objects are freed.

The trickiness lies in the details:

- How do you enumerate all the roots?
- How do you walk an object's outbound references?
- How do you stop the mark phase from infinitely looping on cyclic references?
- How do you handle objects whose references are *implicit* — buried inside compiled bytecode, for example?
- How do you make sure no live object's reference is hidden somewhere the GC doesn't look?

Most of this document is about how logicforth answers each of those questions.

---

## Part 4: Roots — where reachability starts

A *root* is, by definition, a reference the GC trusts as alive. Anything reachable from a root is alive; everything else is dead.

In logicforth, the roots come from five places:

1. The **data stack**.
2. The **return stack**.
3. The **side stack**.
4. The **dictionary** (specifically, variable values and literals embedded in compiled word bodies).
5. The **`gc_roots[]` array** — a small array of "in-flight" temporaries that C-level code is currently holding but hasn't put on any stack yet.

Why these five? Because they're the complete set of places the *executing program* keeps Vals.

- **Data stack**: holds the values user code is computing with right now.
- **Return stack**: holds saved instruction pointers and any user-pushed values (via `>r`). Some of those user-pushed values may be heap references.
- **Side stack**: a third stack for stashing values that mustn't sit on the other two. Same story.
- **Dictionary**: `variable` storage and literal Vals compiled into word bodies live here.
- **`gc_roots`**: see Part 7. This is the subtle one.

If a Val isn't reachable from one of these, it's dead by definition. Nothing in the running program can still reach it.

Look at the "scan roots" code in `gc()`:

```c
void gc(Interpreter *interp) {
    arena.current_epoch++;                                   // invalidate every old object mark
    memset(interp->pair_mark, 0, sizeof(unsigned char) * (size_t)interp->n_pairs);
    interp->pair_free_count = 0;

    for (int i = 0; i < interp->dsp; i++) mark_value(interp, interp->data_stack[i]);
    for (int i = 0; i < interp->rsp; i++) mark_value(interp, interp->return_stack[i]);
    for (int i = 0; i < interp->side_dsp; i++) mark_value(interp, interp->side_stack[i]);
    for (int i = 0; i < interp->n_gc_roots; i++) mark_value(interp, interp->gc_roots[i]);

    /* ... mark dictionary bodies (see Part 6) ... */

    /* ... sweep objects[] and the pairs table (see Part 8) ... */
}
```

There are two collectable tables: `arena.objects` for the composite heap objects, and the `pairs` table for cons cells. Both get traced by `mark_value`, and both get swept at the end. But they record marks differently, and that asymmetry is worth understanding.

The **pairs** table uses a real mark array, `pair_mark`, and `gc()` `memset`s it to zero at the start of every cycle. Cleared bit, set bit, swept, repeat.

**Objects** do not use a mark array at all. There is no `object_mark` array and no per-cycle `memset` clearing it. Instead each `Object` carries a `cell mark_epoch`, and the arena holds a single global counter, `arena.current_epoch`. `gc()` bumps that counter once (`arena.current_epoch++`). An object is considered marked iff `obj->mark_epoch == arena.current_epoch`; marking an object just stamps `obj->mark_epoch = arena.current_epoch`. Bumping the counter by one instantly invalidates every mark left over from the previous cycle, because no surviving object's stamp matches the new epoch until it's re-marked. That replaces an O(n) `memset` over the whole object table with a single increment — the whole reason objects went to an epoch stamp while pairs kept a bit array.

Logic-variable bindings need no mark storage of their own — a reachable `T_LOGIC_VAR` is followed into the `lvar_stack` during marking, so its binding is traced wherever it's reached.

The three stacks plus the `gc_roots` array — that's four of the five root sources right there, each handled by one loop. The dictionary is a Part 6 topic because it needs more machinery.

---

## Part 5: The mark phase

`mark_value` is the workhorse. It takes a `Val` and, if it's a heap reference, marks its target and recurses into the target's children.

```c
void mark_value(Interpreter *interp, Val value) {
    if (VAL_TAG(value) == T_LOGIC_VAR) {            // a logic var: follow it into the lvar stack
        mark_value(interp, interp->lvar_stack[VAL_DATA(value)]);
        return;
    }

    if (VAL_TAG(value) != T_STRING && VAL_TAG(value) != T_SET &&
            VAL_TAG(value) != T_ARRAY && VAL_TAG(value) != T_PAIR &&
            VAL_TAG(value) != T_FRAME && VAL_TAG(value) != T_MATRIX &&
            VAL_TAG(value) != T_CONT) return;       // not a heap reference

    if (VAL_TAG(value) == T_PAIR) {                 // cons cells live in their own table
        int slot = (int)VAL_DATA(value);
        if (interp->pair_mark[slot]) return;
        interp->pair_mark[slot] = 1;
        mark_value(interp, interp->pairs[slot].head);
        mark_value(interp, interp->pairs[slot].tail);
        return;
    }

    int handle = (int)VAL_DATA(value);
    if (handle < 0 || handle >= arena.n_objects) return;

    Object *obj = OBJECT_AT(handle);
    if (!obj || obj->mark_epoch == arena.current_epoch) return;
    obj->mark_epoch = arena.current_epoch;

    if (obj->kind == OBJECT_SET || obj->kind == OBJECT_ARRAY) {
        for (int i = 0; i < obj->len; i++) mark_value(interp, obj->items[i]);
    } else if (obj->kind == OBJECT_FRAME) {
        for (int i = 0; i < obj->len; i++) mark_value(interp, obj->frame.values[i]);
    } else if (obj->kind == OBJECT_CONTINUATION) {
        for (int i = 0; i < obj->continuation.return_len; i++)
            mark_value(interp, obj->continuation.return_slice[i]);
    }
}
```

(The bounds test is `handle >= arena.n_objects` — `n_objects` is the high-water mark, and any valid handle is below it. The earlier `objects_cap` test is unnecessary because `n_objects <= objects_cap` always.)

Read it line by line:

1. **Logic var? Follow the binding.** A `T_LOGIC_VAR` is not a heap object — it's an index into the bump-allocated `lvar_stack`. Mark whatever the variable is bound to (which may itself be a heap reference) and return.

2. **Is this even a heap reference?** Floats, symbols, xts, addrs, marks, and the empty `T_NONE` tag carry no heap pointer. Return immediately.

3. **Pair? Mark it in the pair table.** Cons cells (`T_PAIR`) live in a separate `pairs` table with their own `pair_mark` bits, not in `objects[]`. If the cell is already marked, stop (the cycle guard); otherwise set the bit and recurse into its head and tail.

4. **In bounds and live?** Defensive — handles should always be valid, but this guards against bugs. Fetch the slot via `OBJECT_AT(handle)`; if it's already freed (`NULL`), skip.

5. **Already marked this epoch?** The key to terminating on cycles: if `obj->mark_epoch == arena.current_epoch`, we've already visited this object during the current collection — don't visit it again. Otherwise an array containing itself would loop forever. Marking is the very next line: `obj->mark_epoch = arena.current_epoch`.

6. **Recurse into children, if any.** Strings and matrices are leaf data — their `bytes`/`elements` arrays hold no Vals. Sets and arrays hold Vals (their items); frames hold Vals (their `values`, parallel to `keys`); continuations hold Vals (the captured return-stack slice). Walk each child and recurse.

   For frames we walk `values[]` but not `keys[]`: keys are `cell`s — symbol-pool offsets, plain integers — not Vals, so they reference no heap object.

Notice what `mark_value` does *not* do:

- It doesn't unmark anything. Old object marks are invalidated wholesale by the `arena.current_epoch++` at the start of `gc()`; the `pair_mark` array is `memset` to zero there.
- It doesn't look inside strings or matrices. Those are flat numeric or byte data — no further Vals inside.
- It doesn't walk frame `keys` arrays (they're cells, not Vals).
- It doesn't traverse the `Object` struct's bookkeeping fields (`kind`, `len`, `capacity`). Those aren't Vals.

The recursion terminates because every recursive call either (a) hits a leaf object kind (string, matrix), (b) hits an already-marked object or pair (cycle), or (c) hits a non-reference Val (float, symbol, etc.).

### A worked example

Suppose the data stack holds one Val: an array. The array contains three items: a float, another array (the inner), and a string. The inner array contains a single Val: a string. For this example, the outer's third item and the inner's only item both refer to the *same* string handle.

Layout (handles 1, 2, 3):

- Handle 1: outer array of three Vals: `[T_FLOAT(7), T_ARRAY(2), T_STRING(3)]`.
- Handle 2: inner array of one Val: `[T_STRING(3)]`.
- Handle 3: the string `"hello"`.

Data stack: `[T_ARRAY(1)]`.

Mark phase:

1. `mark_value(T_ARRAY(1))`. Mark handle 1. Recurse on its items.
2. `mark_value(T_FLOAT(7))`. Not a heap ref. Return.
3. `mark_value(T_ARRAY(2))`. Mark handle 2. Recurse on its items.
4. `mark_value(T_STRING(3))` (from the inner). Mark handle 3. No children to recurse into (strings are leaves).
5. Back out of inner. Continue with outer's items.
6. `mark_value(T_STRING(3))` (from the outer). Already marked at step 4. Return immediately.
7. Done.

Handles 1, 2, 3 are all marked. Sweep would free anything else.

If we'd designed the outer's first slot to hold `T_ARRAY(1)` (a self-reference), step 2 would mark handle 1 — already marked at step 1 — and return immediately. No infinite loop. That's what the "third filter" buys you.

---

## Part 6: The dictionary as a special root

The first four roots — the three stacks plus `gc_roots` — hold `Val`s directly. Walk the array, call `mark_value` on each. Easy.

The dictionary is different. The dictionary is one giant `cell[]` array (an array of `int64_t`s, basically) holding compiled word bodies. A typical word body is a sequence of cells:

- Some cells are CFAs (handler indices) — these are not Vals.
- Some cells are inline operands to the previous CFA.

A `Val` literal compiled into a word body looks like this in the dictionary:

```
... [literal_cfa] [packed Val] [next_cfa] ...
```

A `Val` is a single 64-bit cell — it's NaN-boxed, so tag and payload both fit in one `cell`. The literal op carries exactly one operand cell: the Val's raw bits. If that Val is a heap reference (say a `T_ARRAY`), GC must mark the referenced object — otherwise the compiled code that *will* use that Val later would find its target freed underneath it.

Similarly, `variable` storage lives inline in the dictionary. A `variable` word has a one-cell body: the packed Val of the variable's current value.

So the GC has to walk every word body, look at the cells, identify which are Vals (literal operands and variable bodies), and mark those.

This is what `mark_body` does:

```c
void mark_body(Interpreter *interp, int body_start, int body_end) {
    cell literal_ptr = vocab.dict[vocab.literal_cfa];
    cell dostr_ptr = vocab.dict[vocab.dostr_cfa];

    int cursor = body_start;
    while (cursor < body_end) {
        cell handler = vocab.dict[cursor];
        int n = op_cell_count(cursor);

        if (handler == literal_ptr) {
            Val value;
            value.bits = (uint64_t)vocab.dict[cursor + 1];
            mark_value(interp, value);
        } else if (handler == dostr_ptr) {
            Val value = make_string((int)vocab.dict[cursor + 1]);
            mark_value(interp, value);
        }

        cursor += n;
    }
}
```

Walk the body cell by cell. For each cell, `op_cell_count` reports how many cells this op occupies — the CFA plus any inline operands. Then:

- If it's the CFA of `(literal)`, the next cell is the packed Val — read its raw bits straight into a `Val` and mark it.
- If it's the CFA of `(dostr)`, the next cell is a string handle. Build a `T_STRING` Val and mark it.
- Otherwise nothing to mark — branches, locals ops, and `(to-var)` carry only plain-integer operands, and a bare CFA carries none.

Either way, advance by `n` cells.

The crucial property: `op_cell_count` must report the right width for every op. If it miscounts, the walk could (a) run past the end of the body, or (b) mistake an operand cell for a CFA on the next iteration and read it as a Val. Either is a corruption bug.

That's why `op_cell_count` is the single place that knows each compiler-emitted op's width. Every op with inline operands — `(literal)`, `(dostr)`, the branches, the word-local variable ops, the fused superwords — is accounted for there, and it's the first place to update when a new such op is added.

### How does GC find each word's body range?

Word headers are 4 cells of metadata (link, flags, name, source) plus 1 CFA cell. So if word A's CFA is at offset 100 and word B's CFA is at 130, A's body cells are at offsets 101..125 (B's header starts at 126: 4 cells of metadata, then B's CFA at 130).

`gc()` sets up these ranges by collecting all word CFAs (by walking the linked list `latest_cfa → WORD_LINK → ...`), sorting them ascending, then computing each body's end as "start of next CFA's header" (= `next_cfa - 4`). For the last word, the end is `here` (the current dictionary high-water mark).

```c
for (int i = 0; i < num_cfas; i++) {
    int cfa = sorted_cfas[i];
    int body_start = cfa + 1;
    int body_end = (i + 1 < num_cfas) ? sorted_cfas[i + 1] - 4 : vocab.here;
    cfa_handler handler = (cfa_handler)vocab.dict[cfa];

    if (handler == docol) {
        mark_body(interp, body_start, body_end);
    } else {
        if (handler == dovar && body_start < body_end) {
            Val value;
            value.bits = (uint64_t)vocab.dict[body_start];
            mark_value(interp, value);
        }
        mark_body(interp, body_start + 1, body_end);
    }
}
```

The dispatch on handler type matters:

- **`docol`**: a colon-defined word. The whole body is compiled code — walk it all with `mark_body(body_start, body_end)`.
- **Everything else** takes the `else` branch:
  - If the handler is **`dovar`** (a variable), the first body cell is the packed Val of its current value — read it directly and mark it.
  - Then, for *any* non-`docol` handler (including `dovar`), call `mark_body(body_start + 1, body_end)` over the remainder of the body. A word with a non-`docol` handler can still carry trailing compiled cells, and those can hold literals or string handles that need marking. (For a plain `dovar` whose body is a single cell, `body_start + 1 == body_end`, so this scans nothing.) The earlier claim that "other handlers have nothing in the body to mark, skip" is wrong: the `else` branch always scans `body_start + 1 .. body_end`.

---

## Part 7: gc_roots — temporary roots for in-flight work

This is the subtlest piece of machinery in the GC. It exists because of a quiet bug that would otherwise be very hard to find.

### The problem

Suppose a C-side primitive needs to allocate two new objects and combine them. For example, `map` builds a new array, fills it with the results of applying a quotation, and pushes the array onto the data stack at the end.

A naive implementation:

```c
void p_map_naive(Interpreter *interp, cell *cfa) {
    /* ... pop the xt and the source array ... */

    int result_handle = object_new_array(interp, source->len);   // allocate
    Object *result = arena.objects[result_handle];

    for (int i = 0; i < source->len; i++) {
        push(interp, source->items[i]);
        execute_cfa(interp, xt);                                 // <- user code runs
        result->items[i] = pop(interp);
    }

    push(interp, make_array(result_handle));
}
```

Look at the loop. We're filling `result->items[i]` one at a time. Inside the loop, we call `execute_cfa(xt)`, which runs the user's quotation. The quotation is arbitrary user code — it can allocate freely. If it allocates enough objects to exhaust the registry, a GC fires.

What does the GC see when it scans roots in that moment?

- **The data stack**: maybe `source->items[i]` (the value we just pushed) and whatever the quotation has put there. *Not* the result array.
- **The return stack**: saved IPs from the call chain. *Not* the result array.
- **The side stack**: untouched. *Not* the result array.
- **The dictionary**: variables and literals. *Not* the result array.

The result array's handle is sitting in a **local C variable** (`result_handle`) inside `p_map_naive`. The GC has no way to know about that. It sees an unreferenced object in `objects[result_handle]`, marks nothing for it, and frees it during the sweep.

When the loop continues, `result` (a pointer the C code is still holding) points at freed memory. `result->items[i] = pop(interp)` writes to that freed memory. Soon after, the interpreter crashes — or worse, silently corrupts later allocations that happen to reuse the same memory.

This is the C-level analog of a "rooted but unrooted" bug that comes up in any GC'd language with a foreign-function interface. The GC needs to know about every live reference, including ones momentarily held outside the data structures it normally scans.

### The solution

A small array of "in-flight Vals" that C code can push to and pop from:

```c
typedef struct Interpreter {
    ...
    Val gc_roots[MAX_GC_ROOTS];
    int n_gc_roots;
    ...
} Interpreter;
```

Two helpers:

```c
void gc_root_push(Interpreter *interp, Val value) {
    if (interp->n_gc_roots >= MAX_GC_ROOTS) {
        fail(interp, "gc roots exhausted");
        return;
    }
    interp->gc_roots[interp->n_gc_roots++] = value;
}

void gc_root_pop(Interpreter *interp) {
    if (interp->n_gc_roots > 0) {
        interp->n_gc_roots--;
    }
}
```

And in the GC's root-scan code:

```c
for (int i = 0; i < interp->n_gc_roots; i++) mark_value(interp, interp->gc_roots[i]);
```

The fix in `p_map`:

```c
int result_handle = object_new_array(interp, source->len);
gc_root_push(interp, make_array(result_handle));     // <- root the result

for (int i = 0; i < source->len; i++) {
    push(interp, source->items[i]);
    execute_cfa(interp, xt);
    result->items[i] = pop(interp);
}

gc_root_pop(interp);                                  // <- unroot
push(interp, make_array(result_handle));
```

Now when the quotation triggers a GC, the result array is reachable through `gc_roots`, gets marked, survives. The result is unrooted only after the loop is done and the array is safely on the data stack.

### Where this pattern shows up

You'll find `gc_root_push` / `gc_root_pop` calls in:

- `p_map`, `p_mapn`, `p_filter` — building a result array while the user's xt can allocate.
- `p_load`, `p_save`, `p_save_image`, `p_load_image` — the filename string is held in a local Val while file I/O runs.

Each follows the same pattern: a C primitive holds a Val that hasn't reached a stack yet, and it's about to do something that might allocate. Push it to `gc_roots`; do the work; pop.

### Why the array is small (and what it costs)

`MAX_GC_ROOTS` is deliberately small: the array is meant for **in-flight, transient** references that exist only inside a single C primitive's body. Each push has a matching pop a few lines later. Deeply nested in-flight roots would mean deeply nested C primitives, which we don't have.

If the depth ever exceeds `MAX_GC_ROOTS`, `gc_root_push` raises an error ("gc roots exhausted") rather than overrunning the array. Dropping a root silently would risk freeing a live in-flight object during the next GC, so it fails loudly instead. In practice the depth is always 1 or 2.

### A picture of when it matters

```
program runs
   │
   ├─ user calls map
   │     │
   │     ├─ map allocates result array   ── result_handle = X
   │     │   gc_roots: [result(X)]
   │     │
   │     ├─ map loop iteration 1:
   │     │     │
   │     │     ├─ push source item
   │     │     ├─ execute user xt
   │     │     │     │
   │     │     │     └─ user xt allocates lots of objects
   │     │     │        objects[] hits the max_objects ceiling
   │     │     │        ⇒ gc fires
   │     │     │           ─ marks data stack
   │     │     │           ─ marks return stack
   │     │     │           ─ marks side stack
   │     │     │           ─ marks gc_roots → result(X) ✓ marked
   │     │     │           ─ marks dictionary
   │     │     │           ─ sweeps the dead ones
   │     │     │        result(X) survives
   │     │     │
   │     │     └─ pop result; write into result->items[0]  ✓ valid memory
   │     │
   │     ├─ loop iteration 2: ...
   │     ├─ loop ends
   │     ├─ gc_root_pop
   │     │   gc_roots: []
   │     │
   │     └─ push result(X) to data stack
```

Without `gc_roots`, `result(X)` would have no live reference at the moment GC fires inside iteration 1, and the subsequent write would be a use-after-free.

This pattern is general. Any time C code holds a freshly-allocated handle in a local variable and may trigger allocation before that handle reaches an observable place (a stack, a parent object's field, a variable), the handle has to be rooted. `gc_roots` is the place to put it.

---

## Part 8: The sweep phase

After mark, the sweep is mechanical:

```c
arena.n_free_slots = 0;
for (int handle = 0; handle < arena.n_objects; handle++) {
    Object *obj = arena.objects[handle];
    if (obj && obj->mark_epoch == arena.current_epoch) continue;  // live: keep, not free

    if (obj) {
        free_one_object(obj);                            // payload + recycle struct
        arena.objects[handle] = NULL;
    }
    arena.free_slots[arena.n_free_slots++] = handle;     // record the empty slot
}
```

Walk every slot from 0 to `arena.n_objects`. A slot whose object's `mark_epoch` equals the current epoch is live — keep it. Everything else is an empty slot: if an unmarked object is sitting there, `free_one_object` releases it:

```c
void free_one_object(Object *obj) {
    switch (obj->kind) {
        case OBJECT_STRING:       arena_free(obj->bytes); break;
        case OBJECT_SET:
        case OBJECT_ARRAY:        arena_free(obj->items); break;
        case OBJECT_FRAME:        arena_free(obj->frame.keys);
                                  arena_free(obj->frame.values); break;
        case OBJECT_MATRIX:       free(obj->matrix.elements); break;
        case OBJECT_CONTINUATION: free(obj->continuation.return_slice); break;
    }
    arena_free_object(obj);                              // push struct onto freed_object_structs
}
```

First the kind-specific payload — strings/sets/arrays/frames give their `arena_malloc`'d payload back with `arena_free`, while matrices and continuations `free` their `calloc`'d arrays — then `arena_free_object` pushes the outer Object struct onto the `freed_object_structs` free-list for reuse (it is *not* handed back to the OS). The slot is nulled out, and the now-empty handle is pushed onto `arena.free_slots`.

That last step is the point of the sweep: it rebuilds the free list. `arena.free_slots` is a stack of reusable handles, reset to empty at the start of the sweep and refilled with every dead or already-empty slot. The allocator pops from it, so reusing a freed slot is O(1) — no scanning required. Frames are the only kind with two inner allocations to free.

The pairs table is swept the same way, right after. Cons cells aren't `calloc`'d individually — they live in one preallocated `pairs` array — so there's nothing to `free`; the sweep just walks the table and pushes every unmarked slot onto `pair_free_list`. Allocating a pair pops from that list, exactly as object allocation pops from `free_slots`.

The next mark cycle invalidates all object marks at once by bumping `arena.current_epoch`, and zeroes the `pair_mark` array. Every survivor is effectively unmarked again, to be re-marked on the next collection if it's still reachable.

### Why not compact?

After sweep, `arena.objects` has holes — freed slots scattered among live ones. logicforth doesn't compact (move live objects together at the bottom of the array). Compaction would let new allocations always use the highest free slot (cheap O(1) allocation), but it would require updating every reference to a moved object. Logically straightforward: walk every root and every internal reference, fixing up handles. Implementationally bigger than the entire current GC put together.

The cost of not compacting: `arena.objects` stays sparse — freed slots are reused in place rather than packed down, so the array never shrinks below its high-water mark. Allocation stays cheap regardless: the sweep leaves behind a `free_slots` stack of reusable handles, and `object_alloc_slot` just pops one. No scan, no fixups. Acceptable.

---

## Part 9: When does GC run?

In logicforth, GC is lazy. It fires in exactly one place: when `object_alloc_slot` can't find a free slot.

```c
int object_alloc_slot(Interpreter *interp) {
    if (arena.n_objects < arena.max_objects) {
        if (arena.n_objects == arena.objects_cap) {       // table full — grow it first
            int new_cap = arena.objects_cap ? arena.objects_cap * 2 : 1024;
            if (new_cap > arena.max_objects) new_cap = arena.max_objects;
            GROW_OBJECT_TABLE(new_cap);                   // realloc objects + free_slots
        }
        return arena.n_objects++;                         // common path: next slot
    }
    if (arena.n_free_slots > 0) {
        return arena.free_slots[--arena.n_free_slots];    // reuse a freed slot
    }
    if (interp->gc_disabled) {
        return -1;                                        // collection suppressed
    }
    gc(interp);                                           // ceiling hit — must collect
    if (arena.n_free_slots > 0) {
        return arena.free_slots[--arena.n_free_slots];    // gc refilled the free list
    }
    return -1;                                            // truly stuck
}
```

Four cases:

1. **Common path.** `arena.n_objects < arena.max_objects`: hand out the next never-used slot and bump `n_objects` — first growing the table if `n_objects` has reached the current `objects_cap`. The growth seeds at 1024 on the first allocation (`objects_cap` is 0 then) and doubles thereafter, capped at `max_objects`. `GROW_OBJECT_TABLE` reallocs the **two** parallel tables, `objects` and `free_slots` (there is no separate mark table to grow — marks live in each object's `mark_epoch`). Amortized O(1). `max_objects` is the logical ceiling (`MAX_OBJECTS` by default, lowered by `--max-objects`); `objects_cap` is the physical length, which only ever grows up to that ceiling.

2. **Ceiling hit, but freed slots exist.** Pop one off `free_slots`. Still O(1) — the sweep already built this list.

3. **No free slots, and GC is allowed.** Run a collection, which sweeps dead objects and refills `free_slots`. Pop from it.

4. **GC disabled, or still nothing after collecting.** Return `-1`; the caller turns that into an "object registry full" error. The program is holding `max_objects` live references at once.

The `gc_disabled` flag exists for deep copy and reify. Those routines build a new structure piece by piece, and the intermediate handles aren't reachable from any root until the copy is finished — a collection partway through would free them. So `copy_or_reify` collects up front if the heap is nearly full, sets `gc_disabled` for the duration of the copy, and accepts that an allocation may fail (returning `-1`) rather than risk a mid-copy sweep.

The user can also call GC manually with the `gc` Forth word, which just delegates to the C `gc(interp)` function. This is useful for measuring memory use or for debugging — most user programs never need to.

### Why lazy?

A more aggressive collector would run periodically: every N allocations, every M milliseconds, after every primitive. Each strategy has tradeoffs:

- **Per-allocation GC**: simplest invariant, but huge constant overhead.
- **Time-based**: needs a clock, real-time-ish behavior, complicates the interpreter.
- **Allocation-count-based**: a counter, fire every N. Reasonable.
- **On-demand (logicforth's choice)**: zero overhead when not collecting; one large pause when it does.

For an interpreter where allocation tends to come in bursts (parsing, set building, matrix construction), on-demand keeps the steady-state fast. The downside is that when the collection does run, it can take a while — but at the scale of `max_objects`, "a while" is still well under a millisecond on modern hardware.

---

## Part 10: What GC doesn't manage

It's worth being explicit about the things that *look* like memory but aren't tracked by GC.

- **The dictionary itself.** `vocab.dict[]` is a fixed-size inline `cell[]` array (`dict_ensure` only bounds-checks it, exiting if it would overflow `VOCABULARY_INIT_SIZE` — it does not grow). Everything compiled into it lives until program exit or until `forget` rewinds the dictionary. GC doesn't reclaim it.
- **The name pool, source pool, symbol pool.** Static-sized character arrays inside `Vocabulary`, used to hold the names of words/variables, the captured source text of colon definitions, and the bytes of interned symbols. Written by the compiler, lasts until program exit or `forget`.
- **The loaded-files list.** `compiler.loaded_files[]` holds `strdup`'d filename strings (the load-once guard). They persist for the program's lifetime — not GC'd, and not freed; `forget_user` even replays them on reset rather than releasing them.
- **Input buffers, token buffers.** Static-sized arrays inside `Interpreter`. Reused on every input cycle.

The boundary is: anything allocated via the Object registry is GC'd; anything allocated by other means is manually managed (or persists for the program's life).

The `forget` word rewinds the dictionary state, but objects referenced by `forget`'d code only get freed when the next GC notices they're unreachable. So `forget` doesn't immediately release memory; the next GC does.

### Cycles

The implementation handles cycles correctly via the per-object `mark_epoch` check (see Part 5). But cycles are hard to create from user code in practice: arrays and sets aren't mutated in place by any user-level word, so you can't easily build a set that contains itself. Continuations can hold references to other heap objects, but they aren't mutable either. The cycle-safety is a defensive property of the algorithm rather than a feature exercised by typical programs.

### Finalizers

There are none. Object teardown frees memory and that's it. If we someday add a kind of object that wraps an OS resource (a file handle, a database connection), we'd need to either add finalizers or make the resource user-managed. Today the question doesn't arise.

### Weak references

Same: none. Every reference is strong. If you hold a Val pointing at an Object, that Object stays alive.

---

## Part 11: A complete worked example

Let's trace one full GC cycle for a small program.

The user is at the REPL. They have:

```forth
variable favourites
{ "the iliad" "the odyssey" "war and peace" } favourites !
```

State after these commands:

- A string `"the iliad"` allocated as Object handle 1.
- A string `"the odyssey"` allocated as handle 2.
- A string `"war and peace"` allocated as handle 3.
- A set containing those three string Vals as handle 4.
- `favourites` is a variable; its body in the dictionary stores `T_SET(handle=4)`.
- Data stack is empty.

Now the user does:

```forth
favourites @
```

This pushes the variable's current value (the set) onto the data stack:

- Data stack: `[T_SET(4)]`

Then:

```forth
{ "the iliad" } -
```

`{ "the iliad" }` constructs a new set containing a fresh `"the iliad"` string — a fresh string Object, because the parser doesn't intern string literals across calls. Say handle 5 for the new string, handle 6 for the new singleton set.

Then `-` computes set difference, producing a new set as a fresh Object. The result contains `"the odyssey"` and `"war and peace"`. Crucially, the result set holds Vals pointing at the *existing* string handles (2 and 3) — not at fresh copies. Say handle 7 for the result set.

State after the difference operation:

- Handle 1: `"the iliad"` (original). Reachable through `favourites`' set (handle 4) which is held in the variable.
- Handle 2: `"the odyssey"`. Reachable through `favourites`' set, and also through the result set on the data stack.
- Handle 3: `"war and peace"`. Same.
- Handle 4: original set. Reachable through `favourites` variable's stored Val.
- Handle 5: fresh `"the iliad"` string (created just for the difference operation). Was reachable through handle 6.
- Handle 6: temporary singleton set containing handle 5. Was on the data stack briefly, but `-` consumed it.
- Handle 7: result set on the data stack.

Data stack: `[T_SET(7)]`.

Now suppose for the sake of the example that the registry holds only 8 slots and all are in use. The user does something that allocates one more object — anything: build a new string, a new array. The registry is full and `free_slots` is empty. `object_alloc_slot` triggers GC.

GC runs:

1. **Invalidate old marks.** `arena.current_epoch++` (and `memset(pair_mark, 0, ...)`). No object now satisfies `mark_epoch == current_epoch`, so every object starts the cycle unmarked.
2. **Walk data stack.**
   - `T_SET(7)` → mark handle 7. Recurse on its items: `T_STRING(2)`, `T_STRING(3)` → mark handles 2 and 3.
3. **Walk return stack.** At the REPL, this contains saved IPs (`T_ADDR`s) from the dispatch trampoline. None are heap refs. Nothing marked.
4. **Walk side stack.** Empty.
5. **Walk `gc_roots`.** Empty — no C primitive is in flight at the REPL prompt.
6. **Walk dictionary.** The `favourites` variable's body contains `T_SET(4)`. Mark handle 4. Recurse on its items: `T_STRING(1)`, `T_STRING(2)`, `T_STRING(3)`. Handles 2 and 3 already marked. Mark handle 1.

Marks after the mark phase: {1, 2, 3, 4, 7}.

**Sweep.** Walk every slot from 0 to `n_objects`:

- Slot 1: marked. Skip.
- Slot 2: marked. Skip.
- Slot 3: marked. Skip.
- Slot 4: marked. Skip.
- Slot 5: unmarked. `arena_free` the string bytes (`obj->bytes`), then recycle the Object struct onto `freed_object_structs`. `arena.objects[5] = NULL`. Push 5 onto `free_slots`.
- Slot 6: unmarked. `arena_free` the items array, recycle the struct. `arena.objects[6] = NULL`. Push 6 onto `free_slots`.
- Slot 7: marked. Skip.

After sweep: `free_slots` holds 5 and 6. `object_alloc_slot` pops one and returns it; the next allocation reuses that handle. The program continues.

In this example, the GC reclaimed two objects: handle 5 (the temporary `"the iliad"` string used only inside the difference expression) and handle 6 (the temporary singleton set holding it). Both became unreachable as soon as `-` finished and only its result remained on the stack.

---

## Part 12: The big picture

The GC, in slogan form:

- Find all the live objects by walking from roots.
- Free the rest.

The details that make it work:

- A small set of roots that captures everything: three stacks, the dictionary, and a temporary-roots array for C-level in-flight Vals.
- A type-aware mark function that descends into composite Vals (sets, arrays, frames, continuations), marks cons cells in the pairs table, follows logic variables into their bindings, and respects already-visited marks (so cycles terminate).
- A dictionary walker that knows the layout of compiled code well enough to find literals and variable values, and to skip operand cells without misreading them.
- A sweep that frees the right inner allocation per object kind, then the outer struct, and rebuilds the free lists for both tables.
- A lazy trigger: GC fires only when out of slots, keeping steady-state allocation O(1).

The total size of the GC implementation is under 100 lines. It's a small piece of machinery that does a large job — and once you've understood it, you've understood the core of every mark-and-sweep collector ever built.

---

## Where to look in the source

- **`gc()`** in `src/c/core.c` — the entry point. Reads short.
- **`mark_value()`** — the recursive marker. The Val tag filter and the mark-bit termination check are the load-bearing pieces.
- **`mark_body()`** — the dictionary walker. Each branch in it corresponds to one op-layout in compiled code.
- **`object_alloc_slot()`** — the only place GC is triggered automatically.
- **`gc_root_push()` / `gc_root_pop()`** — the in-flight root API. `grep -n gc_root_ src/c/*.c` shows every caller.
- **The `Object` struct in `src/c/logicforth.h`** — the per-object layout the sweep dispatches on.

For broader context:

- **`docs/continuations.md`** — the same kind of pedagogical primer for the continuation machinery, which the GC interoperates with (continuations are heap-allocated Objects whose `return_slice` is walked by `mark_value`).
- **`PLAN.md`** — pending work. If you add a new heap-allocated type (a dictionary/hashmap, for instance), the changes you'd make for GC are: add a new `OBJECT_*` enum, add a `mark_value` case if it can contain Vals, and add a sweep case to free its payload.
