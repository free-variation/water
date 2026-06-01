# Delimited Continuations in logicforth

This document is a primer on delimited continuations and how logicforth implements them. By the end you should understand:

- What a continuation is and why it matters
- What "delimited" means and why it's the practical choice
- How four primitives (`reset`, `shift`, `shift-with`, `resume`) work mechanically
- How exceptions, coroutines, generators, restarts, backtracking, green threads, async I/O, and cooperative schedulers all fall out of those four primitives as small library words

The implementation lives in a single C file. The library words are in `src/forth/lib.l4`. Tests demonstrating each pattern are in `tests/45_continuations.l4`, `tests/47_exceptions.l4`, and `tests/48_interactions.l4` — once you understand the model, those are worth reading alongside this document.

The document moves in roughly three arcs. Parts 1–4 motivate continuations and lay out the runtime substrate. Parts 5–9 cover the four primitives and the unwinding mechanism that makes exception-style flow work. Parts 10–17 build the major patterns (exceptions, coroutines, generators, restarts, backtracking, green threads, async I/O) on top of those primitives, with full traces and stack diagrams. Parts 18–20 collect reference material: a primitive table, the C-side surface area, and pointers into the source.

---

## Part 1: What is a continuation?

Take a function in some language you're familiar with:

```python
def main():
    x = 10
    y = compute_something(x)
    print(y + 1)
```

When `compute_something(x)` is running, "the rest of `main`" — assigning the result to `y`, computing `y + 1`, calling `print` — hasn't happened yet. That pending work is the *continuation* at the point where `compute_something` returns.

In most languages this continuation is implicit. It's whatever happens to be on the call stack. You can't grab it, store it, or invoke it later. You just trust that when the function returns, the right thing happens.

A *first-class* continuation is one you can grab as a value, store in a variable, pass to other functions, and invoke whenever you want. Invoking it means: "jump back to the point where I captured it, but with this value, and continue from there."

If `compute_something` could grab the current continuation and stash it in a global, the program could continue past the assignment to `y` later, possibly more than once, possibly with different values. That's strange and powerful.

### A useful mental model: the stack as a list

Imagine the call stack as a linked list of "what to do next." When you're inside `compute_something`, the head of the list says "return to main, assign the result to `y`." The next node says "compute `y + 1`, pass to `print`." The next says "return from main to whatever called it." And so on out to the bottom.

A continuation, in this picture, is just *a pointer to one of those list nodes*. Capturing the continuation is grabbing the pointer. Invoking it is jumping to that node and walking from there.

If continuations are first-class, you can:

- Grab the pointer, store it
- Walk the list from that point (invoke the continuation)
- Walk it again from the same starting point (invoke the same continuation a second time)
- Hand the pointer to a different part of the program (call from anywhere)

Most languages don't expose this list. The hardware call stack is a hidden runtime detail. But the data structure is there. Continuations are what you get when you make it manipulable.

### Why this is foundational

First-class continuations are *the* foundational control construct. Once you have them, you can build:

- Exceptions and exception handlers
- Generators (`yield` / iterator protocols)
- Coroutines and cooperative threads
- Green threads (M userspace threads on N OS threads)
- Async I/O and event-driven concurrency
- Backtracking search and constraint solving
- The Common Lisp condition / restart system
- Web continuations (HTTP requests as suspended computations)

Languages that have first-class continuations (Scheme, Racket, Standard ML with extensions, Ruby) typically build the rest of the control story on top of them. Languages that don't have them (Python, JavaScript, Go) implement each of the above as a separate language feature — Python has `try/except`, `yield`, `async/await`, and `asyncio.Task` as four largely-independent mechanisms. With continuations, they're four instances of the same mechanism.

This is the central pedagogical claim of this document: a single primitive — captureable, resumable pieces of computation — subsumes a sprawl of seemingly-unrelated language features. The implementation is small. The conceptual unification is what makes the investment worthwhile.

---

## Part 2: Why "delimited"?

Scheme's `call/cc` (call-with-current-continuation) captures *everything* — the entire pending work from the current point all the way out to the program's top level. The captured continuation, when invoked, effectively restarts the program from that point.

That's powerful but unwieldy. The captured object is huge (the whole rest of the program). Invoking it has surprising consequences: what if you stored a captured continuation in a list, then invoked it later? Now your "stored value" rewinds the world.

Worse, undelimited continuations don't *compose*. A continuation in the `call/cc` sense isn't a function that "returns a value"; it's a takeover. You can't write `let result = k(arg)` because there's no "after" to invoking `k`. Once you jump into `k`, everything in scope at the call site is abandoned — the local variables, the caller, the entire C stack above. Trying to use a captured `call/cc`-continuation as a function call is like trying to use `longjmp` as a function call.

A *delimited* continuation captures only what's between two markers:

- The **outer marker** is a boundary you've installed deliberately. In logicforth this is what `reset` does.
- The **inner marker** is wherever you choose to capture. In logicforth this is wherever `shift` is called.

The captured continuation represents the slice of work between those two markers — typically much smaller than "all of the rest of the program," and much more composable.

Crucially, a delimited continuation *is* a function. It has a beginning (the capture point), an end (the outer marker), and a return value (whatever sits on the data stack when the slice finishes). You can call it from anywhere, store its result, call it again with a different argument. It behaves like ordinary code.

### A diagram

Imagine execution as a vertical timeline:

```
top-level
   |
   +-- enter word A
   |       |
   |       +-- enter word B
   |       |       |
   |       |       +-- [reset]        <-- outer marker
   |       |       |       |
   |       |       |       +-- enter word C
   |       |       |       |       |
   |       |       |       |       +-- [shift]   <-- capture here
   |       |       |       |               |
   |       |       |       |               v
   |       |       |       |          (slice = everything
   |       |       |       |           inside the box)
   |       |       |       :
   |       |       |       :
   |       |       +-------:
   |       |
   |       +-------:
   |
```

The captured slice is the box: the work that *would have happened* between the shift and the reset if shift hadn't intervened. The outer code (top-level, A, B above the reset) is untouched and continues running.

Most modern languages with continuation support use the delimited flavor: Racket's `prompt`/`control`, Scala's `shift`/`reset`, OCaml 5's effect handlers (which are a slight generalization). The full-capture `call/cc` is mostly considered a historical mistake.

logicforth uses delimited continuations.

---

## Part 3: Three stacks

To understand the implementation, you need to know about three stacks the interpreter maintains. Each is a fixed-size array of `Val`s (the interpreter's tagged-value type) with an integer stack pointer.

### The data stack

This is where computation happens. `1 2 +` pushes 1, then pushes 2, then `+` pops both and pushes their sum (3). Almost every primitive in the language reads its arguments from the top of the data stack and writes its results back to the top. The data stack is what user code thinks of as "the stack."

### The return stack

This is where the call mechanism remembers where to come back to. When a colon-defined word like

```forth
: square dup * ;
```

is called from somewhere else, the inner interpreter records the *current* instruction pointer on the return stack, then sets the instruction pointer to `square`'s body. When `square`'s body reaches its EXIT, the saved instruction pointer is popped and execution continues where it left off.

The return stack therefore mirrors the call/return structure of execution. It's analogous to what a CPU's hardware call stack does, but managed explicitly in software by the interpreter. This explicit management is what makes continuation capture possible: the runtime owns the call chain as a data structure, and can therefore snapshot, splice, and replace pieces of it.

Users can also push values onto the return stack with `>r` and pop them back with `r>`. This is sometimes useful for temporary storage that doesn't interfere with the data stack. The user must keep `>r`/`r>` balanced within a word, or EXIT will pop garbage.

### The side stack

A third stack, used purely for storage. logicforth added this specifically to support some of the library code on top of continuations.

Why a third stack? The data stack is the worst place to stash temporary values: if a word leaves results on top, your stashed value either gets in the way or has to be manipulated around the results. The return stack is also a bad place: anything pushed there other than a saved instruction pointer (or a special MARK we'll meet later) gets misinterpreted when EXIT or the unwinding logic pops it.

The side stack is just storage. Nothing in the execution machinery looks at it. The garbage collector marks its contents as roots (so objects you stash there don't get freed), but nothing else touches it.

Four primitives access it:

```
>side      ( v -- )         pop data, push onto side
side>      ( -- v )         pop side, push onto data
side-drop  ( -- )           pop side, discard
side-depth ( -- n )         current side-stack depth
```

We'll see why this third stack matters when we get to the exception library.

### Why three stacks, not one

Most languages mix all three on a single hardware stack: arguments, return addresses, and local variables interleave in stack frames. This works fine until you need to capture a slice of computation independently of its data — at which point you have to invent activation-record formats, frame pointers, and stack walking just to know what to copy.

Forth's separation is older than most modern languages and was originally motivated by simplicity on small machines. It turns out to be exactly the right structure for continuations: the call chain (return stack) is *separate* from the in-flight data (data stack). Capturing a continuation means snapshotting a slice of the return stack. The data stack tags along — at capture time, you note what's there; at resume time, you restore it. No frame layout to puzzle over.

The side stack is a small additional piece for situations where library code needs scratch storage that survives unwinding. It would be possible to encode this on the data stack with discipline, but the discipline ends up brittle (every word needs to know how many items below the top are "spare").

---

## Part 4: The inner interpreter

Forth's execution model is built around an *inner interpreter* — a small loop that dispatches one word at a time. Here's the basic structure:

```c
while (running && !error_flag) {
    int cfa_index       = (int)*ip++;
    cfa_handler handler = (cfa_handler)dict[cfa_index];
    handler(&dict[cfa_index]);
}
```

`ip` is the instruction pointer. `dict[]` is the dictionary — a flat array where all compiled code lives. Each word has a code-field-address (CFA), an index into `dict[]` at which its handler function pointer is stored. The next cells after that are the word's body.

The loop:
1. Reads the next cell at `ip` (it's a CFA index), and advances `ip`.
2. Looks up that CFA's handler function in `dict[]`.
3. Calls the handler.

For a primitive (like `+`), the handler does the work directly and returns. For a colon-defined word, the handler is a small function called `docol` that saves the current `ip` onto the return stack and points `ip` at the word's body. The next iteration of the loop dispatches the first cell of the body, and so on.

When the body's EXIT runs, it pops the saved `ip` from the return stack and restores it, so the next iteration continues where we were before the call.

The return stack therefore directly reflects the call chain. If word A calls word B which calls word C, the return stack has three frames stacked up, each pointing to where execution should resume in the caller. EXIT cascading unwinds these frames one at a time.

### Nested run_inner invocations

There's one more subtlety. When a primitive like `execute` or `resume` needs to run a colon-defined word *during its own execution*, it doesn't just modify `ip` and return — it calls `run_inner` again, recursively, from the C side. So the C call stack may have several `run_inner` frames active at once, each running its own inner loop.

This nesting matters for continuations. When `shift` fires, it might need to signal "unwind this whole region" across several `run_inner` levels, all the way back to wherever the matching `reset` was executed. We'll see in Part 9 how this works.

### The substrate

The continuation machinery we're about to build is essentially a way to *manipulate* this call chain non-locally: capture some prefix of it, save it as a value, restore it later. The return stack is the substrate everything operates on.

Three operations are all we need:

- **Snapshot** a portion of the return stack into a heap object (capture).
- **Truncate** the return stack to undo work (unwind).
- **Splice** a saved snapshot back onto the live return stack (resume).

Each of the four primitives we'll meet (`reset`, `shift`, `shift-with`, `resume`) is some combination of these three operations plus a small amount of bookkeeping.

---

## Part 5: RESET — installing a boundary

`reset` is the simplest of the four continuation primitives. It does just one thing: push a `T_MARK` value onto the return stack, with a unique numeric id.

```c
static void p_reset(cell *cfa) {
    (void)cfa;
    Val mark = make_mark();
    mark.data = next_mark_id++;
    rpush(mark);
}
```

That's the entire implementation. `reset` returns immediately. The mark sits on the return stack, mixed in with the regular saved-ip frames from the call chain.

Why is it harmless to leave a marker on the return stack? Because EXIT is taught to skip MARK frames. When EXIT pops a frame and sees that it's a MARK, it discards the mark and pops the next frame instead — the real saved-ip that EXIT needs to jump back to. The mark is invisible to normal control flow; it's just a tag that says "this is a reset boundary."

The mark exists for `shift` to find it.

### Why a unique id?

Every `reset` allocates a fresh id, monotonically increasing. The id tags each mark uniquely, which matters when `reset` regions nest. A `shift` inside an inner reset captures up to the *inner* mark, not some outer one. The unwinding machinery uses the id to know which mark it's targeting, so an unwind from an inner reset doesn't accidentally clear an outer mark with the same generic "I am a mark" tag.

The id is also useful for debugging: when you print the return stack, each MARK shows its id, so you can see the nesting structure at a glance.

### Visualizing the return stack after `reset`

Suppose `reset` was called from inside a word `catch` (we'll define `catch` properly later; for now treat it as a colon definition that contains `reset` in its body). The return stack just before `reset` runs might look like:

```
[ ... outer frames ... ]
[ R_catch_tramp           ]   <-- saved ip: "where to return when catch's body finishes"
                                   (pushed by docol when catch was called)
                          ^ rsp
```

After `reset` pushes the mark:

```
[ ... outer frames ... ]
[ R_catch_tramp           ]
[ MARK id=N               ]
                          ^ rsp
```

The mark has its own unique id (let's say N). It carries no instruction pointer — it's purely a sentinel.

If nothing more happens — `catch`'s body finishes, the inner interpreter reaches EXIT — then EXIT pops the mark (skip), pops `R_catch_tramp`, jumps to where catch was called from. The mark contributed nothing.

But if `shift` or `shift-with` runs inside the reset region, the mark becomes the *target* of the operation.

### What "the reset region" actually means

There is no syntactic bracket here. `reset` doesn't take a body. It's just a sentinel-push. The "reset region" is whatever code happens to execute *after* `reset` runs and *before* the mark gets popped — either by a normal EXIT walking past it, or by an unwinding `shift-with`.

In practice the region tends to be "the rest of the word that contained `reset`, plus everything that word's body calls." For example:

```forth
: catch ( xt -- ... )   reset execute 0 ;
```

The reset region is `execute 0` plus everything `execute` invokes transitively. When EXIT for `catch`'s body eventually runs, it pops the mark on the way out and the region ends.

This is a different design from Scheme's `prompt`, which takes a body expression as an argument. The sentinel-push design is simpler to implement (one line) and integrates naturally with Forth's call/return mechanism, but it requires the unwinding logic to be slightly cleverer about figuring out where "after the reset region" is. We'll see how that works in Part 9.

---

## Part 6: SHIFT — basic capture and unwind

`shift` is the bare capture primitive. It does these things in order:

1. **Find** the nearest MARK on the return stack (the topmost one).
2. **Capture** the frames *above* the mark — the call chain from `reset` to here.
3. **Truncate** the return stack: drop both the mark and the captured frames.
4. **Push k** onto the data stack: a `T_CONT` Val that wraps the captured slice.
5. **Return** to the inner interpreter loop.

Here's the implementation. The capture logic is factored into a helper because `shift-with` reuses it:

```c
static int capture_continuation(int *out_mark_index) {
    int mark_index = rsp - 1;
    while (mark_index >= 0 && return_stack[mark_index].tag != T_MARK) {
        mark_index--;
    }
    if (mark_index < 0) {
        fail("shift outside reset");
        return -1;
    }

    int return_len = rsp - mark_index - 1;
    int resume_ip = (int)(ip - dict);
    int slot = object_new_continuation(&return_stack[mark_index + 1],
                                       return_len, resume_ip);
    if (error_flag) return -1;

    *out_mark_index = mark_index;
    return slot;
}

static void p_shift(cell *cfa) {
    (void)cfa;
    int mark_index;
    int cont_slot = capture_continuation(&mark_index);
    if (cont_slot < 0) return;

    rsp = mark_index;             /* discard the mark and frames above */
    push(make_continuation(cont_slot));
}
```

The captured continuation, stored in an `OBJECT_CONTINUATION`, contains:

- A copy of the captured return-stack frames (a `Val *return_slice` of length `return_len`).
- The `resume_ip` — the dict offset of the cell that would have executed next if shift hadn't fired (in practice, the cell after `shift` in whatever body called it).

The data stack now has the `T_CONT` value `k`. The return stack has been pruned — both the mark and all the call frames that were sitting above it are gone.

### Anatomy of a captured continuation

Think of `k` as a small heap object with two fields:

```
OBJECT_CONTINUATION {
    return_slice: [frame1, frame2, ..., frameN]   // a copy
    resume_ip:    integer offset into dict[]
}
```

The frames are *copies*, not references. The live return stack has been truncated; the captured frames now live only inside the continuation object. This is what makes multi-shot resumption possible (Part 7).

The `resume_ip` is the address of the cell that the inner interpreter would have dispatched next if `shift` had returned normally. In a typical use, `shift` is the last word in a `yield`-like definition, so `resume_ip` points at the EXIT cell of that definition's body.

### What happens next?

After `shift` returns, the inner interpreter loop continues. It reads the next cell at `ip` and dispatches whatever's there. That cell is in the body of the word that called `shift` — typically EXIT (since `shift` is usually the last thing in a `yield`-like word). When EXIT fires, it pops the next frame from the return stack — but the captured frames are gone, so it pops something from *below* the original mark, which jumps to code that's outside the reset region entirely.

The net effect: `shift` "exits the reset region" from the inside, taking with it the captured slice as a Val on the data stack. The caller of the reset-containing word now sees `k` on its data stack.

This is the basic coroutine pattern. `shift` is `yield`. The driver (the code that called the reset-containing word) picks up `k` and decides what to do with it — typically, hold it for later and resume the producer when ready for another value.

### Tracing a coroutine

Here's a concrete trace.

```forth
: yield   shift ;
: producer  1 yield 2 yield 3 ;
: drive   reset producer ;
```

Call `drive`. The execution unfolds like this:

```
1. drive's docol pushes R_drive_tramp onto rstack.
2. drive body executes:
   - reset → push MARK(id=1) onto rstack.
   - producer is dispatched → docol pushes R_producer (saved_ip = next cell after
     producer call in drive body, which is the body's EXIT).
3. producer body executes:
   - 1 is pushed onto dstack. dstack: [1]
   - yield is dispatched → docol pushes R_yield (saved_ip = next cell after yield
     in producer body, which is the `2` cell).
4. yield body executes:
   - shift runs.
     - mark_index = position of MARK on rstack.
     - return_len = 2 (R_producer and R_yield are above MARK).
     - resume_ip = current ip = cell after shift in yield body (the EXIT cell).
     - Allocate continuation with those captured frames and resume_ip.
     - rsp = mark_index. The mark and both captured frames are gone.
     - Push k. dstack: [1, k].
5. shift returned. Inner loop continues.
   - ip is now at yield body's EXIT.
   - EXIT runs: pops the top of rstack (which is now R_drive_tramp, since shift
     removed everything above the mark). ip = R_drive_tramp.saved_ip =
     trampoline+1 (a special "stop the loop" cell).
6. The inner loop reads stop_cfa, sets running=0, ends.
7. drive's execute_cfa returns. The REPL sees the data stack: [1, k].
```

The driver (the REPL) now has `1` (the first yielded value) and `k` (the continuation for "the rest of producer"). It can resume `k` to get the next value:

```forth
swap drop       \ drop the 1, keep k
0 swap resume   \ push 0, swap, resume k
```

`resume` re-enters the captured slice. Producer continues from where it left off — at the cell right after the first `yield`. It pushes `2`, calls `yield` again, captures a new continuation `k2`, leaves with `(2, k2)` on the data stack.

This is real cooperative multitasking, with zero special-purpose machinery beyond `shift` and `resume`.

### Stack diagram at the moment of capture

It can help to draw what each stack looks like at the instant `shift` fires inside `yield`:

```
Data stack:                Return stack:                Dict instruction pointer:
                                                            (ip points to the cell after
                           [ ... outer ... ]                shift in yield's body — the
                           [ R_drive_tramp  ]               EXIT cell)
                           [ MARK id=1      ]
[ 1 ]                      [ R_producer     ]
       ^ dsp               [ R_yield        ]
                                              ^ rsp
```

After `shift` does its work:

```
Data stack:                Return stack:                ip:
[ 1 ]                      [ ... outer ... ]            unchanged (still points to
[ k ]                      [ R_drive_tramp  ]           yield's EXIT cell — the next
       ^ dsp                                ^ rsp        instruction to execute)
```

The captured frames (R_producer and R_yield) plus the MARK are gone from the live return stack. They live inside `k` now. The data stack gained `k`. The next dispatch will execute EXIT, which pops R_drive_tramp, which sends the inner loop to the trampoline-stop cell, which ends the loop.

---

## Part 7: RESUME — re-entering a captured slice

`resume` takes a `T_CONT` on the data stack and re-enters the captured slice. The data stack is left otherwise alone — whatever the caller has arranged is what the slice sees as it resumes.

```c
static void p_resume(cell *cfa) {
    (void)cfa;
    POP(k);
    if (k.tag != T_CONT) { type_error("resume"); return; }

    Object *cont = objects[k.data];
    if (!cont) { fail("resume: continuation is dead"); return; }

    cell *saved_ip = ip;
    int saved_running = running;

    /* Trampoline-stop frame so the slice's eventual EXIT chain
     * terminates cleanly here. */
    trampoline[1] = (cell)stop_cfa;
    rpush(make_addr((int)((trampoline + 1) - dict)));

    /* Fresh MARK delimits the resumed slice. */
    rpush(make_mark());

    /* Splice the captured frames on top. */
    for (int i = 0; i < cont->continuation.return_len; i++) {
        rpush(cont->continuation.return_slice[i]);
    }

    ip = &dict[cont->continuation.resume_ip];
    running = 1;
    run_inner();

    running = saved_running;
    ip = saved_ip;
}
```

The steps:

1. **Pop k** from the data stack.
2. **Save** the current `ip` and `running` flag so we can restore them after the slice finishes.
3. **Push a trampoline-stop frame** onto the return stack. This is the saved-ip that the slice's eventual EXIT chain will land at, ending the slice's execution cleanly.
4. **Push a fresh MARK** above the trampoline-stop frame. This delimits the resumed slice — any `shift` fired inside the slice will target *this* mark, not whatever outer reset might still be active.
5. **Splice the captured frames** above the new mark. The slice's saved-ip's are now on the return stack, ready to be popped by EXITs as the slice unwinds.
6. **Set `ip`** to the slice's resume point — the dict offset of the cell right after the original `shift`.
7. **Run the inner interpreter** in this resumed context.
8. When the slice's EXIT chain unwinds through the spliced frames, reaches the fresh MARK (which EXIT skips), and then the trampoline-stop frame, the loop ends. `resume` restores `ip` and `running` and returns.

### A picture of resume's setup

Before `resume`:

```
Data stack:                Return stack:
[ ...some-arg-for-slice ]  [ ... whatever the resume site has on rstack ... ]
[ k                     ]
                  ^ dsp
```

After `resume` pops `k` and does its setup, before jumping into the slice:

```
Data stack:                Return stack:
[ ...some-arg-for-slice ]  [ ... resume site's rstack ... ]
                  ^ dsp    [ R_to_resume_caller            ]   <-- trampoline-stop
                           [ MARK (fresh)                  ]
                           [ frame1 from k                 ]
                           [ frame2 from k                 ]
                           [ ...                           ]
                                                            ^ rsp

ip = resume_ip from k (pointing at the cell after the original shift)
```

The slice runs. Its EXITs pop the spliced frames one at a time, each time jumping to a saved ip that's a cell *inside the slice's original code path*. Eventually the topmost spliced frame is consumed; the next EXIT skips the fresh MARK and pops the trampoline-stop frame; ip becomes trampoline+1; the loop dispatches stop and exits. `resume` returns to its caller.

### Why a fresh MARK in resume?

When you resume a captured continuation, the resumed slice has effectively become its own reset region. If the slice calls `shift`, where should that shift's target be? Logically, it should be at the resume site — the place where the user invoked the captured slice. The fresh MARK gives shift a target inside the resumed slice's call chain, so a shift in the resumed slice unwinds back to RESUME's caller, not to some outer reset that might be active.

This is the "shift inside the resumed continuation" composability — it works the way you'd expect.

### Multi-shot resume

The captured `OBJECT_CONTINUATION` is never modified by resume. The `return_slice` and `resume_ip` are read but not changed. Each invocation of resume copies the captured frames onto the live return stack.

This means you can call `resume` on the same `k` many times, and each time the slice runs fresh, producing the same effects (modulo any global state changes the slice might make). This is "multi-shot" resumption, and it's what enables backtracking-style code.

```forth
: shifter shift ;
: ex reset 99 shifter 2 * ;

ex swap drop       \ stack: [k]
dup 3 swap resume . cr     \ 6
    4 swap resume . cr     \ 8
```

The same `k` resumed twice with different inputs produces different results — the slice ran twice, multiplying by 2 each time.

### Multi-shot has caveats

Multi-shot resumption is powerful but easy to misuse. Two pitfalls:

**Side effects re-run.** If the captured slice opens a file, mutates a global, or sends a network packet, those effects happen *every time* the slice is resumed. The slice doesn't know it's being re-run. For pure computation this is fine; for I/O it's catastrophic unless you've thought it through.

**Data stack discipline.** The slice expects a particular set of values on the data stack when it resumes. If you set up the data stack wrong before calling resume, the slice will misbehave. Multi-shot doesn't fix this — each invocation needs the same discipline.

For most exception-style and coroutine uses, every continuation is one-shot in practice: you resume it once or you drop it. Multi-shot is reserved for backtracking, probabilistic sampling, and similar cases where re-running really is the point.

---

## Part 8: SHIFT-WITH — capture with a handler

`shift` is great for coroutines (the caller of the reset-containing word picks up `k`). For exceptions you want different semantics: the recovery handler should run *at the place where the reset lives*, not bubble all the way out to the reset's caller.

`shift-with` provides that. It takes a handler xt as its argument:

```forth
( handler-xt -- ... )   shift-with
```

The implementation reuses `shift`'s capture logic, then does additional work:

```c
static void p_shift_with(cell *cfa) {
    (void)cfa;

    POP(handler);
    if (handler.tag != T_XT) { type_error("shift-with"); return; }

    int mark_index;
    int cont_slot = capture_continuation(&mark_index);
    if (cont_slot < 0) return;

    unwind_target = (int)return_stack[mark_index].data;
    rsp = mark_index + 1;         /* keep the mark; run_inner pops it */
    push(make_continuation(cont_slot));

    execute_cfa((int)handler.data);
    if (error_flag) return;

    unwinding = 1;
}
```

The differences from `shift`:

1. **Pop the handler xt** from the data stack.
2. **Capture** as before (same helper).
3. **Store the mark's id** in a global `unwind_target` — this tells the unwinding logic which mark we're targeting.
4. **Truncate** to `mark_index + 1` — drop the captured frames but *keep the mark in place*.
5. **Push k** onto the data stack.
6. **Execute the handler** with k on the stack. The handler runs in the now-outer context (the captured frames are gone, so we're effectively running where reset was sitting).
7. **Set the global `unwinding` flag** when the handler returns.

The handler decides what to do with k. It can:

- **Drop k** and push other values. This is the exception pattern.
- **Call resume on k**. This is the restart pattern — the slice re-enters and runs as if shift had returned whatever the handler resumed with.
- **Stash k** somewhere for later use. This is the coroutine-with-handler pattern.

After the handler returns, the `unwinding` flag is set. The next thing that happens is the unwinding cascade through the inner interpreter levels.

### Why shift-with exists when shift suffices for coroutines

The handler form is needed when you want a guaranteed code execution at the resume-site *after* the capture, with `k` accessible to that code, without forcing the surrounding word to be specifically structured to receive `k`.

For exceptions: the throw site is buried somewhere deep in user code. We don't want every word along the call chain to know "we might be in a catch, and the catch might want to do something with the captured continuation." Instead, `throw` says: "capture, then run *this little function* at the place where catch lives." The function gets `k` and decides what to do — typically discarding it and pushing a flag. The user code doesn't need to know.

For restarts: same pattern. The signal site says "capture, then run the recovery logic." The recovery logic has `k` and chooses to resume it with a recovery value. The signaling code is unchanged.

The bare `shift` is the right primitive for coroutines because the *caller of the reset-containing word* is the natural place for `k` to land — that caller is the scheduler or driver, written to expect `k`. For exceptions and restarts, the natural place for handling logic is *adjacent to the reset*, not after it. `shift-with` puts the handler there.

---

## Part 9: The unwinding flag and the inner interpreter loop

This is the subtle piece that makes exception-style flow work.

After `shift-with`'s handler returns, the `unwinding` flag is set. But just setting a flag doesn't accomplish anything — we need the interpreter to *react* to it. The `run_inner` loop has been modified to check the flag at the top of every iteration:

```c
static void run_inner(void) {
    int initial_rsp = rsp;             /* remember entry-depth */
    while (running && !error_flag) {
        if (unwinding) {
            /* If the target mark is below this level's entry depth,
             * it belongs to an outer run_inner — break to propagate. */
            if (rsp <= initial_rsp) break;

            Val frame = return_stack[--rsp];
            if (frame.tag == T_MARK && (int)frame.data == unwind_target) {
                /* Found the target. Clear the flag, pop the docol frame
                 * just below, and use its saved ip to resume past the
                 * reset region. */
                unwinding = 0;
                if (rsp > 0) {
                    Val ret = return_stack[--rsp];
                    ip = (cell *)(dict + ret.data);
                }
                continue;
            }
            /* Some other frame (a non-matching mark or a saved ip) —
             * discard and keep unwinding within this level. */
            continue;
        }

        /* Normal dispatch */
        int cfa_index       = (int)*ip++;
        cfa_handler handler = (cfa_handler)dict[cfa_index];
        handler(&dict[cfa_index]);
    }
}
```

Each invocation of `run_inner` records its `initial_rsp` — the return-stack depth at the moment it started. The loop's behavior in unwinding mode is:

- **Mark below this level's entry**: break out, propagate to the enclosing `run_inner`.
- **Pop a frame**. If it's the target MARK: clear the flag, pop the next frame (which will be the docol return frame of whoever called reset), set `ip` to that frame's saved ip (which points past the reset region in the caller's body), continue normal dispatch.
- **Otherwise**: discard the frame and continue unwinding at this level.

### Why levels matter

Every `execute_cfa` call (for a colon-defined word that goes through the trampoline mechanism) creates a new `run_inner` invocation on the C call stack. So at any moment there can be several `run_inner` frames active, each with its own `initial_rsp`.

When `shift-with` sets the unwinding flag, the innermost `run_inner` sees it, checks if the mark is in its range, propagates if not, exits cleanly if so. The propagation cascades up the C stack: each level checks and either handles or passes up.

The level that handles is the one whose `initial_rsp` is *below* the mark's index — meaning the mark was pushed during this level's execution. That's the run_inner that ran `reset`. It pops the mark, clears the flag, jumps past the reset region, and continues.

Levels deeper than that (the ones that ran the user xt, the throw call, etc.) all see "mark is below me" and break to propagate.

### Why also pop the docol frame?

When `reset` runs in a colon definition's body, the colon definition's own docol frame is below the mark. After we pop the mark, the next frame is that docol frame — its saved ip points to wherever execution should continue after the reset region.

By popping it and setting `ip` from it, we land at the right place in the caller's code. Normal dispatch then resumes from there.

In the exception case, this is exactly what we want: catch's body is `reset execute 0`. When `shift-with` fires inside the user xt, the unwind hits the matching mark; the docol frame just below it tells us where execution should resume — *outside* catch's body, back in the calling word. The `0` cell that lives in catch's body never runs.

This is the whole reason the unwinding works correctly: the docol frame just below the mark contains exactly the information about "where to continue in the caller's code after the reset region completes."

### The state machine in pictures

Suppose we have:

```forth
: deep   1 throw ;
: middle deep ;
: top    [: middle :] catch ;
```

When `throw` fires inside `deep`, the C stack looks like:

```
C stack (most recent on top):
  run_inner          <-- innermost; running throw's body
  execute_cfa(throw)
  run_inner          <-- running deep's body (which dispatched throw)
  execute_cfa(deep)
  run_inner          <-- running middle's body
  execute_cfa(middle)
  run_inner          <-- running the [: middle :] quotation's body
  execute_cfa(quotation)
  p_execute          <-- the `execute` word inside catch's body
  run_inner          <-- running catch's body (this is the one with reset!)
  execute_cfa(catch)
  REPL or outer caller
```

And the return stack (drawn bottom-to-top):

```
[ R_caller_of_catch    ]   <-- pushed when catch was invoked
[ MARK id=N            ]   <-- pushed by reset in catch's body
[ R_quotation          ]   <-- pushed when execute invoked the quotation
[ R_middle             ]   <-- pushed when quotation called middle
[ R_deep               ]   <-- pushed when middle called deep
[ R_throw              ]   <-- pushed when deep called throw
[ R_shift_with_handler ]   <-- pushed during throw's execution
                              (and the handler has now returned)
```

`shift-with` set `unwind_target = N` and `unwinding = 1`. Now the innermost run_inner sees the flag and starts popping. It pops R_shift_with_handler, R_throw, R_deep. These are all above the level's `initial_rsp` (which was just below R_throw — when this run_inner started, R_throw was the top). When it would pop below its initial_rsp, it breaks out.

The next-outer run_inner takes over. Its `initial_rsp` was set when middle's quotation started. It sees unwinding=1, starts popping: R_middle, R_quotation. Again hits its initial_rsp boundary, breaks out.

Another run_inner up, then another. Eventually we get to the run_inner that was running *catch's body*. Its `initial_rsp` was set when catch was entered — that's just above R_caller_of_catch, *below* MARK N. So the MARK is within this level's range.

This run_inner pops MARK N. It matches `unwind_target`. Clears `unwinding = 0`. Pops the next frame — R_caller_of_catch. Sets ip from it. Continues normal dispatch.

The next thing executed is whatever cell follows the catch call in the caller's word. The data stack has whatever the handler left there.

That's the whole unwind. A simple flag plus per-level `initial_rsp` tracking gives us multi-level stack unwinding that stops precisely at the right point in the right word.

---

## Part 10: Building exceptions

With all that machinery, exceptions are three one-liners in `lib.l4`:

```forth
: throw ( exc -- )                       [: drop 1 :] shift-with ;
: catch ( xt -- result 0 | exc 1 )       reset execute 0 ;
: try-catch ( normal-xt error-xt -- ... )
    >side                                \ stash error-xt
    catch
    if   side> execute                   \ throw: pop handler, run with exc on stack
    else side-drop                       \ success: drop saved handler, results stay
    then ;
```

Let's trace `[: 42 throw :] catch`:

1. The quotation `[: 42 throw :]` is pushed onto the data stack as an xt.
2. `catch` is called. Its body is `reset execute 0`.
3. `reset` pushes a MARK (id=N) onto the return stack.
4. `execute` pops the xt and invokes it.
5. The xt runs: `42` pushes 42 onto the data stack; `throw` runs.
6. `throw`'s body is `[: drop 1 :] shift-with`. The handler quotation is pushed onto the data stack.
7. `shift-with` pops the handler, captures the return-stack frames above the MARK, keeps the MARK in place, pushes `k` onto the data stack (now `[42, k]`), and executes the handler.
8. The handler runs: `drop` pops k, leaving `[42]`; `1` pushes 1, leaving `[42, 1]`. Handler returns.
9. `shift-with` sets `unwinding = 1`.
10. The unwinding cascade begins. Inner-loop levels break and propagate up until reaching the level that owns the MARK.
11. That level pops the MARK (clearing `unwinding`), pops the docol frame for `catch`, and sets `ip` to wherever catch was called from in the calling word.
12. The `0` after `execute` in catch's body is bypassed (catch's body never runs again).
13. Execution continues past catch in the calling word, with `[42, 1]` on the data stack.

Now trace `[: 42 :] catch` (success path):

1. The xt is pushed.
2. catch body: `reset execute 0`.
3. `reset` pushes a MARK.
4. `execute` runs the xt. The xt pushes 42 and returns.
5. The xt's EXIT pops its own docol frame and returns to catch's body, which continues with `0`. The `0` is pushed.
6. Catch's body's EXIT runs. It pops the MARK (skip), pops catch's own docol frame, returns to the caller.
7. Data stack: `[42, 0]`.

So catch returns `(result, 0)` on success or `(exc, 1)` on throw. The flag distinguishes the two cases — and the unwinding mechanism ensures the `0` only runs on success.

### `try-catch`'s side stack

Why does `try-catch` use the side stack? Couldn't it keep the error-xt on the data stack?

Look at try-catch:

```forth
: try-catch ( normal-xt error-xt -- ... )
    >side
    catch
    if   side> execute
    else side-drop
    then ;
```

After `catch`, the data stack has whatever the user xt left, plus the flag. If we'd kept the error-xt on the data stack:

- Success case: data stack is `[error-xt, results..., 0]`. To drop the error-xt and keep the results, we'd need to know how many results there are. The arity isn't known statically.
- Throw case: data stack is `[error-xt, exc, 1]`. We'd need to fish error-xt out from underneath.

Either way, the wrapper would have to compute the arity (via `depth` tracking and `roll`) just to clean up the data stack. The side stack avoids the problem entirely: `>side` stashes error-xt where nothing else can touch it. We retrieve it only when needed.

The return stack would have been worse: `>r` would put error-xt onto the return stack, but the unwinding logic in `run_inner` pops frames in unwinding mode, and a `T_XT` value would either be discarded (if `shift-with` truncated past it) or misinterpreted (if EXIT popped it expecting a saved-ip). The return stack is only safe for T_ADDR (saved IPs) and T_MARK frames.

The side stack is exactly what's needed: a place to put values that nothing else in the system cares about.

### Nested try-catch

Nesting works because each `reset` allocates a fresh mark id and the unwind targets the *innermost* mark by id. A throw deep inside two nested try-catches is always caught by the inner one.

```forth
: outer-h ( exc -- )  ." outer handler: " . cr ;
: inner-h ( exc -- )  ." inner handler: " . cr ;

: demo
    [: [: 1 throw :] ['] inner-h try-catch  2 throw :]
    ['] outer-h try-catch ;

demo
```

Output:

```
inner handler: 1
outer handler: 2
```

The `1 throw` inside the inner try-catch unwinds to the inner reset; control resumes after the inner try-catch in the surrounding xt, which then runs `2 throw`, which unwinds to the outer reset. Each throw finds the topmost (innermost) mark.

### Re-throwing

To re-throw from a handler, the handler just calls `throw` again:

```forth
: rethrow-h ( exc -- )
    dup 100 < if drop exit then    \ swallow small errors
    throw ;                         \ rethrow larger ones
```

This works because `throw` is itself a colon definition that calls `shift-with`. When called from inside a handler, `shift-with` again scans the return stack for the nearest mark — which is now the *outer* try-catch's mark (the inner one was consumed by the unwind). The unwind cascades up to the outer reset, just as if the throw had originated at the inner handler's location.

### Cleanup and "finally"

A `finally` block — code that runs whether the protected region exited normally or via throw — is straightforward:

```forth
: try-finally ( normal-xt cleanup-xt -- ... )
    >side                  \ stash cleanup
    [: ['] noop try-catch :]      \ catch any throw (noop handler swallows)
    execute
    side> execute                  \ run cleanup
    ;
```

Or, with proper rethrow:

```forth
: try-finally ( normal-xt cleanup-xt -- ... )
    >side
    catch
    side> execute                  \ cleanup runs in both cases
    if throw then ;                \ rethrow on the error path
```

The cleanup is unconditional. On the throw path, after cleanup, we rethrow to propagate to any outer try-catch.

---

## Part 11: Coroutines in depth

Coroutines deserve a careful treatment because they're the conceptual middle ground between ordinary function calls and full preemptive threads, and almost everything else in this document is best understood as a specialization of them. Once you see coroutines clearly, generators, async I/O, green threads, and even backtracking are recognizable as the same pattern with different policy.

### Subroutines versus coroutines

Start with the distinction that matters.

A *subroutine* — what most languages just call a "function" — has a strictly hierarchical relationship with its caller. The caller invokes it. It runs from beginning to end. It returns once. Control then flows back to the caller at the point of invocation. The subroutine has no persistent identity: each call is a fresh activation, with fresh local state. When it returns, that activation is destroyed.

The asymmetry is total: callers can call callees, but callees can only return to their callers. There is no other relationship available. The call stack — that linked list of return frames — is what enforces the hierarchy.

A *coroutine* is a generalization. Two coroutines have a peer relationship rather than a hierarchical one. Either can suspend itself and pass control to the other. When it resumes later, it picks up exactly where it left off, with its local state intact. There's no "caller" and "callee" — there are two (or more) ongoing computations that pass control between themselves.

The key word is *suspend*. A subroutine cannot suspend. It can only return, which destroys its activation. A coroutine can suspend, which preserves its activation as a value that can be resumed later. That preserved activation is exactly what a continuation is.

This is why continuations and coroutines are the same idea seen from two angles. A continuation is the static representation of "a suspended computation, ready to be resumed." A coroutine is the dynamic process of using continuations to pass control between two or more computations.

### A simple coroutine

```forth
: yield   shift ;
: producer  1 yield 2 yield 3 yield ;
: drive   reset producer ;
```

`yield` is just `shift` — capture the rest of the producer's work as a continuation `k`, leave it on the data stack, unwind to the reset's caller. `drive` returns to its caller with `(value, k)`.

The producer doesn't know it's being run cooperatively. It reads as ordinary linear code that happens to use a `yield` primitive. The continuation mechanism turns the `yield` calls into pause-points without any compiler support.

Now imagine a driver:

```forth
\ Drive the producer to completion, collecting yielded values.
: drain  ( -- list )
    drive          \ data stack: [1, k1]
    swap >side     \ stash 1
    0 swap resume  \ resume k1; data stack: [2, k2]
    swap >side     \ stash 2
    0 swap resume  \ data stack: [3, k3]
    swap >side     \ stash 3
    0 swap resume  \ producer hits end of body
    ...            \ now what?
;
```

Two things to notice. First, the driver alternates between using `drive` (the first call) and `resume` (subsequent calls). This is asymmetric — the first call starts the producer, the rest re-enter it. Symmetric variants exist; we'll come back.

Second, when the producer finishes — when `3 yield` returns to a body that has no more cells, hits EXIT, and the slice ends — the driver gets... what, exactly? The mechanism we have says the slice's EXIT chain runs out, the fresh MARK is popped, the trampoline-stop frame ends `resume`, and `resume` returns to its caller with whatever the slice left on the data stack. In a producer that ended cleanly, that's nothing — or the last yield's return value, or whatever happens to be there. The driver doesn't have a clean way to distinguish "the producer yielded again" from "the producer finished."

### Termination protocols

The driver needs to know when to stop. Three approaches, in increasing sophistication.

**Sentinel value.** The producer yields a special "I'm done" value before truly exiting:

```forth
symbol :done

: producer  1 yield 2 yield 3 yield :done yield ;
```

The driver checks each yielded value against `:done` and stops the loop when it sees it. Simple but ugly: the protocol leaks into both producer and consumer, and `:done` can't be a legitimate value.

**Tagged yields.** Every yield carries a kind-tag:

```forth
symbol :value
symbol :end

: yield-value ( v -- )  :value 2 shift-as-tuple ;
: yield-end   ( -- )    :end   1 shift-as-tuple ;

: producer  1 yield-value 2 yield-value 3 yield-value yield-end ;
```

The driver dispatches on the tag. Cleaner: any value can be yielded, and the protocol is explicit. Verbose, but explicit beats clever.

**Exception-based termination.** The producer signals completion with `throw`:

```forth
symbol :end-of-stream

: producer  1 yield 2 yield 3 yield :end-of-stream throw ;

: drain ( -- list )
    [: drive  ...drain body... :]
    [: drop ;]                  \ handler: swallow :end-of-stream
    try-catch ;
```

The driver wraps the whole loop in try-catch. When the producer throws `:end-of-stream`, the unwind takes control out of the loop and into the handler, which finishes cleanly. The producer doesn't need an explicit termination value; it just runs out and throws.

This last approach has a clean separation: normal yield handles values, throw handles termination (and other exceptional conditions). It's the closest analog to Python's `StopIteration` exception, which is how Python generators signal end-of-iteration.

The choice between these is a matter of library taste. None is wrong; all three work with the same `shift`/`resume` primitives.

### Bidirectional communication: send and receive

Most introductory coroutine examples are producer-only: the coroutine yields values, the driver consumes. But `resume` can pass values *into* the slice, not just out, and this turns a coroutine into a bidirectional pipe.

```forth
: filter   ( -- )
    begin
        yield   \ suspend; receive next input from resume
        2 *     \ transform whatever resume gave us
                \ ... fall off and loop back to yield ...
    again ;
```

Trace what happens when this runs under `reset`:

1. First call to `yield` (= `shift`) captures `k` and unwinds out. Driver receives `k`.
2. Driver decides what input to feed in. Pushes 5 on the data stack, then `5 k resume`.
3. `resume` enters the slice. The slice's data stack now has `5` on top (whatever the driver arranged).
4. The slice executes `2 *` → data stack has 10.
5. The slice loops back to `yield`, which captures a new `k'`. The 10 is left on the data stack as the value being yielded. Driver receives `(10, k')`.
6. Driver feeds in the next input. And so on.

So `yield` is simultaneously *and inseparably* both:

- A way for the coroutine to output a value (whatever's on top of the data stack when yield captures gets carried out with `k`).
- A way for the coroutine to receive a value (whatever the driver leaves on the data stack before calling `resume` is what's there when the slice resumes).

This dual nature is what makes coroutines powerful as a building block. Python's `yield` expression has exactly this character: it's both an outbound communication and an inbound one, depending on how the caller drives it.

This is also why generators (yields-only) and consumers (receives-only) are mirror images, not different things. They're the same primitive, used with different policies.

### Symmetric versus asymmetric coroutines

A subtle distinction in the coroutine literature.

In our examples so far, the driver and the producer have an *asymmetric* relationship: the driver calls `resume`, the producer calls `yield`. Yield always returns to whoever invoked `resume` most recently. The driver is a "coordinator" with a privileged position; the producer is "subordinate." This is asymmetric coroutines, and Python generators are exactly this style.

A *symmetric* coroutine system has no such hierarchy. Any coroutine can transfer control to any other named coroutine directly, without going through a coordinator. There's a `transfer A` operation that says "save my state, restore A's state." The dance is peer-to-peer.

Asymmetric is easier to implement and reason about. Symmetric is more flexible — it can express patterns (like state machines passing tokens between states) that asymmetric coroutines force you to encode through a coordinator.

In logicforth's primitives, asymmetric coroutines are the natural fit: `shift` always unwinds to the most recent `reset`, which sets up a clear "outer/inner" relationship. Symmetric coroutines can be built on top by writing a coordinator that just shuttles control between named participants — the coordinator is the asymmetric driver, and the participants are asymmetric coroutines under it, but to the participants' perspective they're talking peer-to-peer through the coordinator.

### State and identity

A subtle but important point. A subroutine has no identity — each invocation is fresh. A coroutine *does* have identity: it's a specific suspended activation, with a specific in-progress state.

That identity is just the continuation value `k`. Holding `k` is holding "this particular coroutine, paused at this particular point, with this particular state." If you store `k` in a global variable, you have a name for that coroutine. Multiple variables can refer to the same coroutine. If `k` is dropped and no one holds it, the coroutine is garbage-collected, and its state vanishes.

This is why coroutines feel object-like. Each one is a kind of mini-object: it has private state (the data and return stack contents at suspension), a single method (resume), and identity (the pointer). It's not coincidence that languages with rich coroutine support often subsume some of what other languages need objects for.

### A picture of state at suspension

Concrete picture. Suppose `filter` (the bidirectional doubler above) has been driven through a few iterations and is currently suspended at `yield`. What does the captured continuation actually contain?

```
k = OBJECT_CONTINUATION {
    return_slice: [
        R_filter        // saved-ip pointing into filter's loop body,
                        // at the cell right after `yield` (= `shift`)
    ]
    resume_ip:      <offset of cell after `yield` in filter's body>
}
```

That's it. One return-stack frame and an instruction pointer. The data stack at suspension time is implicitly part of the surrounding context — whatever the slice was leaving behind when it suspended, and whatever the driver arranges before resume. There's no "captured data stack" because `shift` doesn't copy the data stack into `k`.

Two pieces of state, three cells of memory, and you have a fully-suspended computation that can be resumed later. This is part of why green threads on continuations are so cheap: each green thread is just a continuation plus some metadata, costing maybe 50 bytes, compared to an OS thread's kilobytes.

### Coroutine pipelines

Once you have coroutines, you can pipe them. A producer yields values; a filter receives values and yields transformed values; a consumer drives the chain.

```forth
\ Producer yields 1 through n
: range-gen ( n -- ) 1 begin 2dup >= if drop drop exit then dup yield 1+ again ;

\ Filter receives values, yields x*x for each
: square-filter
    begin
        yield   \ get next input
        dup *   \ square it
        yield   \ output (the same yield is "send result")
    again ;

\ Driver: pipe range through square
: pipe-and-print ( n -- )
    \ start the range producer under one reset
    \ start the square filter under another reset
    \ alternate driving each, threading values between them
    ... ;
```

Writing this out in full Forth is verbose, but the conceptual picture is clean: each stage is a coroutine paused at a `yield`, and the driver shuttles values between them. When the producer finishes, the driver signals the filter to terminate, and so on.

This is the spiritual ancestor of Unix pipes, Clojure's transducers, and Reactive Extensions. The unifying mechanism is two-way control transfer between paused computations.

### Multi-producer scheduling and concurrent coroutines

Multi-producer scheduling — running many coroutines, interleaving their progress — is just a queue of `k`s and a loop that resumes them in turn:

```forth
\ Pseudo-code (logicforth doesn't ship a queue type)
: scheduler
    begin
        queue-empty? if exit then
        queue-pop          \ get next k
        0 swap resume      \ run it until it yields again
        \ if it yielded another k, push it back on queue
        \ if it finished, just continue
    again ;
```

Each coroutine runs until it yields. The scheduler picks the next one. Round-robin scheduling falls out automatically. Priorities can be added by using a priority queue instead of FIFO. Cooperative multitasking — the foundation of green threads — is just this.

The key property: switches happen *only* at `yield` points. Between yields, a coroutine has the CPU to itself. This makes reasoning about state much easier than with preemptive threads, where switches can happen at any instruction boundary.

### Why this matters

Coroutines are the single most underused primitive in mainstream programming. Languages add them piecemeal: Python had generators in 2.2, generators-with-send in 2.5, native coroutines (async/await) in 3.5; JavaScript had no generators, then generators (ES6), then async/await (ES2017); C# layered iterators, then async/await, then channels. Each addition required new keywords, new compiler magic, new runtime support.

In a language with first-class delimited continuations, all of these are libraries. Yield is `shift`. Generator is `reset` + yields. Async is `shift-with` + event loop. Channel is `shift-with` + wait queues. None of them needs language-level support.

The reason this isn't more common is mostly historical accident: continuations are unfamiliar, hard to implement efficiently in classical C-stack languages, and politically hard to add to existing language specs. But in a Forth-like system where the runtime owns the call chain explicitly, they cost almost nothing to implement and pay enormous dividends.

---

## Part 12: Building generators

Generators are a special case of coroutines: one-way, producer-only. A generator yields values; its driver consumes them; eventually the generator runs out and signals end-of-stream.

```forth
\ Generator that yields squares from 1 to n
: squares-gen ( n -- )
    1
    begin
        2dup >=
        if  drop drop exit  then
        dup dup * yield
        1+
    again ;
```

This generator has two pieces of state on the data stack: the limit `n` and the current `i`. Each iteration checks `i <= n`, yields `i*i`, and increments. When `i > n`, it exits without yielding — and the driver sees the data stack in a different state than usual, which it can detect.

A more robust protocol uses an explicit done signal. One approach: the generator throws a `:done` exception at the end, and the driver wraps it in try-catch.

```forth
symbol :done

: gen-done   :done throw ;

: each-square ( n -- )
    [: ( n -- )
       1
       begin
         2dup >=
         if  drop drop gen-done  then
         dup dup * yield
         1+
       again :]
    [: drop ;]                 \ done-handler: swallow the :done signal
    try-catch ;
```

This is awkward because try-catch wraps everything in a quotation. A cleaner library would package this as a `for-each-yielded` higher-order word.

### Lazy sequences

A generator is essentially a lazy sequence. You can build the standard lazy-sequence operations (map, filter, take, etc.) as combinators over generators.

```forth
\ map: apply f to each yielded value
: gmap ( gen-xt f -- new-gen-xt )
    ...   \ build a new xt that resumes the underlying gen and yields f(value)
;

\ take: yield only the first n values from a gen
: gtake ( gen-xt n -- new-gen-xt )
    ...
;

\ Filter, zip, concatenate, etc.
```

The implementation of each combinator allocates a fresh continuation chain that wraps the input. The combinatorial richness of lazy sequence libraries (Clojure's `seq`, Python's `itertools`, Haskell's lists) all becomes available, built on `shift`/`resume`.

---

## Part 13: Building restarts (the Common Lisp pattern)

`shift-with`'s handler receives `k`. Most of our exception examples drop `k`, but the handler is free to *resume* it instead. When it does, the slice runs as if shift-with had returned whatever the handler pushed before calling resume.

```forth
: ouch ( -- v ) [: 42 swap resume :] shift-with ;
: caller reset 5 ouch + ;
caller        \ -> 47
```

Trace:

1. `caller` runs `reset`, pushing a MARK.
2. `5` is pushed: `[5]`.
3. `ouch` is called. Its body is `[: 42 swap resume :] shift-with`.
4. The handler quotation is pushed: `[5, handler]`.
5. `shift-with` captures the frames above the MARK (just `R_ouch` — ouch's own docol frame), keeps the MARK, pushes `k`, runs the handler.
6. The handler runs: `42` is pushed (`[5, k, 42]`); `swap` (`[5, 42, k]`); `resume` pops k.
7. `resume` pushes a trampoline-stop frame and a fresh MARK, splices the captured frame (R_ouch) back on, sets `ip` to the slice's resume point (the EXIT cell of ouch's body), and runs `run_inner`.
8. The slice runs:
   - EXIT pops the spliced R_ouch frame. ip jumps to "after `ouch` in caller's body" — the `+` cell.
   - `+` runs. Data stack at this point: `[5, 42]`. After `+`: `[47]`.
   - The next cell is EXIT (end of caller's body).
   - EXIT pops the fresh MARK (skip), then pops the trampoline-stop frame, ending the slice.
9. `resume` returns. The handler body's EXIT runs.
10. Back in `shift-with`. It sets `unwinding = 1`.
11. The unwinding cascade hits the matching MARK at caller's level, clears the flag, jumps past caller's body.
12. Data stack: `[47]`.

The user perceives `ouch` as "a primitive that returned 42" — the recovery value is indistinguishable from a normal return. But it went through capture + unwind + resume internally.

This is exactly the Common Lisp restart pattern: signal a condition, let a handler choose a recovery strategy, continue computation as if nothing went wrong. No separate machinery needed.

### Why restarts are powerful

In a traditional exception system, the throw site is the loser: it relinquishes control to the catch site and never gets it back. The catch site decides what to do, but it doesn't know much about where the throw came from — only that it happened.

In a restart system, the *throw site* offers a menu of recovery strategies, and the *catch site* picks one. The throw site retains a kind of "veto" over how the error is handled, because it controls what's resumable.

In logicforth: the signaling word builds a handler quotation that wraps the resume call. The outer handler (the one in `try-catch`) can either run the signaling word's resume strategy (call it as a function) or override (drop the strategy and do its own thing). This combination is more flexible than either pure exceptions or pure restarts.

### A multi-restart example

```forth
\ A division word that signals on divide-by-zero and offers two restarts:
\   :use-value  – use a caller-supplied default
\   :retry      – retry with a caller-supplied divisor

symbol :div-by-zero
symbol :use-value
symbol :retry

: safe-div ( a b -- result )
    dup 0= if
        [: ( restart-info k -- )
            \ restart-info is on the stack: a tag and possibly a value
            \ k is the continuation back into safe-div
            \ Decide what to do based on the tag.
            swap                    \ k restart-info
            dup :use-value = if
                drop                \ k :use-value popped, we expect a value next
                                    \ ... pull value off stack ...
                swap resume         \ resume safe-div with that value as its result
            then
            ...
        :] shift-with
    then
    /                                \ normal path
;
```

Sketched not fully because Forth syntax for this gets verbose. The idea is: the handler dispatches on a "restart kind" tag, picks the right recovery, and resumes (or doesn't). Different callers, different recovery strategies, same signaling code.

---

## Part 14: Backtracking and logic engines

This is the longest section of the document and probably the most rewarding one. Backtracking is where delimited continuations stop being a clever trick for stack-management problems and start looking like a genuine new computational substrate. A logic engine — a Prolog-like system that solves problems by exploring a tree of possibilities — fits naturally on top of the primitives we already have. Building one shows the depth of what continuations enable.

### What backtracking actually is

Take a search problem. You're trying to find a value (or set of values) that satisfies some constraints. The naive approach is a tree search: at each decision point, try each option in turn; if a branch leads to a dead end, back up and try the next option.

In imperative code this requires explicit machinery. A stack of decision points. Mutable state that you have to remember to undo when backing up. Loops with manual indexing. Recursive functions that thread an "are we still searching?" flag through every layer. The code that *expresses* the search drowns in code that *manages* the search.

Backtracking with continuations inverts this. The decision points become continuation captures. The "back up and try the next option" becomes resuming a captured continuation with a different choice. The search-management machinery disappears into a tiny library — typically two primitives: `amb` (or `choose`) and `fail`. The search code reads as straight-line, declarative description of the constraints; the runtime walks the tree.

The shift is from *imperative search* — "I, the programmer, manage the exploration" — to *declarative search* — "I describe the constraints; the runtime explores."

### McCarthy's amb operator

John McCarthy proposed `amb` in 1961 as a hypothetical primitive for nondeterministic computation. `amb(a, b)` returns either `a` or `b`, but "in a way that makes the rest of the computation succeed if possible." If you write

```
x := amb(1, 2, 3, 4, 5)
y := amb(1, 2, 3, 4, 5)
require x + y == 7
print(x, y)
```

the runtime is to behave as if it knew which choices to make. For an n-deep tree of `amb` calls, this requires either omniscience or exhaustive search; with `fail` it becomes the latter.

`fail` is a way for the program to say "this branch is no good; back up." Combined with `amb`, it gives complete backtracking search:

```
choose a value for x from {1..5}
choose a value for y from {1..5}
if x + y ≠ 7, fail
print (x, y) — and possibly fail to find more solutions
```

The runtime explores: x=1, y=1 (sum=2, fail) → x=1, y=2 (sum=3, fail) → ... → x=1, y=6 — wait, 6 isn't in the set. Backtrack further: x=2, y=1 (sum=3, fail) → ... → x=2, y=5 (sum=7, print). Continue to find x=3, y=4; x=4, y=3; etc.

The user wrote three lines of constraint description. The runtime did the search.

### Why continuations are the right substrate

Here's the key insight that makes this work. When `amb(1, 2, 3, 4, 5)` runs, it needs to "remember how to come back and try a different value." That "how to come back" is exactly the continuation: the rest of the program from this point forward, parameterized over the value `amb` returned.

If we have first-class continuations:

1. `amb` captures the continuation.
2. It picks the first option (say 1), pushes the rest (2, 3, 4, 5) plus the continuation onto a "try-next" stack, and resumes with 1 on the data stack.
3. The slice runs. If it calls `fail`, the runtime pops the "try-next" stack: get a saved continuation and a list of remaining options, pick the next option (say 2), resume.
4. If a slice succeeds, the runtime either reports the solution and stops, or treats success as a kind of fail (to enumerate all solutions).

This is the entire engine. Two primitives (`amb`, `fail`), one global "try-next" stack, multi-shot continuations doing the heavy lifting.

### A first implementation in logicforth

```forth
\ The try-next stack lives on the side stack. Each "choice point" stashes:
\   - the list of remaining options
\   - the continuation back into amb's caller

variable choice-depth          \ how many choice points are pending

: amb ( option-list -- choice )
    \ option-list is a list of values to try. amb pushes the first one,
    \ saves the rest plus a continuation to retry, and returns.
    [: ( option-list k )
        \ k is "the rest of the computation, expecting a value on the
        \ data stack." option-list is "values not yet tried."
        over empty? if
            \ no more options — fall back to outer fail
            2drop side> resume  \ resume the next-outer choice point
        then
        uncons              \ option-list -> (first, rest)
        swap >side          \ stash k temporarily
        >side               \ stash rest of options
        >side               \ stash k
        \ first is on top of data stack — that's our chosen value
        side> dup >side     \ peek at k for resume
        swap resume         \ resume with first on data stack
    :] shift-with ;

: fail ( -- )
    \ Pop the most recent saved (k, options) pair. If there are no
    \ untried options, fail the next-outer choice point.
    side-depth 0= if
        ." no solution" exit
    then
    side>               \ pop k
    side>               \ pop options
    \ tail-call amb-style logic to try the next option
    ... ;
```

This is sketchy because the bookkeeping requires care; the real version threads more state than fits cleanly into a few lines of Forth. But the pattern is recognizable.

The skeleton:

- `amb` captures `k`, picks an option, stashes the rest plus `k` on the side stack, resumes with the chosen option.
- `fail` retrieves the most recent saved `(k, options)`, picks the next option, resumes.
- If `fail` has no untried options, it propagates outward to the next-most-recent choice point.

Multi-shot resume is essential: each saved `k` may be invoked multiple times if the slice succeeds and the user asks for more solutions, or if a deeper branch fails after the saved point.

### A worked example: the N-queens problem

Place N queens on an N×N chessboard so that no two attack each other. The constraints: no two in the same row, column, or diagonal.

```forth
\ Returns true if a queen at (r1,c1) attacks (r2,c2).
: attacks? ( r1 c1 r2 c2 -- bool )
    >r >r                       \ stash r2 c2
    r@ = if r> r> 2drop true exit then              \ same row
    swap r@ swap = if r> r> 2drop true exit then    \ same col
    \ diagonal: |r1-r2| == |c1-c2|
    r> swap r> swap             \ now have r1 c1 r2 c2 again
    rot - abs swap - abs = ;

\ Try to place queen i in some column; collect (row, col) pairs in a list.
: place-queens ( i n placed-so-far -- list-of-placements | fails )
    over i >= if exit then      \ all placed; return placed-so-far
    \ choose a column for queen i
    1 2 3 4 5 6 7 8 list amb    \ for 8x8 board, choose column 1-8
    \ check it doesn't attack any previously placed queen
    ...
    \ recurse to place queen i+1
    place-queens ;
```

Pseudo-code, not real Forth, but the structure is the point: the search is described as

> for each queen, choose a column; check non-attack against previous; recurse.

The `amb` call introduces nondeterminism. The constraint check uses `fail` when violated. The recursion threads through choice points naturally. The runtime explores the tree.

A naive imperative solution would have nested loops, an array of placements, manual backtracking with index decrements. With `amb`/`fail`, the description matches the specification.

### From amb to Prolog: unification and logical variables

`amb`/`fail` gives nondeterministic *choice* and *failure*. Prolog adds two more ideas: *logical variables* and *unification*.

A logical variable is a name that may be bound to a value or may be unbound. Unification is the operation of trying to make two terms equal by binding any unbound variables in them.

```
?- X = 5.
X = 5.

?- foo(X, 3) = foo(7, Y).
X = 7, Y = 3.

?- foo(X, 3) = foo(7, 4).
false.
```

The first unification binds `X` to 5. The second unifies the structures `foo(X, 3)` and `foo(7, Y)`, binding `X = 7` and `Y = 3`. The third tries to make `foo(X, 3)` equal `foo(7, 4)`, which fails on the `3 ≠ 4` mismatch.

Crucially, bindings made during unification must be *backtrackable*. If unification succeeds and the rest of the proof later fails, the bindings need to be undone so other branches can try.

This is the same pattern as `amb`: a piece of state that gets mutated, with an obligation to undo on backtrack. With continuations, the implementation is straightforward.

### Implementing logical variables

A logical variable is a cell with two states: *unbound* (empty) or *bound to a term*. We need three operations:

- `make-var`: allocate a fresh unbound variable.
- `bind`: bind an unbound variable to a value. If the variable is already bound, fail.
- `deref`: follow the chain of bindings to find the value (or terminal unbound variable).

The "fail" outcome of `bind` is critical: if we're trying to unify `X` with `5` and `X` is already bound to `7`, the unification fails. The engine then backtracks to the previous choice point.

Now the key piece: when `bind` succeeds, it records the binding *and stashes an undo entry on a trail*. The trail is a stack of "what to undo." When `fail` causes backtracking, the trail is rolled back to the depth at which the most recent choice point was made.

```forth
\ Each logical variable is an object with a "value" slot; unbound is :unbound.

symbol :unbound

: make-var ( -- var )
    :unbound box ;        \ allocate a box containing :unbound

: deref ( var -- value-or-var )
    dup @ :unbound = if exit then
    @ recurse ;           \ if bound to another var, follow it

: bind ( var value -- )
    \ Bind var to value, recording the old value on the trail.
    over @ :unbound <> if 2drop fail exit then
    over @ trail-push     \ remember old value
    over trail-push       \ remember which var
    swap ! ;              \ do the bind

\ The trail is a side-stack-style structure.
variable trail-depth
\ trail-push and trail-pop manipulate it
```

Now when `amb` captures a choice point, it also records the current trail depth. When `fail` backtracks, it pops the trail back to that depth, undoing every binding made during the failed branch. Then it resumes the saved continuation with the next option.

```forth
\ Augmented amb that snapshots trail depth.
: amb-with-trail ( option-list -- choice )
    [: ( option-list k )
        trail-depth @ >side  \ stash trail depth
        ... rest as before ...
    :] shift-with ;

: fail ( -- )
    side> trail-rewind   \ undo bindings back to saved depth
    side> resume         \ resume continuation with next option
    ... ;
```

The combination of:

- Multi-shot continuations for trying different choices
- Logical variables with a trail
- `amb` snapshotting trail depth, `fail` rewinding the trail

…is the heart of a Prolog implementation. Real Prolog adds many features (indexing, cut, negation-as-failure, definite clause grammars, constraint logic programming) but the engine *core* is exactly this: a few hundred lines on top of continuations.

### Unification as a function

Unification is a recursive procedure that walks two terms, binding logical variables to make the terms equal. The pseudocode:

```
unify(t1, t2):
    t1 = deref(t1)
    t2 = deref(t2)
    if t1 is an unbound var:
        bind(t1, t2)
    else if t2 is an unbound var:
        bind(t2, t1)
    else if t1 and t2 are both atoms:
        if t1 == t2: succeed
        else: fail
    else if t1 and t2 are both compound terms:
        if functor(t1) ≠ functor(t2): fail
        if arity(t1) ≠ arity(t2): fail
        for each argument pair (a1, a2): unify(a1, a2)
    else:
        fail
```

Note how `fail` is used as a control primitive throughout. It's not "return an error"; it's "backtrack." The unification function doesn't explicitly handle the backtracking — `fail` does it implicitly via the continuation machinery.

This is the elegance of the continuation-based approach: components compose without tangling control flow through return codes. Each piece can call `fail` whenever it sees a problem, and the runtime takes care of getting to the right backup point.

### Rules and clauses

A Prolog rule has the form `head :- body`. To prove the head, you must prove each goal in the body. To find a rule that proves a particular query, you unify the query with each rule's head; for each match (using `amb` over all matching rules), you recursively prove the body.

```forth
\ A "clause" is a pair: head-pattern and body-goals.
\ The database is a list of clauses.

: prove ( goal -- )
    \ Find all clauses whose head unifies with goal.
    \ For each, prove the body with the bindings.
    database matching-clauses amb   \ choose a clause
    dup head unify-with-goal        \ may fail
    body prove-all ;                \ prove each goal in the body

: prove-all ( goals -- )
    \ Prove each goal in turn; if any fails, the whole thing fails.
    empty? if exit then
    uncons swap prove prove-all ;
```

This is the Prolog interpreter, in pseudo-Forth. About 20 lines. Real Prolog implementations are more sophisticated for performance (clause indexing, last-call optimization, WAM-style instruction sets), but the conceptual core is this small.

### Cut: pruning the search

Sometimes you want to *commit* to a choice. Once you've found a particular clause that matches, you don't want to backtrack into other clauses. Prolog has `!` (cut) for this.

Cut is straightforward with continuations: it removes saved choice points up to a specified depth. "Cut" means "discard all choice points back to the most recent rule's entry point." When `fail` is later invoked, it skips past the cut and resumes at an even earlier choice point.

```forth
: cut-to-rule-entry ( -- )
    \ Remove saved continuations from the side stack down to the marker
    \ left when the current rule started.
    rule-entry-mark begin
        side-depth over <= if drop exit then
        side-drop
    again ;
```

Each rule entry marks the side stack depth. `cut` removes everything above that mark.

Cut is controversial in logic programming because it breaks the purely declarative model — it introduces operational concerns. But for performance and for expressing certain patterns (like `if-then-else`), it's indispensable.

### Negation as failure

Another Prolog idiom: `\+ G` succeeds if proving `G` fails. With continuations:

```forth
: negation ( goal -- )
    [: prove drop-all-solutions :]
    [: success-means-failure :]
    try-catch ;
```

Sketch: try to prove `goal`. If it succeeds (we got at least one solution), the negation fails. If proving `goal` fails (no solutions exist), the negation succeeds.

The control flow inversion is awkward in any imperative implementation but trivial with delimited continuations: each leaf of the search tree is a separate continuation invocation, and you can decide based on whether any of them succeeded.

### Search strategies

The simplest implementation does depth-first search: try a branch all the way, fail back, try the next. This is what Prolog does, and it can be unfair (infinite loops in one branch prevent exploration of others) but it's simple and stack-efficient.

With continuations, alternative strategies are easy. Saved continuations are first-class values; you can store them in any data structure and pick them out in any order.

- **Breadth-first**: instead of a stack of choice points, use a queue. Adds the same exploration but level-by-level. Fair, but uses more memory.

- **Iterative deepening**: depth-first with a depth limit, repeatedly increased. Combines fairness with memory efficiency.

- **Best-first**: priority queue of choice points, ranked by some heuristic. Branch-and-bound, A*, beam search — all just particular policies for which saved continuation to resume next.

The mechanism doesn't care about the strategy. Different policies give different exploration orders.

### Reusing continuations: streams of solutions

A Prolog query may have multiple solutions. The user wants to see them one at a time, asking for "more" between each.

This is the same as a coroutine that yields each solution and suspends:

```forth
: solve-all ( goal -- )
    [: prove print-solution fail :]      \ try to prove, print, then force backtrack
    [: drop ;]                            \ when fail propagates out, we're done
    try-catch ;

: solve-next ( -- solution | done )
    [: prove yield-solution :]
    \ if prove succeeds, we yield the solution
    \ if it fails (all branches exhausted), we report "done"
    ... ;
```

`yield-solution` is `shift` — capture the continuation (which is "the rest of the search, ready to find the next solution") and bubble it out. The user gets one solution at a time. When they ask for the next, they `resume` the saved continuation, which calls `fail` in the prove engine, which backtracks to find the next solution.

This unification of coroutines and backtracking is a striking property of the continuation substrate: they're the same thing seen from different angles. A search-with-multiple-solutions *is* a generator. The generator's `yield` is the search engine's "we found a solution; pause." The driver's `resume` is "force backtrack to find the next."

### Constraint logic programming

CLP extends logic programming with arithmetic and other constraints over various domains (integers, reals, intervals, finite sets). Variables can have *partial* values — "X is between 1 and 10" — that get refined as constraints are added.

Implementing CLP on top of `amb`/`fail` and logical variables requires one extra piece: *constraint propagation*. When a new constraint is posted, the runtime checks consistency with existing constraints and may infer new bindings or detect inconsistency (failure).

The continuation machinery handles the search exactly as before. What's new is the *state* — constraint stores need to be backtrackable like the trail of bindings was. Adding entries to a constraint store records a trail entry; on backtrack, the entries are removed.

Same pattern, same primitives. Multi-shot continuations + backtrackable state + a domain-specific solver.

### Why this is more than a parlor trick

Logic programming, by itself, is a niche tool. But the patterns generalize.

*Constraint solvers*. SAT solvers, SMT solvers, theorem provers, and type-checkers all do tree search with backtracking. Implementing them on top of continuations gives you natural support for incremental search, multi-shot exploration, and clean separation of search policy from search mechanics.

*Probabilistic programming*. Languages like Church, Anglican, and Pyro express probabilistic models as ordinary programs that "sample" from distributions. Inference proceeds by running the program many times, accumulating statistics. Each run is a captured continuation; resampling means resuming a captured point with a different random draw. Continuations are the natural substrate.

*Game-tree search*. Minimax, alpha-beta, MCTS — all are tree explorations with backtracking and policy. Same machinery.

*Parser combinators with backtracking*. A parser tries alternatives at choice points; on failure, it backtracks and tries another. Same machinery.

*Theorem provers*. Tableaux-based and resolution-based provers explore proof trees. Same machinery.

In each case, what we wrote is recognizable as a special case of "search with multi-shot continuations." The shared infrastructure is the continuation primitives plus a backtrackable state mechanism. Everything else is policy.

### The bigger philosophical claim

The claim continuation enthusiasts have been making for decades — and which the logic engine demonstrates concretely — is that *control flow itself is a domain that should be programmable*. Mainstream languages give you a few fixed patterns: function call/return, exceptions, loops, maybe generators or async/await. Each is hardcoded; if you want something different, you can't get it without modifying the compiler.

Continuations give you the substrate to *build* control patterns. A logic engine is a control pattern. A scheduler is a control pattern. An event loop is a control pattern. A backtracking parser, a state machine generator, an effect handler, an actor system — all are control patterns. With continuations, they're libraries. Without continuations, they're compiler features.

This is why a 700-line Forth interpreter with delimited continuations gives you more expressive control flow than a million-line C++ runtime without them. The C++ runtime has more features. But it doesn't let you express new ones.

---

## Part 15: Green threads

A green thread (sometimes called a fiber or user-space thread) is a thread of control that the runtime manages itself, without involving the OS. M green threads can run on N OS threads, where M is typically much larger than N (sometimes thousands or millions of green threads on a few OS threads).

The defining feature of green threads is that they yield control cooperatively, at well-defined points, rather than being preempted by the OS scheduler at arbitrary instruction boundaries.

Continuations are the foundation.

### The model

A green thread is a captured continuation plus some state (priority, status, perhaps a name). The scheduler is a loop that picks a thread, resumes it, and gets back either a new continuation (thread yielded) or nothing (thread finished).

```forth
\ Sketch of a green-thread runtime in logicforth

\ A thread is just a continuation, optionally wrapped with metadata.
\ The scheduler maintains a queue of ready threads.

variable thread-queue       \ list of ready continuations

: yield   ( -- )
    \ Cooperative yield: capture k, add to queue, switch to next thread.
    [: ( k )
        thread-queue @ k cons thread-queue !    \ append k to queue
        scheduler-pick                          \ pick another thread, resume it
    :] shift-with ;

: scheduler-pick   ( -- )
    \ Resume the next thread in the queue, or exit if empty.
    thread-queue @ empty? if exit then
    thread-queue @ uncons swap thread-queue !
    resume ;

: spawn   ( xt -- )
    \ Wrap xt in a fresh reset, capture it as a starting continuation,
    \ enqueue.
    ... ;

: run-scheduler ( -- )
    begin
        thread-queue @ empty? if exit then
        scheduler-pick
    again ;
```

This is sketchier than the previous patterns because building a full scheduler involves several library-level decisions (how to represent threads, how to handle "main" thread, what happens when a thread finishes, error propagation), but the *core mechanism* is captured by `shift-with` to suspend and `resume` to switch in. All the rest is bookkeeping.

### Comparison with OS threads

OS threads use kernel mechanisms for context switching. Each switch costs ~microseconds and a few kilobytes of stack. The scheduler is preemptive — it can switch you out at any time. Synchronization needs locks because the timing of switches is unpredictable.

Green threads switch in nanoseconds. Their "stack" is the captured continuation, which is just a few cells on the heap. The scheduler is cooperative — switches only happen at `yield` points. This means synchronization is much simpler (you only need to worry about contention at explicit yield points), but it also means a long-running computation that never yields can block the whole runtime.

The right answer depends on workload. Many modern languages now offer both: Go has goroutines (green threads multiplexed onto OS threads), Erlang/Elixir use green processes (with preemption baked in), Java added virtual threads in Project Loom (green threads on the JVM). In each case, continuations or continuation-like primitives are the foundation.

### Channels and message passing

Green threads typically communicate via channels — blocking message queues. A `send` on a channel suspends if the channel is full; a `receive` suspends if it's empty.

```forth
\ A channel is a queue plus two lists of suspended threads:
\   - threads waiting to send (when the channel is full)
\   - threads waiting to receive (when the channel is empty)

: channel-send  ( v ch -- )
    dup channel-full? if
        \ Suspend until someone receives.
        [: ( k )  this-channel-queue-senders @ k cons ... :] shift-with
    then
    channel-push ;

: channel-receive ( ch -- v )
    dup channel-empty? if
        [: ( k )  this-channel-queue-receivers @ k cons ... :] shift-with
    then
    channel-pop ;
```

When a thread suspends, its continuation is parked in the channel's wait list. When a counterpart operation makes the channel non-empty (or non-full), one parked continuation is resumed.

This is the foundation of CSP (Communicating Sequential Processes), the model used by Go, Erlang, and others. It's all just `shift-with` and `resume`.

---

## Part 16: Async I/O

Async I/O is the same pattern as green threads, with one twist: instead of a scheduler that round-robins ready threads, you have an *event loop* that resumes threads when their awaited I/O completes.

```forth
: await   ( pending-io -- result )
    \ Suspend; when the I/O completes, the event loop will resume us
    \ with the result on the data stack.
    [: ( k )
        \ register k with the event loop, tagged with the pending I/O
        swap k pair event-loop-register
    :] shift-with ;

\ User code looks linear:
: fetch-and-process ( url -- result )
    http-get-async        \ returns a pending-io handle
    await                 \ suspend until the response arrives
    parse-json
    process-data ;
```

When `http-get-async` issues the request and returns a handle, `await` suspends the current thread. The event loop polls (or selects, or epolls) the registered I/O handles; when one is ready, it resumes the associated continuation with the I/O result on the data stack. The user code looks completely linear — no callbacks, no Promise chains.

This is async/await as a library. The compiler doesn't need to be modified, the type system doesn't need an `async` keyword, the runtime doesn't need a special task scheduler. It's all `shift-with` and `resume` with a domain-specific event loop.

### Modern reframings

Languages that built async/await without continuations (Python, JavaScript, C#) had to do a lot of compiler work to make code that looks linear into state machines that the event loop can drive. The compiler splits an `async` function at each `await` into a series of states, generates a state-machine struct, threads the event loop through every step. The result is impressive but complex.

Languages that built async/await on top of continuations (Scheme, OCaml 5 with effect handlers, Java with virtual threads) don't need this compiler magic. The code stays linear *and* the runtime handles suspension naturally, because the runtime has continuation support.

This is one of the cleanest cases for why first-class continuations matter: they let library authors build features that, in other languages, require compiler changes.

---

## Part 17: Composing patterns

The real power emerges when you compose these patterns.

### Exceptions inside coroutines

What if a coroutine throws? With our implementation, throw unwinds to the nearest reset. If the coroutine was started by `reset producer`, then throw unwinds to that reset — which means control resumes in *the caller of the reset-containing word*, with the unwinding completed. The coroutine's `k` is gone; the caller sees the exception flag on the data stack.

If the coroutine should handle its own exceptions internally, wrap parts of it in `try-catch`:

```forth
: robust-producer
    [: 1 yield 2 yield (might-throw) yield :]
    [: drop -1 yield :]
    try-catch ;
```

Now if `(might-throw)` throws, the try-catch catches it inside the producer, the handler yields -1 instead, and the producer can continue.

### Coroutines inside exception handlers

A handler can itself spawn coroutines. There's no special interaction — the handler is just code that runs at the catch site, and it can use any primitive including `shift` and `resume`.

### Async I/O with backtracking

You can backtrack across async operations. The captured continuation includes the entire computation, including any in-flight async waits. Resuming the captured continuation re-executes the async waits, which fire the I/O again.

This is dangerous for I/O with side effects (re-issuing a payment, for instance), but for read-only queries (read this file, query this DB) it just works. Combined with caching, it lets you write backtracking search over async data sources.

### Green threads with restart handlers

Each green thread can install its own try-catch. A throw in one thread doesn't affect other threads — the unwind targets the *thread's* reset, not any global one. This is how Erlang's "let it crash" philosophy works in practice: each process (their term for green thread) has its own error containment.

---

## Part 18: Pitfalls and discipline

Continuations are powerful and easy to misuse. A few common pitfalls.

### Captured stale state

A continuation captures the *data stack discipline* expected at resume time, but not the data stack itself. The slice expects certain values to be there when it resumes. If you set up the data stack wrong before calling `resume`, the slice will misbehave silently.

Mitigation: be explicit about each continuation's expected stack effect, especially at the resume site. Treat `k` like a function with a specific signature.

### Side effects on multi-shot resume

Every resume re-executes the slice from the resume point. If the slice has I/O, the I/O happens again. If the slice mutates a global, the mutation happens again.

Mitigation: design your slices to be effect-free wherever possible. When effects are necessary, use transactional state (commit only on success), or restrict to one-shot resumption.

### Memory holds

A captured continuation references everything reachable through the saved frames. If you stash `k` in a global, you're holding a heap structure that includes the entire captured slice's environment.

Mitigation: drop continuations when you no longer need them. The GC can't reclaim them while they're reachable, even if you'd logically "moved past" them.

### Mark id collisions across saves

The mark id counter (`next_mark_id`) is global and monotonic. If you serialize a continuation and load it later, the old mark ids might collide with current ones. Don't reuse continuations across persistence boundaries without renaming marks.

### Escaping the reset

`shift` outside a reset is an error. The capture has no target. The runtime detects this and signals `"shift outside reset"`. But if your code path can reach `shift` without a guaranteed enclosing `reset`, you have a bug. Wrap top-level coroutine drivers in `reset`.

---

## Part 19: A summary table

### Primitives (in C)

| Word | Stack effect | Effect |
|---|---|---|
| `reset` | `( -- )` | Push uniquely-tagged MARK on return stack. |
| `shift` | `( -- k )` | Capture up to nearest MARK, remove MARK and frames above, push k. |
| `shift-with` | `( handler -- ... )` | Like shift, but keep MARK, run handler in outer context, set unwinding flag. |
| `resume` | `( ... k -- ... )` | Re-enter captured slice; effects appear on the data stack. |
| `>side` | `( v -- )` | Push v on side stack. |
| `side>` | `( -- v )` | Pop side stack onto data stack. |
| `side-drop` | `( -- )` | Discard top of side stack. |
| `side-depth` | `( -- n )` | Current side-stack depth. |

### Library words (in lib.l4)

| Word | Stack effect | Built from |
|---|---|---|
| `throw` | `( exc -- )` | `[: drop 1 :] shift-with` |
| `catch` | `( xt -- result 0 \| exc 1 )` | `reset execute 0` |
| `try-catch` | `( normal-xt error-xt -- ... )` | side stack + `catch` + if/else |

### Patterns at a glance

| Pattern | What you write | Built on |
|---|---|---|
| Exception | `throw v` inside `try-catch` | `shift-with` (drop k) |
| Recovery / restart | Handler that calls `resume` on k | `shift-with` (use k) |
| Coroutine yield/resume | `yield = shift`, driver calls `resume` | bare `shift` |
| Generator | Same as coroutine, with values | bare `shift` |
| Backtracking choice | `amb` saves k, `fail` resumes it | `shift-with` (stash k) |
| Green thread | `yield = shift-with(enqueue+pick)` | `shift-with` + scheduler |
| Async/await | `await = shift-with(register-with-event-loop)` | `shift-with` + event loop |
| Cooperative I/O | `read-async`, `write-async`, etc. via await | `shift-with` |
| Logic programming | unification + `amb` + `fail` | `shift-with` (multi-shot) |

---

## Part 20: Where to look

If you want to dig into the C code:

- **`p_reset`** — the simplest primitive.
- **`capture_continuation`** — shared helper for shift and shift-with.
- **`p_shift`** — calls the helper, truncates the return stack including the mark.
- **`p_shift_with`** — calls the helper, keeps the mark, runs the handler, sets unwinding.
- **`p_resume`** — splices captured frames back onto the return stack, runs the nested inner loop.
- **`run_inner`** — the modified loop with unwinding-aware behavior. Look at the top-of-loop check.
- **`OBJECT_CONTINUATION`** — the union arm holding a captured slice.

If you want to read the library code:

- **`src/forth/lib.l4`** — throw, catch, try-catch are defined there in five lines.

If you want to read tests:

- **`tests/45_continuations.l4`** — basic shift / resume patterns, including multi-shot.
- **`tests/47_exceptions.l4`** — every exception case worth knowing about.
- **`tests/48_interactions.l4`** — the interesting interactions: catch inside a captured continuation, handler-resumes (restarts), side stack discipline, GC interactions.

If you want to see the model in action: open the REPL, try the examples in this document, modify them, see what breaks. Continuations are abstract; running them concretely is the fastest way to build intuition.

The mechanism is small. The C-side surface area is a few hundred lines. The library code is a few dozen. What you get for it is the foundation of every nontrivial control structure in modern programming, available as one composable abstraction.