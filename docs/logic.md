# Logic programming in Telic

This is a primer on logic programming — computing with *unknowns* and *search*
rather than fixed values — and on how Telic provides it. By the end you
should understand:

- What a logic variable is, and how a *substitution* records what the unknowns
  stand for
- How *unification* matches two terms two-directionally, binding unknowns as it
  goes
- Why a *trail* is the key to undoing bindings, and how it makes backtracking
  cheap
- How disjunction (`amb`) and failure (`fail`) turn a program into a search over a
  tree of choices
- How *reification* extracts a finished answer from the mutable machinery
- How these pieces compose with datasets into pattern queries over facts

It's a conceptual tour. The engine lives in `src/c/logic.c` and parts of
`src/c/core.c`; the fact-database layer is in the standard library. Telic's
logic engine is behaviorally a microKanren — the classic minimal relational
language — but realized on the interpreter's own substrate (a mutable binding
store, a trail, and delimited continuations) rather than the streams-of-
substitutions of the textbook presentation. Part 7 draws the correspondence.

---

## Part 1: Computing with unknowns

Ordinary code computes forward: you have values, combine them, get a result.
`1 2 +` has two known inputs and one known output, control flows one way, and
every variable holds a definite value at every moment.

Logic programming relaxes both assumptions. A *logic variable* can stand for a
value not yet known — a hole to be filled in later, or never. And the basic
operation isn't "compute a result" but *unify*: assert that two terms are equal
and let the engine work out what the unknowns must be for that to hold. If more
than one assignment could work, the engine *searches* — tries possibilities and
backtracks out of the ones that lead nowhere. The payoff is the one backtracking
and the fact database trade on: you stop writing the search and start writing the
*constraints*. Three ingredients make it work — a place to record what each
unknown stands for, a way to make two terms equal, and a way to undo and try
again — and the rest of the document is each in turn.

---

## Part 2: Logic variables and the substitution

A logic variable is a tagged value whose payload is not a value but an *index*
into the binding store — a flat array that is the *substitution*, recording what
each variable currently stands for. The `lvar` word hands back a fresh one,
unbound:

```forth
lvar        ( -- var )    \ a new logic variable, standing for nothing yet
```

A variable's slot holds either "still unbound" or some value meaning "this
variable stands for that." Because the bound value can itself be (or contain)
another variable, resolving a variable to what it *ultimately* stands for means
following the chain until it ends at a non-variable or an unbound variable — the
walk called `deref`, aliased `?`:

```forth
?           ( v -- val )  \ resolve a variable to its current binding (or itself)
```

A variable is created explicitly: `lvar` pushes a fresh one, `lvar to x` names a
persistent global, and a `?` prefix in a locals list (`| ?x |`) declares a fresh
per-call local inside a definition or quotation. Capitalized names for logic
vars are stylistic convention, not syntax.

There's one more way to write an unknown: the wildcard `_`, an *anonymous*
variable that allocates no slot, unifies with anything, and records nothing —
useful when a position must match but its value doesn't matter.

```forth
_           ( -- unbound )  \ matches anything, binds nothing
```

The substitution is *mutable*: binding a variable writes into its slot. That is
the central implementation choice, and it's what makes the trail (Part 4)
necessary.

---

## Part 3: Unification

Unification is the heart of the system: given two terms, make them equal —
binding whatever unknowns it must, failing if no binding can. It's symmetric —
neither side is "the pattern," both can contain unknowns, information flows both
ways — and it's a recursive case analysis. Resolve both sides through the
substitution, then, in priority order:

1. **Same variable.** Two unbound variables that already share an identity are
   equal; nothing to do.
2. **A variable meets a term.** Bind the variable to the term — this is where an
   unknown acquires a value; afterward the variable resolves to that term.
3. **The wildcard** matches anything and binds nothing.
4. **Two compounds of the same shape** recurse into their parts.
5. **Two ground values** unify exactly when they compare equal.

The structural case (4) is where unification threads bindings through an entire
structure in one pass: arrays unify by position (same length, corresponding
elements unify, and a trailing `rest` pattern takes the remaining elements); and frames
unify as **open records** — only the keys the two frames *share* are constrained,
and a key present on just one side is left alone. That open-record rule is what
lets a frame double as a query pattern: a pattern `{ :role :wizard }` unifies with
any row that has `:role :wizard` regardless of its other columns, which is
precisely relational selection-and-projection falling out of the unification rule.

A *goal* is any computation run for success or failure: it either completes,
leaving its results, or fails, and failure is a control transfer — the search
backs up to the nearest choice point — never a returned boolean. Run as a goal,
unification (`unify`, aliased `~`) either succeeds — leaving the
now-unified term, so `V 42 ~` binds `V` (declared beforehand with `lvar to V`) and
leaves `42` — or fails and backtracks; it is the archetypal goal, and the words
that take a goal per element (`choose`, `solutions`, `take-solutions`) expect
exactly this contract.
A program describes a relation by sequencing unification goals: each `~` either
narrows the unknowns or fails the branch.

```forth unify-binds
lvar to V
V 42 ~ . cr
V ? . cr
```
```output
42
42
```

Sometimes you want to know *whether* two terms could unify without keeping the
bindings — a pure predicate. `matches?` unifies, then immediately rewinds the
substitution to where it started, answering the yes/no question and leaving the
store untouched. The mechanism it uses to rewind is the same one that powers all
of backtracking: the trail.

```forth open-record-matches
{ :role :wizard :name :tim } { :role :wizard } matches? . cr
{ :role :fighter :name :bob } { :role :wizard } matches? . cr
```
```output
1
0
```

---

## Part 4: The trail — undoing bindings

A mutable substitution has an obvious problem: search means trying a possibility
and, if it fails, pretending it never happened — but unification has been writing
bindings into the store the whole time. To back out, the engine must know exactly
which slots it wrote and reset them. That record is the **trail**: a stack of the
variable identities bound so far, in order. Binding does two things — write the
slot, and push its identity onto the trail. Undoing is then mechanical: pop trail
entries back to a remembered mark, setting each variable unbound again.

The pattern is always the same — *note* the trail depth before a tentative
computation, *undo to* that depth to erase it. `matches?` does exactly this around
a single unification; search does it around a whole branch. This is the classic
Warren-Abstract-Machine trail, and it's what makes backtracking cheap: undoing a
branch costs one trail-pop per binding the branch made, not a copy of the whole
substitution. The price is that bindings are chronological — you can only rewind in
reverse order, to a mark — which is exactly the discipline depth-first search
imposes anyway.

---

## Part 5: Search — choice and failure

Unification narrows the unknowns along a single line of reasoning; search explores
*alternatives*. Two primitives provide it:

```forth
amb         ( branch1 branch2 -- )   \ try branch1; if it fails, try branch2
fail        ( -- )                   \ abandon this branch; back up to the nearest choice
```

`amb` ("ambiguous") is the disjunction of logic programming; `fail` declares the
current line dead and walks the search back to the most recent unexplored
alternative. A multi-way choice is nested binary `amb`. Crucially, `amb`
*commits* to the first branch that succeeds — if `branch1` succeeds it never tries
`branch2`, it just drops the choice point — so to enumerate more than one solution
you record the answer and then explicitly `fail` to drive the search onward.
`solutions` and `take-solutions` package that pattern over a candidate list —
`choose` collecting every success instead of committing to one.

A choice point is a *snapshot*. `amb` is built directly on the delimited-
continuation machinery (`continuations.md`): it marks the return stack with a
distinct *choice* prompt and saves everything a branch could disturb — the data
stack depth, the trail depth (i.e. the substitution), and the count of variables
allocated so far. It then runs the first branch. If the branch fails, `fail`
unwinds the return stack back to that prompt, landing inside the `amb`, which
restores the snapshot — same data stack, every binding the branch made undone via
the trail, the slots it allocated reclaimed — and runs the second branch from
exactly the state the first one began in. A unification deep inside a failed branch
leaves no trace.

With these pieces, a search program is conjunction (sequencing goals; any failure
propagates outward to the enclosing choice) nested inside disjunction (`amb`),
which `amb`/`fail` walk depth-first, binding on the way down and unbinding on the
way back up.

```forth amb-fail
[: fail :] [: "second branch" :] amb . cr
```
```output
second branch
```

---

## Part 6: Reification — reading out an answer

When a search succeeds, the answer lives in the substitution: the query variables
now resolve to their solved values. But that answer is entangled with the mutable
store — it shares slots the next backtrack will unbind, and unsolved parts of it
are still bare variable handles that mean nothing outside the engine.
*Reification* takes a clean snapshot: it walks a term, resolving every bound
variable to its value and giving every *unbound* variable a stable stand-in, using
a map so that a variable appearing several times is renamed consistently (shared
structure stays shared). Two modes share the walk:

- **`copy`** gives each distinct unbound variable a brand-new logic variable — an
  alpha-renaming, an independent term with the same shape and sharing. Useful for a
  private instance whose variables won't collide with anyone else's.
- **`reify`** gives each distinct unbound variable a fresh *symbol* (interned as
  `_0`, `_1`, …, printed `:_0`, `:_1`, …). The result is an ordinary ground value
  — printable, comparable, storable — that no longer references the substitution.
  This is how a solved query becomes a returnable answer.

Both deep-copy the surrounding structure, dereferencing variables as they go, so a
reified term reflects every binding in force at the moment of reification.

```forth reify-snapshot
[ lvar lvar 5 ] reify . cr
```
```output
[ :_0 :_1 5 ]
```

---

## Part 7: The relationship to microKanren

microKanren is the standard minimal logic language: four operators — `fresh`
(introduce a variable), `≡` (unify), `disj` (or), `conj` (and) — over a
*substitution*, with goals mapping a substitution to a (possibly infinite) *stream*
of substitutions. Telic provides the same operations and the same behavior by
a different route:

| microKanren | Telic | how it differs |
|---|---|---|
| `fresh` | `lvar` | A variable is a handle into the binding store, not an abstract symbol. |
| `≡` (unify) | `~` / `unify` | Same two-directional unification. |
| `disj` (or) | `amb` | A binary choice point, built on a delimited continuation. |
| `conj` (and) | sequencing of goals | Run one goal then the next; `fail` propagates. |
| the substitution | the binding store | **Mutable** store + a **trail**, not a persistent immutable map. |
| a stream of answers | backtracking search | A depth-first walk via `amb`/`fail`, not a lazy stream. |
| `reify` | `reify` | Same idea: name the leftover unbound variables for output. |

The substantive differences are the last two rows. microKanren's substitution is
*persistent* — extending it returns a new map and leaves the old intact, so
branches just hold different maps and need no undo — and its search is a lazy
*stream* that disjunction interleaves. Telic keeps one *mutable* substitution
undone with the trail, and drives the search with delimited continuations. The two
compute the same answers; the mutable-plus-trail approach fits an imperative
interpreter that already owns its call chain as data — a binding is one array
write, undoing a branch is a few trail-pops, a choice point is a return-stack mark
plus a few saved integers — so the relational operators *are* primitives over the
interpreter's own state, with no transpilation to a stream library.

Like most Prologs, Telic omits the **occurs check**: unifying a variable with
a term doesn't first check whether the variable appears inside that term, so it can
build a cyclic structure. The win is that binding stays a single array write; the
cost is that a later `copy` or `reify` walking such a cycle would recur without
end, which is why both guard the walk with a depth cap and bail rather than loop.
One feature beyond the textbook core is the **open-record unification on frames**
of Part 3 — what lets the same engine serve as a relational query.

---

## Part 8: A worked application — querying a dataset

Facts are held in a dataset — a frame of typed columns — and a *query
pattern* is a frame naming some of its columns: a partial row. `query` keeps
the rows whose columns equal the pattern's ground values (each key an `eq` mask
over one column, the masks multiplied), and a value that is an unbound logic
variable or `_` constrains nothing: that is relational *select*, done by the
column kernels, with projection left to `@` on the answer.

Unification applies to single rows. `query-rows` answers the selected rows as
frames, and unifying the pattern with each row frame — `{ :age Age } row ~` —
binds the pattern's variables by the open-record rule of Part 3: shared keys
must agree, the variable takes the cell, extra columns are ignored. `matches?`
does the same without disturbing the store, and `choose` over the rows turns
"which row satisfies this goal?" into the search of Part 5, backtracking to the
next row when a goal fails. Nothing here needs new primitives: column masks
select, unification binds, and the dataset words (`merge-by`, `aggregate`,
`rows>dataset`, `db-query`) hold, join and load the facts.

---

For broader context: `continuations.md` is the `amb`/`fail` choice-point machinery
(the same prompt mechanism, used for backtracking). The garbage collector follows
a reachable logic variable into the binding store, so its binding is kept alive.
