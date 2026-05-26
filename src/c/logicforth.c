/* logicforth.c — an indirect-threaded Forth, with tagged values, symbols,
 *             strings, sets, and arrays. ===================================
 *
 *
 *                                 PART ONE
 *                         WHAT THE CORE LANGUAGE IS
 *
 * Forth is a programming language built on three simple ideas: a stack, a
 * dictionary, and a pair of cooperating interpreters. To run a program you
 * type tokens, separated by whitespace. The system reads each token in
 * turn. If the token names something in the dictionary, the system runs the
 * thing it names. If the token parses as a number, the system pushes the
 * number onto the stack. That is the whole language at this level — there
 * are no expressions, no operator precedence, no syntax in the conventional
 * sense. Operations get their arguments from the stack and leave their
 * results there:
 *
 *     2 3 + .          (push 2, push 3, add, print top)  → 5
 *
 * The "dictionary" is a linked list of named entries called "words". Some
 * words are primitives — their behavior is implemented directly in C. The
 * arithmetic and stack-manipulation operations are like this. Other words
 * are "colon definitions" — sequences of references to other words. The
 * user creates them with the defining syntax : name ... ;. For example:
 *
 *     : square dup * ;
 *     5 square .       → 25
 *
 * SQUARE is now in the dictionary, and its body is a tiny program that
 * duplicates the top of the stack and multiplies. The job of the system is
 * to walk such a body, calling each referenced word in turn.
 *
 *
 * THE TWO INTERPRETERS
 * --------------------
 * The "outer" interpreter reads source tokens and decides what to do with
 * each. The "inner" interpreter walks the body of a colon definition,
 * executing the references inside. The outer interpreter only runs while
 * we're reading user input; the inner interpreter only runs while we're
 * inside a colon body. They cooperate at the boundary: when the outer
 * interpreter encounters a colon definition's name, it kicks the inner
 * interpreter into motion; when the body finishes, the inner interpreter
 * unwinds and control returns to the outer.
 *
 * The outer interpreter has a second mode: "compile mode", entered by :
 * and exited by ;. In compile mode, instead of executing what the tokens
 * name, it compiles references to them into the body of the new word.
 * Numbers get compiled as literals; word names get compiled as references.
 * This is how : square dup * ; actually builds the body of SQUARE:
 * during compilation, the outer interpreter writes references to DUP and
 * * and an EXIT marker into the dictionary.
 *
 *
 *                                 PART TWO
 *                          THREADED CODE AND ITC
 *
 * Once we accept that a colon definition's body is a list of references to
 * other words, the question is: what kind of reference, and how does
 * execution step through them? This is "threaded code" — the body is a
 * "thread" that the inner interpreter follows. There are four classical
 * answers, differing in what the body contains and how dispatch works:
 *
 *     Direct-threaded (DTC).   Each body cell is a machine code address.
 *                              The inner interpreter jumps to it. Often
 *                              the fastest; not portable in C.
 *
 *     Subroutine-threaded.     Each body cell is a real CALL instruction.
 *                              No software inner interpreter needed; the
 *                              CPU's call/return mechanism does every-
 *                              thing. Largest bodies; very fast on CPUs
 *                              with deep return-stack predictors.
*
*     Token-threaded.          Each body cell is a small integer indexing
*                              a dispatch table. Most compact bodies,
	*                              which gives it good i-cache behavior;
*                              roughly comparable to ITC in per-step
*                              cost on modern hardware.
*
*     Indirect-threaded (ITC). Each body cell is a pointer to a "code
*                              field" cell. Each word has a code field
*                              whose value is the address of a handler.
*                              One extra dereference per step. Same
*                              ballpark as token-threaded.
*
* This implementation uses ITC. The extra indirection buys us uniformity:
* every word — primitive, colon definition, variable, constant, whatever —
* has the same shape. A name and header up front, a code field, and then
* (for non-primitives) a body. The compiler doesn't have to know which
* kind it's emitting a reference to; it just writes a pointer to the code
* field. The kind of word manifests through which handler lives in that
* code field:
*
*     · A primitive's code field holds a function pointer to a C function
*       that performs the primitive. When the inner interpreter dispatches
*       through that code field, the C function runs and returns.
*
*     · A colon definition's code field holds the address of a single
*       shared handler called DOCOL. DOCOL saves the current
*       instruction pointer on the return stack, then redirects it to the
*       body. The inner interpreter then walks the body. When the body
*       ends, an EXIT primitive restores the saved instruction pointer.
*
*     · A variable's code field holds the address of DOVAR, which pushes
*       the address of the cell immediately after the code field.
*
* Other kinds of words are built by the same pattern: give them a header,
	* a code field with a handler appropriate to that kind, and any body data
	* the handler needs.
	*
	*
	*                                PART THREE
	*                            DICTIONARY MEMORY
	*
	* The entire dictionary — headers, code fields, bodies — lives in one flat
	* array of cells, DICT[]. There is no separate heap and no use of malloc
	* for dictionary storage. A pointer HERE marks the next free cell;
	* defining a new word advances HERE.
	*
	* Each word's header sits immediately before its code field. The layout,
	* with positions given relative to the code field address ("CFA"):
	*
	*     dict[cfa - 4]   link       index of the previous word's CFA, or 0
	*     dict[cfa - 3]   flags      bit 0 = immediate
	*     dict[cfa - 2]   name idx   offset into the byte pool NAMEPOOL[]
	*     dict[cfa - 1]   src idx    offset into SOURCE_POOL[], or 0
	*     dict[cfa    ]   code field function pointer cast to a cell
	*     dict[cfa + 1]   body...    optional, depending on the word's kind
	*
	* "CFA" is short for "code field address" and is the conventional way to
	* refer to a word. To get to the header you subtract; to get to the body
	* you add one. References stored in the body of a colon definition are
	* pointers — CELLs holding &dict[other_cfa]. The inner interpreter
	* reads one of these, dereferences it to get the handler, and calls it.
	*
	* The src idx field is what enables image save: when a colon definition
	* is compiled, the original body source is captured and stashed in
	* SOURCE_POOL[], and this field points at it. Non-colon words (primi-
			* tives, variables, symbols) carry 0 here. See "image save/load" near
	* the end of the file for details.
	*
	* The dictionary as a whole is a singly-linked list threaded backward
	* through the link fields. The variable LATEST_CFA points to the most
	* recent definition. Searching by name (FIND) walks backward to the
	* start of the dictionary. Because new definitions are prepended, a redef-
	* inition shadows the old version — but the old version is still in
	* memory and any compiled references to it still call it.
	*
	*
	*                                 PART FOUR
	*                           WHAT THIS FORTH ADDS
	*
	* The core ITC engine described above is small. On top of it we layer:
	*
	*     · Tagged values on the stack. Every stack item carries a small tag
	*       indicating its kind (number, symbol, string, set, array, etc.) and
	*       8 bytes of payload. Operators like + and = dispatch on the
	*       tags of their operands. This replaces classical Forth's untyped
	*       stack with something closer to Lisp's discriminated runtime values.
	*
	*     · One numeric type: double-precision float. There is no integer.
	*       3 and 3.0 are the same value. This loses exactness for very
	*       large integers and for set membership of computed floats, but
	*       removes the int/float duality and all its promotion rules.
	*
	*     · First-class symbols. The defining word symbol foo creates a
	*       new word FOO whose runtime behavior is to push a symbol value
	*       that identifies itself. Symbols compare by identity and print as
	*       their names. They're inert atoms — they evaluate to themselves.
	*
	*     · First-class strings. The literal "hello" pushes a string value.
	*       Strings may span lines. They support {n}-style interpolation
	*       against the stack at the moment the literal is encountered.
	*
	*     · First-class mutable sets. The literal { a b c } builds a set.
	*       Sets are sorted internally so membership tests are binary
	*       searches. Duplicate adds are silently ignored. Operations: union,
	*       intersection, difference, member?, cardinality.
	*
	*     · First-class fixed-size arrays via [ 1 2 3 ].
	*
	*     · Polymorphic operators. + adds numbers, concatenates strings,
	*       and unions sets. = compares within compatible types.
	*
	*     · ' (tick) parses the next token at compile or interpret time and
	*       pushes its execution token, enabling higher-order operations like
	*       MAP.
	*
	*     · WORDS lists the dictionary; FORGET discards a word and
	*       everything defined after it.
	*
	* The extensions are layered on without changing the core. The inner
	* interpreter still reads code-field-pointers and dispatches. The
	* dictionary is still a linked list. Strings and sets live in a separate
	* heap-allocated registry because they don't fit in one cell; the stack
	* holds small integer handles to them. The body of a colon definition
	* still holds raw cell-sized references — for tagged-literal values, we
	* use two consecutive raw cells (one for tag, one for the 8-byte payload).
	* ============================================================================
	*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>

	/* A raw dictionary cell. 64-bit because we want to store function
	 * pointers, doubles, and indices without size-related hand-wringing. */
	typedef int64_t cell;


	/* ---- tagged values: the stack representation -------------------------- */
	/* Every item on the data and return stacks is a tagged Val: a small tag
	 * plus an 8-byte payload. Operators inspect the tag to decide what to do.
	 * Doubles live in the payload via memcpy — this avoids any aliasing
	 * trouble and works the same on every machine. Handles (for objects in
	 * the object registry) are small integers stored directly. */

	/* Ordering matters: val_cmp uses the numeric Tag value as the cross-type
	 * tie-breaker when comparing values of different kinds. The sequence
	 * below goes from conceptually simplest to most complex — atoms first
	 * (T_SYM is just an identity), then scalars (T_FLOAT carries arithmetic
	 * semantics), then text (T_STRING is variable-length bytes), then
	 * collections, then internal/reserved types. The specific numbers are
	 * never inspected outside this comparison, so the ordering choice is
	 * cosmetic: it determines how heterogeneous sets sort and how mixed
	 * stacks display. */
	typedef enum {
		T_NONE = 0,
		T_SYM,        /* symbol; payload = CFA of the defining word            */
		T_FLOAT,      /* the only numeric type                                 */
		T_STRING,     /* string; payload = index into objects[]                */
		T_SET,        /* set; payload = index into objects[]                   */
		T_ARRAY,      /* array; payload = index into objects[]                 */
		T_MATRIX,	  /* matrix: payload = index into objects[]				  */
		T_XT,         /* execution token; payload = CFA                        */
		T_ADDR,       /* dictionary address; payload = cell index into dict[]  */
		T_CONT,		  /* delimited continuation; payload = index into objects[] */
		T_MARK        /* internal marker used by { } and [ ] collection */
	} Tag;

typedef struct { Tag tag; int64_t data; } Val;

static double unpack_float(Val packed)  { double number; memcpy(&number, &packed.data, 8); return number; }
static Val make_float(double number)    { Val value; value.tag = T_FLOAT;  memcpy(&value.data, &number, 8); return value; }
static Val make_sym(int cfa)            { Val value; value.tag = T_SYM;    value.data = cfa; return value; }
static Val make_string(int handle)      { Val value; value.tag = T_STRING; value.data = handle; return value; }
static Val make_set(int handle)         { Val value; value.tag = T_SET;    value.data = handle; return value; }
static Val make_array(int handle)       { Val value; value.tag = T_ARRAY;  value.data = handle; return value; }
static Val make_matrix(int handle)      { Val value; value.tag = T_MATRIX; value.data = handle; return value; }
static Val make_xt(int cfa)             { Val value; value.tag = T_XT;     value.data = cfa; return value; }
static Val make_addr(int cell_index)    { Val value; value.tag = T_ADDR;   value.data = cell_index; return value; }
static Val make_continuation(int handle){ Val value; value.tag = T_CONT; value.data = handle; return value; }
static Val make_mark(void)              { Val value; value.tag = T_MARK;   value.data = 0; return value; }

/* Static footprint. Sized generously — total cost is around 10 MB,
 * irrelevant on modern hardware, but it leaves headroom for workloads
 * that compile, run, and discard large numbers of definitions. */
#define MEMSZ      1048576    /* dictionary cells (8 MB at 8 bytes each) */
#define NAMEPOOL   32768      /* word-name bytes                          */
#define DSTACK     256        /* data stack depth (Forth tradition: shallow) */
#define RSTACK     256        /* return stack depth                       */
#define MAXOBJS    65536      /* live string/set/array/matrix slots              */
#define INBUFSZ    16384      /* max source bytes per single :…;      */
#define SOURCEPOOL 1048576    /* total captured body-source storage (1 MB) */
#define SYMBOLPOOL 32768      /* interned symbol names — never rolled back  */


/* ---- universe of state ----------------------------------------------- */

/* dict[]: the dictionary. A flat cell array growing upward. It holds word
 * headers, code-field function pointers, and the bodies of colon
 * definitions. HERE is the next free cell index. */
static cell dict[MEMSZ];
static int  here = 0;

/* Word names live separately in a packed byte pool. Headers store an
 * offset into this pool rather than the bytes inline; this keeps the
 * dictionary cell-aligned. */
static char namepool[NAMEPOOL];
static int  names_here = 0;

/* Captured body source text for every colon definition, stored as
 * null-terminated strings packed into one byte pool. Each header's
 * SRCIDX field is the offset of that word's source within this pool,
 * or 0 if the word has none (primitives, variables, symbols). We start
 * SOURCES_HERE at 1 so that offset 0 is reserved as the "no source"
 * sentinel — source_pool[0] stays a null byte forever as a defensive
 * empty string.
 *
 * This pool exists for one purpose: it lets SAVE emit a colon
 * definition back out as the exact text the user originally typed,
 * which is what makes the saved image human-readable and re-editable
 * without any decompilation. */
static char source_pool[SOURCEPOOL];
static int  sources_here = 1;

/* Interned symbol names. Unlike namepool and source_pool, this pool is
 * NEVER rolled back by FORGET — symbol identity needs to survive
 * dictionary churn so that a T_SYM value created before a forget still
 * compares equal to a fresh symbol foo defined after.
 *
 * intern_symbol(name) returns the offset of an existing entry with
 * the same bytes if one exists, otherwise appends and returns the new
 * offset. Two symbol foo declarations therefore produce the same
 * T_SYM payload. */
static char symbol_pool[SYMBOLPOOL];
static int  symbol_pool_here = 0;

/* While compiling a colon definition, this holds the inbuf offset at
 * which the body source begins — set by : (p_colon) right after the
 * name token has been consumed, and read by ; (p_semi) to slice out
 * the body bytes and copy them into source_pool. Cleared on every
 * compile termination (normal ;, error, or REPL reset). The fact that
 * inbuf is *not* reset between lines during a multi-line compile is
 * what lets this offset stay valid across fgets calls — see the main
 * loop. */
static int  compiling_src_start = 0;

/* The outer interpreter's input buffer. The REPL appends fgets'd lines
 * here and RUN_OUTER() walks through them token by token. Declared up
 * here (rather than down beside the tokenizer) so the colon/semi pair
 * can reference inbuf_pos directly when capturing body source. See "input
 * buffer and tokeniser" further down for the rest of the machinery. */
static char inbuf[INBUFSZ];
static int  inbuf_len = 0;
static int  inbuf_pos = 0;
static int  need_more = 0;

/* The data stack. All arithmetic, comparison, I/O, and the immediate-
 * word back-patching mechanism push and pop tagged Vals from here. */
static Val data_stack[DSTACK]; 
static int dsp = 0;

/* The return stack. The inner interpreter uses it to remember where to
 * resume after a colon definition finishes. The user can also push to it
 * via >r and pop back via r>. These two roles share one stack by Forth
 * tradition; the user must keep >r and r> balanced within a word. */
static Val return_stack[RSTACK];
static int rsp = 0;

/* Side stack: a third stack used purely for storage. Holds arbitrary
 * Vals without the data stack's interference with results or the
 * return stack's tag-interpretation by execution machinery. Accessed
 * via >side / side> / side-drop / side-depth. GC marks contents as
 * roots. */
#define SIDESTACK_DEPTH 256
static Val side_stack[SIDESTACK_DEPTH];
static int side_dsp = 0;

/* The instruction pointer used by the inner interpreter. It always points
 * at the NEXT body cell to be processed. Modified by DOCOL, P_EXIT,
 * the branch primitives, and the trampoline mechanism described below. */
static cell *ip = NULL;

/* Heads of the dictionary's linked list and the state of the outer
 * interpreter (compile mode vs. execute mode). */
static int  latest_cfa = 0;
static int  compiling = 0;

/* Two snapshots of bootstrap state, both captured during main():
 *
 *   initial_*   — taken at the end of primitive registration, *before*
 *                 lib.l4 loads. Used by SAVE-IMAGE / LOAD-IMAGE:
 *                 the binary image considers lib.l4 contents part of
 *                 the user state, so they round-trip through the image.
 *
 *   lib_end_*   — taken right after lib.l4 finishes loading. Used by
 *                 the text SAVE: a vocabulary dump skips lib.l4 since
 *                 the next interpreter loads it automatically, so re-
 *                 emitting it would just bloat the file and create
 *                 redundant shadowed definitions on reload. */
static int  initial_here              = 0;
static int  initial_latest_cfa        = 0;
static int  initial_names_here        = 0;
static int  initial_sources_here      = 0;
static int  initial_symbol_pool_here  = 0;

static int  lib_end_latest_cfa        = 0;

/* Session-level history of files loaded via LOAD. RELOAD truncates
 * state back to initial_* and re-runs every entry in order. Recording
 * happens only at the outermost call (load_depth == 0) so that files
 * loaded indirectly by other files don't pollute the list. */
#define MAX_LOADED_FILES 64
static char *loaded_files[MAX_LOADED_FILES];
static int   n_loaded_files = 0;
static int   load_depth = 0;

/* Sticky error flag; reset at the top of each input chunk. */
static int  error_flag = 0;

/* Cleared by the EXIT primitive when it sees an empty return stack; this
 * breaks the inner-interpreter loop and returns control to the outer. */
static int  running = 0;

/* Delimited-continuation unwinding state. shift-with sets unwinding=1
 * and stores the target mark's id in unwind_target. Each run_inner
 * level checks the flag at the top of its loop; if set, it pops one
 * frame, and if that frame is the matching MARK it clears the flag
 * and exits its loop. Otherwise the level either continues popping
 * (if the mark is above its initial_rsp) or breaks to propagate
 * upward. */
static int unwinding = 0;
static int unwind_target = 0;
static int next_mark_id = 1;

/* Header accessor macros. The four header cells sit just before each
 * CFA in memory order; the offsets here match the layout diagram up at
 * the top of the file. Adding a new field means picking another negative
 * index — bumping the size constant in create_header — and updating
 * FORGET and the GC's dictionary walker to keep their body-boundary
 * arithmetic in sync. */
#define LINK(cfa)    dict[(cfa) - 4]
#define FLAGS(cfa)   dict[(cfa) - 3]
#define NAMEIDX(cfa) dict[(cfa) - 2]
#define SRCIDX(cfa)  dict[(cfa) - 1]
#define IS_IMM(cfa)  (FLAGS(cfa) & 1)

/* Forward decl: FAIL reports an error message and sets the sticky
 * error_flag. Defined alongside type_error down with the other
 * error-reporting helpers, but used much earlier (in object allocators,
 * stack push/pop, dictionary growth checks). printf-style formatting. */
static void fail(const char *fmt, ...);


/* ---- object registry -------------------------------------------------- */
/* Strings, sets, and arrays don't fit in a single 8-byte cell, so they
 * live in heap-allocated Object structs. The stack carries small integer
 * handles (indices into objects[]). This separation lets the dictionary
 * remain a uniform cell array while permitting variable-size first-class
 * compound values.
 *
 * Strings, arrays, and sets are all value-semantic from the user's
 * perspective: no exposed word mutates an existing collection. Every
 * operation that appears to "change" a set or string — concatenation,
 * union, intersection, difference, map — builds a fresh Object and
 * returns its handle. The internal helpers (set_add, set_member,
 * realloc on items growth) do mutate, but they're only invoked while
 * constructing a brand-new collection that no other handle yet refers
 * to. Two stack copies of the same handle therefore always observe the
 * same contents over their entire lifetime. */

typedef enum { OBJECT_STRING, OBJECT_SET, OBJECT_ARRAY, OBJECT_MATRIX, OBJECT_CONTINUATION } ObjectKind;
typedef struct {
	ObjectKind kind;
	int len, cap;
	/* A tagged union: exactly one of the storage pointers is live at a
	 * time, selected by KIND. The anonymous-union form (C11) keeps
	 * existing access syntax — obj->bytes for OBJECT_STRING, obj->items for
	 * OBJECT_SET/OBJECT_ARRAY — while making it explicit at the type level that
	 * the two pointers share storage and only one is meaningful. */
	union {
		char *bytes;     /* OBJECT_STRING */
		Val  *items;     /* OBJECT_SET, OBJECT_ARRAY */
		struct {
			int rows;
			int columns;
			double *elements;
		} matrix;
		struct {
			Val *return_slice;
			int return_len;
			int resume_ip;
		} continuation;
	};
} Object;

/* Element access for OBJECT_MATRIX. Both macros take an Object* (not a
 * handle) — the caller is expected to have dereferenced once already.
 * Row-major layout: element (i, j) lives at elements[i * columns + j].
 *
 * MAT(m, i, j) is an lvalue — usable on both sides of =:
 *     double x      = MAT(m, i, j);
 *     MAT(m, i, j)  = 1.0;
 *
 * No bounds checking — these are tight inner-loop helpers. Caller must
 * have validated indices. */
#define MAT(m, i, j) ((m)->matrix.elements[(i) * (m)->matrix.columns + (j)])

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static Object *objects[MAXOBJS];
static int  n_objects = 0;

/* GC roots beyond the two stacks and the dictionary: a small fixed stack
 * that C-level primitives use to keep heap objects alive across calls
 * that might trigger collection. MAP is the canonical example — it
 * allocates an intermediate result set whose handle is only held in a C
 * local; without rooting, a GC triggered by the user's xt would free the
 * partially-built result and leave us writing into a dangling Object. */
#define GC_ROOTS_MAX 16
static Val gc_roots[GC_ROOTS_MAX];
static int n_gc_roots = 0;

static void gc_root_push(Val value) {
	if (n_gc_roots < GC_ROOTS_MAX) gc_roots[n_gc_roots++] = value;
}
static void gc_root_pop(void) { if (n_gc_roots > 0) n_gc_roots--; }

/* The collector itself is defined later in the file — it needs to know
 * about the inline-data primitives (lit, branch, dostr) to walk colon
 * bodies, and their CFAs are declared after this section. */
static void gc(void);

/* Pick a slot in objects[]. Three tiers, cheapest first:
 *
 *   1. The high-water mark N_OBJECTS hasn't hit MAXOBJS yet — just take
 *      the next slot. O(1), the common case.
 *
 *   2. n_objects is at the cap, but a previous sweep left NULL holes. Scan
 *      to find one. O(MAXOBJS), but no GC churn.
 *
 *   3. No holes either — only now do we actually run the collector,
 *      then re-scan. GC is the last resort because it's stop-the-world:
 *      it walks the data stack, return stack, gc_roots, every header,
 *      and every reachable Object. We don't want to pay that cost while
 *      slots are still freely available.
 *
 * A return of -1 means every slot is genuinely reachable and there's
 * nothing more we can do — the caller turns this into the sticky error_flag
 * flag. */
static int object_alloc_slot(void) {
	if (n_objects < MAXOBJS) return n_objects++;

	for (int i = 0; i < MAXOBJS; i++) if (objects[i] == NULL) return i;
	gc();

	for (int i = 0; i < MAXOBJS; i++) if (objects[i] == NULL) return i;
	return -1;
}

static int object_new_string(const char *bytes, int length) {
	int slot = object_alloc_slot();
	if (slot < 0) {
		fail("object registry full"); 
		return -1; 
	}

	Object *obj = calloc(1, sizeof(*obj));
	obj->kind = OBJECT_STRING;
	obj->len = length;
	obj->cap = length;
	obj->bytes = malloc((size_t)length + 1);
	memcpy(obj->bytes, bytes, (size_t)length); obj->bytes[length] = 0;

	objects[slot] = obj;
	return slot;
}

/* Starting capacity for a freshly-built set's items array. Small enough
 * to avoid wasting memory on sets that stay tiny; the doubling growth
 * in set_add (cap *= 2 on overflow) takes over from there. */
#define SET_INITIAL_CAPACITY 4

static int object_new_set(void) {
	int slot = object_alloc_slot();
	if (slot < 0) {
		fail("object registry full"); 
		return -1; 
	}

	Object *obj = calloc(1, sizeof(*obj));
	obj->kind = OBJECT_SET;
	obj->cap = SET_INITIAL_CAPACITY;
	obj->items = malloc(sizeof(Val) * (size_t)obj->cap);

	objects[slot] = obj;
	return slot;
}

static int object_new_array(int num_elements) {
	int slot = object_alloc_slot();
	if (slot < 0) {
		fail("object registry full"); 
		return -1; 
	}

	Object *obj = calloc(1, sizeof(*obj));
	obj->kind = OBJECT_ARRAY;
	obj->len = num_elements;
	obj->cap = num_elements;
	obj->items = malloc(sizeof(Val) * (size_t)MAX(num_elements, 1));

	objects[slot] = obj;
	return slot;
}

static int object_new_matrix(int num_rows, int num_columns) {
	int slot = object_alloc_slot();
	if (slot < 0) {
		fail("object registry full"); 
		return -1;
	}

	Object *obj = calloc(1, sizeof(*obj));
	obj->kind = OBJECT_MATRIX;
	obj->matrix.rows = num_rows;
	obj->matrix.columns = num_columns;
	size_t num_elements = (size_t)(num_rows * num_columns);
	obj->matrix.elements = calloc(num_elements, sizeof(double));
	memset(obj->matrix.elements, 0, num_elements);

	objects[slot] = obj;
	return slot; 
}

static int object_new_continuation(const Val *frames, int return_len, int resume_ip) {
	int slot = object_alloc_slot();
	if (slot < 0) {
		fail("object registry full");
		return -1;
	}

	Object *obj = calloc(1, sizeof(*obj));
	obj->kind = OBJECT_CONTINUATION;
	obj->continuation.return_len = return_len;
	obj->continuation.resume_ip = resume_ip;
	obj->continuation.return_slice = malloc(sizeof(Val) * (size_t)MAX(return_len, 1));
	memcpy(obj->continuation.return_slice, frames, sizeof(Val) * (size_t)return_len);

	objects[slot] = obj;
	return slot;
}


/* ---- stack helpers ---------------------------------------------------- */
/* Bounds-checked push and pop. Errors are sticky: a single bad operation
 * sets ERROR_FLAG, the outer interpreter notices, and the current input is
 * abandoned. */

static void push(Val value)  { if (dsp < DSTACK) data_stack[dsp++] = value; else fail("data stack overflow"); }
static Val  pop(void)        { if (dsp > 0) return data_stack[--dsp]; fail("data stack underflow"); Val none = {T_NONE,0}; return none; }
static void rpush(Val value) { if (rsp < RSTACK) return_stack[rsp++] = value; else fail("return stack overflow"); }
static Val  rpop(void)       { if (rsp > 0) return return_stack[--rsp]; fail("return stack underflow"); Val none = {T_NONE,0}; return none; }

/* Declare-and-pop with built-in error short-circuit. Use only at
 * statement scope (top of function or block), never as a single-
 * statement if-body. The bare RETURN makes it usable only in VOID
 * functions; the one int-returning helper that pops (create_matrix)
 * expands manually with an explicit return -1. */
#define POP(name) Val name = pop(); if (error_flag) return


/* ---- value comparison ------------------------------------------------- */
/* We need a total order over Vals so sets can stay sorted (which makes
 * insertion, membership, and ordered iteration all simple). Tags order
 * each other by their enum value; within a tag, the natural ordering
 * applies. This lets sets contain heterogeneous values without breaking
 * the binary-search invariants in set_add and set_member. */

static int val_cmp(Val left, Val right) {
	/* Cross-type tie-breaker: order by tag value (see the Tag enum
	 * for the chosen sequence). Never zero here, since tags differ. */
	if (left.tag != right.tag) return (int)left.tag - (int)right.tag;

	switch (left.tag) {
		case T_FLOAT: {
						  double left_value  = unpack_float(left);
						  double right_value = unpack_float(right);
						  if (left_value < right_value) return -1;
						  if (left_value > right_value) return  1;
						  return 0;
					  }
		case T_SYM: case T_XT: case T_ADDR:
					  /* Compare by identity / index — for symbols, by the CFA of
					   * the defining word; for xt, by the target word's CFA; for
					   * addr, by the dict[] cell index. */
					  if (left.data < right.data) return -1;
					  if (left.data > right.data) return  1;
					  return 0;
		case T_STRING: {
						   Object *left_string  = objects[left.data];
						   Object *right_string = objects[right.data];
						   int compare_length = MIN(left_string->len, right_string->len);
						   int byte_diff = memcmp(left_string->bytes, right_string->bytes,
								   (size_t)compare_length);
						   if (byte_diff) return byte_diff;
						   /* Same prefix — shorter string sorts first. */
						   return left_string->len - right_string->len;
					   }
		case T_SET: case T_ARRAY: {
									  Object *left_collection  = objects[left.data];
									  Object *right_collection = objects[right.data];
									  int compare_length = MIN(left_collection->len, right_collection->len);
									  for (int i = 0; i < compare_length; i++) {
										  int element_cmp = val_cmp(left_collection->items[i],
												  right_collection->items[i]);
										  if (element_cmp) return element_cmp;
									  }
									  /* All compared elements equal — shorter collection sorts first. */
									  return left_collection->len - right_collection->len;
								  }

		default: return 0;
	}
}


/* ---- set operations --------------------------------------------------- */
/* Sets are kept sorted by val_cmp. Insertion does binary search to find
 * the slot; if the element is already present, the call is a no-op
 * (silent dedup). Union, intersection, and difference all build new sets
 * by walking the sorted backing arrays. */

static void set_add(int set_handle, Val value) {
	Object *set = objects[set_handle];

	/* Binary search for the insertion point. The set is kept sorted by
	 * val_cmp, so a hit on VALUE means it's already a member — skip. */
	int low = 0, high = set->len;
	while (low < high) {
		int mid = (low + high) / 2;
		int cmp = val_cmp(set->items[mid], value);

		if (cmp == 0) return;                /* already present, no-op */
		if (cmp < 0) low = mid + 1;
		else         high = mid;
	}

	/* Insertion point is LOW. Grow the items array if full. */
	if (set->len >= set->cap) {
		set->cap *= 2;
		set->items = realloc(set->items, sizeof(Val) * (size_t)set->cap);
	}

	/* Slide [low..end) right by one slot to open the gap, then drop in. */
	memmove(&set->items[low + 1], &set->items[low],
			sizeof(Val) * (size_t)(set->len - low));
	set->items[low] = value;
	set->len++;
}

static int set_member(int set_handle, Val value) {
	Object *set = objects[set_handle];

	int low = 0, high = set->len;
	while (low < high) {
		int mid = (low + high) / 2;
		int cmp = val_cmp(set->items[mid], value);
		if (cmp == 0) return 1;
		if (cmp < 0) low = mid + 1;
		else         high = mid;
	}
	return 0;
}

/* The three set-set operations all build a fresh result by walking the
 * inputs and selectively adding to a new set. Dedup falls out of
 * set_add's "already present" early-return. */

static int set_union(int handle_a, int handle_b) {
	int union_handle = object_new_set();
	Object *set_a = objects[handle_a];
	Object *set_b = objects[handle_b];

	for (int i = 0; i < set_a->len; i++) set_add(union_handle, set_a->items[i]);
	for (int i = 0; i < set_b->len; i++) set_add(union_handle, set_b->items[i]);

	return union_handle;
}

static int set_intersect(int handle_a, int handle_b) {
	int intersection_handle = object_new_set();
	Object *set_a = objects[handle_a];

	for (int i = 0; i < set_a->len; i++)
		if (set_member(handle_b, set_a->items[i]))
			set_add(intersection_handle, set_a->items[i]);

	return intersection_handle;
}

static int set_difference(int handle_a, int handle_b) {
	int difference_handle = object_new_set();
	Object *set_a = objects[handle_a];

	for (int i = 0; i < set_a->len; i++)
		if (!set_member(handle_b, set_a->items[i]))
			set_add(difference_handle, set_a->items[i]);

	return difference_handle;
}


/* ---- printing --------------------------------------------------------- */
/* The . primitive uses this. Type-aware: strings unquoted, symbols as
 * names, sets and arrays in their literal form, integers-as-floats as
 * integers when they happen to be whole. 
 * Matrices are nicely formatted as tables. */

static void print_val(Val value);

static void print_double(double number) {
	if (number == (double)(int64_t)number && number > -1e15 && number < 1e15)
		printf("%lld", (long long)number);
	else
		printf("%g", number);
}

/* Cap how many elements of a collection get printed. Anything longer
 * than FIRST + LAST shows the first FIRST, an ellipsis, and the last
 * LAST — that way truncation always shortens the output rather than
 * expanding it (which is what would happen at lengths 11..13 if we
 * truncated every collection with more than 10 elements).
 *
 * PRINT_TRUNCATE is the on/off switch. It applies to every print
 * call, including nested collections inside whatever's being printed.
 * T
 he .a primitive flips it off for one call by save/setting/
 * restoring it around print_val. */
#define PRINT_FIRST 10
#define PRINT_LAST  3
static int print_truncate = 1;

static void print_items(Object *collection) {
	int length = collection->len;

	if (!print_truncate || length <= PRINT_FIRST + PRINT_LAST) {
		for (int i = 0; i < length; i++) {
			print_val(collection->items[i]);
			putchar(' ');
		}
	} else {
		for (int i = 0; i < PRINT_FIRST; i++) {
			print_val(collection->items[i]);
			putchar(' ');
		}
		fputs("... ", stdout);
		for (int i = length - PRINT_LAST; i < length; i++) {
			print_val(collection->items[i]);
			putchar(' ');
		}
	}
}

/* Inline one-line form used by PRINT_VAL (which .s and string
 * interpolation route through). Walks the flat element array; truncates
 * with a single ... when total elements exceed the threshold. */
static void print_corners(Object *matrix) {
	double *elements = matrix->matrix.elements;
	int n = matrix->matrix.rows * matrix->matrix.columns;

	if (!print_truncate || n <= PRINT_FIRST + PRINT_LAST) {
		for (int i = 0; i < n; i++) {
			putchar(' ');
			print_double(elements[i]);
		}
	} else {
		for (int i = 0; i < PRINT_FIRST; i++) {
			putchar(' ');
			print_double(elements[i]);
		}
		fputs(" ...", stdout);
		for (int i = n - PRINT_LAST; i < n; i++) {
			putchar(' ');
			print_double(elements[i]);
		}
	}
}

/* 2D grid layout used by . (single-value print). One row per source
 * row, fixed-width cells, scientific notation for values that don't
 * fit comfortably as fixed. Truncates independently on each axis —
 * worst case shows the four corners with row- and column-ellipses
 * between. Disabled by PRINT_TRUNCATE (used by .a to dump full). */
#define MATRIX_DISP_FIRST_ROWS 5
#define MATRIX_DISP_LAST_ROWS  3
#define MATRIX_DISP_FIRST_COLS 5
#define MATRIX_DISP_LAST_COLS  3

static void print_matrix_cell(double value) {
	printf(" %10.4g", value);   /* leading space + 10-wide field */
}

static void print_matrix_grid(Object *m) {
	int rows = m->matrix.rows;
	int cols = m->matrix.columns;
	int rows_trunc = print_truncate
		&& rows > MATRIX_DISP_FIRST_ROWS + MATRIX_DISP_LAST_ROWS;
	int cols_trunc = print_truncate
		&& cols > MATRIX_DISP_FIRST_COLS + MATRIX_DISP_LAST_COLS;

	printf("<matrix %dx%d>\n", rows, cols);

	for (int i = 0; i < rows; i++) {
		if (rows_trunc) {
			if (i == MATRIX_DISP_FIRST_ROWS) fputs(" ...\n", stdout);
			if (i >= MATRIX_DISP_FIRST_ROWS && i < rows - MATRIX_DISP_LAST_ROWS)
				continue;
		}

		for (int j = 0; j < cols; j++) {
			if (cols_trunc) {
				if (j == MATRIX_DISP_FIRST_COLS) printf(" %10s", "...");
				if (j >= MATRIX_DISP_FIRST_COLS && j < cols - MATRIX_DISP_LAST_COLS)
					continue;
			}
			print_matrix_cell(MAT(m, i, j));
		}
		putchar('\n');
	}
}


static void print_val(Val value) {
	switch (value.tag) {
		case T_FLOAT:   print_double(unpack_float(value)); break;
		case T_SYM:     fputs(&symbol_pool[value.data], stdout); break;
		case T_STRING:  fputs(objects[value.data]->bytes, stdout); break;
		case T_SET:     fputs("{ ", stdout); print_items(objects[value.data]); putchar('}'); break;
		case T_ARRAY:   fputs("[ ", stdout); print_items(objects[value.data]); putchar(']'); break;
		case T_XT:      printf("<xt %lld>",   (long long)value.data); break;
		case T_ADDR:    printf("<addr %lld>", (long long)value.data); break;
		case T_MATRIX: {
						   Object *matrix = objects[value.data];
						   printf("<matrix %dx%d: ", matrix->matrix.rows, matrix->matrix.columns);
						   print_corners(matrix);
						   putchar('>');
						   break;
					   }
		default:        printf("<?>"); break;
	}
}

/* Compressed one-token representation of a Val, written to stdout.
 * Used by the REPL prompt to show the top of stack at a glance. Kept
 * under ~12 chars so the prompt doesn't crowd out user input. */
static void print_val_compact(Val value) {
	switch (value.tag) {
		case T_FLOAT: {
			double number = unpack_float(value);
			if (number == (double)(int64_t)number && number > -1e12 && number < 1e12)
				printf("%lld", (long long)number);
			else
				printf("%.4g", number);
			break;
		}
		case T_STRING: {
			Object *obj = objects[value.data];
			if (obj->len <= 10) printf("\"%.*s\"", obj->len, obj->bytes);
			else                printf("\"%.9s…\"", obj->bytes);
			break;
		}
		case T_SYM: {
			const char *name = &symbol_pool[value.data];
			int len = (int)strlen(name);
			if (len <= 10) fputs(name, stdout);
			else           printf("%.9s…", name);
			break;
		}
		case T_SET:     printf("{%d}", objects[value.data]->len); break;
		case T_ARRAY:   printf("[%d]", objects[value.data]->len); break;
		case T_MATRIX: {
			Object *m = objects[value.data];
			printf("M%dx%d", m->matrix.rows, m->matrix.columns);
			break;
		}
		case T_XT: {
			int target = (int)value.data;
			const char *name = NULL;
			for (int cfa = latest_cfa; cfa != 0; cfa = (int)LINK(cfa)) {
				if (cfa == target) { name = &namepool[NAMEIDX(cfa)]; break; }
			}
			if (name) {
				int len = (int)strlen(name);
				if (len <= 9) printf("'%s", name);
				else          printf("'%.8s…", name);
			} else {
				printf("'?");
			}
			break;
		}
		case T_ADDR:    printf("@%lld", (long long)value.data); break;
		case T_CONT:    fputs("k", stdout); break;
		default:        fputs("?", stdout); break;
	}
}

static void print_prompt_state(void) {
	if (error_flag) {
		printf("<%d|error> ", dsp);
		return;
	}
	if (dsp == 0) {
		printf("<0> ");
		return;
	}
	printf("<%d|", dsp);
	print_val_compact(data_stack[dsp - 1]);
	printf("> ");
}


/* ---- dictionary search ------------------------------------------------ */
/* FIND walks the linked list of headers, newest-first, comparing names.
 * Because new definitions are prepended, a redefinition shadows the
 * previous version: FIND returns the newer CFA. Old compiled references
 * to the previous version still call it — they were resolved at compile
 * time to a specific CFA index, not to a name. */

static int find(const char *name) {
	int cfa = latest_cfa;
	while (cfa != 0) {
		if (strcmp(&namepool[NAMEIDX(cfa)], name) == 0) return cfa;
		cfa = (int)LINK(cfa);
	}
	return 0;
}


/* ---- code-field handlers --------------------------------------------- */
/* Three handlers cover all the kinds of words we have:
 *
 *   docol  — for colon definitions. Save the current instruction pointer
 *            on the return stack, then redirect it to the body. The inner
 *            interpreter resumes its loop and walks the new body. When
 *            the body ends with an EXIT, the saved ip is restored.
 *
 *   dosym  — for symbol-defining words. The "behavior" of a symbol is to
 *            push itself onto the stack as a tagged Val. Its identity is
 *            the CFA of the defining word, which is unique by construction
 *            and conveniently gives us access to the name (via NAMEIDX).
 *
 *   dovar  — for variable-defining words. Pushes the address of the body
 *            slot, which the user reaches via @ and !.
 *
 * Each handler does only the work specific to its kind. None of them
 * "walks the body" — that's the job of the inner interpreter, which
 * continues looping after the handler returns. */

static void docol(cell *cfa) {
	/* Save the current ip as a T_ADDR tagged Val on the return stack.
	 * We store the dict[] offset rather than the raw pointer so the
	 * return stack contains only valid Vals — the same convention
	 * dovar uses for variable addresses. */
	rpush(make_addr((int)(ip - dict)));
	ip = cfa + 1;
}

/* dosym: handler for symbol-defining words. The defining word's body
 * holds a single cell: the symbol's offset in symbol_pool, stashed
 * there by p_symbol at definition time. Running it just pushes a
 * T_SYM Val whose payload is that offset. Two symbol foo declar-
 * ations get the same offset (intern), so they're equal. The offset
 * survives FORGET because symbol_pool is never rolled back. */
static void dosym(cell *cfa) {
	int symbol_offset = (int)cfa[1];
	push(make_sym(symbol_offset));
}

/* dovar: handler for variable-defining words. Running a variable's CFA
 * pushes the dict[] index of the cell immediately after the code field
 * — that's where the variable's two-cell storage (tag + payload) lives.
 * The user reads through that address with @ (p_fetch) and writes
 * through it with ! (p_store). */
static void dovar(cell *cfa) {
	int body_index = (int)((cfa + 1) - dict);
	push(make_addr(body_index));
}


/* Cached CFAs of internal primitives we'll need to reference during
 * compilation, plus (stop) which is used as the trampoline terminator in
 * execute_cfa. Resolved in main() at startup. */
static int	exit_cfa = 0, 
			literal_cfa = 0, 
			branch_cfa = 0, 
			zbranch_cfa = 0, 
			dostr_cfa = 0, 
			stop_cfa = 0;


/* ---- the inner interpreter ------------------------------------------- */
/* This is the engine. It runs whenever we're inside a colon definition.
 *
 * Each iteration:
 *
 *   1. Read the next body cell. It contains a pointer to some word's CFA.
 *   2. Dereference that pointer to get the handler stored in the code
 *      field.
 *   3. Call the handler, passing the CFA so handlers like docol/dovar
 *      can find their bodies.
 *
 * The handler may do anything: push to the data stack (arithmetic, stack
 * ops), read additional cells from the ip stream (literals, branches),
 * modify ip itself (docol, p_exit, branches), or clear RUNNING to stop
 * the loop. This is "NEXT" in classical Forth terminology. */

typedef void (*cfa_handler)(cell *cfa);

static void run_inner(void) {
	int initial_rsp = rsp;

	while (running && !error_flag) {
		/* Read the next body cell — it's the dict[] index of some
		 * word's CFA — advance past it, look up the handler in dict[],
		 * and call it. The handler still receives the CFA *pointer*
		 * (&dict[index]) so docol/dovar can compute their body
		 * locations the same way. This is "NEXT" — one dispatch step
		 * in classical Forth terminology, here in index-threaded form
		 * (one indexed-load per step rather than two derefs). */
		if (unwinding) {
			/* If the mark we're looking for is below this level's
			 * starting rstack depth, it belongs to an outer run_inner.
			 * Stop here and let that outer level handle it. */
			if (rsp <= initial_rsp) break;

			Val frame = return_stack[--rsp];
			if (frame.tag == T_MARK && (int)frame.data == unwind_target) {
				/* Found our target mark - clear flag and exit cleanly. */
				unwinding = 0;

				 /* Also pop the docol return frame just below the mark (pushed
				  * by whoever called reset). Use its saved ip to resume past the
				  * catch's body, in the caller's code. */
				if (rsp > 0) {
					Val ret = return_stack[--rsp];
					ip = (cell *)(dict + ret.data);
				}
				continue;
			}
			/* some other frame (T_ADDR or non-matching T_MARK)
			 * discard and keep unwinding within this level. */
			continue;
		}

		int cfa_index         = (int)*ip++;
		cfa_handler handler   = (cfa_handler)dict[cfa_index];
		handler(&dict[cfa_index]);
	}
}


/* ---- execute_cfa: launching execution from the outer interpreter ----- */
/* The outer interpreter calls this when it wants to run a word. For colon
 * definitions, we need to set ip to the body and let the inner
 * interpreter run. We do this with a two-cell "trampoline": its first cell
 * is a pointer to the word's CFA, its second is a pointer to (stop)'s CFA.
 *
 * After the called word's docol pushes its own return marker, its body
 * runs, and its EXIT pops that marker and lands us at trampoline[1]. The
 * (stop) primitive there just clears RUNNING and returns; it does NOT
 * pop the return stack. That distinction is the whole point of having a
 * separate terminator: when execute_cfa is called reentrantly — for
 * example by MAP from inside a colon definition that's itself inside
 * another colon definition — the enclosing words' return markers are
 * already on return_stack, and we must not consume them. An EXIT-as-terminator
 * would walk right up the stack, prematurely returning from words that
 * have plenty left to do.
 *
 * Pure primitives are still dispatched directly, without going through
 * the trampoline. There's no need: a primitive runs and returns. The
 * trampoline only exists to give the inner-interpreter loop something to
 * stop on, and only colon definitions need that loop. */

static cell trampoline[2];

static void execute_cfa(int cfa) {
	cfa_handler handler = (cfa_handler)dict[cfa];
	if (handler != &docol) { handler(&dict[cfa]); return; }

	cell *saved_ip = ip;
	int saved_running = running;
	trampoline[0] = (cell)cfa;
	trampoline[1] = (cell)stop_cfa;
	ip = trampoline;
	running = 1;

	run_inner();

	running = saved_running;
	ip = saved_ip;
}


/* ---- defining new words: headers and code fields --------------------- */
/* alloc_name copies a name into the byte pool. create_header lays down
 * the four header cells (link, flags, name index, source index) and
 * returns the CFA — the index of the cell that will become the code
 * field. The caller fills in that cell with whichever handler is
 * appropriate.
 *
 * The source-index field is initialized to 0 ("no source") and is later
 * filled in by ; (p_semi) for colon definitions, which captures the
 * body source text and stashes it in source_pool[] for image save. For
 * everything else (primitives, variables, symbols) it stays at 0.
 *
 * define_primitive is the convenience wrapper that does both: create a header,
 * then write a function pointer into the code field. Returns the CFA so
 * we can cache it for internal primitives we reference during
 * compilation. */

static int alloc_name(const char *name) {
	int length = (int)strlen(name) + 1;
	if (names_here + length > NAMEPOOL) { fail("name pool full"); return 0; }

	int name_offset = names_here;
	memcpy(&namepool[names_here], name, (size_t)length);
	names_here += length;

	return name_offset;
}

/* Look up NAME in the symbol pool. If a matching entry exists, return
 * its offset; otherwise append and return the new offset. Linear scan
 * is fine — the pool is small and symbol churn is low. */
static int intern_symbol(const char *name) {
	for (int i = 0; i < symbol_pool_here; ) {
		if (strcmp(&symbol_pool[i], name) == 0) return i;
		i += (int)strlen(&symbol_pool[i]) + 1;
	}
	int length = (int)strlen(name) + 1;
	if (symbol_pool_here + length > SYMBOLPOOL) {
		fail("symbol pool full");
		return 0;
	}
	int offset = symbol_pool_here;
	memcpy(&symbol_pool[symbol_pool_here], name, (size_t)length);
	symbol_pool_here += length;
	return offset;
}
static int create_header(const char *name, int immediate) {
	if (here + 4 >= MEMSZ) { fail("dictionary full"); return 0; }

	int previous_latest = latest_cfa;
	int name_offset     = alloc_name(name);
	dict[here++] = previous_latest;
	dict[here++] = immediate ? 1 : 0;
	dict[here++] = name_offset;
	dict[here++] = 0;            /* SRCIDX — filled in later by ; if any */

	/* The CFA cell is at HERE — record it as the new latest and
	 * return it. The caller will fill it in with the handler. */
	latest_cfa = here;
	return latest_cfa;
}

static int define_primitive(const char *name, cfa_handler handler, int immediate) {
	int cfa = create_header(name, immediate);

	if (here < MEMSZ) dict[here++] = (cell)handler; else fail("dictionary full");

	return cfa;
}

/* EMIT appends a single raw cell to the dictionary at HERE. Used
 * during compilation of colon definitions and for inline data (literals
 * and branch offsets). */
static void emit(cell value) { if (here < MEMSZ) dict[here++] = value; else fail("dictionary full"); }

/* Compiling a tagged literal: we emit a (lit) reference, then two raw
 * cells encoding the Val (tag, then payload). At runtime (lit) reads
 * those two cells and reconstructs the Val.
 *
 * This is the one place where the body of a colon definition contains
 * something other than CFA references: the two raw cells immediately
 * after a (lit) reference are inline data. The inner interpreter never
 * sees them as separate dispatch steps — (lit)'s handler consumes them. */
static void emit_val_literal(Val value) {
	emit((cell)literal_cfa);
	emit((cell)value.tag);
	emit(value.data);
}


/* ---- core primitives -------------------------------------------------- */
/* Naming convention: every C function whose name begins P_ is a
 * *primitive* — the C implementation of a Forth-level word, invoked by
 * the inner interpreter through the cfa_handler typedef. So P_ADD
 * is the body of the + word, P_DUP is DUP, and so on. The
 * mapping between word names and primitive functions lives in main(),
 * where each primitive is registered via DEFINE_PRIMITIVE.
 *
 * Each primitive matches the cfa_handler signature, taking a cell *cfa
 * argument. Most primitives ignore that argument — they get their
 * inputs from the data stack via pop() and leave outputs there via
 * push(). The cfa parameter is only meaningful to handlers for kinds
 * of words (docol, dosym, dovar — already covered above).
 *
 * We start with EXIT and the inline-data primitives, which are the
 * machinery that makes colon definitions work, then move outward to the
 * user-visible operations. */

/* Report an error and raise the sticky error_flag. The REPL catches
 * the flag at the top of the next outer-loop iteration and resets
 * state. Variadic so call sites can format detail directly. */
static char error_message[256];

static void fail(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(error_message, sizeof(error_message), fmt, args);
	va_end(args);
	error_flag = 1;
}
static void type_error(const char *op) { fail("type error in %s", op); }

/* EXIT: pop the saved instruction pointer from the return stack and jump
 * to it. When the return stack is empty, we're at the top level; clear
 * RUNNING and let the inner-interpreter loop terminate cleanly. */
static void p_exit(cell *cfa) {
	(void)cfa;

	/* Skip past any MARK sentinels left by RESET. They get pushed onto
	 * the return stack to delimit a continuation-capture region; the
	 * region ends naturally when the enclosing word exits past them. */
	while (rsp > 0 && return_stack[rsp - 1].tag == T_MARK) rsp--;

	if (rsp <= 0) {
		running = 0; 
		return; 
	}

	Val saved_ip = return_stack[--rsp];
	ip = (cell *)(dict + saved_ip.data);
}

/* (stop): used only as the second cell of execute_cfa's trampoline. The
 * call sequence is: trampoline[0] dispatches the target word, the target
 * runs to its EXIT, EXIT pops its own saved ip and lands on (stop), and
 * (stop) breaks the inner loop without touching the return stack. See
 * execute_cfa for why a different terminator from EXIT is necessary. */
static void p_stop(cell *cfa) { (void)cfa; running = 0; }

/* The "inline data" primitives. Each one reads cell(s) from the ip
 * stream, advancing ip past them, and uses what it read:
 *
 *   (lit)      — reads two cells (tag, payload), pushes the reconstructed
 *                Val onto the data stack
 *   (branch)   — reads one cell (offset), jumps unconditionally
 *   (0branch)  — reads one cell (offset), jumps if the top of stack is
 *                zero/false; otherwise just continues
 *
 * Offsets are stored as cell-counts relative to the offset cell itself,
 * which lets the compiler compute them with simple arithmetic at compile
 * time. */
static void p_literal(cell *cfa) {
	(void)cfa;
	Val literal;

	literal.tag  = (Tag)*ip++;
	literal.data = *ip++;

	push(literal);
}
static void p_branch(cell *cfa)  {
	(void)cfa; 
	ip += *ip; 
}

static void p_0branch(cell *cfa) {
	(void)cfa;

	cell offset = *ip++;
	POP(condition);
	int is_false = (condition.tag == T_FLOAT) ? (unpack_float(condition) == 0.0)
		: (condition.data == 0);
	if (is_false) ip += offset - 1;
}

/* (dostr): string-literal handler. The cell immediately after this in
 * the body holds the handle of a template string. We perform
 * interpolation against the current data stack at run time, push the
 * resulting string, and advance past the handle. */
static int interpolate(int template_handle);   /* forward                  */

static void p_dostr(cell *cfa) {
	(void)cfa;

	int template_handle = (int)*ip++;
	push(make_string(interpolate(template_handle)));
}


/* ---- polymorphic arithmetic and comparison ---------------------------- */
/* +, -, * are overloaded across the types where overloading makes
 * sense. + adds numbers, concatenates strings, unions sets. - does
 * numeric subtraction and set difference. * does multiplication and
 * set intersection. The remaining type combinations raise a type error.
 *
 * Equality is universal: val_cmp gives us a total ordering and equality
 * falls out for free. */

static int string_concat(int left_handle, int right_handle) {
	Object *left  = objects[left_handle];
	Object *right = objects[right_handle];
	int combined_length = left->len + right->len;

	char *buffer = malloc((size_t)combined_length + 1);
	memcpy(buffer,             left->bytes,  (size_t)left->len);
	memcpy(buffer + left->len, right->bytes, (size_t)right->len);
	int result_handle = object_new_string(buffer, combined_length);
	free(buffer);

	return result_handle;
}

typedef double (*scalar_operator)(double, double);
static double scalar_add(double a, double b) { return a + b; }
static double scalar_subtract(double a, double b) { return a - b; }
static double scalar_multiply(double a, double b) { return a * b; }
static double scalar_divide(double a, double b) { return a / b; }

static int matrix_scalar_op(Val left_val, Val right_val, scalar_operator op) {
	Object *left = objects[left_val.data];
	Object *right = objects[right_val.data];

	if (left->matrix.rows != right->matrix.rows || left->matrix.columns != right->matrix.columns) {
		type_error("matrix shapes");
		return -1;
	}

	int rows = left->matrix.rows;
	int columns = right->matrix.columns;
	int target_handle = object_new_matrix(rows, columns);
	if (error_flag) return -1;

	Object *target = objects[target_handle];
	for (int i = 0; i < rows * columns; i++) {
		target->matrix.elements[i] = op(left->matrix.elements[i], right->matrix.elements[i]);
	}

	return target_handle;
}

static void p_add(cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);

	if (left.tag == T_FLOAT && right.tag == T_FLOAT)
		push(make_float(unpack_float(left) + unpack_float(right)));
	else if (left.tag == T_STRING && right.tag == T_STRING)
		push(make_string(string_concat((int)left.data, (int)right.data)));
	else if (left.tag == T_SET && right.tag == T_SET)
		push(make_set(set_union((int)left.data, (int)right.data)));
	else if (left.tag == T_MATRIX && right.tag == T_MATRIX) {
		int target_handle = matrix_scalar_op(left, right, scalar_add);
		if (target_handle < 0) return;
		push(make_matrix(target_handle));
	}
	else type_error("+");
}
static void p_sub(cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);

	if (left.tag == T_FLOAT && right.tag == T_FLOAT)
		push(make_float(unpack_float(left) - unpack_float(right)));
	else if (left.tag == T_SET && right.tag == T_SET)
		push(make_set(set_difference((int)left.data, (int)right.data)));
	else if (left.tag == T_MATRIX && right.tag == T_MATRIX) {
		int target_handle = matrix_scalar_op(left, right, scalar_subtract);
		if (target_handle < 0) return;
		push(make_matrix(target_handle));
	}
	else type_error("-");
}
static void p_mul(cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);

	if (left.tag == T_FLOAT && right.tag == T_FLOAT)
		push(make_float(unpack_float(left) * unpack_float(right)));
	else if (left.tag == T_SET && right.tag == T_SET)
		push(make_set(set_intersect((int)left.data, (int)right.data)));
	else if (left.tag == T_MATRIX && right.tag == T_MATRIX) {
		int target_handle = matrix_scalar_op(left, right, scalar_multiply);
		if (target_handle < 0) return;
		push(make_matrix(target_handle));
	}
	else type_error("*");
}
static void p_div(cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);
	if (left.tag == T_FLOAT && right.tag == T_FLOAT && unpack_float(right) != 0.0)
		push(make_float(unpack_float(left) / unpack_float(right)));
	else if (left.tag == T_MATRIX && right.tag == T_MATRIX) {
		int target_handle = matrix_scalar_op(left, right, scalar_divide);
		if (target_handle < 0) return;
		push(make_matrix(target_handle));
	}
	else type_error("/");
}
static void p_neg(cell *cfa) {
	(void)cfa;
	POP(operand);
	if (operand.tag == T_FLOAT) push(make_float(-unpack_float(operand)));
	else type_error("negate");
}

/* Booleans live in T_FLOAT: -1.0 is true, 0.0 is false. The choice of
 * -1 over 1 follows Forth tradition — bitwise AND on flag values then
 * coincides with logical AND. */
static Val make_bool(int is_true) { return make_float(is_true ? -1.0 : 0.0); }
static int truthy(Val value)  { return (value.tag == T_FLOAT) ? (unpack_float(value) != 0.0) : (value.data != 0); }

static void p_eq(cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);
	push(make_bool(val_cmp(left, right) == 0));
}
static void p_lt(cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);
	push(make_bool(val_cmp(left, right) < 0));
}
static void p_gt(cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);
	push(make_bool(val_cmp(left, right) > 0));
}
static void p_zeq(cell *cfa) {
	(void)cfa;

	POP(operand);
	push(make_bool(!truthy(operand)));
}


/* ---- stack manipulation, I/O, return-stack access -------------------- */

static void p_dup(cell *cfa) {
	(void)cfa;

	POP(top);
	push(top);
	push(top);
}
static void p_drop(cell *cfa) {
	(void)cfa;

	(void)pop();
}
static void p_swap(cell *cfa) {
	(void)cfa;

	POP(top);
	POP(second);
	push(top);
	push(second);
}
static void p_over(cell *cfa) {
	(void)cfa;

	POP(top);
	POP(second);
	push(second);
	push(top);
	push(second);
}
static void p_rot(cell *cfa) {
	(void)cfa;

	POP(top);
	POP(middle);
	POP(bottom);
	push(middle);
	push(top);
	push(bottom);
}

static void p_depth(cell *cfa) {
	(void)cfa;

	push(make_float((double)dsp));
}

static void p_roll(cell *cfa) {
	(void)cfa;

	POP(n_val);
	if (n_val.tag != T_FLOAT) {
		type_error("roll");
		return;
	}

	int n = (int)unpack_float(n_val);
	if (n < 0 || n >= dsp) {
		fail ("roll: index out of range");
		return;
	}

	int src = dsp - 1 - n;
	Val v = data_stack[src];
	memmove(&data_stack[src], &data_stack[src + 1], (size_t)n * sizeof(Val));
	data_stack[dsp - 1] = v;
}

static void p_dot(cell *cfa) {
	(void)cfa;

	POP(value);
	if (value.tag == T_MATRIX) {
		print_matrix_grid(objects[value.data]);
	} else {
		print_val(value);
		putchar(' ');
	}
	fflush(stdout);
}

/* .a: print one value with truncation disabled — useful when the
 * caller actually wants to see a huge set or array. The save/clear/
 * restore dance preserves whatever PRINT_TRUNCATE was before, so
 * nested calls don't get permanently flipped (and any concurrent
 * printing during the call, e.g. from a side effect, also runs
 * un-truncated). The truncation rule itself lives in print_items. */
static void p_dot_all(cell *cfa) {
	(void)cfa;

	int saved = print_truncate;
	print_truncate = 0;
	POP(value);
	if (value.tag == T_MATRIX) {
		print_matrix_grid(objects[value.data]);
	} else {
		print_val(value);
		putchar(' ');
	}
	fflush(stdout);
	print_truncate = saved;
}
static void p_cr(cell *cfa) {
	(void)cfa;

	putchar('\n');
	fflush(stdout);
}
static void p_emit_(cell *cfa) {
	(void)cfa;

	POP(character);
	if (character.tag == T_FLOAT) {
		putchar((int)unpack_float(character));
		fflush(stdout);
	} else type_error("emit");
}
static void p_dots(cell *cfa) {
	(void)cfa;

	for (int i = 0; i < dsp; i++) {
		print_val(data_stack[i]);
		putchar(' ');
	}
	fflush(stdout);
}
static void p_bye(cell *cfa) {
	(void)cfa;

	exit(0);
}

static void p_tor(cell *cfa) {
	(void)cfa;

	POP(value);
	rpush(value);
}
static void p_rfrom(cell *cfa) {
	(void)cfa;

	push(rpop());
}
static void p_rfetch(cell *cfa) {
	(void)cfa;

	if (rsp > 0) push(return_stack[rsp - 1]);
	else fail("return stack empty");
}

static void p_to_side(cell *cfa) {
	(void)cfa;

	POP(value);
	if (side_dsp >= SIDESTACK_DEPTH) {
		fail("side stack overflow");
		return;
	}
	side_stack[side_dsp++] = value;
}

static void p_side_to(cell *cfa) {
	(void)cfa;

	if (side_dsp <= 0) {
		fail("side stack underflow");
		return;
	}
	push(side_stack[--side_dsp]);
}

static void p_side_drop(cell *cfa) {
	(void)cfa;

	if (side_dsp <= 0) {
		fail("side stack underflow");
		return;
	}
	side_dsp--;
}

static void p_side_depth(cell *cfa) {
	(void)cfa;

	push(make_float((double)side_dsp));
}


/* ---- variables: @ and ! ---------------------------------------------- */
/* variable foo reserves a body of two raw cells (tag, payload) — enough
 * to hold one Val. dovar pushes the address (cell index) of the tag
 * cell. The user accesses the stored value via @ and !, which read and
 * write the two-cell pair. */

static void p_fetch(cell *cfa) {
	(void)cfa;

	POP(address);
	if (address.tag != T_ADDR) {
		type_error("@");
		return;
	}
	int cell_index = (int)address.data;
	Val loaded;
	loaded.tag  = (Tag)dict[cell_index];
	loaded.data = dict[cell_index + 1];
	push(loaded);
}
static void p_store(cell *cfa) {
	(void)cfa;

	POP(addr);
	POP(value);
	if (addr.tag != T_ADDR) {
		type_error("!");
		return;
	}
	int cell_index = (int)addr.data;
	dict[cell_index]     = (cell)value.tag;
	dict[cell_index + 1] = value.data;
}


/* ---- set and array literals via stack markers ------------------------ */
/* { and [ push a special T_MARK value. The user then types elements,
 * which push themselves. } and ] scan down the stack to find the
 * marker, collect everything above it into a new set or array, drop the
 * marker, and push the resulting object handle.
 *
 * These are deliberately NOT immediate. In interpret mode the outer
 * interpreter executes them directly and the data stack acts as the
 * collection buffer. In compile mode they get compiled into the body
 * like any other primitive reference, so at run time { pushes the
 * mark, the compiled element literals push themselves, and } collects
 * — exactly the same mechanism, just deferred to run time. */

static void p_setopen(cell *cfa) {
	(void)cfa;

	push(make_mark());
}
static void p_setclose(cell *cfa) {
	(void)cfa;

	/* Find the matching mark on the stack: scan downward until we hit one. */
	int mark_index = dsp;
	while (mark_index > 0 && data_stack[mark_index - 1].tag != T_MARK) mark_index--;
	if (mark_index == 0) {
		type_error("}");
		return;
	}
	int set_handle = object_new_set();
	for (int i = mark_index; i < dsp; i++) set_add(set_handle, data_stack[i]);
	dsp = mark_index - 1;       /* drop everything from the mark up */
	push(make_set(set_handle));
}

static void p_array_open(cell *cfa) {
	(void)cfa;

	push(make_mark());
}
static void p_array_close(cell *cfa) {
	(void)cfa;

	int mark_index = dsp;
	while (mark_index > 0 && data_stack[mark_index - 1].tag != T_MARK) mark_index--;
	if (mark_index == 0) {
		type_error("]");
		return;
	}
	int num_elements = dsp - mark_index;
	int array_handle = object_new_array(num_elements);
	for (int i = 0; i < num_elements; i++)
		objects[array_handle]->items[i] = data_stack[mark_index + i];
	dsp = mark_index - 1;
	push(make_array(array_handle));
}

static void p_array(cell *cfa) {
	(void)cfa;

	POP(count_value);
	if (count_value.tag != T_FLOAT) {
		type_error("array");
		return;
	}

	int count = (int)unpack_float(count_value);
	if (count < 0 || count > dsp) {
		type_error("array");
		return;
	}

	int array_handle = object_new_array(count);
	if (error_flag) return;
	Object *array = objects[array_handle];

	int first_item = dsp - count;
	for (int i = 0; i < count; i++) 
		array->items[i] = data_stack[first_item + i];
	dsp = first_item;

	push(make_array(array_handle));
}

/* ---- set, array, and higher-order operations ------------------------- */

static void p_cardinality(cell *cfa) {
	(void)cfa;

	POP(collection);
	if (collection.tag == T_SET || collection.tag == T_ARRAY || collection.tag == T_STRING)
		push(make_float((double)objects[collection.data]->len));
	else type_error("cardinality");
}
static void p_member(cell *cfa) {
	(void)cfa;

	POP(value);
	POP(set_value);
	if (set_value.tag != T_SET) {
		type_error("member?");
		return;
	}
	push(make_bool(set_member((int)set_value.data, value)));
}
static void p_at_i(cell *cfa) {
	(void)cfa;

	POP(index_value);
	if (index_value.tag != T_FLOAT) {
		type_error("@i index");
		return;
	}
	int index = (int)unpack_float(index_value);

	POP(source_val);
	if (source_val.tag == T_ARRAY) {
		Object *array = objects[source_val.data];
		if (index < 0 || index >= array->len) { 
			type_error("@i: array index out of bounds"); 
			return; 
		}

		push(array->items[index]);
	} else if (source_val.tag == T_MATRIX) {
		Object *source = objects[source_val.data];
		if (index < 0 || index >= source->matrix.rows) { 
			type_error("@i: row index out of bounds"); 
			return; 
		}

		int num_columns = source->matrix.columns;
		int row_handle = object_new_matrix(1, num_columns);
		if (error_flag) return;

		Object *row = objects[row_handle];
		for (int j = 0; j < num_columns; j++)
			MAT(row, 0, j) = MAT(source, index, j);

		push(make_matrix(row_handle));
	} else {
		type_error("@i: needs array or matrix");
	}
}

static void p_at_j(cell *cfa) {
	(void)cfa;

	POP(index_value);
	if (index_value.tag != T_FLOAT) {
		type_error("@j index");
		return;
	}
	int index = (int)unpack_float(index_value);

	POP(source_val);
	if (source_val.tag != T_MATRIX) {
		type_error("@j: needs matrix");
		return;
	}

	Object *source = objects[source_val.data];
	if (index < 0 || index >= source->matrix.columns) {
		type_error("@j: column index out of bounds");
		return;
	}

	int num_rows = source->matrix.rows;
	int col_handle = object_new_matrix(num_rows, 1);
	if (error_flag) return;

	Object *col = objects[col_handle];
	for (int i = 0; i < num_rows; i++)
		MAT(col, i, 0) = MAT(source, i, index);

	push(make_matrix(col_handle));
}

static void p_at_ij(cell *cfa) {
	(void)cfa;

	POP(j_value);
	if (j_value.tag != T_FLOAT) {
		type_error("@i,j column index");
		return;
	}
	int j = (int)unpack_float(j_value);

	POP(i_value);
	if (i_value.tag != T_FLOAT) {
		type_error("@i,j row index");
		return;
	}
	int i = (int)unpack_float(i_value);

	POP(source_val);
	if (source_val.tag != T_MATRIX) {
		type_error("@i,j: needs matrix");
		return;
	}

	Object *source = objects[source_val.data];
	if (i < 0 || i >= source->matrix.rows) {
		type_error("@i,j: row index out of bounds");
		return;
	}
	if (j < 0 || j >= source->matrix.columns) {
		type_error("@i,j: column index out of bounds");
		return;
	}

	push(make_float(MAT(source, i, j)));
}

static int dgemm_kernel(int transpose_a, int transpose_b,
		double alpha,
		int a_handle, int b_handle,
		double beta, int c_handle) {
	int i, j, k, m, n, p;

	Object *A = objects[a_handle];
	Object *B = objects[b_handle];
	Object *C = objects[c_handle];

	int op_a_rows = transpose_a ? A->matrix.columns : A->matrix.rows;
	int op_a_cols = transpose_a ? A->matrix.rows    : A->matrix.columns;
	int op_b_rows = transpose_b ? B->matrix.columns : B->matrix.rows;
	int op_b_cols = transpose_b ? B->matrix.rows    : B->matrix.columns;

	if (op_a_cols != op_b_rows) {
		type_error("dgemm: inner dimensions must match");
		return -1;
	}

	if (C->matrix.rows != op_a_rows || C->matrix.columns != op_b_cols) {
		type_error("dgemm: C shape must match the product shape");
		return -1;
	}

	m = op_a_rows;
	n = op_b_cols;
	k = op_a_cols;

	int matmult_handle = object_new_matrix(m, n);
	if (error_flag) return -1;
	Object *matmult = objects[matmult_handle];

	if (!transpose_a && !transpose_b) {
		double * restrict out_elements = matmult->matrix.elements;
		const double * restrict a_elements = A->matrix.elements;
		const double * restrict b_elements = B->matrix.elements;
		const double * restrict c_elements = C->matrix.elements;

		for (i = 0; i < m; i++) 
			for (j = 0; j < n; j++)
				out_elements[i * n + j] = beta * c_elements[i * n + j];

		for (i = 0; i < m; i++) {
			for (p = 0; p < k; p++) {
				double a_val = alpha * a_elements[i * k + p];
				const double *b_row = &b_elements[p * n];
				double *out_row = &out_elements[i * n];
				for (j = 0; j < n; j++)
					out_row[j] += a_val * b_row[j];
			}
		}
	} else {
		for (i = 0; i < m; i++) {
			for (j = 0; j < n; j++) {
				double sum = 0.0;
				for (p = 0; p < k; p++) {
					double a_val = transpose_a ? MAT(A, p, i) : MAT(A, i, p);
					double b_val = transpose_b ? MAT(B, j, p) : MAT(B, p, j);
					sum += a_val * b_val;
				}
				MAT(matmult, i, j) = alpha * sum + beta * MAT(C, i, j);
			}
		}
	}

	return matmult_handle;
}

/* Shared body for the four DGEMM variants. Pops the five operands in
 * the stack-effect order ( alpha A B beta C -- result ), validates
 * types, calls the kernel with the right transpose flags, pushes the
 * resulting matrix. */
static void p_dgemm_helper(int transpose_a, int transpose_b) {
	POP(c_val);
	POP(beta_val);
	POP(b_val);
	POP(a_val);
	POP(alpha_val);

	if (alpha_val.tag != T_FLOAT || beta_val.tag != T_FLOAT) {
		type_error("dgemm: alpha and beta must be floats");
		return;
	}
	if (a_val.tag != T_MATRIX || b_val.tag != T_MATRIX || c_val.tag != T_MATRIX) {
		type_error("dgemm: A, B, C must be matrices");
		return;
	}

	int matmult_handle = dgemm_kernel(transpose_a, transpose_b,
			unpack_float(alpha_val),
			(int)a_val.data, (int)b_val.data,
			unpack_float(beta_val),
			(int)c_val.data);
	if (error_flag) return;
	push(make_matrix(matmult_handle));
}

/* The four permuted variants. Each computes alpha * op_A(A) * op_B(B)
 * + beta * C, where op_A and op_B are either identity or transpose,
 * chosen by the n/t suffixes (first letter for A, second for B). */
static void p_dgemm_nn(cell *cfa) {(void)cfa; p_dgemm_helper(0, 0);}
static void p_dgemm_tn(cell *cfa) {(void)cfa; p_dgemm_helper(1, 0);}
static void p_dgemm_nt(cell *cfa) {(void)cfa; p_dgemm_helper(0, 1);}
static void p_dgemm_tt(cell *cfa) {(void)cfa; p_dgemm_helper(1, 1);}

/* Matrix reductions: sum, max, min, each across the whole matrix and
 * along each axis. Three helpers parameterized by a (accumulator,
 * element) -> accumulator function and an identity value cover all
 * nine variants. */
typedef double (*reducer)(double accumulator, double element);
static double reduce_add(double accumulator, double element) { return accumulator + element; }
static double reduce_max(double accumulator, double element) { return accumulator > element ? accumulator : element; }
static double reduce_min(double accumulator, double element) { return accumulator < element ? accumulator : element; }

static double matrix_reduce_overall(Object *source, reducer fn, double identity) {
	int num_elements = source->matrix.rows * source->matrix.columns;
	double accumulator = identity;
	for (int i = 0; i < num_elements; i++)
		accumulator = fn(accumulator, source->matrix.elements[i]);
	return accumulator;
}

static int matrix_reduce_rows(Object *source, reducer fn, double identity) {
	int rows = source->matrix.rows;
	int cols = source->matrix.columns;
	int target_handle = object_new_matrix(rows, 1);
	if (error_flag) return -1;
	Object *target = objects[target_handle];
	for (int i = 0; i < rows; i++) {
		double accumulator = identity;
		for (int j = 0; j < cols; j++)
			accumulator = fn(accumulator, MAT(source, i, j));
		MAT(target, i, 0) = accumulator;
	}
	return target_handle;
}

/* Loop order is i,j (not j,i) so source reads and target writes are
 * both stride-1; target row is seeded with IDENTITY before the walk. */
static int matrix_reduce_columns(Object *source, reducer fn, double identity) {
	int rows = source->matrix.rows;
	int cols = source->matrix.columns;
	int target_handle = object_new_matrix(1, cols);
	if (error_flag) return -1;
	Object *target = objects[target_handle];
	for (int j = 0; j < cols; j++) MAT(target, 0, j) = identity;
	for (int i = 0; i < rows; i++)
		for (int j = 0; j < cols; j++)
			MAT(target, 0, j) = fn(MAT(target, 0, j), MAT(source, i, j));
	return target_handle;
}

#define REDUCE_OVERALL(primitive_name, word_name, fn, identity)                 \
	static void primitive_name(cell *cfa) {                                     \
		(void)cfa;                                                              \
		POP(source_val);                                                        \
		if (source_val.tag != T_MATRIX) {                                       \
			type_error(word_name);                                              \
			return;                                                             \
		}                                                                       \
		push(make_float(matrix_reduce_overall(objects[source_val.data],         \
						fn, identity)));                  \
	}

#define REDUCE_ROWS(primitive_name, word_name, fn, identity)                    \
	static void primitive_name(cell *cfa) {                                     \
		(void)cfa;                                                              \
		POP(source_val);                                                        \
		if (source_val.tag != T_MATRIX) {                                       \
			type_error(word_name);                                              \
			return;                                                             \
		}                                                                       \
		int target_handle = matrix_reduce_rows(objects[source_val.data],        \
				fn, identity);                   \
		if (!error_flag) push(make_matrix(target_handle));                      \
	}

#define REDUCE_COLUMNS(primitive_name, word_name, fn, identity)                 \
	static void primitive_name(cell *cfa) {                                     \
		(void)cfa;                                                              \
		POP(source_val);                                                        \
		if (source_val.tag != T_MATRIX) {                                       \
			type_error(word_name);                                              \
			return;                                                             \
		}                                                                       \
		int target_handle = matrix_reduce_columns(objects[source_val.data],     \
				fn, identity);                \
		if (!error_flag) push(make_matrix(target_handle));                      \
	}

REDUCE_OVERALL(p_sum,           "sum",           reduce_add,  0.0)
REDUCE_OVERALL(p_max,           "max",           reduce_max, -INFINITY)
REDUCE_OVERALL(p_min,           "min",           reduce_min,  INFINITY)
REDUCE_ROWS   (p_row_sums,      "row-sums",      reduce_add,  0.0)
REDUCE_ROWS   (p_row_maxes,     "row-maxes",     reduce_max, -INFINITY)
REDUCE_ROWS   (p_row_mins,      "row-mins",      reduce_min,  INFINITY)
REDUCE_COLUMNS(p_column_sums,   "column-sums",   reduce_add,  0.0)
REDUCE_COLUMNS(p_column_maxes,  "column-maxes",  reduce_max, -INFINITY)
REDUCE_COLUMNS(p_column_mins,   "column-mins",   reduce_min,  INFINITY)

/* SET: ( v1 v2 ... vN N -- set ) build a set from the top N stack items.
 * The count goes on top so the items can be pushed in their natural
 * left-to-right order: 1 2 3 4 3 set consumes 2 3 4 and leaves 1
 * below. Duplicates (by val_cmp) are silently dropped — set_add bails
 * on a hit, so 5 5 6 3 set yields the two-element set { 5 6 }. This
 * is intentional set semantics, but it can surprise: dup 2 set on any
 * single value produces a one-element set, not a two-element one. */
static void p_set(cell *cfa) {
	(void)cfa;

	POP(count_value);
	if (count_value.tag != T_FLOAT) {
		type_error("set");
		return;
	}

	int count = (int)unpack_float(count_value);
	if (count < 0 || count > dsp) {
		type_error("set");
		return;
	}

	int set_handle = object_new_set();
	if (error_flag) return;

	int first_item = dsp - count;
	for (int i = 0; i < count; i++) set_add(set_handle, data_stack[first_item + i]);
	dsp = first_item;

	push(make_set(set_handle));
}

static void p_union(cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);
	if (left.tag != T_SET || right.tag != T_SET) {
		type_error("union");
		return;
	}
	push(make_set(set_union((int)left.data, (int)right.data)));
}
static void p_intersect(cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);
	if (left.tag != T_SET || right.tag != T_SET) {
		type_error("intersection");
		return;
	}
	push(make_set(set_intersect((int)left.data, (int)right.data)));
}
static void p_difference(cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);
	if (left.tag != T_SET || right.tag != T_SET) {
		type_error("difference");
		return;
	}
	push(make_set(set_difference((int)left.data, (int)right.data)));
}

/* EXECUTE takes an execution token (xt) — the handle for a word
   — and
 * runs it. Together with ' (tick), this gives us higher-order
 * operations: a word that takes a word as data. */
static void p_execute(cell *cfa) {
	(void)cfa;

	POP(value);
	if (value.tag != T_XT) {
		type_error("execute");
		return;
	}
	execute_cfa((int)value.data);
}

/* RESET marks the start of a delimited continuation region. All it
 * does is push a MARK sentinel onto the return stack — SHIFT later
 * walks up to find it, and EXIT discards it when the enclosing word
 * returns past it. The region extends from this point to the end of
 * the surrounding word; factor into a sub-word if you need a smaller
 * region. */
static void p_reset(cell *cfa) {
	(void)cfa;

	Val mark = make_mark();
	mark.data = next_mark_id++;
	rpush(mark);
}

/* SHIFT captures the slice of execution from here back to the nearest
 * RESET, packages it as a continuation, and pushes that continuation
 * on the data stack. The unwind removes the captured frames and the
 * mark from the return stack, so when this primitive returns the
 * inner loop continues with whatever's next in the source — typically
 * the EXIT of the containing word, which bubbles control out past the
 * RESET region.
 *
 *   ( -- k )
 *
 * The caller of the RESET-containing word receives k on the data
 * stack and decides what to do with it: discard (exception),
 * RESUME it once (one-shot return), RESUME it many times (back-
 * tracking, generators), stash it (coroutines). */
/* Find the nearest MARK on the return stack and build a continuation
 * object capturing the frames above it. Returns the new object's slot
 * and writes the mark's rstack index through out_mark_index, or
 * returns -1 (with error_flag set) on failure. Doesn't touch the
 * rstack — the caller decides whether to keep or discard the mark. */
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

/* SHIFT captures the slice up to the nearest RESET and pushes the
 * continuation on the data stack. The captured frames AND the mark
 * are removed; control continues in the surrounding word's body. Used
 * for coroutines, generators, backtracking — patterns where the caller
 * of the RESET-containing word picks up the continuation directly.
 *
 *   ( -- k )
 */
static void p_shift(cell *cfa) {
    (void)cfa;

    int mark_index;
    int cont_slot = capture_continuation(&mark_index);
    if (cont_slot < 0) return;

    rsp = mark_index;             /* discard captured frames AND the mark */
    push(make_continuation(cont_slot));
}

/* SHIFT-WITH captures the slice up to the nearest RESET and runs a
 * handler xt in the outer context. The handler's stack effect becomes
 * the entire reset region's stack effect — any code in the surrounding
 * word past the reset is skipped via the unwinding flag.
 *
 *   ( handler-xt -- ... )
 *
 * The captured continuation k is passed to the handler on the data
 * stack. A handler that drops k is the exception pattern; a handler
 * that stashes k somewhere is the coroutine-suspend pattern. */
static void p_shift_with(cell *cfa) {
    (void)cfa;

    POP(handler);
    if (handler.tag != T_XT) {
        type_error("shift-with");
        return;
    }

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

/* RESUME re-enters a captured continuation. The data stack is left
 * as-is; whatever the caller has arranged there is what the slice
 * sees when it resumes.
 *
 *   ( k -- )
 *
 * RESUME pushes a frame routing back here, a fresh MARK so SHIFTs
 * inside the slice are caught at this resume site, splices the
 * captured frames, then jumps to the saved resume point. Multi-shot:
 * the captured frames are read-only, so the same k can be RESUMEd
 * many times. */
static void p_resume(cell *cfa) {
	(void)cfa;

	POP(k);
	if (k.tag != T_CONT) {
		type_error("resume");
		return;
	}

	Object *cont = objects[k.data];
	cell *saved_ip = ip;
	int saved_running = running;

	/* Trampoline-stop frame so the slice's eventual EXIT chain
	 * terminates cleanly here. Same mechanism execute_cfa uses for
	 * docol; works whether RESUME was called from REPL top-level or
	 * from inside another colon def. */
	trampoline[1] = (cell)stop_cfa;
	rpush(make_addr((int)((trampoline + 1) - dict)));

	/* Fresh MARK delimits the resumed slice. */
	rpush(make_mark());

	/* Splice the captured frames on top. */
	for (int i = 0; i < cont->continuation.return_len; i++)
		rpush(cont->continuation.return_slice[i]);

	ip = &dict[cont->continuation.resume_ip];
	running = 1;
	run_inner();

	running = saved_running;
	ip = saved_ip;
}


/* MAP: apply an xt to each element of a set or array, collect the
 * results into a new array. The result is always an array — not a set —
 * so element count and source iteration order are both preserved. (A
 * set would silently dedup f x = f y cases, which is rarely what the
 * user wants when they say "apply f to each.") For a set input the
 * iteration order follows the set's internal sort; for an array input
 * it follows the array's positional order. We snapshot the source's
 * elements first because the user's mapping word might do anything to
 * the registry.
 *
 * Two GC roots are needed during the loop: SV is the source, which
 * is no longer on the data stack and whose items we read from across xt
 * calls; and the partially-built result array, whose handle is only held
 * in a C local until we push it at the end. We also zero-initialize the
 * array's items[] before the loop, because object_new_array leaves them
 * uninitialized — and a GC triggered mid-loop would walk those slots as
 * Vals during marking. Zero bytes deserialize as {T_NONE, 0}, which
 * mark_val ignores. */
static void p_map(cell *cfa) {
	(void)cfa;

	POP(xt);
	POP(source_value);
	if (xt.tag != T_XT || (source_value.tag != T_SET && source_value.tag != T_ARRAY)) {
		type_error("map");
		return;
	}
	Object *source = objects[source_value.data];
	int source_length = source->len;
	Val *snapshot = malloc(sizeof(Val) * (size_t)MAX(source_length, 1));
	memcpy(snapshot, source->items, sizeof(Val) * (size_t)source_length);
	gc_root_push(source_value);
	int result_handle = object_new_array(source_length);
	if (error_flag) { gc_root_pop(); free(snapshot); return; }
	Object *result = objects[result_handle];
	memset(result->items, 0, sizeof(Val) * (size_t)MAX(source_length, 1));
	gc_root_push(make_array(result_handle));
	for (int i = 0; i < source_length && !error_flag; i++) {
		push(snapshot[i]);
		execute_cfa((int)xt.data);
		if (error_flag) break;
		result->items[i] = pop();
	}
	gc_root_pop();
	gc_root_pop();
	free(snapshot);
	push(make_array(result_handle));
}

/* MAPN: n-ary zip-map. ( arr1 arr2 ... arrN xt N -- result )
 *
 * All N source arrays must be the same length. For each index i the xt
 * is called with arr1[i], arr2[i], ..., arrN[i] pushed in that order on
 * top of the data stack; the xt is expected to consume those N values
 * and leave one result, which becomes result[i].
 *
 * Rooting strategy: the source arrays stay on the data stack for the
 * duration of the loop, and that's what keeps them alive across xt
 * calls that might trigger GC. We push the per-row inputs ABOVE the
 * sources, call xt, pop exactly one result. After the loop the sources
 * are dropped and the result array is pushed.
 *
 * This is a deliberately different strategy from p_map, which snapshots
 * its single source into a malloc'd C buffer and uses an explicit
 * gc_root_push for the source set. Either approach works; the choice
 * here is that mapn might have many sources (N is user-controlled), and
 * gc_roots[] has a fixed small capacity (GC_ROOTS_MAX), so pinning them
 * all via gc_root_push would risk overflow. The data stack has plenty
 * of room and is already a GC root by construction. */
static void p_mapn(cell *cfa) {
	(void)cfa;

	POP(arity_value);
	if (arity_value.tag != T_FLOAT) {
		type_error("mapn");
		return;
	}
	int arity = (int)unpack_float(arity_value);
	if (arity < 1) {
		type_error("mapn");
		return;
	}
	POP(xt);
	if (xt.tag != T_XT) {
		type_error("mapn");
		return;
	}
	if (arity > dsp) {
		type_error("mapn");
		return;
	}

	/* The N source arrays sit at data_stack[first_source .. dsp-1]. */
	int first_source = dsp - arity;
	int row_count = -1;
	for (int i = 0; i < arity; i++) {
		if (data_stack[first_source + i].tag != T_ARRAY) {
			type_error("mapn");
			return;
		}
		Object *source = objects[data_stack[first_source + i].data];
		if (row_count < 0) row_count = source->len;
		else if (source->len != row_count) {
			type_error("mapn");
			return;
		}
	}

	int result_handle = object_new_array(row_count);
	if (error_flag) return;
	Object *result = objects[result_handle];
	memset(result->items, 0, sizeof(Val) * (size_t)MAX(row_count, 1));
	gc_root_push(make_array(result_handle));

	for (int row = 0; row < row_count && !error_flag; row++) {
		int dsp_before = dsp;
		for (int source_index = 0; source_index < arity; source_index++) {
			Object *source = objects[data_stack[first_source + source_index].data];
			push(source->items[row]);
		}
		execute_cfa((int)xt.data);
		if (error_flag) break;
		if (dsp != dsp_before + 1) { type_error("mapn"); break; }
		result->items[row] = pop();
	}
	gc_root_pop();

	if (!error_flag) {
		dsp = first_source;
		push(make_array(result_handle));
	}
}

/* WORDS: list every name in the dictionary. Walks the linked list
 * newest-to-oldest. */
static void p_words(cell *cfa) {
	(void)cfa;

	int cnt = 0;
	for (int cf = latest_cfa; cf != 0; cf = (int)LINK(cf)) {
		fputs(&namepool[NAMEIDX(cf)], stdout);
		putchar(' ');
		if (++cnt % 8 == 0) putchar('\n');
	}
	if (cnt % 8) putchar('\n');
	fflush(stdout);
}

/* SEE: print a word's definition. Takes an xt off the stack, so the
 * natural usage is ' foo see — same pattern as ' foo execute or
 * ' f map. The output form depends on the kind of word:
 *
 *   · Colon definitions: the captured body source (see p_semi), printed
 *     in the same : name <body> ; form that SAVE uses.
 *   · Variables: variable <name> plus a comment with the current
 *     value, since the value isn't part of the declaration itself.
 *   · Symbols: just symbol <name>.
 *   · Primitives: a note that the word is built-in — there's no source
 *     to show because the implementation is in C.
 *   · Anonymous xt (from a top-level [: ... :]): no name to resolve,
 *     so we can only flag that the body is unnamed.
 *
 * For colon defs the source is exactly what the user typed, comments
 * and whitespace included. */
static void p_see(cell *cfa) {
	(void)cfa;

	POP(xt);
	if (xt.tag != T_XT) {
		type_error("see");
		return;
	}
	int target_cfa = (int)xt.data;
	/* Walk the chain to find the name for this CFA; if we don't find
	 * one, the xt is an anonymous quotation. */
	const char *name = NULL;
	for (int cf = latest_cfa; cf != 0; cf = (int)LINK(cf)) {
		if (cf == target_cfa) { name = &namepool[NAMEIDX(cf)]; break; }
	}
	cfa_handler handler = (cfa_handler)dict[target_cfa];
	if (handler == docol) {
		if (!name) {
			/* Anonymous quotation. No header means no SRCIDX field —
			 * dict[cfa-1] is whatever happened to be there, not a valid
			 * source-pool offset, so we don't try to read it. */
			printf("[: ... :]  \\ anonymous, no source\n");
		} else {
			int src_idx = (int)SRCIDX(target_cfa);
			if (src_idx > 0) printf(": %s%s;\n", name, &source_pool[src_idx]);
			else             printf(": %s ... ;  \\ no source captured\n", name);
		}
	} else if (handler == dovar) {
		Val val; val.tag = (Tag)dict[target_cfa + 1]; val.data = dict[target_cfa + 2];
		printf("variable %s  \\ current value: ", name ? name : "?");
		print_val(val);
		putchar('\n');
	} else if (handler == dosym) {
		printf("symbol %s\n", name ? name : "?");
	} else {
		printf("%s is a primitive\n", name ? name : "?");
	}
	fflush(stdout);
}


/* ---- immediate words: control flow via back-patching ----------------- */
/* "Immediate" words execute even when the outer interpreter is in
 * compile mode. They are how control structures and definition
 * terminators work: IF does not compile itself into the new word's body
 * — it RUNS, at compile time, and emits the right inline branch
 * instructions into the body.
 *
 * All forward-jumping control structures share the same trick. At the
 * site of the forward jump (e.g. IF), we emit a branch instruction
 * followed by a placeholder offset of zero. We don't know yet how far the
 * jump goes; the matching closing word (THEN, ELSE) will. So at compile
 * time, IF leaves the address of the placeholder cell on the data stack.
 * THEN later pops that address and writes the now-known offset into it.
 * This is "back-patching", and it's the cleanest way to handle forward
 * references in a single-pass compiler.
 *
 * The data stack is doing double duty here: at compile time it carries
 * compile-time bookkeeping (the placeholder address), and at run time it
 * carries the user's values. They never coexist — compile-time stack use
 * is bracketed by the immediate words, which clean up after themselves. */

/* ; — close the current colon definition. Beyond the obvious work
 * (emit EXIT, leave compile mode), this also captures the body source
 * text and stores it for image save.
 *
 * The capture works because the outer interpreter has just consumed the
 * ; token: INBUF_POS points at the first whitespace after ;, so the
 * ; itself lives at inbuf_pos - 1 in inbuf. The body source spans from
 * COMPILING_SRC_START (set by : right after the name was read) up
 * to but not including inbuf_pos - 1. We copy those bytes verbatim into
 * SOURCE_POOL and record the offset in the new word's SRCIDX header
 * cell. LATEST_CFA still refers to the word being closed — the body
 * compilation didn't create any new headers (anon quotations don't have
 * headers, by design) so it hasn't moved.
 *
 * For multi-line definitions to work, the REPL main loop must NOT have
 * reset inbuf between lines. See the "outer REPL loop" code at the
 * bottom of main: while COMPILING is set, inbuf accumulates so this
 * slice is contiguous. */
static void p_semi(cell *cfa) {
	(void)cfa;

	emit((cell)exit_cfa);
	if (compiling_src_start > 0 && latest_cfa != 0) {
		int src_end = inbuf_pos - 1;                  /* position of the ';' */
		int src_len = src_end - compiling_src_start;
		if (src_len < 0) src_len = 0;
		if (sources_here + src_len + 1 > SOURCEPOOL) {
			fail("source pool full");
		} else {
			int source_offset = sources_here;
			memcpy(&source_pool[sources_here],
					&inbuf[compiling_src_start],
					(size_t)src_len);
			source_pool[sources_here + src_len] = 0;
			sources_here += src_len + 1;
			SRCIDX(latest_cfa) = source_offset;
		}
	}
	compiling = 0;
	compiling_src_start = 0;
}
static void p_if(cell *cfa) {
	(void)cfa;

	emit((cell)zbranch_cfa);
	push(make_float((double)here));
	emit(0);
}
static void p_then(cell *cfa) {
	(void)cfa;

	POP(slot_val);
	int slot = (int)unpack_float(slot_val);
	dict[slot] = (here - slot);
}
static void p_else(cell *cfa) {
	(void)cfa;

	POP(slot_val);
	int slot = (int)unpack_float(slot_val);
	emit((cell)branch_cfa);
	push(make_float((double)here));
	emit(0);
	dict[slot] = (here - slot);
}
static void p_begin(cell *cfa) {
	(void)cfa;
	push(make_float((double)here));
}
static void p_until(cell *cfa) {
	(void)cfa;

	POP(back_val);
	int back = (int)unpack_float(back_val);
	emit((cell)zbranch_cfa);
	emit(back - here);
}
static void p_again(cell *cfa) {
	(void)cfa;

	POP(back_val);
	int back = (int)unpack_float(back_val);
	emit((cell)branch_cfa);
	emit(back - here);
}

/* [: and :] — anonymous colon definitions ("quotations"). [: starts
 * compiling a nameless word; :] closes it and the resulting xt is the
 * payload. Used for higher-order code: { 1 2 3 } [: 2 * :] map.
 *
 * The anon has no dictionary header — it's never looked up by name and
 * skipping the link/flags/name fields means the dictionary chain stays
 * clean (FORGET works as before, and the GC's chain-based body walker
 * doesn't get confused by anon headers sitting in the middle of an
 * enclosing word's body). We just lay down a docol cell and a body.
 *
 * Nested case (inside a colon definition): we need to skip over the anon
 * at run time so the enclosing code reaches it only via EXECUTE. So
 * we emit a forward (branch) in the enclosing body, then lay down the
 * anon inline, then (after :]) patch the branch to land past the anon
 * and emit a (lit) that pushes the anon's xt. The compile-time data
 * stack carries the anon's CFA and the branch slot to be patched.
 *
 * Top-level case: there's no enclosing body to skip past, so we just
 * lay down the anon. The compile-time data stack still carries the
 * anon's CFA; the slot field is -1 as a flag meaning "no patch needed,
 * push the xt for the user instead of compiling a literal."
 *
 * Trade-off worth knowing about: because anons have no header, they
 * also have no SRCIDX, which means SAVE can't preserve them as xt
 * values on the data stack. An anon embedded inside a named definition
 * round-trips fine (its source is part of the enclosing word's captured
 * body), but a bare [: ... :] left on the stack at save time has no
 * recoverable identity. See p_save for how that case is flagged. */
static void p_qcolon(cell *cfa) {
	(void)cfa;

	int branch_slot = -1;
	if (compiling) {
		emit((cell)branch_cfa);
		branch_slot = here;
		emit(0);
	}
	int anon_cfa = here;
	emit((cell)&docol);
	compiling = 1;
	push(make_float((double)anon_cfa));
	push(make_float((double)branch_slot));
}
static void p_qsemi(cell *cfa) {
	(void)cfa;

	emit((cell)exit_cfa);
	POP(branch_slot_val);
	POP(anon_cfa_val);
	int branch_slot = (int)unpack_float(branch_slot_val);
	int anon_cfa    = (int)unpack_float(anon_cfa_val);
	if (branch_slot < 0) {
		compiling = 0;
		push(make_xt(anon_cfa));
	} else {
		dict[branch_slot] = (here - branch_slot);
		emit_val_literal(make_xt(anon_cfa));
	}
}


/* ---- defining words: : variable symbol ' forget---------------------- */
/* All defining words follow the same pattern: read the next source
 * token (the new word's name), create a header for it, and lay down the
 * appropriate code field handler. They differ only in which handler.
 *
 * : writes docol and switches to compile mode. The subsequent tokens,
 * up to ;, get compiled into the body.
 *
 * VARIABLE writes dovar and reserves two cells for the storage.
 *
 * SYMBOL writes dosym and reserves nothing — the symbol's identity is
 * its own CFA, no body needed. */

static char *next_token(void);

static void p_tick(cell *cfa) {
	(void)cfa;

	char *token = next_token();
	if (!token) { type_error("'"); return; }
	int target_cfa = find(token);
	if (!target_cfa) { fail("%s", token); return; }
	Val value = make_xt(target_cfa);
	if (compiling) emit_val_literal(value);
	else push(value);
}

static void p_colon(cell *cfa) {
	(void)cfa; 

	char *token = next_token();
	if (!token) {
		fail(": needs a name"); 
		return; 
	}

	create_header(token, 0);
	emit((cell)&docol);
	compiling = 1;
	/* Remember where in inbuf the body source starts so ; can copy it
	 * out. INBUF_POS here is the inbuf position just past the name token,
	 * which is exactly where we want the captured source to begin. */
	compiling_src_start = inbuf_pos;
}
static void p_variable(cell *cfa) {
	(void)cfa;

	char *token = next_token();
	if (!token) {
		fail("variable needs a name");
		return;
	}

	create_header(token, 0);
	emit((cell)&dovar);
	emit((cell)T_FLOAT);
	/* Initial value 0.0 — emit the IEEE-754 bit pattern of zero, via a
	 * cell-sized temporary so the float bits land in the int64 cell. */
	double zero_value = 0.0;
	cell   zero_bits;
	memcpy(&zero_bits, &zero_value, 8);
	emit(zero_bits);
}
static void p_symbol(cell *cfa) {
	(void)cfa; 

	char *token = next_token();
	if (!token) {
		fail("symbol needs a name"); 
		return; 
	}

	create_header(token, 0);
	emit((cell)&dosym);
	/* Body cell: the symbol's interned offset. dosym reads cfa[1]. */
	emit((cell)intern_symbol(token));
}

/* forget: discard the named word and everything defined after it. We
 * roll back here (so the dictionary memory above is available again),
 * names_here (so the name pool reclaims any names of forgotten words),
 * latest_cfa (so the linked list's head moves back), and sources_here
 * (so any captured body sources for forgotten colon defs are dropped).
 * Objects in the object registry that were referenced only from
 * forgotten code are no longer reachable from any root, so the next
 * GC will sweep them up.
 *
 * The sources_here rollback is more involved than the other two: words
 * that survived the forget might have sources at scattered offsets in
 * source_pool, so we walk the surviving dictionary chain to find the
 * highest used source-pool extent, and set sources_here there. Words
 * without a stored source (SRCIDX == 0) are skipped. */
static void p_forget(cell *cfa) {
	(void)cfa; 

	char *token = next_token();
	if (!token) {
		fail("forget needs a name"); 
		return; 
	}
	int target_cfa = find(token);
	if (!target_cfa) {
		fail("%s", token);
		return; 
	}
	here       = target_cfa - 4;
	names_here = (int)NAMEIDX(target_cfa);
	latest_cfa = (int)LINK(target_cfa);

	int max_src_end = 1;  /* offset 0 is reserved as "no source" sentinel */
	for (int surviving_cfa = latest_cfa; surviving_cfa != 0; surviving_cfa = (int)LINK(surviving_cfa)) {
		int src_offset = (int)SRCIDX(surviving_cfa);
		if (src_offset > 0) {
			int src_end = src_offset + (int)strlen(&source_pool[src_offset]) + 1;
			if (src_end > max_src_end) max_src_end = src_end;
		}
	}
	sources_here = max_src_end;
}


/* ---- input buffer and tokeniser -------------------------------------- */
/* The outer interpreter reads lines and accumulates them in INBUF. A
 * pointer INBUF_POS walks through the buffer producing tokens. Most tokens
 * are whitespace-delimited and trivial. Two situations need special
 * handling:
 *
 *   · String literals starting with " are read up to the matching ",
 *     not whitespace. If end-of-buffer is reached before the closing
 *     quote, the parser sets NEED_MORE and returns; main() reads
 *     another line and appends.
 *
 *   · ( and \ introduce Forth-style comments — paren comments end at
 *     ), line comments end at newline. These are handled inline by the
 *     dispatcher rather than the tokenizer.
 *
 * The buffer itself (INBUF, INBUF_LEN, INBUF_POS, NEED_MORE) is
 * declared up in the "universe of state" section because : and ;
 * need to read inbuf_pos during source capture. */

static void inbuf_reset(void) { inbuf_len = 0; inbuf_pos = 0; inbuf[0] = 0; need_more = 0; }

static char tokbuf[INBUFSZ];

static int read_string_literal(void) {
	int start = inbuf_pos + 1;          /* skip the opening " */
	int cursor = start;
	while (cursor < inbuf_len && inbuf[cursor] != '"') cursor++;
	if (cursor >= inbuf_len) { need_more = 1; return -1; }
	int length = cursor - start;
	memcpy(tokbuf, inbuf + start, (size_t)length);
	tokbuf[length] = 0;
	inbuf_pos = cursor + 1;             /* skip the closing " */
	return length;
}

static char *next_token(void) {
	while (inbuf_pos < inbuf_len && isspace((unsigned char)inbuf[inbuf_pos])) inbuf_pos++;
	if (inbuf_pos >= inbuf_len) return NULL;
	int start = inbuf_pos;
	while (inbuf_pos < inbuf_len && !isspace((unsigned char)inbuf[inbuf_pos])) inbuf_pos++;
	int length = inbuf_pos - start;
	if (length >= (int)sizeof(tokbuf)) length = sizeof(tokbuf) - 1;
	memcpy(tokbuf, inbuf + start, (size_t)length);
	tokbuf[length] = 0;
	return tokbuf;
}

static int parse_float(const char *text, double *out) {
	if (!*text) return 0;

	char *end_of_number;
	double value = strtod(text, &end_of_number);
	if (*end_of_number != 0) return 0;
	*out = value;
	return 1;
}


/* ---- string interpolation -------------------------------------------- */
/* When a string literal is encountered, we examine its content for {n}
 * placeholders. Each one is replaced with the value at position n on the
 * data stack, counting from the top (0 = top). All referenced stack
 * positions are dropped together at the end — references happen before
 * the drop, so they all see the original stack state.
 *
 * We do two passes: the first determines the maximum index referenced,
 * the second builds the output. The first pass also serves to validate
 * — if any reference is out of bounds we set the error flag and return
 * an empty string. */

static int interpolate(int template_handle) {
	Object *template = objects[template_handle];

	/* First pass: scan for {n} placeholders, track the highest stack
	 * index referenced (so we know how many to drop at the end). */
	int max_ref = -1, any_placeholders = 0;
	for (int cursor = 0; cursor < template->len; ) {
		if (template->bytes[cursor] == '{') {
			int scan = cursor + 1, digit_value = 0, saw_digit = 0;
			while (scan < template->len && isdigit((unsigned char)template->bytes[scan])) {
				digit_value = digit_value * 10 + (template->bytes[scan] - '0');
				scan++;
				saw_digit = 1;
			}
			if (saw_digit && scan < template->len && template->bytes[scan] == '}') {
				if (digit_value > max_ref) max_ref = digit_value;
				any_placeholders = 1;
				cursor = scan + 1;
				continue;
			}
		}
		cursor++;
	}

	/* Second pass: emit, substituting {n} with the rendered value at
	 * stack position n (0 = top). */
	char *out_buffer = malloc((size_t)template->len * 4 + 64);
	int out_length = 0;
	for (int cursor = 0; cursor < template->len; ) {
		if (template->bytes[cursor] == '{') {
			int scan = cursor + 1, digit_value = 0, saw_digit = 0;
			while (scan < template->len && isdigit((unsigned char)template->bytes[scan])) {
				digit_value = digit_value * 10 + (template->bytes[scan] - '0');
				scan++;
				saw_digit = 1;
			}
			if (saw_digit && scan < template->len && template->bytes[scan] == '}') {
				int stack_index = dsp - 1 - digit_value;
				if (stack_index < 0) {
					type_error("string interpolation: stack too shallow");
					free(out_buffer);
					return object_new_string("", 0);
				}
				char rendered[256];
				int rendered_length = 0;
				Val value = data_stack[stack_index];
				switch (value.tag) {
					case T_FLOAT: {
									  double number = unpack_float(value);
									  if (number == (double)(int64_t)number && number > -1e15 && number < 1e15)
										  rendered_length = snprintf(rendered, sizeof(rendered), "%lld", (long long)number);
									  else
										  rendered_length = snprintf(rendered, sizeof(rendered), "%g", number);
									  break;
								  }
					case T_SYM:
								  rendered_length = snprintf(rendered, sizeof(rendered),
										  "%s", &symbol_pool[value.data]);
								  break;
					case T_STRING: {
									   Object *string_obj = objects[value.data];
									   int copy_length = MIN(string_obj->len, (int)sizeof(rendered) - 1);
									   memcpy(rendered, string_obj->bytes, (size_t)copy_length);
									   rendered[copy_length] = 0;
									   rendered_length = copy_length;
									   break;
								   }
					default:
								   rendered_length = snprintf(rendered, sizeof(rendered), "<?>");
								   break;
				}
				memcpy(out_buffer + out_length, rendered, (size_t)rendered_length);
				out_length += rendered_length;
				cursor = scan + 1;
				continue;
			}
		}
		out_buffer[out_length++] = template->bytes[cursor++];
	}

	/* Drop the referenced stack positions in one go. */
	if (any_placeholders && max_ref >= 0) {
		int items_to_drop = max_ref + 1;
		if (items_to_drop > dsp) items_to_drop = dsp;
		dsp -= items_to_drop;
	}

	int result_handle = object_new_string(out_buffer, out_length);
	free(out_buffer);
	return result_handle;
}


/* ---- the outer interpreter ------------------------------------------- */
/* The dispatcher: for each token (or special character) decide what to
 * do.
 *
 *   · String literals ("...") are handled specially: we read until the
 *     closing quote (spanning lines if necessary), then either push the
 *     resulting string at interpret time (after interpolation) or emit a
 *     (dostr) reference plus the template handle at compile time.
 *
 *   · Comments ( ... ) and \ to end of line are dropped.
 *
 *   · A known word: if we're compiling and it isn't immediate, emit a
 *     reference; otherwise execute it now.
 *
 *   · A number: if compiling, emit a tagged literal; otherwise push.
 *
 *   · Anything else: report and abort the current input. */

static void run_outer(void) {
	while (!error_flag) {
		while (inbuf_pos < inbuf_len && isspace((unsigned char)inbuf[inbuf_pos])) inbuf_pos++;
		if (inbuf_pos >= inbuf_len) return;

		char ch = inbuf[inbuf_pos];
		if (ch == '"') {
			int n = read_string_literal();
			if (n < 0) return;
			int handle = object_new_string(tokbuf, n);
			if (compiling) {
				emit((cell)dostr_cfa);
				emit((cell)handle);
			} else {
				int r = interpolate(handle);
				push(make_string(r));
			}
			continue;
		}
		if (ch == '(' && (inbuf_pos + 1 >= inbuf_len || isspace((unsigned char)inbuf[inbuf_pos + 1]))) {
			while (inbuf_pos < inbuf_len && inbuf[inbuf_pos] != ')') inbuf_pos++;
			if (inbuf_pos < inbuf_len) inbuf_pos++;
			continue;
		}
		if (ch == '\\' && (inbuf_pos + 1 >= inbuf_len || isspace((unsigned char)inbuf[inbuf_pos + 1]))) {
			while (inbuf_pos < inbuf_len && inbuf[inbuf_pos] != '\n') inbuf_pos++;
			continue;
		}

		char *tok = next_token();
		if (!tok) return;
		int cf = find(tok);
		if (cf) {
			if (compiling && !IS_IMM(cf)) emit((cell)cf);
			else execute_cfa(cf);
			continue;
		}

		double dv;
		if (parse_float(tok, &dv)) {
			Val value = make_float(dv);
			if (compiling) emit_val_literal(value);
			else push(value);
			continue;
		}
		fail("%s", tok);
		return;
	}
}


/* ---- load: process a source file as if typed at the REPL ------------- */
/* LOAD takes a filename as a string on the stack and runs the file's
 * contents through run_outer() as if the user had typed them.
 *
 * The implementation swaps inbuf to hold the file's contents while
 * processing, then restores the previous input state. Nested loads
 * stack naturally — each call saves its predecessor's buffer. Errors
 * inside the loaded file propagate to the caller via the global ERROR_FLAG,
 * which main() handles by clearing the stacks and compile state.
 *
 * Two structural problems specific to file input are caught explicitly:
 * a string literal with no closing " would leave the parser in
 * need_more state forever (there's no next line to fetch from a file
 * the way there is from stdin); and a colon definition with no ;
 * would leave the system in compile mode after load returns. Both
 * become errors here. */

/* Record a filename in the session-level loaded_files history if it
 * isn't already there. Called from p_load at the outermost level so
 * RELOAD later can replay just the user's explicit load sequence —
 * files pulled in transitively by other files aren't recorded. */
static void record_loaded_file(const char *filename) {
	for (int i = 0; i < n_loaded_files; i++) {
		if (strcmp(loaded_files[i], filename) == 0) return;
	}
	if (n_loaded_files >= MAX_LOADED_FILES) {
		fail("load: %d-file history limit reached", MAX_LOADED_FILES);
		return;
	}
	loaded_files[n_loaded_files] = strdup(filename);
	n_loaded_files++;
}

/* Run a source file. Factored out of p_load so RELOAD can call it
 * directly without going through the data stack or the loaded_files
 * recording path. The caller is responsible for keeping the filename
 * string alive across this call. */
static void load_file(const char *filename) {
	FILE *file = fopen(filename, "r");
	if (!file) {
		fail("cannot open %s", filename);
		return;
	}

	fseek(file, 0, SEEK_END);
	long file_size = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (file_size < 0 || file_size >= INBUFSZ) {
		fail("%s too large or invalid (%ld bytes, max %d)",
				filename, file_size, INBUFSZ - 1);
		fclose(file);
		return;
	}

	char *saved_inbuf_contents = malloc((size_t)inbuf_len + 1);
	memcpy(saved_inbuf_contents, inbuf, (size_t)inbuf_len);
	int saved_inbuf_len = inbuf_len;
	int saved_inbuf_pos = inbuf_pos;
	int saved_need_more = need_more;

	size_t bytes_read = fread(inbuf, 1, (size_t)file_size, file);
	fclose(file);
	inbuf[bytes_read] = 0;
	inbuf_len = (int)bytes_read;
	inbuf_pos = 0;
	need_more = 0;

	load_depth++;
	run_outer();
	load_depth--;

	if (!error_flag && need_more) {
		fail("%s: unterminated string literal", filename);
	}
	if (!error_flag && compiling) {
		fail("%s: unterminated definition", filename);
		compiling = 0;
	}

	memcpy(inbuf, saved_inbuf_contents, (size_t)saved_inbuf_len);
	inbuf[saved_inbuf_len] = 0;
	inbuf_len = saved_inbuf_len;
	inbuf_pos = saved_inbuf_pos;
	need_more = saved_need_more;
	free(saved_inbuf_contents);
}

static void p_load(cell *cfa) {
	(void)cfa;

	POP(value);
	if (value.tag != T_STRING) {
		type_error("load");
		return;
	}
	gc_root_push(value);

	const char *filename = objects[value.data]->bytes;
	if (load_depth == 0)
		record_loaded_file(filename);
	load_file(filename);

	gc_root_pop();
}

/* Forward decl — FORGET_USER lives down beside the image primitives
 * (since it's also called from LOAD-IMAGE), but P_RELOAD here needs
 * to know it exists. */
static void forget_user(void);

/* RELOAD truncates user state and re-runs every source file that
 * LOAD recorded this session, in the order they were first loaded.
 * lib.l4 is the first entry (main() loads it via p_load at startup), so
 * the standard library is rebuilt before any user files. Useful when
 * editing a .l4 file in another window — typing RELOAD re-executes
 * the whole user chain against the current file contents.
 *
 * Nested loads inside source files were not recorded (only depth-0
 * calls are), so we don't double-load anything during reload. */
static void p_reload(cell *cfa) {
	(void)cfa;

	forget_user();

	for (int i = 0; i < n_loaded_files; i++) {
		load_file(loaded_files[i]);
		if (error_flag) return;
	}
}



/* ---- garbage collection: mark and sweep ------------------------------ */
/* Strings, sets, and arrays accumulate in objects[] as side effects of
 * normal execution — every + on strings, every set operation, every
 * MAP registers new Objects. Without reclamation the array fills and
 * allocation fails. We run a stop-the-world mark-and-sweep when
 * object_alloc_slot can't find a free slot.
 *
 * Roots are everywhere a live handle can hide:
 *
 *   · the data stack and return stack (any Val with a heap tag)
 *   · the gc_roots[] stack (C-level temporaries from primitives like map)
 *   · the dictionary, both as inline literals in colon bodies and as
 *     initial values in variable bodies
 *
 * Marking sets and arrays is recursive — their items are themselves Vals
 * that may carry heap handles. There are no cycles possible here: no
 * primitive constructs a set or array that references itself, since the
 * compound types are all built by collecting from the data stack or by
 * applying pure constructors. So a plain depth-first mark suffices.
 *
 * The dictionary walk is the only fiddly part. A colon body is a mix of
 * CFA references (one cell each) and inline data — two cells after a
 * (lit), one cell after a (branch) or (0branch) or (dostr). To identify
 * the heap-bearing literals we recognise the body cell as a reference to
 * (lit) or (dostr) and read the following cells accordingly. We walk
 * each word's body using the next word's header as the end boundary,
 * which is why we collect the dictionary's CFAs in ascending order
 * before walking. The latest word's body extends up to HERE. */

static int object_mark[MAXOBJS];

static void mark_val(Val value) {
	if (value.tag != T_STRING && 
			value.tag != T_SET && 
			value.tag != T_ARRAY && 
			value.tag != T_MATRIX &&
			value.tag != T_CONT) return;

	int handle = (int)value.data;
	if (handle < 0 || handle >= MAXOBJS || !objects[handle] || object_mark[handle]) return;

	object_mark[handle] = 1;
	Object *obj = objects[handle];
	if (obj->kind == OBJECT_SET || obj->kind == OBJECT_ARRAY) {
		for (int i = 0; i < obj->len; i++) mark_val(obj->items[i]);
	} else if (obj->kind == OBJECT_CONTINUATION) {
		for (int i = 0; i < obj->continuation.return_len; i++)
			mark_val(obj->continuation.return_slice[i]);
	}
}

static void mark_body(int body_start, int body_end) {
	int cursor = body_start;

	while (cursor < body_end) {
		cell ref = dict[cursor];
		if (ref == (cell)literal_cfa && cursor + 2 < body_end) {
			Tag tag = (Tag)dict[cursor + 1];
			Val value; value.tag = tag; value.data = dict[cursor + 2];
			mark_val(value);
			cursor += 3;
		} else if (ref == (cell)dostr_cfa && cursor + 1 < body_end) {
			Val value; value.tag = T_STRING; value.data = dict[cursor + 1];
			mark_val(value);
			cursor += 2;
		} else if ((ref == (cell)branch_cfa
					|| ref == (cell)zbranch_cfa) && cursor + 1 < body_end) {
			cursor += 2;
		} else {
			cursor++;
		}
	}
}

static void gc(void) {
	memset(object_mark, 0, sizeof(object_mark));

	for (int i = 0; i < dsp; i++)        mark_val(data_stack[i]);
	for (int i = 0; i < rsp; i++)        mark_val(return_stack[i]);
	for (int i = 0; i < side_dsp; i++)   mark_val(side_stack[i]);
	for (int i = 0; i < n_gc_roots; i++) mark_val(gc_roots[i]);

	/* Collect every word's CFA into ascending order so we know where
	 * each body ends — body_end of word N is (CFA of word N+1) − 4
	 * (skipping the next word's 4-cell header). The last (highest-CFA)
	 * word's body runs to HERE. */
	static int sorted_cfas[MEMSZ / 4];
	int num_cfas = 0;
	for (int cfa = latest_cfa; cfa != 0; cfa = (int)LINK(cfa)) {
		if (num_cfas < (int)(sizeof sorted_cfas / sizeof sorted_cfas[0]))
			sorted_cfas[num_cfas++] = cfa;
	}
	/* Insertion sort — fine for the sizes we expect; the dictionary
	 * chain is short relative to MEMSZ. */
	for (int i = 1; i < num_cfas; i++) {
		int current = sorted_cfas[i];
		int slot = i - 1;
		while (slot >= 0 && sorted_cfas[slot] > current) {
			sorted_cfas[slot + 1] = sorted_cfas[slot];
			slot--;
		}
		sorted_cfas[slot + 1] = current;
	}

	for (int i = 0; i < num_cfas; i++) {
		int cfa = sorted_cfas[i];
		int body_start = cfa + 1;
		int body_end   = (i + 1 < num_cfas) ? sorted_cfas[i + 1] - 4 : here;
		cfa_handler handler = (cfa_handler)dict[cfa];
		if (handler == docol) {
			mark_body(body_start, body_end);
		} else if (handler == dovar && body_start + 1 < body_end) {
			Val value;
			value.tag  = (Tag)dict[body_start];
			value.data = dict[body_start + 1];
			mark_val(value);
		}
		/* primitives, dosym: no body to scan */
	}

	/* Sweep — free anything unmarked. */
	for (int handle = 0; handle < n_objects; handle++) {
		Object *obj = objects[handle];
		if (!obj || object_mark[handle]) continue;

		switch (obj->kind) {
			case OBJECT_STRING: free(obj->bytes); break;
			case OBJECT_SET:
			case OBJECT_ARRAY: free(obj->items); break;
			case OBJECT_MATRIX: free(obj->matrix.elements); break;
			case OBJECT_CONTINUATION: free(obj->continuation.return_slice); break;
		}
		free(objects[handle]);
		objects[handle] = NULL;
	}
}

static void p_gc(cell *cfa) {
	(void)cfa;

	gc();
}

static void p_clear(cell *cfa) {
	(void)cfa;

	dsp = 0;
}


/* ---- text save: write user vocabulary as a re-loadable .l4 file ---- */
/* SAVE writes the user's vocabulary as a .l4 source file that
 * LOAD can re-execute. Scope: only words defined past lib.l4 (the
 * standard library reloads itself automatically next time). Form:
 *
 *   : name <body source> ;       — colon defs, body from source_pool
 *   variable name                 — declaration only; no value
 *   symbol name                   — declaration only
 *
 * Runtime state (variable values, data stack) is deliberately omitted
 * — that's what SAVE-IMAGE is for. The two words address different
 * needs: SAVE for source you want to share or diff, SAVE-IMAGE
 * for "stop and resume." */
static void p_save(cell *cfa) {
	(void)cfa;

	POP(value);
	if (value.tag != T_STRING) {
		type_error("save");
		return;
	}
	gc_root_push(value);
	const char *filename = objects[value.data]->bytes;

	FILE *file = fopen(filename, "w");
	if (!file) {
		fail("cannot create %s", filename);
		gc_root_pop();
		return;
	}

	/* Walk the dictionary chain newest-first, collect, then reverse so
	 * we emit oldest-first — later defs may reference earlier ones, so
	 * LOAD has to see the earlier ones first.
	 *
	 * Stop at the lib.l4 boundary: anything at or below
	 * lib_end_latest_cfa is standard library, which the next
	 * interpreter loads on its own. */
	int collected_cfas[MEMSZ];
	int num_cfas = 0;
	for (int cfa = latest_cfa; cfa > lib_end_latest_cfa; cfa = (int)LINK(cfa)) {
		if (num_cfas < MEMSZ) collected_cfas[num_cfas++] = cfa;
	}

	fprintf(file, "\\ logicforth vocabulary\n\n");

	for (int i = num_cfas - 1; i >= 0; i--) {
		int cfa = collected_cfas[i];
		const char *name = &namepool[NAMEIDX(cfa)];
		cfa_handler handler = (cfa_handler)dict[cfa];

		if (handler == docol) {
			int src_offset = (int)SRCIDX(cfa);
			const char *body_source = (src_offset > 0) ? &source_pool[src_offset] : "";
			fprintf(file, ": %s%s;\n", name, body_source);
		} else if (handler == dovar) {
			fprintf(file, "variable %s\n", name);
		} else if (handler == dosym) {
			fprintf(file, "symbol %s\n", name);
		}
		/* Primitives have no on-disk representation — they come back
		 * automatically when main() re-registers them at startup. */
	}

	fclose(file);
	gc_root_pop();
}

/* ---- binary image: save-image / load-image --------------------------
 *
 * One file, host-byte-order binary (same machine in / same machine out
 * only — no endianness conversion). Layout:
 *
 *   header   : magic[4] "LF4I", uint32 version
 *   sizes    : 7 × int32  — user_dict_cells, user_namepool_bytes,
 *              user_sourcepool_bytes, user_symbolpool_bytes,
 *              latest_cfa, dsp, n_objects
 *   markers  : 5 × int32 — initial_here, initial_latest_cfa,
 *              initial_names_here, initial_sources_here,
 *              initial_symbol_pool_here. Cross-checked against the live
 *              interpreter on load; mismatch refuses the load.
 *   dict     : user_dict_cells × int64 — raw dump of dict[initial_here..]
 *   handlers : int32 user_word_count, then user_word_count × {int32
 *              cfa_abs, uint8 kind} — kind ∈ {1=docol, 2=dovar, 3=dosym}.
 *              Reapplied to dict[cfa_abs] on load to replace the
 *              process-local handler pointers.
 *   pools    : raw bytes of namepool / source_pool / symbol_pool slices
 *   objects  : for slot in 0..n_objects-1, uint8 presence, then if
 *              present uint8 kind + int32 len + int32 cap + payload
 *              (strings: len bytes; sets/arrays: len Vals; matrices:
 *              int32 rows + int32 cols + rows*cols doubles).
 *   stack    : dsp Vals.
 *
 * Vals serialise as int32 tag followed by int64 data — avoids depending
 * on the Val struct's compiler-chosen padding. */

#define IMAGE_MAGIC    "LF4I"
#define IMAGE_VERSION  ((uint32_t)1)
#define HANDLER_DOCOL  1
#define HANDLER_DOVAR  2
#define HANDLER_DOSYM  3

static void w_u8 (FILE *f, uint8_t v)  { fwrite(&v, 1, 1, f); }
static void w_i32(FILE *f, int32_t v)  { fwrite(&v, 4, 1, f); }
static void w_i64(FILE *f, int64_t v)  { fwrite(&v, 8, 1, f); }
static void w_val(FILE *f, Val v)      { w_i32(f, (int32_t)v.tag); w_i64(f, v.data); }

static int r_u8 (FILE *f, uint8_t *v)  { return fread(v, 1, 1, f) == 1; }
static int r_u32(FILE *f, uint32_t *v) { return fread(v, 4, 1, f) == 1; }
static int r_i32(FILE *f, int32_t *v)  { return fread(v, 4, 1, f) == 1; }
static int r_i64(FILE *f, int64_t *v)  { return fread(v, 8, 1, f) == 1; }
static int r_val(FILE *f, Val *v) {
	int32_t tag;
	int64_t data;
	if (!r_i32(f, &tag) || !r_i64(f, &data)) return 0;
	v->tag = (Tag)tag;
	v->data = data;
	return 1;
}

static void p_save_image(cell *cfa) {
	(void)cfa;

	POP(value);
	if (value.tag != T_STRING) {
		type_error("save-image");
		return;
	}
	gc_root_push(value);
	const char *filename = objects[value.data]->bytes;

	FILE *file = fopen(filename, "wb");
	if (!file) {
		fail("cannot create %s", filename);
		gc_root_pop();
		return;
	}

	/* Walk the user portion of the dict chain. Stop when a link points
	 * into primitive territory — that CFA is a primitive and not ours. */
	int user_word_count = 0;
	int collected[MEMSZ];
	for (int c = latest_cfa; c >= initial_here; c = (int)LINK(c)) {
		collected[user_word_count++] = c;
	}

	/* Header */
	fwrite(IMAGE_MAGIC, 1, 4, file);
	w_i32(file, (int32_t)IMAGE_VERSION);

	/* Sizes */
	int32_t user_dict_cells       = here             - initial_here;
	int32_t user_namepool_bytes   = names_here       - initial_names_here;
	int32_t user_sourcepool_bytes = sources_here     - initial_sources_here;
	int32_t user_symbolpool_bytes = symbol_pool_here - initial_symbol_pool_here;
	w_i32(file, user_dict_cells);
	w_i32(file, user_namepool_bytes);
	w_i32(file, user_sourcepool_bytes);
	w_i32(file, user_symbolpool_bytes);
	w_i32(file, latest_cfa);
	w_i32(file, dsp);
	w_i32(file, n_objects);

	/* Markers */
	w_i32(file, initial_here);
	w_i32(file, initial_latest_cfa);
	w_i32(file, initial_names_here);
	w_i32(file, initial_sources_here);
	w_i32(file, initial_symbol_pool_here);

	/* User dict slice (raw). CFA cells in here contain process-local
	 * handler pointers that the load path will overwrite using the
	 * handler table that follows. */
	for (int i = 0; i < user_dict_cells; i++)
		w_i64(file, (int64_t)dict[initial_here + i]);

	/* Handler tag table */
	w_i32(file, user_word_count);
	for (int i = 0; i < user_word_count; i++) {
		int c = collected[i];
		cfa_handler h = (cfa_handler)dict[c];
		uint8_t kind = (h == docol) ? HANDLER_DOCOL
			: (h == dovar) ? HANDLER_DOVAR
			: (h == dosym) ? HANDLER_DOSYM : 0;
		if (kind == 0) {
			fail("save-image: unrecognised handler at cfa %d", c);
			fclose(file);
			gc_root_pop();
			return;
		}
		w_i32(file, c);
		w_u8(file, kind);
	}

	/* Pool slices */
	fwrite(&namepool[initial_names_here],            1, (size_t)user_namepool_bytes,   file);
	fwrite(&source_pool[initial_sources_here],       1, (size_t)user_sourcepool_bytes, file);
	fwrite(&symbol_pool[initial_symbol_pool_here],   1, (size_t)user_symbolpool_bytes, file);

	/* Object table.*/
	for (int slot = 0; slot < n_objects; slot++) {
		Object *obj = objects[slot];
		if (!obj) {
			w_u8(file, 0);
			continue;
		}
		w_u8(file, 1);
		w_u8(file, (uint8_t)obj->kind);
		w_i32(file, obj->len);
		w_i32(file, obj->cap);
		switch (obj->kind) {
			case OBJECT_STRING:
				fwrite(obj->bytes, 1, (size_t)obj->len, file);
				break;
			case OBJECT_SET:
			case OBJECT_ARRAY:
				for (int j = 0; j < obj->len; j++) w_val(file, obj->items[j]);
				break;
			case OBJECT_MATRIX: {
									w_i32(file, obj->matrix.rows);
									w_i32(file, obj->matrix.columns);
									size_t n = (size_t)obj->matrix.rows * (size_t)obj->matrix.columns;
									if (n > 0) fwrite(obj->matrix.elements, sizeof(double), n, file);
									break;
								}
			case OBJECT_CONTINUATION:
								w_i32(file, obj->continuation.return_len);
								for (int j = 0; j < obj->continuation.return_len; j++)
									w_val(file, obj->continuation.return_slice[j]);
								w_i32(file, obj->continuation.resume_ip);
								break;
		}
	}

	/* Data stack */
	for (int i = 0; i < dsp; i++) w_val(file, data_stack[i]);

	fclose(file);
	gc_root_pop();
}

static void free_one_object(Object *obj) {
	switch (obj->kind) {
		case OBJECT_STRING: free(obj->bytes);            break;
		case OBJECT_SET:
		case OBJECT_ARRAY:  free(obj->items);            break;
		case OBJECT_MATRIX: free(obj->matrix.elements);  break;
		case OBJECT_CONTINUATION: free(obj->continuation.return_slice); break;
	}
	free(obj);
}

/* Roll every kind of user-area state back to the post-primitive-
 * registration boundary: dict, name/source/symbol pools, the object
 * registry, both stacks. Shared by LOAD-IMAGE (which then grafts
 * saved state on top) and RELOAD (which then re-runs the recorded
 * source files). Doesn't touch loaded_files or the lib_end snapshot —
 * those are session-level concerns outside the VM's own state. */
static void forget_user(void) {
	for (int i = 0; i < n_objects; i++) {
		if (objects[i]) {
			free_one_object(objects[i]);
			objects[i] = NULL;
		}
	}
	n_objects        = 0;
	dsp              = 0;
	rsp              = 0;
	here             = initial_here;
	latest_cfa       = initial_latest_cfa;
	names_here       = initial_names_here;
	sources_here     = initial_sources_here;
	symbol_pool_here = initial_symbol_pool_here;
}

static void p_load_image(cell *cfa) {
	(void)cfa;

	POP(value);
	if (value.tag != T_STRING) {
		type_error("load-image");
		return;
	}
	/* Copy the filename into a stack buffer — the live string object
	 * gets freed when we truncate state below, so we can't rely on the
	 * Val payload's bytes pointer past that point. */
	char filename[4096];
	int fnlen = objects[value.data]->len;
	if (fnlen >= (int)sizeof(filename)) {
		fail("filename too long");
		return;
	}
	memcpy(filename, objects[value.data]->bytes, (size_t)fnlen);
	filename[fnlen] = 0;

	FILE *file = fopen(filename, "rb");
	if (!file) {
		fail("cannot open %s", filename);
		return;
	}

	/* Header */
	char magic[4];
	if (fread(magic, 1, 4, file) != 4 || memcmp(magic, IMAGE_MAGIC, 4) != 0) {
		fail("%s: not a logicforth image", filename);
		fclose(file);
		return;
	}
	uint32_t version;
	if (!r_u32(file, &version) || version != IMAGE_VERSION) {
		fail("%s: version %u, expected %u", filename, version, IMAGE_VERSION);
		fclose(file);
		return;
	}

	/* Sizes */
	int32_t user_dict_cells, user_namepool_bytes, user_sourcepool_bytes, user_symbolpool_bytes;
	int32_t saved_latest_cfa, saved_dsp, saved_n_objects;
	if (!r_i32(file, &user_dict_cells)
			|| !r_i32(file, &user_namepool_bytes)
			|| !r_i32(file, &user_sourcepool_bytes)
			|| !r_i32(file, &user_symbolpool_bytes)
			|| !r_i32(file, &saved_latest_cfa)
			|| !r_i32(file, &saved_dsp)
			|| !r_i32(file, &saved_n_objects))
	{
		fail("%s: truncated sizes", filename);
		fclose(file);
		return;
	}

	/* Markers — must match this interpreter's bootstrap exactly */
	int32_t m_here, m_latest, m_names, m_sources, m_symbols;
	if (!r_i32(file, &m_here)    || !r_i32(file, &m_latest)
			|| !r_i32(file, &m_names)   || !r_i32(file, &m_sources)
			|| !r_i32(file, &m_symbols))
	{
		fail("%s: truncated markers", filename);
		fclose(file);
		return;
	}
	if (m_here    != initial_here              || m_latest  != initial_latest_cfa
			|| m_names   != initial_names_here        || m_sources != initial_sources_here
			|| m_symbols != initial_symbol_pool_here)
	{
		fail("%s: interpreter bootstrap mismatch (rebuild needed)", filename);
		fclose(file);
		return;
	}

	/* Bounds sanity */
	if (user_dict_cells       < 0 || initial_here       + user_dict_cells       > MEMSZ
			|| user_namepool_bytes   < 0 || initial_names_here + user_namepool_bytes   > NAMEPOOL
			|| user_sourcepool_bytes < 0 || initial_sources_here + user_sourcepool_bytes > SOURCEPOOL
			|| user_symbolpool_bytes < 0 || initial_symbol_pool_here + user_symbolpool_bytes > SYMBOLPOOL
			|| saved_dsp < 0 || saved_dsp > DSTACK
			|| saved_n_objects < 0 || saved_n_objects > MAXOBJS)
	{
		fail("%s: image sizes out of bounds", filename);
		fclose(file);
		return;
	}

	/* Truncate live state. We're past validation; from here on we
	 * either succeed or leave the interpreter in a partially-restored
	 * state — caller's responsibility to restart on failure. */
	forget_user();

	/* User dict slice */
	for (int i = 0; i < user_dict_cells; i++) {
		int64_t c;
		if (!r_i64(file, &c)) {
			fail("%s: truncated dict", filename);
			goto done;
		}
		dict[initial_here + i] = (cell)c;
	}
	here = initial_here + user_dict_cells;

	/* Handler-tag fixup */
	int32_t user_word_count;
	if (!r_i32(file, &user_word_count)) {
		fail("%s: missing handler table", filename);
		goto done;
	}
	for (int i = 0; i < user_word_count; i++) {
		int32_t c;
		uint8_t kind;
		if (!r_i32(file, &c) || !r_u8(file, &kind)) {
			fail("%s: truncated handler table", filename);
			goto done;
		}
		if (c < initial_here || c >= here) {
			fail("%s: handler cfa out of range", filename);
			goto done;
		}
		cfa_handler h = (kind == HANDLER_DOCOL) ? docol
			: (kind == HANDLER_DOVAR) ? dovar
			: (kind == HANDLER_DOSYM) ? dosym : NULL;
		if (!h) {
			fail("%s: bad handler kind %u", filename, kind);
			goto done;
		}
		dict[c] = (cell)h;
	}

	/* Pool slices */
	if (fread(&namepool[initial_names_here], 1, (size_t)user_namepool_bytes, file) != (size_t)user_namepool_bytes
			|| fread(&source_pool[initial_sources_here], 1, (size_t)user_sourcepool_bytes, file) != (size_t)user_sourcepool_bytes
			|| fread(&symbol_pool[initial_symbol_pool_here], 1, (size_t)user_symbolpool_bytes, file) != (size_t)user_symbolpool_bytes)
	{
		fail("%s: truncated pools", filename);
		goto done;
	}
	names_here       = initial_names_here       + user_namepool_bytes;
	sources_here     = initial_sources_here     + user_sourcepool_bytes;
	symbol_pool_here = initial_symbol_pool_here + user_symbolpool_bytes;

	/* Object table */
	for (int slot = 0; slot < saved_n_objects; slot++) {
		uint8_t presence;
		if (!r_u8(file, &presence)) {
			fail("%s: truncated objects", filename);
			goto done;
		}
		if (presence == 0) {
			objects[slot] = NULL;
			continue;
		}

		uint8_t kind;
		int32_t len, cap;
		if (!r_u8(file, &kind) || !r_i32(file, &len) || !r_i32(file, &cap)) {
			fail("%s: truncated object header", filename);
			goto done;
		}
		Object *obj = calloc(1, sizeof(*obj));
		obj->kind = kind;
		obj->len = len;
		obj->cap = cap;
		switch (kind) {
			case OBJECT_STRING:
				obj->bytes = malloc((size_t)len + 1);
				if (len > 0 && fread(obj->bytes, 1, (size_t)len, file) != (size_t)len) {
					free(obj->bytes);
					free(obj);
					fail("%s: truncated string", filename);
					goto done;
				}
				obj->bytes[len] = 0;
				break;
			case OBJECT_SET:
			case OBJECT_ARRAY:
				obj->items = malloc(sizeof(Val) * (size_t)MAX(cap, 1));
				for (int j = 0; j < len; j++) {
					if (!r_val(file, &obj->items[j])) {
						free(obj->items);
						free(obj);
						fail("%s: truncated items", filename);
						goto done;
					}
				}
				break;
			case OBJECT_MATRIX: {
									int32_t rows, cols;
									if (!r_i32(file, &rows) || !r_i32(file, &cols)) {
										free(obj);
										fail("%s: truncated matrix header", filename);
										goto done;
									}
									obj->matrix.rows = rows;
									obj->matrix.columns = cols;
									size_t n = (size_t)rows * (size_t)cols;
									obj->matrix.elements = calloc(n > 0 ? n : 1, sizeof(double));
									if (n > 0 && fread(obj->matrix.elements, sizeof(double), n, file) != n) {
										free(obj->matrix.elements);
										free(obj);
										fail("%s: truncated matrix data", filename);
										goto done;
									}
									break;
								}
			case OBJECT_CONTINUATION: {
										  int32_t return_len;
										  if (!r_i32(file, &return_len) || return_len < 0) {
											  free(obj);
											  fail("%s: bad continuation header", filename);
											  goto done;
										  }
										  obj->continuation.return_len = return_len;
										  obj->continuation.return_slice =
											  malloc(sizeof(Val) * (size_t)MAX(return_len, 1));
										  for (int j = 0; j < return_len; j++) {
											  if (!r_val(file, &obj->continuation.return_slice[j])) {
												  free(obj->continuation.return_slice);
												  free(obj);
												  fail("%s: truncated continuation slice", filename);
												  goto done;
											  }
										  }
										  int32_t resume_ip;
										  if (!r_i32(file, &resume_ip)
												  || resume_ip < initial_here || resume_ip >= here)
										  {
											  free(obj->continuation.return_slice);
											  free(obj);
											  fail("%s: continuation resume_ip out of range", filename);
											  goto done;
										  }
										  obj->continuation.resume_ip = resume_ip;
										  break;
									  }
			default:
									  free(obj);
									  fail("%s: unknown object kind %u", filename, kind);
									  goto done;
		}
		objects[slot] = obj;
	}
	n_objects = saved_n_objects;

	/* Data stack */
	for (int i = 0; i < saved_dsp; i++) {
		if (!r_val(file, &data_stack[i])) {
			fail("%s: truncated stack", filename);
			goto done;
		}
	}
	dsp = saved_dsp;

	latest_cfa = saved_latest_cfa;

done:
	fclose(file);
}


/* ---- matrix functions -------------------- */
/* create_matrix is the one non-void pop site in the file. POP's bare
 * RETURN doesn't fit a function that returns int — expand manually
 * with an explicit return -1 on error. */
static int create_matrix() {
	Val right = pop();
	if (error_flag) return -1;
	Val left = pop();
	if (error_flag) return -1;
	if (left.tag != T_FLOAT || right.tag != T_FLOAT) {
		type_error("matrix dimensions");
		return -1;
	}

	int num_rows = (int)(unpack_float(left));
	int num_columns = (int)(unpack_float(right));
	if (num_rows < 0 || num_columns < 0) {
		type_error("matrix dimensions");
		return -1;
	}

	return object_new_matrix(num_rows, num_columns);
}

static void p_0_matrix(cell *cfa) {
	(void)cfa;
	int matrix_handle = create_matrix();
	if (error_flag) return;
	push(make_matrix(matrix_handle));
}

static void p_diagonal_matrix(cell *cfa) {
	(void)cfa;

	p_dup(NULL);
	int diag_matrix_handle = create_matrix();
	if (error_flag) return;

	POP(diag_val);
	if (diag_val.tag != T_FLOAT) {
		type_error("diagonal matrix scalar");
		return;
	}

	Object *diag_matrix = objects[diag_matrix_handle];
	double diag_element = unpack_float(diag_val);
	for (int i = 0; i < diag_matrix->matrix.rows; i++) {
		MAT(diag_matrix, i, i) = diag_element;
	}

	push(make_matrix(diag_matrix_handle));
}

static void p_diagonal(cell *cfa) {
	(void)cfa;

	POP(source_val);
	if (source_val.tag != T_MATRIX) {
		type_error("diagonal requires matrix");
		return;
	}

	Object *source = objects[source_val.data];
	int diag_len = MIN(source->matrix.rows, source->matrix.columns);
	int diag_handle = object_new_matrix(1, diag_len);
	if (error_flag) return;
	Object *diagonal = objects[diag_handle];

	for (int i = 0; i < diag_len; i++)
		diagonal->matrix.elements[i] = MAT(source, i, i);

	push(make_matrix(diag_handle));
}

/* reshape: ( matrix rows cols -- new-matrix )
 * Same row-major elements, new dimensions. rows*cols must equal the
 * source's total element count. */
static void p_reshape(cell *cfa) {
	(void)cfa;

	POP(cols_val);
	if (cols_val.tag != T_FLOAT) {
		type_error("reshape cols");
		return;
	}
	int new_cols = (int)unpack_float(cols_val);

	POP(rows_val);
	if (rows_val.tag != T_FLOAT) {
		type_error("reshape rows");
		return;
	}
	int new_rows = (int)unpack_float(rows_val);

	POP(source_val);
	if (source_val.tag != T_MATRIX) {
		type_error("reshape needs matrix");
		return;
	}

	Object *source = objects[source_val.data];
	int total = source->matrix.rows * source->matrix.columns;
	if (new_rows * new_cols != total) {
		type_error("reshape: element count must match");
		return;
	}

	int target_handle = object_new_matrix(new_rows, new_cols);
	if (error_flag) return;
	Object *target = objects[target_handle];

	memcpy(target->matrix.elements, source->matrix.elements,
			(size_t)total * sizeof(double));

	push(make_matrix(target_handle));
}

static void p_matrix(cell *cfa) {
	(void)cfa;

	int matrix_handle = create_matrix();
	if (error_flag) return;

	POP(array_val);
	if (array_val.tag != T_ARRAY) {
		type_error("matrix needs array");
		return;
	}

	Object *matrix = objects[matrix_handle];
	Object *input_array = objects[array_val.data];
	int num_elements = matrix->matrix.rows * matrix->matrix.columns;
	if (input_array->len != num_elements) {
		type_error("matrix array length");
		return;
	}

	for (int i = 0; i < num_elements; i++) {
		if (input_array->items[i].tag != T_FLOAT) {
			type_error("matrix array element");
			return;
		}
		matrix->matrix.elements[i] = unpack_float(input_array->items[i]);
	}

	push(make_matrix(matrix_handle));
}

static void p_dim(cell *cfa) {
	(void)cfa;

	POP(matrix_val);
	if (matrix_val.tag != T_MATRIX) {
		type_error("dim needs matrix");
		return;
	}

	Object *matrix = objects[matrix_val.data];
	push(make_float(matrix->matrix.rows));
	push(make_float(matrix->matrix.columns));
}

static void p_array_of(cell *cfa) {
	(void)cfa;

	POP(size_val);
	if (size_val.tag != T_FLOAT) {
		type_error("array length");
		return;
	}

	POP(init_val);

	int array_len = (int)(unpack_float(size_val));
	int array_handle = object_new_array(array_len);
	if (error_flag) return;

	Object *array = objects[array_handle];
	for (int i = 0; i < array_len; i++) {
		array->items[i] = init_val;
	}

	push(make_array(array_handle));
}

static void p_transpose(cell *cfa) {
	(void)cfa;

	POP(matrix_val);
	if (matrix_val.tag != T_MATRIX) {
		type_error("transpose matrix");
		return;
	}

	Object *source = objects[matrix_val.data];
	int target_handle = object_new_matrix(source->matrix.columns, source->matrix.rows);
	if (error_flag) return;
	Object *target = objects[target_handle];
	for (int i = 0; i < source->matrix.rows; i++)
		for (int j = 0; j < source->matrix.columns; j++)
			MAT(target, j, i) = MAT(source, i, j);

	push(make_matrix(target_handle));
}

static void unary_op(Val operand, double (*function)(double), const char *name) {
	if (operand.tag == T_FLOAT) {
		push(make_float(function(unpack_float(operand))));
	} else if (operand.tag == T_MATRIX) {
		Object *source = objects[operand.data];
		int target_handle = object_new_matrix(source->matrix.rows, source->matrix.columns);
		if (error_flag) return;

		Object *target = objects[target_handle];
		int num_elements = source->matrix.rows * source->matrix.columns;
		for (int i = 0; i < num_elements; i++)
			target->matrix.elements[i] = function(source->matrix.elements[i]);

		push(make_matrix(target_handle));
	} else {
		type_error(name);
	}
}

static void p_abs(cell *cfa)  { (void)cfa; POP(operand); unary_op(operand, fabs, "abs");  }
static void p_sqrt(cell *cfa) { (void)cfa; POP(operand); unary_op(operand, sqrt, "sqrt"); }
static void p_exp(cell *cfa)  { (void)cfa; POP(operand); unary_op(operand, exp,  "exp");  }
static void p_log(cell *cfa)  { (void)cfa; POP(operand); unary_op(operand, log,  "log");  }
static void p_sin(cell *cfa)  { (void)cfa; POP(operand); unary_op(operand, sin,  "sin");  }
static void p_cos(cell *cfa)  { (void)cfa; POP(operand); unary_op(operand, cos,  "cos");  }
static void p_tan(cell *cfa)  { (void)cfa; POP(operand); unary_op(operand, tan,  "tan");  }
static void p_tanh(cell *cfa) { (void)cfa; POP(operand); unary_op(operand, tanh, "tanh"); }


/* ---- main: bootstrap the dictionary, run the REPL -------------------- */
/* We install every primitive into the dictionary. Ordering matters only
 * for the cached CFAs (exit, lit, branch, 0branch, dostr, stop) — they
 * must be defined before any immediate word that emits a reference to
 * them, since those references are resolved at definition time. */

int main(void) {
	define_primitive("+",      p_add,   0);
	define_primitive("-",      p_sub,   0);
	define_primitive("*",      p_mul,   0);
	define_primitive("/",      p_div,   0);
	define_primitive("negate", p_neg,   0);
	define_primitive("dup",    p_dup,   0);
	define_primitive("drop",   p_drop,  0);
	define_primitive("swap",   p_swap,  0);
	define_primitive("over",   p_over,  0);
	define_primitive("rot",    p_rot,   0);
	define_primitive("depth",  p_depth, 0);
	define_primitive("roll",   p_roll,  0);
	define_primitive("=",      p_eq,    0);
	define_primitive("<",      p_lt,    0);
	define_primitive(">",      p_gt,    0);
	define_primitive("0=",     p_zeq,   0);
	define_primitive(".",      p_dot,   0);
	define_primitive(".a",     p_dot_all, 0);
	define_primitive("cr",     p_cr,    0);
	define_primitive("emit",   p_emit_, 0);
	define_primitive(".s",     p_dots,  0);
	define_primitive("bye",    p_bye,   0);
	define_primitive("clear",  p_clear, 0);
	define_primitive("gc",	   p_gc,    0);
	define_primitive("load",       p_load,       0);
	define_primitive("save",       p_save,       0);
	define_primitive("save-image", p_save_image, 0);
	define_primitive("load-image", p_load_image, 0);
	define_primitive("reload",     p_reload,     0);
	define_primitive(">r",         p_tor,        0);
	define_primitive("r>",         p_rfrom,      0);
	define_primitive("r@",         p_rfetch,     0);
	define_primitive(">side",      p_to_side,    0);
	define_primitive("side>",      p_side_to,    0);
	define_primitive("side-drop",  p_side_drop,  0);
	define_primitive("side-depth", p_side_depth, 0);
	define_primitive("@",          p_fetch,      0);
	define_primitive("!",          p_store,      0);

	define_primitive("reset", 		p_reset, 0);
	define_primitive("shift", 		p_shift, 0);
	define_primitive("shift-with", 	p_shift_with, 0);
	define_primitive("resume", 	p_resume, 0);

	define_primitive("{",      p_setopen,  0);
	define_primitive("}",      p_setclose, 0);
	define_primitive("[",      p_array_open,  0);
	define_primitive("]",      p_array_close, 0);

	define_primitive("array",		 p_array, 0);
	define_primitive("array-of",	 p_array_of, 0);
	define_primitive("cardinality",  p_cardinality, 0);
	define_primitive("member?",      p_member,      0);
	define_primitive("set",          p_set,         0);
	define_primitive("union",        p_union,       0);
	define_primitive("intersection", p_intersect,   0);
	define_primitive("difference",   p_difference,  0);
	define_primitive("execute",      p_execute,     0);
	define_primitive("map",          p_map,         0);
	define_primitive("mapn",         p_mapn,        0);
	define_primitive("words",        p_words,       0);
	define_primitive("see",          p_see,         0);

	exit_cfa    = define_primitive("exit",      p_exit,    0);
	literal_cfa = define_primitive("(lit)",     p_literal,     0);
	branch_cfa  = define_primitive("(branch)",  p_branch,  0);
	zbranch_cfa = define_primitive("(0branch)", p_0branch, 0);
	dostr_cfa   = define_primitive("(dostr)",   p_dostr,   0);
	stop_cfa    = define_primitive("(stop)",    p_stop,    0);

	define_primitive(":",        p_colon,    0);
	define_primitive("variable", p_variable, 0);
	define_primitive("symbol",   p_symbol,   0);
	define_primitive("forget",   p_forget,   0);
	define_primitive("'",        p_tick,     1);

	define_primitive(";",     p_semi,  1);
	define_primitive("if",    p_if,    1);
	define_primitive("then",  p_then,  1);
	define_primitive("else",  p_else,  1);
	define_primitive("begin", p_begin, 1);
	define_primitive("until", p_until, 1);
	define_primitive("again", p_again, 1);
	define_primitive("[:",    p_qcolon, 1);
	define_primitive(":]",    p_qsemi,  1);

	define_primitive("0-matrix",		p_0_matrix, 0);
	define_primitive("matrix",			p_matrix, 0);
	define_primitive("dim",				p_dim, 0);
	define_primitive("transpose",		p_transpose, 0);
	define_primitive("diagonal-matrix",	p_diagonal_matrix, 0);
	define_primitive("@i",           	p_at_i,  0);
	define_primitive("@j",           	p_at_j,  0);
	define_primitive("@i,j",         	p_at_ij, 0);
	define_primitive("diagonal",		p_diagonal, 0);
	define_primitive("reshape",			p_reshape, 0);
	define_primitive("sum",				p_sum, 0);
	define_primitive("row-sums",		p_row_sums, 0);
	define_primitive("column-sums",		p_column_sums, 0);
	define_primitive("max",				p_max, 0);
	define_primitive("min",				p_min, 0);
	define_primitive("row-maxes",		p_row_maxes, 0);
	define_primitive("row-mins",		p_row_mins, 0);
	define_primitive("column-maxes",	p_column_maxes, 0);
	define_primitive("column-mins",		p_column_mins, 0);

	define_primitive("dgemm-nn",     	p_dgemm_nn, 0);
	define_primitive("dgemm-tn",     	p_dgemm_tn, 0);
	define_primitive("dgemm-nt",     	p_dgemm_nt, 0);
	define_primitive("dgemm-tt",     	p_dgemm_tt, 0);

	define_primitive("abs",		p_abs, 0);
	define_primitive("sqrt",	p_sqrt, 0);
	define_primitive("exp",		p_exp, 0);
	define_primitive("log",		p_log, 0);
	define_primitive("sin",		p_sin, 0);
	define_primitive("cos",		p_cos, 0);
	define_primitive("tan",		p_tan, 0);
	define_primitive("tanh",	p_tanh, 0);

	/* Freeze the post-primitive-registration markers. Everything past
	 * these is "user state" from the image's point of view. lib.l4 will
	 * be loaded next; its definitions are part of the image, by design. */
	initial_here             = here;
	initial_latest_cfa       = latest_cfa;
	initial_names_here       = names_here;
	initial_sources_here     = sources_here;
	initial_symbol_pool_here = symbol_pool_here;

	/* load the standard library */
	push(make_string(object_new_string("src/forth/lib.l4", 16)));
	p_load(NULL);
	if (error_flag) {
		printf("lib.l4 load error\n");
		return 1;
	}

	/* Boundary between bootstrap and "what the user wrote at the REPL"
	 * — used by the text SAVE to scope its vocabulary dump. */
	lib_end_latest_cfa = latest_cfa;

	printf("logicforth\n");
	char line[1024];
	/* REPL outer loop. Three states the loop can leave a line in:
	 *
	 *   1. NEED_MORE — a string literal opened on this line and didn't
	 *      close. Skip the cleanup and keep inbuf intact; the next fgets
	 *      appends to it so read_string_literal can finish.
	 *
	 *   2. COMPILING — we're inside a : ... ; that crossed a line
	 *      boundary. Again skip cleanup so inbuf keeps accumulating;
	 *      ; will need the body source to still be there when it
	 *      captures it into source_pool.
	 *
	 *   3. Otherwise — finished a top-level chunk: print "ok" (if no
	 *      error) and reset inbuf for the next fgets.
	 *
	 * On error we reset everything that holds compile-time state,
	 * including compiling_src_start so the next : starts clean. */
	while (fgets(line, sizeof(line), stdin)) {
		int line_len = (int)strlen(line);
		if (inbuf_len + line_len < INBUFSZ - 1) {
			memcpy(inbuf + inbuf_len, line, (size_t)line_len + 1);
			inbuf_len += line_len;
		}
		
		error_flag = 0;
		need_more = 0;
		run_outer();
		
		if (need_more) continue;
		
		if (error_flag) {
			compiling = 0;
			dsp = 0;
			rsp = 0;
			compiling_src_start = 0;
		}
		
		if (compiling) continue;

		print_prompt_state();
		if (error_flag) fputs(error_message, stdout);
		else            fputs("ok", stdout);
		putchar('\n');
		fflush(stdout);

		inbuf_reset();
	}
	return 0;
}
