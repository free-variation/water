# Threaded code in Water

This is a primer on how Water's inner dispatch loop is organized, and why.
By the end you should understand:

- What "threaded code" means — and that it has nothing to do with concurrency —
  and where it sits between an AST walker and a JIT
- How a compiled word is laid out and dispatched through
- How *direct threading* and *tail-call dispatch* combine to make each virtual
  operation cost about one memory read plus one jump
- What that organization implies for the compiler, the GC's walk over compiled
  bodies, the trampoline that calls words from C, and the image format

It's a conceptual tour. The dispatch machinery lives in `src/c/core.c`; this
document explains the ideas, not their field names.

---

## Part 1: The inner loop

Take a small Forth-style program: `1 2 3 4 + + +`. It pushes four numbers and
adds three times. Inside the interpreter that's seven virtual operations — push,
push, push, push, add, add, add — and to run the program the interpreter executes
them in order. For each one it has to get from "the next operation in the
compiled stream" to "the first machine instruction of the C code that implements
it."

That transition is this document's whole subject. The handler's own work —
popping operands, computing, pushing a result — is unavoidable. What's avoidable
is the per-operation overhead of *reaching* the handler, and in a program of any
size that cost is paid millions or billions of times, so a load saved per
operation, or a branch the CPU predicts instead of mispredicts, shows up directly
in wall-clock time.

---

## Part 2: What "threaded" means here

The "thread" in *threaded code* has nothing to do with concurrency. It's textile
imagery — a thread connecting beads on a string — for a program whose executable
form is a sequence of small cells the interpreter chases one after another. The
usage comes from early Forth, where memory was tiny: a program encoded as a list
of "addresses of routines" was both compact (one cell per op) and fast to
dispatch (no parsing, no integer-keyed lookup).

The spectrum of ways to run a program:

- **AST walker.** Walk a tree of parsed nodes, dispatching on each node's type.
  Simple, but every step chases pointers between scattered heap nodes and every
  type test is a branch.
- **Bytecode interpreter.** Compile to a flat array of integer opcodes; a loop
  reads the next opcode and switches on it. Cache-friendly, but every dispatch
  pays an opcode→handler lookup.
- **Threaded code.** Each op in the compiled stream is *itself* the dispatch
  target — the opcode→handler lookup is gone. This is what Water uses.
- **JIT.** Emit machine code and let the CPU run it directly. Fastest, but
  requires native codegen, executable memory, and patching.

Threaded code sits between bytecode and JIT: some of JIT's speed (no integer
dispatch) without its complexity (no codegen). It's also the natural fit for a
Forth-family language — the dictionary itself becomes the executable, with no
separate opcode space on the side. Within threaded code, two flavors differ in
how each op points at its handler: *indirect* and *direct*.

---

## Part 3: A useful anchor — subroutine threading

Consider the extreme: don't interpret at all, and let the compiled program *be*
native code. That's *subroutine threading* — each op becomes a literal CPU `CALL`
to its handler, and the CPU's own program counter walks from CALL to CALL with no
dispatch loop. It's fast, but it means generating native code (per-architecture,
per-OS, unportable), it isn't relocatable (CALL targets are addresses, so a saved
image breaks when code moves), and you can't hook dispatch for profiling or
tracing without injecting trampolines. Interpreted threading keeps dispatch in
software, in one place, for a few nanoseconds per op; Water's tail-call
dispatch (Part 7) recovers much of subroutine threading's speed while keeping the
op stream as inspectable, relocatable data.

---

## Part 4: Indirect versus direct threading

In *indirect*-threaded code a compiled body holds dispatch *tokens*. A token
doesn't name a handler — it names a code field, a cell that holds the handler — so
dispatch is two reads: token → code field → handler → call. The extra read buys
two things: every word kind looks identical at the call site (the handler at the
code field disambiguates a colon definition from a variable from a symbol), and
tokens are small stable indices that survive being written to disk.

In *direct*-threaded code a body holds the handler addresses themselves — one read
instead of two. Water uses direct threading, and pays for the saved read in
two places, each handled elsewhere in this document:

- Body cells now hold raw function pointers, which aren't stable across launches
  (address-space randomization moves them). The image format translates them on
  save and load (Part 10).
- A handler can no longer recover *which* word it's acting on from a token,
  because there is no token — so a call site that needs to name a target carries
  it as an extra operand cell (Part 6).

---

## Part 5: A word's anatomy

The dictionary is one large array of cells (64-bit each). It's a fixed-size inline
buffer — bump-allocated into and bounds-checked, never reallocated — so a cell's
position is its permanent identity.

Each word occupies a run of cells: a four-cell header, then the *code field* (the
CFA), then the body.

```
position:   N-4  N-3  N-2  N-1   N+0     N+1    N+2   ...
          ┌────┬─────┬────┬────┬───────┬──────┬──────┐
contents: │LINK│FLAGS│NAME│SRC │handler│ body │ body │ ...
          └────┴─────┴────┴────┴───────┴──────┴──────┘
          ←──────── header ───────→  ↑   ←──── body ──→
                                    CFA = N
```

The header sits at negative offsets from the CFA: a LINK to the previous word
(so the whole dictionary is a list walkable from its head), FLAGS (immediate /
inline / internal bits), and offsets into the name and source pools. The CFA cell
holds the word's *handler* — a C function pointer — and the body follows. The CFA
is the word's identity: it's what a lookup returns, what tick (`'`) captures into
an execution token, and what compiled bodies reference.

The CFA holds one of four handler kinds:

- **A primitive** — a C function like the one behind `+` or `dup`. The body is
  empty; the handler does the work, pulling operands from the data stack.
- **The colon handler** — for `: … ;` words. The body is a stream of compiled
  ops, and the handler begins walking it.
- **The variable handler** — the body is one cell, the variable's value as a
  packed `Val`.
- **The symbol handler** — the body is one cell, the symbol-pool offset of the
  interned name.

Defining a primitive is the simplest case: create the header and store the
handler in the code field, with nothing after it.

---

## Part 6: Body layout

A `Val` is one cell (it's NaN-boxed; see `nan-boxing.md`), which keeps the body
simple: every cell is either a *dispatch cell* (a handler pointer) or an *operand
cell* (a plain value the preceding handler consumes).

A reference to a primitive compiles to one dispatch cell — the dispatcher reads it
and calls it. A reference to a colon definition, a variable, or a symbol needs one
more thing: the handler has to know *which* target it's acting on, and with no
token to carry that, the call site stores the target's CFA in an operand cell
right after the handler. So a colon call, a variable read, and a symbol push are
each a handler cell followed by a target-CFA cell, while a primitive call is a
lone handler cell.

Several primitives carry inline operands of their own. The prototype is the
literal: its compiled form is the literal handler followed by the packed `Val`,
and the handler reads that operand from the instruction stream and advances past
it. The same shape — read operands from the stream, advancing — is shared by every
operand-bearing op: string literals (a string handle), the branches (a jump
offset), the word-local-variable ops (depth and slot numbers), and the fused
superwords.

That raises a requirement: anything that walks a body *without executing it* — the
GC's body scan, the inliner, the image translator — must know how many cells each
op occupies, so it lands on the next dispatch cell instead of misreading an
operand as a handler. A single function is the source of truth for that
per-op width; it keys off the handler value and accounts for every operand-bearing
op (including the one variable-width op, whose width depends on its operand). It is
the first thing to update when a new operand-carrying op is added — get it wrong
and a body walk either runs off the end or reads an operand as code.

---

## Part 7: The dispatch chain

Because body cells hold handler pointers directly, "dispatch" is: read the cell,
call it. Water does that with no central loop in the hot path — each handler
ends by **tail-calling the next**. A small macro (`DISPATCH`) is the shared tail:
it reads the handler at the current instruction pointer, advances the pointer, and
jumps to that handler with a forced tail call (`musttail`), so the next handler
reuses the current stack frame rather than nesting. A run of compiled ops
therefore executes as a chain of jumps through handler bodies at constant stack
depth, however many ops run.

The dictionary is a single process-global, so the dispatch read is one indexed
load off a known base — no `interpreter → dictionary` pointer chase. That matters
because the load happens on every dispatch.

### The register convention

Every handler receives three arguments: the interpreter, a pointer to its first
operand cell, and a pointer one past the top of the data stack. Because each
handler tail-calls the next with updated values, those two pointers live in
argument registers for the whole chain — the instruction pointer and stack
pointer are never loaded from memory between converted ops. The `Interpreter`
fields (`ip`, `dsp`) still exist, but as the *spilled* copy of the truth: every
dispatch stores the current values back (fire-and-forget stores, off the
critical path), so any C code the chain drops into — `fail`, the collector, a
handler that still works through the struct — sees coherent state without being
handed the registers.

That asymmetry — registers authoritative inside the chain, memory refreshed at
every hop and read only at boundaries — is what the dispatch macros divide
between them. `DISPATCH_REGISTERS` is the converted handler's tail: store-sync,
check the break flags, fetch the next handler, tail-call with advanced pointers.
`DISPATCH` is the unconverted tail: reload the pointers from the struct (such a
handler has been updating the struct directly) and chain the same way, so
converted and unconverted ops interleave freely in one body. `SYNC_REGISTERS`
is the escape hatch a converted handler uses before calling C code that can
fail or inspect the stack; the stack-guard macros (`REQUIRE_STACK_ROOM`,
`REQUIRE_STACK_DEPTH`) bundle their bounds compare with that sync on the
failure path.

The chain has to break cleanly when something interrupts ordinary flow. Before
jumping, the dispatch tail checks a few interpreter flags and *returns* instead of
chaining if any is set:

- **an error** — a fault has set the error flag;
- **unwinding** — `shift`/`shift-with` or a backtrack is unwinding the return
  stack (see `continuations.md`);
- **a pending collection** — a byte-pressure GC has been requested and deferred to
  a safepoint (see `gc.md`).

Returning hands control back to `run_inner`, the loop that started the chain.
`run_inner` is the entry and cleanup point: in steady state its body runs *once* —
it reads the first handler and calls it, and the tail-call chain takes over until
something returns. When the chain breaks, `run_inner` is where the consequence is
handled: it services a pending collection at this between-words safepoint (the
deferral is what keeps a collection from freeing operands a primitive has popped
into C locals but not yet rooted), advances an in-progress unwind toward its
target, or exits on a halt or error.

The colon, variable, and symbol handlers each read their target CFA from the
operand cell and act: the colon handler saves a return address and jumps to the
target's body (its closing exit pops back); the variable and symbol handlers push
the stored value. Each then dispatches on.

---

## Part 8: Why tail-call dispatch is fast

Beyond direct threading's one-read-per-op, two effects:

- **Per-op branch prediction.** Each dispatch site is a distinct indirect jump at
  a distinct address, so the CPU's indirect-branch predictor keeps a separate
  profile per site: the jump out of a local-fetch op predicts its own typical
  successor, independent of the jump out of an arithmetic op. A single central
  dispatch site — one jump with dozens of targets — predicts far worse.
- **No prologue/epilogue between ops.** With forced tail calls the chain is a
  sequence of unconditional jumps through handler bodies — no per-op register
  save/restore.
- **State in registers by construction.** The instruction and stack pointers
  ride in argument registers through the chain (Part 7's register convention);
  the struct sees only refresh stores. The memory round trips that would
  otherwise sit on every op's critical path — load `ip`, load `dsp`, store both
  back, and the store-to-load stall between one op's write and the next op's
  read — are gone.

The cost is a per-op test of the break flags (predicted, almost always not taken)
and a compiler that supports guaranteed tail calls. A computed-goto interpreter
gets the same per-site prediction, but requires every handler to be a label inside
one giant function; tail-call dispatch keeps each handler as its own C function,
spread across several source files, while still ending in its own indirect jump.

---

## Part 9: Quickening — dispatch cells as inline caches

A dispatch cell is writable data, and the register convention hands every
handler the address of its own cell (one cell before its operands). Water uses
that for *quickening*: a polymorphic op that observes its operand's type
rewrites its own call site to a type-specialized handler, so the next execution
of that site skips the type ladder. The specialized handler re-checks the tags
it depends on — a *guard* — and on a mismatch rewrites the cell back to the
generic handler and tail-calls it, so the current occurrence still executes
correctly and the site re-specializes to whatever it now sees. Every value the
cell can ever hold is a valid handler, so no ordering of rewrites produces a
wrong program — only a slower one, if a site's types genuinely alternate.

The rewrites are fenced out of parallel regions (`RETARGET_OP` declines to
write while workers run): the dictionary is shared across worker threads,
racing stores on it are undefined behavior, and quickening is only a cache —
skipping a write costs a missed specialization, never correctness. Specialized
handlers are registered like any primitive, so the image format (Part 11) saves
a quickened cell as itself and `see-compiled` names it; each carries the same
operand-cell count as its generic form, so body walks (Part 6) are unaffected.

---

## Part 10: Calling a word from C — the trampoline

`run_inner` drives the in-loop chain. When C code needs to invoke a word directly
— to run a quotation — it goes through a small trampoline. Variables and symbols
are handled inline (they only push a value), but a colon-defined word can't just
be called: its handler sets the instruction pointer and returns, and outside
`run_inner` nothing would dispatch the body. A primitive can't be called bare
either, because its closing dispatch would tail-jump to whatever cell the pointer
happens to land on. Both need a defined "next cell" that stops the chain.

The trampoline is a few cells reserved at the bottom of the dictionary. The C
caller writes a call into them — the target's handler, its operand if it needs
one, and then a *stop* op — points the instruction pointer at them, and runs the
chain once; the stop op halts the loop and control returns to the caller, which
restores the saved state. Each interpreter owns its own disjoint trampoline cells
(named by a per-interpreter base, with enough reserved for the coordinator and
every worker), so interpreters running concurrently never write each other's — the
worker model is `multicore.md`'s subject. For tight loops that call a word once per
iteration (the combinators), the setup is split out so the trampoline is written
once and only the chain is paid per iteration — and the per-iteration entry
skips `run_inner`'s loop entirely, dispatching the body's first handler directly
and falling back to `run_inner` only when the chain breaks without finishing (a
GC safepoint or an unwind mid-body).

---

## Part 11: The GC's view, and the image format

**The GC** has to find every `Val` reachable from compiled code — literals and
variable values embedded in word bodies — without misreading a dispatch cell or an
operand. It walks each body using the same per-op width function as Part 6, marks
the operands of the literal and string-literal ops, and skips the rest. The body
layout is exactly what makes that walk possible; `gc.md` has the full treatment.

**The image format** (`save-image` / `load-image`) snapshots and restores the full
interpreter state as a binary file. Most of the dictionary is position-independent
and saved verbatim — header cells, CFA-index operands, branch offsets, slot
numbers, packed `Val`s; none of those move between runs. The exception is dispatch
cells, which hold raw function addresses that randomization places differently
each launch. So the image translates them: every handler that can appear in a body
is registered at bootstrap and assigned a stable id (its index in the registry,
stable because the bootstrap is deterministic); on save each dispatch cell is
written as its handler id, on load each id is mapped back to this process's
pointer. One op needs care — the colon handler is two cells at a call site (handler
+ target CFA) but a bare one cell where the inliner uses it to mark an inlined
block — and the two are told apart by whether the following cell is a registered
handler or a dictionary index. Finally, the base dictionary (primitives + the
standard library) is rebuilt identically every launch and never saved; an image
carries only the words defined afterward, and load refuses an image whose recorded
base Watermarks or format version don't match the running build.

---

For broader context: `nan-boxing.md` is the one-cell `Val` the body layout relies
on; `gc.md` is the collector whose safepoint breaks the dispatch chain and whose
body walk this layout enables; `multicore.md` is the per-interpreter trampoline and
the worker model.
