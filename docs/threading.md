# Threaded Code in logicforth

This is a primer on how logicforth's inner dispatch loop is organized, and
why it's organized that way. By the end you should understand:

- What "threaded code" means, why it has nothing to do with concurrency,
  and where it sits between an AST walker and a JIT compiler
- How logicforth lays out a compiled word and dispatches through it
- How *direct threading* and *tail-call dispatch* combine to make each
  virtual operation cost about one memory read plus one jump
- What that organization implies for the rest of the interpreter: the
  compiler that emits code, the GC's walk over compiled bodies, the
  trampoline that calls words from C, and the image save/load format

The code lives in `src/c/core.c` (`run_inner`, `execute_cfa`, `docol`,
`dovar`, `dosym`, `op_cell_count`, `mark_body`) and `src/c/logicforth.h`
(the `DISPATCH` macro, the `WORD_*` accessors).

---

## Part 1: The inner loop

Take a simple Forth-style program: `1 2 3 4 + + +`. It pushes four
numbers, applies `+` three times, and leaves their sum on the stack.

Inside the interpreter, this program is a sequence of seven virtual
operations: push 1, push 2, push 3, push 4, add, add, add. To run the
program, the interpreter executes them in order. For each one, it has to
get from "the next operation in the compiled stream" to "the first
machine instruction of the C code that implements it."

That transition is the whole subject of this document. The handler's own
work — popping operands, computing, pushing a result — is unavoidable.
What's avoidable, or at least minimizable, is the per-operation overhead
of *reaching* the handler. In a program of any size that cost is paid
millions or billions of times, so a load saved per operation, or a
branch the CPU can predict instead of mispredict, shows up directly in
wall-clock time.

---

## Part 2: What "threaded" means here

The word "thread" in *threaded code* has nothing to do with concurrency. It
comes from textile imagery: a thread connecting beads on a string. A
"threaded program" is one whose executable form is a sequence of small
cells — each one a bead — that the interpreter chases like sliding along a
string.

The usage predates the modern concurrency sense by decades. It came out of
the early Forth implementations, where memory was tiny and every byte
mattered. A program encoded as a list of "addresses of routines" was both
compact (each op is one cell) and fast to dispatch (no parsing, no
integer-keyed lookup).

Here is the spectrum of how a program can be interpreted:

- **AST walker.** Parse the source into a tree of nodes; walk the tree,
  dispatching on each node's type. Simple to implement, very slow: every
  step chases pointers between scattered heap nodes, and every node-type
  test is a switch or a virtual call.

- **Bytecode interpreter.** Compile to a flat array of small integer
  opcodes. The dispatch loop reads the next opcode and switches on it.
  Cache-friendly, but every dispatch pays for an integer-opcode → handler
  lookup, usually a jump table.

- **Threaded code.** Instead of integers that index a handler table, each
  op in the compiled stream is *itself* the dispatch target. The
  integer-to-handler lookup a bytecode interpreter pays per op is gone.
  This is what logicforth uses.

- **JIT compilation.** Generate machine code on the fly; the CPU executes
  it directly, with no interpretation loop. Fastest, but requires emitting
  native instructions, managing executable memory, and patching code as
  words are defined.

Threaded code sits between bytecode and JIT: it gets some of JIT's speed
(no integer dispatch) without JIT's complexity (no native codegen). It's
also the natural fit for a Forth-family language — the dictionary itself
becomes the executable, with no separate opcode space or handler table on
the side.

Within threaded code, two flavors matter: **indirect** and **direct**. They
differ in how each op in the compiled stream points at its handler.

---

## Part 3: A useful anchor — subroutine threading

Before the flavor logicforth uses, consider the extreme case: what if we
didn't interpret at all, and the compiled program *was* native code?

That's *subroutine threading* (STC): each op becomes a literal CPU `CALL`
instruction pointing at its handler, and the compiled body of a word is a
sequence of those CALLs emitted into executable memory. The CPU's own
program counter walks from CALL to CALL; there is no dispatch loop. Each
handler ends in `RET`, returning to the instruction after the CALL.

"Dispatch" here is what the CPU does for free between instructions, so the
per-op cost is just a CALL+RET round-trip. The reasons not to do it:

1. **You have to generate native code** — target calling conventions, the
   right bytes per CALL, executable memory pages. Per-architecture,
   per-OS, unportable.
2. **Code isn't relocatable.** CALL targets are absolute or PC-relative
   addresses; move a handler and every compiled body referencing it
   breaks. Saving an image becomes hard.
3. **You can't easily inspect or hook dispatch** — profiling, tracing,
   single-stepping — without injecting trampolines.

Interpreted threading keeps dispatch in software, in one place we control,
for a few nanoseconds per op. logicforth's tail-call dispatch (Part 8)
recovers much of STC's speed while keeping the op stream as inspectable,
relocatable data.

---

## Part 4: Indirect versus direct threading

In *indirect-threaded code* (ITC), a compiled body holds *dispatch tokens*.
A token doesn't name a handler directly — it names a *code field*, a memory
cell that holds the handler. Dispatch is two reads:

```
read body cell        → token (an index)
read code-field[token] → handler (a function pointer)
call handler
```

The "indirect" part is the second read. It buys two things: every word kind
looks identical at the call site (the handler at the code field
disambiguates colon defs from variables from symbols), and tokens can be
small stable indices that survive being written to disk and reloaded.

In *direct-threaded code* (DTC), a body holds the *handler addresses
themselves*. The token-to-code-field indirection is gone:

```
read body cell → handler (a function pointer)
call handler
```

One read instead of two. logicforth uses direct threading. The cost it
pays for the saved read is twofold, and both costs are paid elsewhere in
this document:

- Body cells now hold raw function pointers, which are not stable across
  process launches (address-space layout randomization moves them). The
  image format translates them on save and load (Part 11).
- A handler like `dovar` can no longer recover "which variable?" from the
  token, because there's no token — so call sites that need to name a
  target carry it as an extra operand cell (Part 6).

---

## Part 5: A word's anatomy

The dictionary is one large `cell[]` array, where `cell` is `int64_t`:

```c
typedef struct Vocabulary {
    cell *dict;        // the dictionary: one giant int64 array
    int here;          // bump-allocator high-water mark: next free cell
    int latest_cfa;    // head of the dictionary chain
    /* ... pools for names, source, symbols ... */
} Vocabulary;
```

Each word occupies a run of cells:

```
position:   N-4  N-3  N-2  N-1   N+0     N+1    N+2   ...
          ┌────┬─────┬────┬────┬───────┬──────┬──────┐
contents: │LINK│FLAGS│NAME│SRC │handler│ body │ body │ ...
          └────┴─────┴────┴────┴───────┴──────┴──────┘
          ←──────── header ───────→  ↑   ←──── body ──→
                                    CFA = N
```

The 4-cell header (LINK, FLAGS, NAME and SOURCE pool offsets) sits at
negative offsets from the *code field address* (CFA). The CFA cell holds
the word's *handler* — a C function pointer. The body follows. The
`WORD_*` macros read the header relative to the CFA:

```c
#define WORD_LINK(v, cfa)   ((v)->dict[(cfa) - 4])
#define WORD_FLAGS(v, cfa)  ((v)->dict[(cfa) - 3])
#define WORD_NAME(v, cfa)   ((v)->dict[(cfa) - 2])
#define WORD_SOURCE(v, cfa) ((v)->dict[(cfa) - 1])
```

LINK chains each word to the previous one, so the whole dictionary is a
singly-linked list walkable from `latest_cfa`. The CFA is the word's
identity: it's what `find` returns, what `'` (tick) captures into an
execution token, and what compiled bodies reference.

### Four handler kinds

The CFA cell holds one of four handlers:

- **A primitive handler** — a C function like `p_add` or `p_dup`. The body
  is empty; the handler does the work itself, pulling arguments from the
  data stack.
- **`docol`** — the handler for colon-defined words. The body is a stream of
  compiled ops; `docol` begins walking it.
- **`dovar`** — the handler for variables. The body is one cell: the
  variable's value as a packed `Val`.
- **`dosym`** — the handler for symbol words. The body is one cell: the
  symbol-pool offset of the interned name.

`define_primitive` builds the simplest case — a word whose entire
definition is a handler in the code field:

```c
int define_primitive(Interpreter *interp, const char *name, cfa_handler handler, int flags) {
    int cfa = create_header(interp, name, flags);
    emit(interp, (cell)handler);     // the CFA cell holds the handler directly
    /* ... register the handler for image translation (Part 11) ... */
    return cfa;
}
```

`flags` is a bitmask: bit 1 immediate, bit 2 inline, bit 4 internal (the
`WORD_IS_IMMEDIATE` / `WORD_IS_INLINE` / `WORD_IS_INTERNAL` macros test
them). The cast `(cell)handler` stores a C function pointer in an `int64_t`
cell; on the 64-bit targets logicforth supports, function pointers fit and
survive the round-trip.

---

## Part 6: Body layout

A `Val` is a single 64-bit cell — it's NaN-boxed, so tag and payload both
fit in one `cell` (see nan-boxing.md). That keeps body layout simple: every
cell is either a *dispatch cell* (a handler pointer) or an *operand cell*
(a plain value the preceding handler consumes).

### Dispatch cells

A reference to another word compiles to its handler pointer. For a
primitive, that one cell is the whole op — `+` compiles to one cell,
`&p_add`. The dispatcher reads it and calls it; the handler takes its
arguments from the data stack.

A reference to a colon def, a variable, or a symbol needs one more thing:
the handler (`docol`, `dovar`, `dosym`) has to know *which* target it's
acting on. So the call site carries the target's CFA as an operand cell
right after the handler pointer. `emit_call` is the helper that emits the
right shape:

```c
void emit_call(Interpreter *interp, int target_cfa) {
    cfa_handler handler = (cfa_handler)interp->vocab->dict[target_cfa];
    emit(interp, (cell)handler);                       // handler pointer
    if (handler == docol || handler == dovar || handler == dosym)
        emit(interp, (cell)target_cfa);                // target CFA as operand
}
```

So a colon call is `[&docol][target_cfa]`, a variable read is
`[&dovar][var_cfa]`, a symbol push is `[&dosym][sym_cfa]`, and a primitive
call is just `[&p_add]`.

### Operand cells

Several primitives carry inline operands of their own. The prototype is the
literal:

```c
void emit_val_literal(Interpreter *interp, Val value) {
    emit_call(interp, interp->vocab->literal_cfa);     // emits the (literal) handler pointer
    emit(interp, (cell)value.bits);                    // the packed Val
}

void p_literal(Interpreter *interp) {
    Val value;
    value.bits = (uint64_t)interp->vocab->dict[interp->ip++];   // read operand, advance
    push(interp, value);
    DISPATCH(interp);
}
```

The pattern — read operands from `dict[ip++]`, advancing past them — is
shared by every operand-bearing op: string literals (`(dostr)`, a string
handle), the branches (`(branch)` / `(0branch)` and an offset), the
word-local variable ops (`(enter-locals)`, `(local@)`, `(local!)`, and
their depth/slot operands), `(to-var)`, and the fused superword and
local-accumulator ops.

### Knowing each op's width: `op_cell_count`

Anything that walks a body without executing it — the GC's `mark_body`, the
inliner, the image translator — has to know how many cells each op occupies,
so it advances to the next dispatch cell instead of misreading an operand as
a handler. `op_cell_count` is that single source of truth: given the cell at
a cursor, it returns the op's total width (dispatch cell plus operands). It
keys off the handler value, recognizing literals, strings, branches, the
locals ops (including the variable-width `(enter-locals-mixed)`, whose width
depends on its operand), and the fused superwords.

`docol`, `dovar`, and `dosym` get one cell from `op_cell_count`, and a
trailing target-CFA cell follows at a call site. A body walker that only
needs to skip cells (like `mark_body`) treats that trailing CFA as its own
one-cell unit and moves on, which lands it correctly on the next op either
way. A walker that has to classify every cell exactly (the image
translator) treats the CFA as the operand it is — see Part 11.

---

## Part 7: The dispatch chain

Because body cells hold handler pointers directly, "dispatch" is: read the
cell, call it. logicforth does that with no central loop in the hot path —
each handler ends by tail-calling the next, through the `DISPATCH` macro:

```c
#define DISPATCH(interp) do { \
    if ((interp)->unwinding || (interp)->error_flag) \
        return; \
    __attribute__((musttail)) \
    return ((cfa_handler)(interp)->dict[(interp)->ip++])(interp); \
} while (0)
```

`interp->dict` is a cached copy of `interp->vocab->dict` held directly on the
`Interpreter`. The dictionary lives in the `Vocabulary`, so the "natural" read
is `interp->vocab->dict[ip]` — two dependent loads (`interp` → `vocab` →
`dict`) before the index. Caching the base on the interpreter cuts that to one,
which matters because this load happens on *every* dispatch. The cache is kept
in sync trivially: `vocab->dict` is (re)allocated in exactly two places —
`interp_new` and `dict_ensure` — and each assigns `interp->dict = vocab->dict`
right after. The hot reads below (`docol`, `dovar`, `dosym`, `run_inner`,
`execute_cfa`, the operand fetches) all go through `interp->dict`; the snippets
in this document write `vocab->dict` for clarity, since the two always alias.

`__attribute__((musttail))` forces the compiler to emit a jump rather than a
call+return: the next handler reuses the current stack frame. A run of
compiled ops executes as a chain of jumps through handler bodies at constant
stack depth, no matter how many ops run.

Every non-halting handler ends with `DISPATCH(interp)`. The check at the top
is how the chain breaks cleanly: `fail` sets `error_flag` and `shift-with`
sets `unwinding`, and the next `DISPATCH` sees the flag and returns instead
of chaining.

### `run_inner` — the entry and cleanup point

`run_inner` is where the chain starts and where control returns when it
breaks:

```c
void run_inner(Interpreter *interp) {
    int initial_rsp = interp->rsp;
    while (interp->running && !interp->error_flag) {
        if (interp->unwinding) {
            /* pop the return stack looking for the target mark; resume
               there, or stop if we've unwound past where we started */
            ...
        }
        cfa_handler handler = (cfa_handler)interp->vocab->dict[interp->ip++];
        handler(interp);
    }
}
```

In steady state the loop body runs *once*: it reads the first handler and
calls it, and the tail-call chain takes over until something returns —
a halt (`running = 0`), an error, or an unwind. Then control falls back to
this loop, which either resumes (after handling an unwind) or exits.

### The handlers that thread a target through

`docol`, `dovar`, and `dosym` read their target CFA from the operand cell,
advance past it, and do their work:

```c
void docol(Interpreter *interp) {
    int target_cfa = (int)interp->vocab->dict[interp->ip++];   // operand
    rpush(interp, make_addr(interp->ip));                      // return address
    interp->ip = target_cfa + 1;                               // jump to body
    DISPATCH(interp);
}

void dovar(Interpreter *interp) {
    int var_cfa = (int)interp->vocab->dict[interp->ip++];
    Val value;
    value.bits = (uint64_t)interp->vocab->dict[var_cfa + 1];   // the variable's packed Val
    push(interp, value);
    DISPATCH(interp);
}

void dosym(Interpreter *interp) {
    int sym_cfa = (int)interp->vocab->dict[interp->ip++];
    push(interp, make_symbol((int)interp->vocab->dict[sym_cfa + 1]));
    DISPATCH(interp);
}
```

`docol` saves the post-operand `ip` as the return address (where `(exit)`
will pop back to) and jumps to the target's body. The body's final `(exit)`
pops that address, and the chain continues in the caller.

---

## Part 8: Why tail-call dispatch is fast

Two effects, beyond direct threading's one-read-per-op:

- **Per-op branch prediction.** Each `DISPATCH` site is a distinct
  indirect-jump instruction at a distinct address. The CPU's
  indirect-branch predictor builds a separate profile per site, so the jump
  out of a local-fetch op predicts its own typical successor, independent of
  the jump out of an arithmetic op. A single central dispatch site (one jump
  with dozens of possible targets) predicts far worse.
- **No prologue/epilogue between ops.** With `musttail` the chain is a
  sequence of unconditional jumps through handler bodies — no register
  save/restore per op, and hot interpreter state can stay in registers
  across handlers.

The cost is a per-op branch on the unwinding/error flags (a predicted,
almost-always-not-taken test) and a compiler requirement: `musttail` needs a
recent clang or gcc.

A computed-goto interpreter (`goto *dict[ip++]` at the end of each handler)
gets the same per-site prediction benefit, but requires every handler to
live as a label inside one giant function. Tail-call dispatch keeps each
handler as its own C function — spread across `core.c`, `words.c`,
`collections.c`, `matrix.c`, `functional.c` — while still ending each in its
own indirect jump.

---

## Part 9: Calling a word from C — the trampoline

`run_inner` drives the in-loop chain. When C code needs to invoke a word
directly — to run a quotation, say — it calls `execute_cfa`. Variables and
symbols are handled inline, since they only push a value:

```c
void execute_cfa(Interpreter *interp, int cfa) {
    cfa_handler handler = (cfa_handler)interp->vocab->dict[cfa];
    if (handler == dovar) { push_variable(interp, cfa); return; }
    if (handler == dosym) { push_symbol(interp, cfa);   return; }
    /* docol and primitives go through the trampoline below */
}
```

A `docol`-handled word can't just be called directly: `docol` sets `ip` and
returns, but outside `run_inner` nothing would dispatch the body. And a
primitive can't be called bare either, because its closing `DISPATCH` would
tail-jump to whatever cell `ip` happens to point at. Both need a defined
"next cell" that stops the chain.

The mechanism is a tiny *trampoline*: a fixed run of cells reserved at the
bottom of the dictionary (`TRAMPOLINE_SLOT` is 0; `DICT_RESERVED` cells are
held back from word storage). `execute_cfa` writes the call into those cells,
points `ip` at them, runs `run_inner` once, and restores the saved state:

```c
cell stop_handler = interp->vocab->dict[interp->vocab->stop_cfa];
if (handler == docol) {
    dict[TRAMPOLINE_SLOT]     = (cell)docol;          // dispatch docol
    dict[TRAMPOLINE_SLOT + 1] = (cell)cfa;            //   with this target
    dict[TRAMPOLINE_SLOT + 2] = stop_handler;         // exit returns here → stop
} else {
    dict[TRAMPOLINE_SLOT]     = (cell)handler;        // dispatch the primitive
    dict[TRAMPOLINE_SLOT + 1] = stop_handler;         // its DISPATCH lands here → stop
    dict[TRAMPOLINE_SLOT + 2] = stop_handler;
}
interp->ip = TRAMPOLINE_SLOT;
interp->running = 1;
run_inner(interp);
```

`(stop)` sets `running = 0`, so the loop in `run_inner` exits and control
returns to `execute_cfa`, which restores the previous `ip`, `running`, and
trampoline cells. The reserved region is three cells wide because the docol
case needs handler, target, and stop.

For tight loops that call a word once per iteration (the combinators in
`functional.c`), `call_open` / `call_invoke` / `call_close` split this up so
the trampoline is set up once and only `run_inner` is paid per iteration.

---

## Part 10: The GC's view of a body

The garbage collector has to find every `Val` reachable from compiled code —
literals and variable values embedded in word bodies — without misreading a
dispatch cell or an operand as something it isn't. `mark_body` walks each
body with `op_cell_count`, marks the operands of `(literal)` and `(dostr)`
ops, and skips everything else. The full treatment is in gc.md; the point
here is that the body layout of Part 6 is exactly what makes that walk
possible, and `op_cell_count` is the shared definition of each op's width.

---

## Part 11: The image format

`save-image` and `load-image` snapshot and restore the full interpreter
state — dictionary, objects, stacks — as a binary file. Most of the
dictionary is position-independent and saved verbatim: header cells are
indices and pool offsets, operand cells are CFA indices, branch offsets,
slot numbers, and packed `Val`s. None of those move between runs.

Handler pointers are the exception. A dispatch cell holds a raw C function
address, and address-space layout randomization places the code segment at a
different base every launch. Saved verbatim and reloaded in another process,
those addresses would be garbage. So the image translates them.

### The handler registry

Every handler that can appear in a body is a known, finite quantity. Each is
created through `define_primitive` (which appends it to a `handler_registry`
array), plus `docol`, `dovar`, and `dosym`, which are registered explicitly.
A handler's *id* is its index in that array — stable across runs because the
bootstrap that builds the registry is deterministic. `handler_to_id` maps a
pointer to its id (a scan, used only while saving); the reverse is an array
index.

### Translating bodies

On save, each colon word's body is walked with `op_cell_count`: every
dispatch cell is written as its handler id, and operand cells are written
verbatim. On load, the walk runs again and each id is mapped back to the
current process's handler pointer. A word's CFA cell (`docol`/`dovar`/
`dosym`) is carried in a small separate table and resolved the same way;
variable and symbol bodies are data and are copied through untouched.

One op needs care: `docol` is variable-width in a body. At a call site it's
two cells, `[&docol][target_cfa]`. But the inliner (`inline_word_body`, which
splices a word's body into another to avoid a call) emits a bare one-cell
`docol` to mark the entry of an inlined nested block, with no operand. The
two are told apart by the cell that follows. At a call site it's the
target's CFA — a dictionary index larger than any handler id, since words
are laid down only after the handler registry is built. At an inlined block
it's the block's first op. So the translator peeks: on save, whether that
cell is a registered handler pointer; on load, whether its value is below
the number of registered handlers. Either test sizes the `docol` as one
cell or two.

### What an image contains, and the bootstrap match

The dictionary is rebuilt from scratch every launch: `define_primitive` lays
down the primitives, then lib.l4 is compiled. The `init_*` watermarks are
captured *after* that, so the base — primitives and lib.l4 — is never saved.
An image carries only the words defined afterward, and their references into
the base resolve correctly because the base is rebuilt identically. `load`
checks the saved watermarks against the running interpreter's and refuses an
image whose base doesn't match (a different build), and the format carries a
version number so incompatible images are rejected rather than misread.

---

## Part 12: Where to look in the source

- **`DISPATCH(interp)`** in `logicforth.h` — the tail-call dispatcher every
  non-halting handler ends with: the unwinding/error check, then a `musttail`
  jump to the next handler.
- **`run_inner`** in `core.c` — entry point and cleanup point for the chain;
  the hot path runs its body once and the tail-call chain takes over.
- **`docol` / `dovar` / `dosym`** in `core.c` — read their target CFA from the
  operand cell, act, then `DISPATCH`.
- **`execute_cfa`**, **`call_open` / `call_invoke` / `call_close` /
  `call_step`** in `core.c` — invoking a word from C through the
  `TRAMPOLINE_SLOT` reserved cells; the split form amortizes setup across a
  combinator's loop.
- **`define_primitive`** in `core.c` — defines a primitive and registers its
  handler for image translation.
- **`emit_call`**, **`emit_val_literal`**, **`p_literal`** in `core.c` — the
  compile-time emission of dispatch cells and inline operands.
- **`op_cell_count`** in `core.c` — the per-op width used by the GC walker,
  the inliner, and the image translator.
- **`mark_body`** in `core.c` — the GC's body walk (see gc.md).
- **`p_save_image` / `p_load_image`** in `core.c` — the image format and the
  handler-id translation.
