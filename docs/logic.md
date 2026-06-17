# Logic Programming in logicforth

This document is a primer on logic programming — computing with *unknowns* and *search* rather than with fixed values — and on how logicforth provides it. By the end you should understand:

- What a logic variable is, and how a *substitution* records what the unknowns stand for
- How *unification* matches two terms two-directionally, binding unknowns as it goes
- Why a *trail* is the key to undoing bindings, and how it makes backtracking cheap
- How disjunction (`amb`) and failure (`fail`) turn a program into a search over a tree of choices
- How *reification* extracts a finished answer out of the mutable machinery
- How these pieces compose into a relational fact database

The core lives in `src/c/logic.c` (`unify`, `deref`, `p_amb`, and friends) and parts of `src/c/core.c` (logic-variable allocation, the copy/reify walker). The fact-database layer is in `src/forth/lib.l4`. logicforth's logic engine is behaviorally a microKanren — the classic minimal relational language — but realized on the interpreter's own substrate (a mutable binding store, a trail, and delimited continuations) rather than on the streams-of-substitutions that the textbook presentation uses. Part 7 draws the correspondence explicitly.

---

## Part 1: Computing with unknowns

Ordinary code computes forward. You have values; you combine them; you get a result. `1 2 +` has two known inputs and produces one known output. Control flows one way, and every variable holds a definite value at every moment.

Logic programming relaxes both assumptions. A *logic variable* can stand for a value that isn't known yet — a hole to be filled in later, or never. And the basic operation isn't "compute a result" but *unify*: assert that two terms are equal, and let the engine work out what the unknowns must be for that to hold. If more than one assignment of unknowns could work, the engine *searches* — it tries possibilities, and backtracks out of the ones that lead nowhere.

The payoff is the same one that backtracking (continuations.md, Part 14) and the fact database (Part 8 below) trade on: you stop writing the search and start writing the *constraints*. "Find an `x` and a `y` such that `x + y = 7` and both are between 1 and 5" becomes a description, and the runtime does the looking.

Three ingredients make this work, and the rest of the document is about each in turn:

- A place to record what each unknown currently stands for — the **substitution** (Part 2).
- A way to make two terms equal, discovering bindings — **unification** (Part 3).
- A way to undo bindings and try alternatives — the **trail** (Part 4) and **search** (Part 5).

---

## Part 2: Logic variables and the substitution

A logic variable is a `Val` like any other, tagged `T_LOGIC_VAR`. Its payload is not a value but an *index* — a handle into the `lvar_stack`, a flat array that is the substitution: the store of what each variable currently stands for.

```c
int object_new_logic_var(Interpreter *interp) {
    alloc_count_lvar++;
    GROW_IF_FULL_SYS(interp->lvar_top, interp->lvar_cap, interp->lvar_stack);
    int id = interp->lvar_top++;
    interp->lvar_stack[id] = make_tagged(T_UNBOUND, 0);   // starts unbound
    return id;
}
```

The `lvar` word hands one back, freshly unbound:

```forth
lvar        ( -- var )    \ a new logic variable, standing for nothing yet
```

A variable's slot in `lvar_stack` holds one of two things: `T_UNBOUND`, meaning "still an unknown," or some `Val`, meaning "this variable stands for that." Because the bound value can itself be (or contain) another logic variable, resolving a variable to what it *ultimately* stands for means following the chain until it ends. That walk is `deref`:

```c
Val deref(Interpreter *interp, Val value) {
    while (VAL_TAG(value) == T_LOGIC_VAR) {
        Val binding = interp->lvar_stack[VAL_DATA(value)];
        if (VAL_TAG(binding) == T_UNBOUND)
            break;                 // reached an unbound variable — stop here
        value = binding;           // bound — follow it and keep walking
    }
    return value;
}
```

`deref` returns either a non-variable value, or the *representative* unbound variable at the end of a chain. The `$` word exposes it:

```forth
$           ( v -- val )  \ resolve a variable to its current binding (or itself, if unbound)
```

There is one more way to write "an unknown": the wildcard `_`, an *anonymous* variable. It pushes a bare `T_UNBOUND` rather than allocating a slot. It unifies with anything and records nothing — useful when a position has to match but its value doesn't matter.

```forth
_           ( -- unbound )  \ matches anything, binds nothing
```

The substitution is *mutable*: binding a variable writes into `lvar_stack[id]`. That is the central implementation choice, and it's what makes the trail (Part 4) necessary.

---

## Part 3: Unification

Unification is the heart of the system. Given two terms, it makes them equal — binding whatever unknowns it must, and failing if no binding can. It is symmetric: neither side is "the pattern" and neither is "the input." Both can contain unknowns; information flows both ways.

The algorithm is a recursive case analysis. First resolve both sides through the substitution, then:

```c
int unify(Interpreter *interp, Val left_val, Val right_val) {
    left_val  = deref(interp, left_val);
    right_val = deref(interp, right_val);

    // two unbound variables that are already the same variable: trivially equal
    if (VAL_TAG(left_val) == T_LOGIC_VAR && VAL_TAG(right_val) == T_LOGIC_VAR
            && VAL_DATA(left_val) == VAL_DATA(right_val))
        return 1;

    // an unbound variable on either side: bind it to the other term
    if (VAL_TAG(left_val) == T_LOGIC_VAR)  { bind_var(interp, VAL_DATA(left_val),  right_val); return 1; }
    if (VAL_TAG(right_val) == T_LOGIC_VAR) { bind_var(interp, VAL_DATA(right_val), left_val);  return 1; }

    // the anonymous wildcard matches anything, binding nothing
    if (VAL_TAG(left_val) == T_UNBOUND || VAL_TAG(right_val) == T_UNBOUND)
        return 1;

    // structural cases recurse (arrays, pairs, frames — below)
    // ...

    // anything else: unify iff the two values compare equal
    return val_cmp(interp, left_val, right_val) == 0;
}
```

Read the cases as a priority list:

1. **Same variable.** Two unbound variables that share a handle are already equal; nothing to do.
2. **A variable meets a term.** Bind the variable to the term. This is where an unknown acquires a value. After this, `deref` of that variable yields the term.
3. **The wildcard.** A `T_UNBOUND` that isn't a named variable matches anything and binds nothing.
4. **Two compound terms of the same shape.** Recurse into their parts (next).
5. **Two ground values.** They unify exactly when they're equal (`val_cmp`).

### Structural unification

Compound terms unify component-wise, and the components can themselves contain unknowns — so unification threads bindings through an entire structure in one pass.

- **Arrays** unify by position: same length, and corresponding elements unify.
- **Pairs** (cons cells) unify head-with-head and tail-with-tail.
- **Frames** unify as *open records*: only the keys the two frames share are constrained; a key present on just one side is left alone.

The frame case is worth seeing, because the open-record semantics is what makes frames double as query patterns (Part 8):

```c
if (VAL_TAG(left_val) == T_FRAME && VAL_TAG(right_val) == T_FRAME) {
    Object *left = OBJECT_AT(VAL_DATA(left_val));
    Object *right = OBJECT_AT(VAL_DATA(right_val));
    int i = 0, j = 0;
    while (i < left->len && j < right->len) {       // keys are sorted in each frame
        cell left_key = left->frame.keys[i];
        cell right_key = right->frame.keys[j];
        if (left_key == right_key) {                // shared key: values must unify
            if (!unify(interp, left->frame.values[i], right->frame.values[j]))
                return 0;
            i++; j++;
        } else if (left_key < right_key) i++;       // key only on the left: no constraint
        else j++;                                   // key only on the right: no constraint
    }
    return 1;
}
```

A pattern frame `{ :role :wizard }` unifies with any row frame that has `:role :wizard`, no matter what other columns the row carries. Extra keys on either side impose no constraint. That is precisely relational selection-and-projection, falling straight out of the unification rule.

### `unify` and `~` as goals

The word `unify` (aliased `~`) runs unification as a *goal* — a step that either succeeds or fails:

```c
void p_unify(Interpreter *interp) {
    POP(right);
    POP(left);
    if (unify(interp, left, right))
        push(interp, deref(interp, left));   // success: leave the now-unified term
    else
        backtrack(interp);                   // failure: fail this branch of the search
}
```

On success it leaves the unified term (so `V 42 ~` binds `V` and leaves `42`); on failure it backtracks (Part 5). A program describes a relation by sequencing unification goals: each `~` either narrows the unknowns or fails the branch.

### Testing a match without committing: `matches?`

Sometimes you want to know *whether* two terms could unify without actually keeping the bindings — a pure predicate. `matches?` unifies, then immediately rewinds the substitution:

```c
void p_matches(Interpreter *interp) {
    POP(row);
    POP(pattern);
    int trail_mark = interp->bind_trail_top;     // remember where the trail was
    int matched = unify(interp, pattern, row);
    trail_undo_to(interp, trail_mark);           // undo any bindings unify made
    push(interp, make_bool(matched));
}
```

`matches?` is non-destructive: it answers the yes/no question and leaves the store exactly as it found it. The mechanism it uses to do that — marking the trail and undoing back to the mark — is the same mechanism that powers all of backtracking, which is the next part.

---

## Part 4: The trail — undoing bindings

A mutable substitution has an obvious problem. Search means trying a possibility, and if it doesn't pan out, *pretending it never happened* and trying another. But unification has been writing bindings into `lvar_stack` the whole time. To back out, the engine has to know exactly which slots it wrote, and reset them.

That record is the **trail**: a stack of the variable handles that have been bound, in order. Binding does two things — write the slot, and push its handle:

```c
static void bind_var(Interpreter *interp, int var_handle, Val value) {
    GROW_IF_FULL_SYS(interp->bind_trail_top, interp->bind_trail_cap, interp->bind_trail);
    interp->lvar_stack[var_handle] = value;                 // record the binding
    interp->bind_trail[interp->bind_trail_top++] = var_handle;  // remember we did
}
```

Undoing is then mechanical: pop trail entries back to a remembered mark, setting each variable unbound again.

```c
static void trail_undo_to(Interpreter *interp, int mark) {
    while (interp->bind_trail_top > mark) {
        int var_handle = interp->bind_trail[--interp->bind_trail_top];
        interp->lvar_stack[var_handle] = make_tagged(T_UNBOUND, 0);
    }
}
```

The pattern is always the same: *note* the trail depth before a tentative computation, and *undo to* that depth to erase it. `matches?` (Part 3) does exactly this around a single unification. Search (Part 5) does it around a whole branch.

This is the classic Warren-Abstract-Machine trail, and it makes backtracking cheap: undoing a branch costs one trail-pop per binding the branch made, not a copy of the whole substitution. The price is that bindings are chronological — you can only rewind in the reverse order they happened, to a mark — which is exactly the discipline a depth-first search imposes anyway.

---

## Part 5: Search — choice and failure

Unification narrows the unknowns along a single line of reasoning. Search explores *alternatives*. Two primitives provide it.

`amb` ("ambiguous") is binary choice between two branch quotations: try the first; if it fails, try the second. `fail` declares the current line of reasoning dead and asks to backtrack.

```forth
amb         ( branch1 branch2 -- )   \ try branch1; if it fails, try branch2
fail        ( -- )                   \ abandon this branch; back up to the nearest choice
```

`amb` is the disjunction of logic programming, and `fail` is the trigger that walks the search back to the most recent unexplored alternative. A multi-way choice is expressed as nested binary `amb` — `amb(a, amb(b, c))` chooses among three. `amb` *commits* to the first branch that succeeds: if `branch1` succeeds it never tries `branch2` — it just drops the choice point. The success-then-`fail` idiom below relies on this: a success isn't re-entered, so to keep searching you have to explicitly `fail` back out of it.

### A choice point is a snapshot

`amb` is built directly on the delimited-continuation machinery (continuations.md). It marks the return stack with a *choice* prompt (`PROMPT_CHOICE`, a distinct kind from the *exception* prompt that `reset`/`shift` use, so the two nest without interfering), and snapshots everything a branch could disturb:

```c
void p_amb(Interpreter *interp) {
    POP_XT(branch2, "amb");
    POP_XT(branch1, "amb");

    int saved_dsp   = interp->dsp;            // the data stack ...
    int saved_trail = interp->bind_trail_top; // ... the substitution (via the trail) ...
    int saved_lvar  = interp->lvar_top;       // ... and the variables allocated so far

    int mark_index = interp->rsp;
    int mark_id = push_prompt(interp, PROMPT_CHOICE);

    execute_cfa(interp, branch1);             // explore the first alternative

    if (interp->unwinding && interp->unwind_target == mark_id) {
        interp->unwinding = 0;                // a fail unwound back to this choice
        interp->rsp = mark_index;
        interp->dsp = saved_dsp;              // restore the snapshot ...
        trail_undo_to(interp, saved_trail);   // ... unbinding everything branch1 bound ...
        interp->lvar_top = saved_lvar;        // ... and reclaiming branch1's fresh variables
        execute_cfa(interp, branch2);         // then explore the second alternative
    } else if (!interp->unwinding) {
        interp->rsp = mark_index;             // branch1 succeeded — drop the choice point
    }
}
```

`fail` (its C side is `backtrack`) finds the nearest choice prompt, sets it as the unwind target, and raises the `unwinding` flag; the inner-interpreter loop then unwinds the return stack back to that prompt — which lands inside the `amb` that pushed it, in the `if` branch above. Restoring the snapshot is what makes the second alternative start from exactly the state the first one began in: same data stack, same substitution (every binding `branch1` made is undone), same set of live variables (the slots `branch1` allocated are reclaimed by rewinding `lvar_top`).

So the trail of Part 4 and the variable store of Part 2 are both rolled back at a choice point, automatically. A unification deep inside a failed branch leaves no trace.

### Conjunction, disjunction, and the search tree

With these pieces, the shape of a search program is:

- **Conjunction** ("this *and* that") is sequencing: run one goal, then the next. If any goal fails, the whole conjunction fails — `fail` propagates outward to the enclosing choice.
- **Disjunction** ("this *or* that") is `amb`: run as a choice between branches.

Nesting conjunctions inside disjunctions builds a tree of possibilities, and `amb`/`fail` walk it depth-first, binding on the way down and unbinding on the way back up. To enumerate *every* solution rather than stop at the first, treat a success as one more thing to back out of: record the answer, then `fail` to drive the search on to the next leaf. The fact-database query in Part 8 and the backtracking examples in continuations.md both use this success-then-fail idiom.

---

## Part 6: Reification — reading out an answer

When a search succeeds, the answer lives in the substitution: the query variables now `deref` to their solved values. But that answer is entangled with the mutable store — it shares variable slots that the next backtrack will unbind, and unsolved parts of it are still bare `T_LOGIC_VAR` handles that mean nothing outside the engine. *Reification* takes a clean snapshot: it walks a term, resolving every bound variable to its value and giving every *unbound* variable a stable, printable name.

The walk is shared with plain structural copying; a flag selects the treatment of variables:

```c
static Val varmap_lookup(Interpreter *interp, VarMap *map, int slot) {
    for (int i = 0; i < map->count; i++)         // already seen this variable?
        if (map->entries[i].slot == slot)
            return map->entries[i].value;         // reuse the same stand-in (consistent renaming)

    Val fresh;
    if (map->reify)
        fresh = make_symbol(intern_symbol(interp, /* stored as "_0", "_1"; prints as ":_0", ":_1" */));  // a printable name
    else
        fresh = make_logic_var(object_new_logic_var(interp));               // a brand-new variable

    /* record slot -> fresh in the map, and return fresh */
}
```

The map guarantees *consistency*: a variable that appears several times in a term is renamed to the same stand-in every time, so shared structure stays shared. The two modes are:

- **`copy`** gives each distinct unbound variable a brand-new logic variable. The result is an independent term with the same shape and the same sharing — an *alpha-renaming*. Useful when you need a private instance of a term whose variables won't collide with anyone else's.
- **`reify`** gives each distinct unbound variable a fresh *symbol* (interned as `_0`, `_1`, … and printed as `:_0`, `:_1`, … since symbols always display with a leading colon). The result is an ordinary ground value — printable, comparable, storable — that no longer references the substitution at all. This is how a solved query is turned into a returnable answer.

Both deep-copy the surrounding structure (strings, arrays, sets, pairs, frames, matrices), dereferencing variables as they go, so a reified term reflects all the bindings in force at the moment of reification.

---

## Part 7: The relationship to microKanren

microKanren is the standard minimal logic language: four operators — `fresh` (introduce a variable), `≡` (unify), `disj` (or), `conj` (and) — over a *substitution*, with goals that map a substitution to a (possibly infinite) *stream* of substitutions. logicforth provides the same four operations and the same behavior, but takes a different route to them:

| microKanren | logicforth | how it differs |
|---|---|---|
| `fresh` | `lvar` | A variable is a handle into `lvar_stack`, not an abstract symbol. |
| `≡` (unify) | `~` / `unify` | Same two-directional unification; same occurs-free binding. |
| `disj` (or) | `amb` | A binary choice point, built on a delimited continuation. |
| `conj` (and) | sequencing of goals | Run one goal then the next; `fail` propagates. |
| the substitution | `lvar_stack` | **Mutable** store + a **trail**, not a persistent immutable map. |
| a stream of answers | backtracking search | A depth-first walk via `amb`/`fail`, not a lazy stream. |
| `reify` | `reify` | Same idea: name the leftover unbound variables for output. |

The substantive difference is the last two rows. microKanren's substitution is *persistent*: extending it returns a new map and leaves the old one intact, so alternative branches simply hold different maps and need no undo. Its search is a *stream*: a goal yields a lazy sequence of substitutions, and disjunction interleaves sequences.

logicforth instead keeps one *mutable* substitution and undoes it with the trail, and drives the search with delimited continuations rather than streams. The two designs compute the same answers. The mutable-plus-trail approach fits an imperative interpreter that already owns its call chain as data (the same property continuations.md relies on): a binding is a single array write, undoing a branch is a few trail-pops, and a choice point is a return-stack mark plus three saved integers. There is no transpilation to a stream combinator library; the relational operators *are* primitives over the interpreter's own state.

Like most Prologs, logicforth omits the **occurs check**: `unify` binds a variable to a term without first checking whether the variable appears inside that term, so unifying a variable with a structure containing itself is allowed and creates a cyclic structure. The win is that binding stays a single array write; the cost is that a later `copy` or `reify` walking such a cycle would recur without end, which is why both guard the walk with `MAX_NESTING_DEPTH` (core.c) and bail out rather than loop.

One feature logicforth adds beyond the textbook core is **open-record unification on frames** (Part 3): two frames unify on their shared keys and ignore the rest. Classical logic languages unify terms by fixed arity and position; the open-record rule makes a frame behave as a partial constraint, which is what lets the same unification engine serve as a relational query (Part 8).

---

## Part 8: A worked application — the fact database

The fact database is logic programming over frames, built entirely in lib.l4 on the primitives above. It shows the layer composing into something recognizable: a small relational store.

A **relation** is a frame with two fields: a set of `:rows`, and an `:index` mapping each indexed column to a sub-index (a frame from column value to the set of rows holding that value). A **row** is a frame keyed by column name. A **query pattern** is also a frame — a partial row.

The whole query mechanism rests on the open-record rule. Asking "which rows match this pattern?" is asking "which rows unify with this pattern frame?", and `matches?` answers exactly that without disturbing the store:

```forth
( rel pattern -- [rows] )    \ rows matching pattern
: query
    |> rel pattern |
    rel pattern candidates                       \ narrow using the indexes first
    rel pattern covering? if
        dup size take                            \ index alone answers the query
    else
        [: |> row | pattern row matches? :] filter   \ otherwise test each candidate
    then ;
```

`matches?` is the relational *select* (keep the rows that fit the pattern) and *project* (the pattern names only the columns it cares about, and open-record unification ignores the rest) in a single operation. The surrounding machinery — `candidates` intersecting the smallest indexed buckets first, `covering?` recognizing when the indexes alone settle the answer — is ordinary optimization; the meaning of a query is the unification.

Asserting a row adds it to `:rows` and to each indexed column's bucket. There are two removals: `retract-row` takes one exact row and removes it from both `:rows` and every indexed bucket, while the user-facing `retract` takes a pattern, runs `query` to find every row matching it, and `retract-row`s each one — so a single `retract` can remove many rows. Because rows and buckets are sets keyed by value, duplicate rows collapse, and a row is found by content rather than identity. None of this needs new primitives: it is logic variables, unification over frames, and the set and frame data structures, assembled in a library.

---

## Part 9: Where to look in the source

The core engine is in `src/c/logic.c`:

- **`object_new_logic_var`** (in `core.c`) — allocate a fresh unbound slot in `lvar_stack`. The `lvar` word wraps it.
- **`deref`** — walk a variable to what it ultimately stands for.
- **`unify`** — the unification algorithm: variable, wildcard, structural (array/pair/frame), and equality cases.
- **`bind_var` / `trail_undo_to`** — bind a variable and record it on the trail; rewind the trail to a mark.
- **`p_unify`** (`unify` / `~`), **`p_matches`** (`matches?`), **`p_deref`** (`deref` / `$`), **`p_wildcard`** (`_`) — the user-facing goal and helper words.
- **`p_amb`** (`amb`) and **`backtrack`** (`fail`, in `words.c`) — the choice point and the backtracking trigger, built on the `PROMPT_CHOICE` prompt.

Reification is in `core.c`:

- **`copy_value_inner` / `varmap_lookup`** — the shared copy/reify walk and its consistent variable renaming.
- **`p_copy`** (`copy`) and **`p_reify`** (`reify`) — alpha-rename to fresh variables, or name unbound variables for output.

The fact database is in `src/forth/lib.l4`:

- **`relation`, `assert`, `retract`, `query`, `count-matches`** — the relational layer.
- **`candidates`, `covering?`, `bucket-of`** — index-driven query narrowing.

For broader context:

- **`docs/continuations.md`** — `amb`/`fail` are the `PROMPT_CHOICE` half of the delimited-continuation machinery; Part 14 there builds backtracking from the same parts.
- **`docs/gc.md`** — the collector follows a `T_LOGIC_VAR` into `lvar_stack` when marking, so a reachable variable's binding is kept alive.
