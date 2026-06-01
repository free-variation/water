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

Before we can talk about GC, we need to understand what's being collected. In logicforth, the answer is: anything stored in the `Object *objects[MAX_OBJECTS]` array.

The interpreter holds this array as a member of `Interpreter`:

```c
typedef struct Interpreter {
    ...
    Object *objects[MAX_OBJECTS];   // MAX_OBJECTS = 262144
    int n_objects;                  // highest slot used so far + 1
    ...
} Interpreter;
```

Each slot is either `NULL` (free) or a pointer to a heap-allocated `Object`. An `Object` is a tagged variant — it can be a string, a set, an array, a matrix, or a continuation:

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
} Object;
```

So an `Object` has its own outer allocation (the struct, `calloc`'d in `object_new_string`, etc.) plus an inner allocation (or two, for frames) for its payload — the `bytes`, `items`, the frame's parallel `keys` and `values`, the matrix `elements`, or the continuation's `return_slice`. Freeing an Object requires freeing each piece.

User code never sees an `Object*` directly. It sees a *handle* — the index into `objects[]`. Handles are wrapped in `Val`s, the interpreter's universal tagged-value type:

```c
typedef enum {
    T_NONE = 0,
    T_SYMBOL,
    T_FLOAT,
    T_STRING,
    T_SET,
    T_ARRAY,
    T_FRAME,
    T_MATRIX,
    T_XT,
    T_ADDR,
    T_CONT,
    T_MARK
} Tag;

typedef struct {
    Tag tag;
    int64_t data;
} Val;
```

A `Val` is 16 bytes: a `Tag` and a 64-bit `data` field. The tag tells you what's in `data`:

- `T_FLOAT` — `data` holds the bits of an IEEE 754 double.
- `T_SYMBOL` — `data` is an offset into the symbol pool (a static string area).
- `T_XT` / `T_ADDR` — `data` is a position in the dictionary.
- `T_MARK` / `T_NONE` — `data` is unused or a small integer (a mark id).
- `T_STRING`, `T_SET`, `T_ARRAY`, `T_FRAME`, `T_MATRIX`, `T_CONT` — `data` is a *handle*: an index into `objects[]`.

That last group is the only group the GC cares about. Everything else (floats, symbols, execution tokens, dictionary addresses, marks) is self-contained in the `Val` and doesn't refer to any heap object.

So the GC's job, in slogan form: find all the live `objects[]` slots, free the dead ones.

---

## Part 3: Mark-and-sweep — the algorithm

logicforth uses *mark-and-sweep*, one of the oldest GC algorithms. It runs in two phases.

**Phase 1: Mark.** Starting from the *roots* (a set of pointers the runtime knows are definitely live), traverse every reachable object. For each object you visit, set a "marked" flag. By the end of this phase, every reachable object is marked; every unreachable object is unmarked.

**Phase 2: Sweep.** Walk through every slot in `objects[]`. Free the ones that aren't marked. Clear the marks on the survivors so the next collection can use them.

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
    memset(interp->object_mark, 0, sizeof(interp->object_mark));

    for (int i = 0; i < interp->dsp; i++) mark_value(interp, interp->data_stack[i]);
    for (int i = 0; i < interp->rsp; i++) mark_value(interp, interp->return_stack[i]);
    for (int i = 0; i < interp->side_dsp; i++) mark_value(interp, interp->side_stack[i]);
    for (int i = 0; i < interp->n_gc_roots; i++) mark_value(interp, interp->gc_roots[i]);

    /* ... mark dictionary bodies (see Part 6) ... */

    /* ... sweep (see Part 8) ... */
}
```

The three stacks plus the `gc_roots` array — that's four of the five root sources right there, each handled by one loop. The dictionary is a Part 6 topic because it needs more machinery.

---

## Part 5: The mark phase

`mark_value` is the workhorse. It takes a `Val` and, if it's a heap reference, marks its target and recurses into the target's children.

```c
void mark_value(Interpreter *interp, Val value) {
    if (value.tag != T_STRING &&
            value.tag != T_SET &&
            value.tag != T_ARRAY &&
            value.tag != T_FRAME &&
            value.tag != T_MATRIX &&
            value.tag != T_CONT) return;

    int handle = (int)value.data;
    if (handle < 0 || handle >= MAX_OBJECTS || !interp->objects[handle] || interp->object_mark[handle]) return;

    interp->object_mark[handle] = 1;
    Object *obj = interp->objects[handle];
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

Read it line by line:

1. **First filter: is this even a heap reference?** Floats, symbols, xts, addrs, marks, and the empty `T_NONE` tag all carry no heap pointer. Return immediately.

2. **Second filter: is the handle in bounds and the slot live?** Defensive — handles should always be valid, but this guards against bugs. If the slot's already been freed (`NULL`) or never allocated, skip.

3. **Third filter: is it already marked?** This is the key to terminating on cycles. If we've already visited this object during this collection, don't visit it again. Otherwise, an array containing itself would loop forever.

4. **Mark it.** Set `object_mark[handle] = 1`.

5. **Recurse into children, if any.** Strings and matrices don't contain Vals — their internal `bytes` and `elements` arrays are leaf data. Sets and arrays contain Vals (their items); frames contain Vals (their `values` array, parallel to `keys`); continuations contain Vals (the captured return-stack slice). For those, walk each child and recurse.

   Notice that for frames we walk `values[]` but not `keys[]`. The keys are `cell`s — symbol-pool offsets, plain integers — not Vals, so they don't reference any heap object and need no marking.

Notice what `mark_value` does *not* do:

- It doesn't unmark anything. Only the `memset` at the start of `gc()` clears the mark array.
- It doesn't look inside strings or matrices. Those are flat numeric or byte data — no further Vals inside.
- It doesn't walk frame `keys` arrays (they're cells, not Vals).
- It doesn't traverse the `Object` struct's bookkeeping fields (`kind`, `len`, `capacity`). Those aren't Vals.

The recursion terminates because every recursive call either (a) hits a leaf object kind (string, matrix), (b) hits an already-marked object (cycle), or (c) hits a non-handle Val (float, symbol, etc.).

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
... [literal_cfa] [tag] [data] [next_cfa] ...
```

The `tag` and `data` cells together encode a `Val`. If that Val is a heap reference (say a `T_ARRAY`), GC must mark the referenced object — otherwise the compiled code that *will* use that Val later would find its target freed underneath it.

Similarly, `variable` storage lives inline in the dictionary. A `variable` word has a 2-cell body: `[tag] [data]`, encoding the variable's current Val.

So the GC has to walk every word body, look at the cells, identify which are part of a Val literal or variable, and mark those Vals.

This is what `mark_body` does:

```c
void mark_body(Interpreter *interp, int body_start, int body_end) {
    int cursor = body_start;
    while (cursor < body_end) {
        cell ref = interp->vocab->dict[cursor];
        if (ref == (cell)interp->vocab->literal_cfa && cursor + 2 < body_end) {
            Tag tag = (Tag)interp->vocab->dict[cursor + 1];
            Val value; value.tag = tag; value.data = interp->vocab->dict[cursor + 2];
            mark_value(interp, value);
            cursor += 3;
        } else if (ref == (cell)interp->vocab->dostr_cfa && cursor + 1 < body_end) {
            Val value; value.tag = T_STRING; value.data = interp->vocab->dict[cursor + 1];
            mark_value(interp, value);
            cursor += 2;
        } else if ((ref == (cell)interp->vocab->branch_cfa
                    || ref == (cell)interp->vocab->zbranch_cfa) && cursor + 1 < body_end) {
            cursor += 2;          /* branches: 1 operand cell, no Vals */
        } else if ((ref == (cell)interp->vocab->to_var_cfa
                    || ref == (cell)interp->vocab->enter_locals_cfa
                    || ref == (cell)interp->vocab->leave_locals_cfa) && cursor + 1 < body_end) {
            cursor += 2;          /* one-cell operand, no Vals */
        } else if ((ref == (cell)interp->vocab->local_fetch_cfa
                    || ref == (cell)interp->vocab->local_store_cfa) && cursor + 2 < body_end) {
            cursor += 3;          /* two-cell operand, no Vals */
        } else {
            cursor++;             /* plain CFA, no inline operand */
        }
    }
}
```

Walk the body cell by cell. For each cell:

- If it's the CFA of `(literal)`, the next two cells are the tag+data of a literal Val. Build the Val and mark it. Skip three cells total (the CFA plus its two operands).
- If it's the CFA of `(dostr)`, the next cell is a string handle. Build a `T_STRING` Val and mark it. Skip two cells.
- If it's a CFA with non-Val inline operands (branches, the locals ops, `(to-var)`), skip the operand cells but don't mark anything — those operands are just plain integers, not Vals.
- Otherwise, it's a plain CFA with no inline operands. Move forward one cell.

The crucial property: each branch must correctly identify how many cells the op consumes. If the GC miscounts, it could (a) walk past the end of the body, or (b) mistake an operand cell for a CFA on the next iteration and try to read it as a Val. Either is a corruption bug.

This is also why each new compiler-emitted op with inline operands has to be registered here. When word-local variable ops were added (`(enter-locals)`, `(local@)`, etc.), `mark_body` had to learn how many operand cells each consumes. If you ever add a new op with inline operands, this is the first place to update.

### How does GC find each word's body range?

Word headers are 4 cells of metadata (link, flags, name, source) plus 1 CFA cell. So if word A's CFA is at offset 100 and word B's CFA is at 130, A's body cells are at offsets 101..125 (B's header starts at 126: 4 cells of metadata, then B's CFA at 130).

`gc()` sets up these ranges by collecting all word CFAs (by walking the linked list `latest_cfa → WORD_LINK → ...`), sorting them ascending, then computing each body's end as "start of next CFA's header" (= `next_cfa - 4`). For the last word, the end is `here` (the current dictionary high-water mark).

```c
for (int i = 0; i < num_cfas; i++) {
    int cfa = sorted_cfas[i];
    int body_start = cfa + 1;
    int body_end = (i + 1 < num_cfas) ? sorted_cfas[i + 1] - 4 : interp->vocab->here;
    cfa_handler handler = (cfa_handler)interp->vocab->dict[cfa];
    if (handler == docol) {
        mark_body(interp, body_start, body_end);
    } else if (handler == dovar && body_start + 1 < body_end) {
        Val value;
        value.tag = (Tag)interp->vocab->dict[body_start];
        value.data = interp->vocab->dict[body_start + 1];
        mark_value(interp, value);
    }
}
```

The dispatch on handler type matters:

- **`docol`**: a colon-defined word. Body is compiled code — walk with `mark_body`.
- **`dovar`**: a variable. Body is exactly two cells (tag, data). Read directly and mark.
- **Other handlers** (primitive C functions, `dosym`, etc.): nothing in the body to mark. Skip.

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
    Object *result = interp->objects[result_handle];

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
#define MAX_GC_ROOTS 16

typedef struct Interpreter {
    ...
    Val gc_roots[MAX_GC_ROOTS];
    int n_gc_roots;
    ...
} Interpreter;
```

Two helpers, both in `core.c`:

```c
void gc_root_push(Interpreter *interp, Val value) {
    if (interp->n_gc_roots < MAX_GC_ROOTS) {
        interp->gc_roots[interp->n_gc_roots++] = value;
    }
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

`MAX_GC_ROOTS` is 16. That's tiny on purpose: the array is meant for **in-flight, transient** references that exist only inside a single C primitive's body. Each push has a matching pop a few lines later. Deeply nested in-flight roots would mean deeply nested C primitives, which we don't have.

If you exceed 16, `gc_root_push` silently does nothing — there's no error path. This is a latent bug: if user code somehow triggered 17 concurrent in-flight roots, the 17th would be at risk during the next GC. In practice the depth is always 1 or 2.

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
   │     │     │        objects[] fills up
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
for (int handle = 0; handle < interp->n_objects; handle++) {
    Object *obj = interp->objects[handle];
    if (!obj || interp->object_mark[handle]) continue;

    switch (obj->kind) {
        case OBJECT_STRING:       free(obj->bytes); break;
        case OBJECT_SET:
        case OBJECT_ARRAY:        free(obj->items); break;
        case OBJECT_FRAME:        free(obj->frame.keys); free(obj->frame.values); break;
        case OBJECT_MATRIX:       free(obj->matrix.elements); break;
        case OBJECT_CONTINUATION: free(obj->continuation.return_slice); break;
    }
    free(interp->objects[handle]);
    interp->objects[handle] = NULL;
}
```

Walk every slot from 0 to `n_objects`. If the slot's already `NULL` (already free), skip. If it's marked (still live), skip. Otherwise free the object: first the kind-specific payload (the bytes, items, frame keys *and* values, matrix elements, or continuation return_slice — whichever applies), then the outer Object struct itself. Null out the slot so it can be reused. Frames are the only kind with two inner allocations to free.

The next mark cycle starts with `memset(object_mark, 0, ...)`, clearing every survivor's mark bit. They'll be re-marked on the next collection if still reachable.

### Why not compact?

After sweep, `objects[]` has holes — freed slots scattered among live ones. logicforth doesn't compact (move live objects together at the bottom of the array). Compaction would let new allocations always use the highest free slot (cheap O(1) allocation), but it would require updating every reference to a moved object. Logically straightforward: walk every root and every internal reference, fixing up handles. Implementationally bigger than the entire current GC put together.

The cost of not compacting: `object_alloc_slot` may have to search for a free slot, which is a linear scan in the worst case. In practice, allocation fills the high water mark first and only scans on the rare full-table case. Acceptable.

---

## Part 9: When does GC run?

In logicforth, GC is lazy. It fires in exactly one place: when `object_alloc_slot` can't find a free slot.

```c
int object_alloc_slot(Interpreter *interp) {
    if (interp->n_objects < MAX_OBJECTS) {
        return interp->n_objects++;        // common path: use next slot
    }

    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (interp->objects[i] == NULL) {
            return i;                      // a hole exists somewhere
        }
    }
    gc(interp);                            // no hole — must collect

    for (int i = 0; i < MAX_OBJECTS; i++) {
        if (interp->objects[i] == NULL) {
            return i;                      // gc made room
        }
    }
    return -1;                             // truly stuck
}
```

Three cases:

1. **Common path.** `n_objects < MAX_OBJECTS`: hand out the next slot, bump `n_objects`. O(1).

2. **High water hit, but holes exist.** Linear scan finds a `NULL` slot, reuses it.

3. **No holes either.** Run GC. Scan again for `NULL`. If still nothing, the program is hopelessly stuck holding `MAX_OBJECTS = 262144` live references at once.

The user can also call GC manually with the `gc` Forth word, which just delegates to the C `gc(interp)` function. This is useful for measuring memory use or for debugging — most user programs never need to.

### Why lazy?

A more aggressive collector would run periodically: every N allocations, every M milliseconds, after every primitive. Each strategy has tradeoffs:

- **Per-allocation GC**: simplest invariant, but huge constant overhead.
- **Time-based**: needs a clock, real-time-ish behavior, complicates the interpreter.
- **Allocation-count-based**: a counter, fire every N. Reasonable.
- **On-demand (logicforth's choice)**: zero overhead when not collecting; one large pause when it does.

For an interpreter where allocation tends to come in bursts (parsing, set building, matrix construction), on-demand keeps the steady-state fast. The downside is that when the collection does run, it can take a while — but at the scale of `MAX_OBJECTS = 262144`, "a while" is still well under a millisecond on modern hardware.

---

## Part 10: What GC doesn't manage

It's worth being explicit about the things that *look* like memory but aren't tracked by GC.

- **The dictionary itself.** `vocab->dict[]` is a `cell[]` array allocated once and grown by `dict_ensure` when needed. Everything compiled into it lives until program exit or until `forget` rewinds the dictionary. GC doesn't reclaim it.
- **The name pool, source pool, symbol pool.** Static-sized character arrays inside `Vocabulary`, used to hold the names of words/variables, the captured source text of colon definitions, and the bytes of interned symbols. Written by the compiler, lasts until program exit or `forget`.
- **The loaded-files list.** `loaded_files[]` holds `strdup`'d filename strings. Manually `free`'d in `forget_user`, not GC'd.
- **Input buffers, token buffers.** Static-sized arrays inside `Interpreter`. Reused on every input cycle.

The boundary is: anything allocated via the Object registry is GC'd; anything allocated by other means is manually managed (or persists for the program's life).

The `forget` word rewinds the dictionary state, but objects referenced by `forget`'d code only get freed when the next GC notices they're unreachable. So `forget` doesn't immediately release memory; the next GC does.

### Cycles

The implementation handles cycles correctly via the mark bit (see Part 5). But cycles are hard to create from user code in practice: arrays and sets aren't mutated in place by any user-level word, so you can't easily build a set that contains itself. Continuations can hold references to other heap objects, but they aren't mutable either. The cycle-safety is a defensive property of the algorithm rather than a feature exercised by typical programs.

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

Now suppose for the sake of the example that `MAX_OBJECTS = 8` (the real value is 65536). The user does something that allocates one more object — anything: build a new string, a new array. The registry is full. `object_alloc_slot` triggers GC.

GC runs:

1. **Clear marks.** `memset(object_mark, 0, ...)`.
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
- Slot 5: unmarked. Free the string bytes (`obj->bytes`) and the Object struct itself. `objects[5] = NULL`.
- Slot 6: unmarked. Free the items array and the Object. `objects[6] = NULL`.
- Slot 7: marked. Skip.

After sweep: slots 5 and 6 are free. `object_alloc_slot` re-scans, finds slot 5, returns 5. The next allocation uses slot 5. The program continues.

In this example, the GC reclaimed two objects: handle 5 (the temporary `"the iliad"` string used only inside the difference expression) and handle 6 (the temporary singleton set holding it). Both became unreachable as soon as `-` finished and only its result remained on the stack.

---

## Part 12: The big picture

The GC, in slogan form:

- Find all the live objects by walking from roots.
- Free the rest.

The details that make it work:

- A small set of roots that captures everything: three stacks, the dictionary, and a temporary-roots array for C-level in-flight Vals.
- A type-aware mark function that descends into composite Vals (sets, arrays, continuations) and respects already-visited marks (so cycles terminate).
- A dictionary walker that knows the layout of compiled code well enough to find literals and variable values, and to skip operand cells without misreading them.
- A sweep that frees the right inner allocation per object kind, then the outer struct.
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
