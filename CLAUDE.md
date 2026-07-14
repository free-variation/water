# Water — code conventions

## C (src/c)
- No comments anywhere in .c/.h. Constraints a future change must honor go
  in PLAN.md under "Source invariants".
- Tabs for indentation. One statement per line; one declaration per line.
- Descriptive names, nouns not adjectives: `sorted_vector`, never `sorted`;
  no filler names (result, tmp except swap temps, found, val).
- Counts carry n_: n_rows, n_elements, n_terms.
- Hoist repeated field accesses into named locals before use.
- Blank lines separate paragraphs: guard clauses, allocation+check, result.
- Runtime error messages carry no word prefix — the error trace names the
  failing op ("in sort ← rank") and catch delivers `{ :message :trace }`.
  Lowercase, ASCII only ("expected a vector (nx1 or 1xn); got %dx%d").
  Compile-time diagnostics (compiler.c) keep their construct prefix;
  filename contexts keep the filename.
- Multi-statement macros wrap in do { } while (0) unless they exist to leak
  a binding (LOWER_BOUND pattern).
- Domain files own single-representation kernels; words.c owns
  tag-dispatched words; subsystem-sized dispatch families get their own
  file (indexing.c, superwords.c).
- Nothing on the dispatch hot path: DISPATCH/DISPATCH_REGISTERS/docol and
  the REQUIRE macros gain no instructions.

## C word implementation
- Words are void p_<name>(DISPATCH_ARGS), registered in core.c. Name
  mapping: > becomes _to_ (p_string_to_chars), ? drops (p_has), ! becomes
  _set/_store/_inplace or drops (p_env_set, p_slice_store, p_add_inplace,
  p_set_add); libc collisions take a trailing underscore (p_emit_).
- Words are register-threaded by default: work on chain_ip/chain_sp, open
  with REQUIRE_STACK_DEPTH/ROOM, end DISPATCH_REGISTERS; kernels return
  handles for the word to write into chain_sp. A word may be interp-state
  (POP_*/PEEK_* + push, end DISPATCH(interp), blank line before it) only
  for one of two decidable reasons: it re-enters the interpreter
  (execute_xt/call_step — the stack moves under it), or its tail calls
  push-style shared helpers (binary_op/unary_op/push_quantity). Never by
  convenience or perceived cost. Early success exits dispatch inside the
  branch.
- SYNC_REGISTERS before fail/allocation only in ops that read operands
  from chain_ip (their resume point diverges from the entry spill) or
  that deliberately expose an adjusted sp. A plain word's registers equal
  the spill until its final DISPATCH_REGISTERS, so its body carries no
  syncs — and it must not allocate after writing result slots above sp
  (they sit above the spilled dsp, invisible to GC).
- Float fast path first in polymorphic words: stay in registers, exit via
  DISPATCH_REGISTERS in the if; then SYNC_REGISTERS (sp minus consumed
  operands) and the tag chain.
- Errors: the detector calls fail(interp, ...) then bare return. Callers
  re-check with `if (interp->error_flag) return;` after every fallible
  call. Helpers signal with -1 (handle-producers), 0 (did-it-work), or
  NULL (pointer-producers) — fail was already called; the sentinel only
  stops the caller.
- POP_* for consumed scalars; PEEK_*_AT for heap operands that must stay
  stack-rooted across allocation, committed at the end by dsp adjustment
  or in-place overwrite. Raw Val is x_val, unwrapped is x.
- gc_root_push right after allocation, pop before the final stack write;
  every error path pops before returning; roots never live across
  DISPATCH. Zero a fill-incrementally array before rooting it.
- static by default; only p_* words and water.h API are extern. Helpers
  sit directly above their first user; forward-declare only recursion.
- Word families are SHOUT-CASE macros taking (c_name, "word-name", op),
  instantiations listed immediately below, one per line.
- Quickened specializations guard their assumed tags and demote via
  RETARGET_OP(generic) + MUSTTAIL on mismatch; the generic retargets to
  them at the matching branch. Register each as an internal primitive
  named "(word.tag)" beside the (@i.array) block in core.c — that covers
  the image format and see-compiled. Same operand width as the generic,
  always.
- int for lengths, indices, handles; size_t only as explicit casts at
  malloc/memcpy sites; int64_t to guard overflow before clamping. All
  casts explicit.
- Grow by doubling from a small constant through a checked realloc temp;
  GROW_IF_FULL for arena arrays, GROW_IF_FULL_SYS for malloc-owned.
- Cleanup is inline per error path (free/fclose/finalize/gc_root_pop
  before return); no goto outside image.c.
- Message formats: "expected X; got %s" (literal type phrase); half-open
  ranges "[%d, %d) out of bounds for length %d"; "(max %d)".
- Printing words fflush(stdout) before DISPATCH. Unused params silenced
  with (void)x; at the top.
- Typedefs CamelCase; enum members domain-prefixed SHOUT; kernel context
  structs XxxContext built with designated initializers. File-scope state
  grouped at top, static, domain-prefixed. static inline for hot
  predicates, always_inline only for the hottest water.h helpers,
  MUSTTAIL for tail recursion. Near-duplicate words factor through a
  callback + op-string helper, leaving p_* as two-liners. File-local
  #define tunables sit beside their use; file-private POP_X macros copy
  the water.h family's shape.

## water.h layout (comment-free; this is its table of contents)
1. Guard, VERSION, includes, cell typedef.
2. Capacity constants grouped by subsystem: dictionary/pools, stacks/
   locals, GC/arena, workers, logic, regex/JSON, databases, trace/print.
3. Value model: Tag, Val, NaN-box macros, make_* constructors.
4. Object model: ObjectKind, Object, segment inlines, Pair.
5. Memory: HandleSpace, PairPool, Arena, alloc contexts, GROW_*/OBJECT_AT
   /MAT macros.
6. Program state: QuotationSpan, Vocabulary, Interpreter, Compiler,
   DISPATCH_ARGS, cfa_handler, WORD_* macros, CallContext, HelpEntry.
7. Word-body macros: POP_*, NEW_*, PEEK_*, FRAME_LOOKUP, LOWER_BOUND.
8. Declarations grouped by owning .c file in SRCS order, alphabetical
   within a file: API functions first, then all p_* words.
9. Tail: inline functions whose bodies call declared functions (push/pop,
   rpush/rpop, gc_root_*, truthy, frame_walk, call_step).
New constant → its subsystem cluster in 2. New type → lowest layer that
can express it. New declaration → its file's block, alphabetical slot. A
new inline that calls functions → the tail.

## Forth (src/forth, lib/, tests)
- The embedded library is src/forth/*.h2o, concatenated in FORTH_SRCS
  order (Makefile) and burned into the binary. Binding is early, so a
  word must be defined in an earlier file (or earlier in the same file)
  than every use; changing the order changes what compiles. A new
  embedded word goes in the file owning its domain, and its reference
  row's prefix names that file. lib/ holds the loadable libraries.
- Stack-effect comment line above each definition: ( a b -- c ) \ summary.
- Markers postfix after ; — inline, internal.
- Plumbing words are marked internal.
- C escape hatches are parenthesized primitives wrapped by the public
  word: `: wall-now (wall-now) s ;`. Fully-parameterized primitives carry
  -ext, wrapped by a defaulting word.
- Lib overrides a C word by redefinition (statistics.h2o's dgemm-*).
  Binding is early: earlier compilations keep the old target; a
  self-reference recurses, so capture the old xt with `'` first if
  needed.
- LAPACK-free stats live in the embedded library (wasm-capable);
  statistics.h2o accelerates one by redefining it in toto (early
  binding makes partial masking useless). The word's golden runs native
  (masked) and wasm (unmasked) and must agree, pinning both copies.
- Locals: `>name` receives from the stack at entry, bare names are
  uninitialized scratch; quotations receive with `|> a b |`. Counter
  loops: `0 to i begin i n lt while ... f++ i repeat`.

## Docs and generated files
- PLAN.md is a focused list of FUTURE WORK, written imperatively: no
  completed work, no status narration, no discussion of present or past
  state. When a feature lands, its entry shrinks to the residuals or
  vanishes.
- README describes the language as it is: terse present-tense capability
  statements, one line per feature. No past narratives, no measurements,
  no reflections or design history — detail belongs in docs/reference.md.
- docs/reference.md is the source of truth: gen-help.py (help table,
  automatic in make) and gen-editors.py (make editors) consume it. Never
  hand-edit help_table.c, repl_highlight_groups.h, editors/.
- Every word gets a reference.md row; the words canary group
  "undocumented" must stay empty.
- New words: reference row, README mention if user-facing, golden test,
  wasm suite run.

## Tests
- Golden pairs in tests/ (see run.sh); regenerate with ./water -b <
  tests/NNN_name.h2o > tests/NNN_name.expected — inspect every changed
  line before accepting.
- Seeded RNG for anything random; both native and wasm suites must pass.
- Header comment names the word, stack effect, semantics. Sections split
  with `\ === title ===`. Every output line carries an aligned trailing
  expected-value comment. Error cases grouped at the end with the reason
  parenthesized. `clear` between sections.
