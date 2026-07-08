# A template JIT for Water — design notes

This document records the design for native code generation in Water, worked
out before any of it is built. It exists because the constraints are easy to
re-derive wrongly: the obvious ways to make a Forth fast — native calls, native
return addresses — quietly destroy the property that makes Water's
continuations, images, and GC simple. By the end you should understand:

- What is left to gain after the interpreter work, and why only codegen can
  reach it
- What a template JIT is, and why it fits Water's dictionary unusually well
- The one invariant the whole design bends around: control state is
  serializable data
- The shape that preserves it — compile bodies, not control — and what each
  continuation operation costs under it
- What the JIT consumes from the existing machinery (fusion, quickening, the
  register convention), and which shelved interpreter optimizations it
  subsumes
- The costs: executable memory, invalidation, the fallback path, and the
  things deliberately ruled out

It's a plan, not a description of code. Read `threading.md` first — the
dispatch model, the register convention, and quickening are all assumed here.

---

## Part 1: What's left on the table

The interpreter's constant factors have been spent. Dispatch is a register-
pinned musttail chain; the compiler fuses idiomatic runs into
superinstructions; polymorphic ops quicken to guarded monomorphic forms; the
combinator drivers enter the chain directly. A fused float loop runs around
six dispatched ops per iteration at roughly a nanosecond apiece.

What remains is the dispatch itself. Each op still pays an operand fetch, a
next-handler load, an indirect branch, and the break-flag test — and the
values flowing between ops still round-trip through the data stack in memory,
because no register allocation crosses an op boundary. Native code for the
same loop body would be a dozen instructions with the loop-carried values in
registers: roughly a 3–4× gap on dispatch-bound code, and nothing interpreter-
shaped closes it. The C-bound words — regex, SQLite, dgemm, JSON — gain
nothing from any of this; they are already native.

---

## Part 2: What a template JIT is

A template JIT keeps a pre-built fragment of machine code — a template — for
each VM op, and compiles a word by concatenating the fragments for its ops in
order, patching each one's operand cells into the copied bytes as immediates.
There is no IR and no optimizer. Two things make it fast anyway:

- **Dispatch disappears.** The next template is literally the next bytes in
  memory; the program counter falls through where the interpreter loaded a
  handler pointer and jumped. Branch ops emit real branches to the native
  offsets of their targets.
- **Operands become immediates.** Where a handler reads a slot number or a
  literal from the instruction stream, the template has a hole at a known
  offset, patched at emit time.

Templates can be written by hand per architecture, or harvested from C — the
*copy-and-patch* approach: compile each handler-like fragment at build time
with its operands declared as external symbols, so the compiler emits
relocations where they're used; emitting a template at JIT time is a memcpy
plus applying the relocations. Copy-and-patch keeps the templates in C (the
existing handlers are already nearly the right shape) and makes porting mostly
a recompile. The step that reaches beyond dispatch elimination is keeping the
top of stack in a register: each template is compiled in a few variants with
inputs and outputs pinned to different registers, and the emitter picks the
variant matching the current stack state. At that point a fused loop body
compiles to nearly what a C compiler would emit.

Water is an unusually good host for this. A JIT's front-end work — producing a
linear, resolved, operand-inline instruction stream — is exactly what the
compiler already writes into the dictionary. Superword fusion has already made
each op coarse enough to be worth a template; quickening has already made the
hot polymorphic sites monomorphic with explicit guards, which is precisely the
form a template wants.

---

## Part 3: The invariant — control state is data

Everything distinctive about Water's control story rests on one fact: a
continuation is *pure virtual state*. `shift` captures a slice of the virtual
return stack — `T_ADDR` cells holding dictionary indices, locals frames,
marks — plus a `resume_ip` that is a dictionary index. Nothing about the C
stack is ever captured. That is why resume is multi-shot (slices are values,
copied on use), why `save-image` can serialize live continuations (dictionary
indices are stable; machine addresses are not), why the GC can walk captured
frames with the same body-walk it uses everywhere, and why the unwinding
machinery is a loop over data rather than stack gymnastics.

Every design decision below is this invariant, applied. A JIT that moves
control into native form — real calls, return addresses on the machine
stack — breaks all four properties at once, and the systems that took that
path (Scheme compilers that copy C-stack segments to the heap) paid for it
with exactly the serialization and portability problems Water currently does
not have.

---

## Part 4: The shape — compile bodies, not control

JIT-compiled code is straight-line native *within* a word body. All
inter-word control keeps manipulating the same virtual structures the
interpreter does:

- **Calls stay virtual.** A call to another word pushes the same `T_ADDR` the
  interpreter would push — the dict index of the following cell — onto the
  virtual return stack. The native transfer either goes through a small
  dispatcher (trampolined; the C stack stays flat, today's discipline) or is a
  native call *paired with* the virtual push, where `exit` translates the
  popped virtual address to a native one. The trampolined form is the first
  cut; it concedes that calls don't get faster, which is acceptable because
  the dispatch-bound code the JIT targets is intra-word.
- **Branches within a body become native branches.** They carry no state.
- **The return stack, locals frames, marks, and unwinding are untouched.**
  Compiled code maintains them exactly as the handlers do.

The one new structure is a **side table per compiled word** mapping cell index
to native code offset. Templates produce it for free — each op's template
lands at a known offset as it is emitted. The table is what makes the
dict-index world and the native-code world interconvertible, and it is the
only thing resume optimization ever needs.

Each continuation operation then falls out:

- **Capture** is unchanged, byte for byte. Compiled code maintained the same
  virtual return stack, so a continuation captured under JIT is the same
  object as one captured under interpretation. (This equivalence is the
  correctness test: run the suite both ways, compare captured continuations.
  The `execute_xt` episode — where a missing one-frame difference in return-
  stack shape broke `catch` — is the cautionary precedent: the return-stack
  shape at every point must match the interpreter exactly.)
- **Resume** copies the slice back and enters at `resume_ip`. First cut:
  *always* re-enter through the interpreter — continuation-heavy code runs at
  today's speed, and control returns to compiled code at the next word call.
  Later, if generator-heavy profiles justify it, look `resume_ip` up in the
  owning word's side table and jump to the native offset.
- **Deopt is always possible**, because the virtual state is complete at
  every op boundary. Any situation the JIT declines to handle — an invalidated
  word, an op with no template, a resume into cold code — falls back to the
  interpreter at the current `ip`. This escape hatch is the design's load-
  bearing convenience: the JIT never has to be total.
- **`save-image` / `load-image` are unaffected.** Compiled code is a cache.
  Everything serialized is dictionary indices; native code is dropped at save
  and rebuilt lazily after load.
- **Exceptions, `amb`, generators** are built on capture and inherit the
  solution.

Register caching inside a body is fenced by the same logic: locals live in
return-stack slots so that capture sees them, so a body may cache a local in a
register only between capture points — and since any call might transitively
reach `shift`, every call site spills whatever is register-cached. Template
JITs track per-op stack state at emit time anyway; this is bookkeeping, not
architecture.

---

## Part 5: What the JIT consumes, and what it subsumes

The existing machinery lines up as the JIT's front end:

- **Superword fusion** (`superwords.md`) already collapsed the idiomatic runs;
  each fused op maps to one template with its operand cells as patch holes.
  The compare-and-branch fusions map one-to-one onto native compare-and-branch.
- **Quickening** (`threading.md`, Part 9) already rewrote hot polymorphic
  sites to guarded monomorphic ops. A quickened cell compiles to its fast
  body with the guard as a cheap native test that deopts on failure —
  the interpreter's guard-and-retarget becomes the JIT's guard-and-exit.
- **The register convention** (`threading.md`, Part 7) already established
  the spilled-truth discipline — registers authoritative in the hot path, the
  `Interpreter` struct synced at boundaries. The JIT keeps the same contract:
  compiled code syncs `ip`/`dsp` at every point where it can leave native
  code, which is exactly the set of places the interpreter's converted
  handlers sync today.

Three interpreter optimizations were considered and deliberately shelved
because the JIT gets them as side effects; do not build them by hand first:

- **TOS in a register** — the template variants with pinned input/output
  registers *are* this, generalized.
- **The loop-back patch** for `times`/`map`-style drivers — a compiled loop
  body keeps iteration in native code by construction.
- **Guard pages / bounds-check elimination** — a compiled body's stack effect
  is known at emit time; one entry check replaces per-op checks, and loop
  bounds hoist where the emitter can see the trip count.

---

## Part 6: Costs and requirements

- **Executable memory.** `mmap`/`mprotect` with W^X discipline; on macOS,
  `MAP_JIT` plus `pthread_jit_write_protect_np` toggling around emission.
- **Templates per architecture**, or the copy-and-patch build step (compile
  fragments, harvest bytes and relocations into tables). arm64 first; the
  interpreter remains the only implementation everywhere else.
- **Invalidation.** `forget` and redefinition drop compiled code for the
  affected words (the dictionary truncation already defines the boundary);
  quickening's retargeting must either invalidate or be treated as a
  different generic-op template that re-reads its cell.
- **The interpreter stays, permanently.** It is the WASM implementation, the
  cold-code path, the deopt target, and the semantics of record. Every word
  must run correctly with the JIT absent or declining.
- **Parallelism.** Workers share the dictionary read-only during a region;
  compiled code must be emitted outside parallel regions (the same fence
  quickening uses), and per-worker execution of shared compiled code is
  read-only and safe.

Ruled out, permanently, by Part 3:

- **Subroutine threading with native calls** — return addresses on the C
  stack break capture, images, and the GC walk.
- **Native-stack copying for continuations** — works in one process image,
  breaks `save-image` and multi-shot semantics; it trades the invariant for
  speed exactly where Water's design says not to.
- **A tracing JIT** — trace exits and guard side-states put control state in
  native form implicitly; the complexity budget goes to the same place the
  template JIT spends it, with less predictable behavior.

---

## Part 7: What to expect

Where it pays: dispatch-bound code — the fused float loops, the array/segment
index loops, locals-heavy solver code. The measured interpreter runs those at
roughly 3–6 ns per op; templates with register-pinned stack state should land
within striking distance of native, the remaining 3–4×.

Where it doesn't: anything C-bound (regex, JSON, SQLite, dgemm, set algebra),
which is most of the benchmark suite's absolute time; call-heavy recursive
code, until calls move past the trampolined first cut; and logic-engine
workloads, whose costs are clause selection and heap-allocated variables —
execution-model questions (`first-argument indexing`), not codegen ones.

The staging that follows from all this: entry points and executable-memory
plumbing first, with every word still interpreted; then templates for the
fused-loop op families and a compile-on-Nth-execution trigger for hot words;
measure against the interpreter on the bench suite at each step, with the
suite run both JIT-on and JIT-off as the correctness gate. The design's test
for done-ness is not "everything compiles" — it is "nothing observable
changes except time."
