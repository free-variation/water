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
 * user creates them with the defining syntax `: name ... ;`. For example:
 *
 *     : square dup * ;
 *     5 square .       → 25
 *
 * `square` is now in the dictionary, and its body is a tiny program that
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
 * The outer interpreter has a second mode: "compile mode", entered by `:`
 * and exited by `;`. In compile mode, instead of executing what the tokens
 * name, it compiles references to them into the body of the new word.
 * Numbers get compiled as literals; word names get compiled as references.
 * This is how `: square dup * ;` actually builds the body of `square`:
 * during compilation, the outer interpreter writes references to `dup` and
 * `*` and an EXIT marker into the dictionary.
 *
 *
 *                                 PART TWO
 *                          THREADED CODE AND ITC
 *
 * Once we accept that a colon definition's body is a list of references to
 * other words, the question is: what kind of reference, and how does
 * execution step through them? This is "threaded code" — the body is a
 * "thread" that the inner interpreter follows. There are four classical
 * answers, differing in what the body contains and what dispatch costs:
 *
 *     Direct-threaded (DTC).   Each body cell is a machine code address.
 *                              The inner interpreter jumps to it. Fastest;
 *                              not portable in C.
 *
 *     Subroutine-threaded.     Each body cell is a real CALL instruction.
 *                              No software inner interpreter needed; the
 *                              CPU's call/return mechanism does everything.
 *
 *     Token-threaded.          Each body cell is a small integer indexing a
 *                              dispatch table. Compact, slowest.
 *
 *     Indirect-threaded (ITC). Each body cell is a pointer to a "code
 *                              field" cell. Each word has a code field
 *                              whose value is the address of a handler.
 *                              One extra dereference per step.
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
 *       shared handler called `docol`. `docol` saves the current
 *       instruction pointer on the return stack, then redirects it to the
 *       body. The inner interpreter then walks the body. When the body
 *       ends, an EXIT primitive restores the saved instruction pointer.
 *
 *     · A variable's code field holds the address of `dovar`, which pushes
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
 * array of cells, `mem[]`. There is no separate heap and no use of malloc
 * for dictionary storage. A pointer `here` marks the next free cell;
 * defining a new word advances `here`.
 *
 * Each word's header sits immediately before its code field. The layout,
 * with positions given relative to the code field address ("CFA"):
 *
 *     mem[cfa - 3]   link       index of the previous word's CFA, or 0
 *     mem[cfa - 2]   flags      bit 0 = immediate
 *     mem[cfa - 1]   name idx   offset into the byte pool `namepool[]`
 *     mem[cfa    ]   code field function pointer cast to a cell
 *     mem[cfa + 1]   body...    optional, depending on the word's kind
 *
 * "CFA" is short for "code field address" and is the conventional way to
 * refer to a word. To get to the header you subtract; to get to the body
 * you add one. References stored in the body of a colon definition are
 * pointers — `cell`s holding `&mem[other_cfa]`. The inner interpreter
 * reads one of these, dereferences it to get the handler, and calls it.
 *
 * The dictionary as a whole is a singly-linked list threaded backward
 * through the link fields. The variable `latest_cfa` points to the most
 * recent definition. Searching by name (`find`) walks backward to the
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
 *       8 bytes of payload. Operators like `+` and `=` dispatch on the
 *       tags of their operands. This replaces classical Forth's untyped
 *       stack with something closer to Lisp's discriminated runtime values.
 *
 *     · One numeric type: double-precision float. There is no integer.
 *       `3` and `3.0` are the same value. This loses exactness for very
 *       large integers and for set membership of computed floats, but
 *       removes the int/float duality and all its promotion rules.
 *
 *     · First-class symbols. The defining word `symbol foo` creates a
 *       new word `foo` whose runtime behavior is to push a symbol value
 *       that identifies itself. Symbols compare by identity and print as
 *       their names. They're inert atoms — they evaluate to themselves.
 *
 *     · First-class strings. The literal `"hello"` pushes a string value.
 *       Strings may span lines. They support `{n}`-style interpolation
 *       against the stack at the moment the literal is encountered.
 *
 *     · First-class mutable sets. The literal `{ a b c }` builds a set.
 *       Sets are sorted internally so membership tests are binary
 *       searches. Duplicate adds are silently ignored. Operations: union,
 *       intersection, difference, member?, cardinality.
 *
 *     · First-class fixed-size arrays via `[ 1 2 3 ]`.
 *
 *     · Polymorphic operators. `+` adds numbers, concatenates strings,
 *       and unions sets. `=` compares within compatible types.
 *
 *     · `'` (tick) parses the next token at compile or interpret time and
 *       pushes its execution token, enabling higher-order operations like
 *       `map`.
 *
 *     · `words` lists the dictionary; `forget` discards a word and
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

/* A raw dictionary cell. 64-bit because we want to store function
 * pointers, doubles, and indices without size-related hand-wringing. */
typedef int64_t cell;


/* ---- tagged values: the stack representation -------------------------- */
/* Every item on the data and return stacks is a tagged Val: a small tag
 * plus an 8-byte payload. Operators inspect the tag to decide what to do.
 * Doubles live in the payload via memcpy — this avoids any aliasing
 * trouble and works the same on every machine. Handles (for objects in
 * the object registry) are small integers stored directly. */

typedef enum {
    T_NONE = 0,
    T_FLOAT,      /* the only numeric type                                 */
    T_SYM,        /* symbol; payload = CFA of the defining word            */
    T_STR,        /* string; payload = index into objs[]                   */
    T_SET,        /* set; payload = index into objs[]                      */
    T_ARR,        /* array; payload = index into objs[]                    */
    T_XT,         /* execution token; payload = CFA                        */
    T_ADDR,       /* dictionary address; payload = cell index into mem[]   */
    T_MARK        /* internal marker used by `{` `}` and `[` `]` collection */
} Tag;

typedef struct { Tag tag; int64_t v; } Val;

static double val_f(Val x)       { double d; memcpy(&d, &x.v, 8); return d; }
static Val mk_float(double d)    { Val r; r.tag = T_FLOAT; memcpy(&r.v, &d, 8); return r; }
static Val mk_sym(int cfa)       { Val r; r.tag = T_SYM;   r.v = cfa; return r; }
static Val mk_str(int h)         { Val r; r.tag = T_STR;   r.v = h;   return r; }
static Val mk_set(int h)         { Val r; r.tag = T_SET;   r.v = h;   return r; }
static Val mk_arr(int h)         { Val r; r.tag = T_ARR;   r.v = h;   return r; }
static Val mk_xt(int cfa)        { Val r; r.tag = T_XT;    r.v = cfa; return r; }
static Val mk_addr(int idx)      { Val r; r.tag = T_ADDR;  r.v = idx; return r; }
static Val mk_mark(void)         { Val r; r.tag = T_MARK;  r.v = 0;   return r; }

#define MEMSZ    32768
#define NAMEPOOL 8192
#define DSTACK   256
#define RSTACK   256
#define MAXOBJS  1024
#define INBUFSZ  16384


/* ---- universe of state ----------------------------------------------- */

/* mem[]: the dictionary. A flat cell array growing upward. It holds word
 * headers, code-field function pointers, and the bodies of colon
 * definitions. `here` is the next free cell index. */
static cell mem[MEMSZ];
static int  here = 0;

/* Word names live separately in a packed byte pool. Headers store an
 * offset into this pool rather than the bytes inline; this keeps the
 * dictionary cell-aligned. */
static char namepool[NAMEPOOL];
static int  names_here = 0;

/* The data stack. All arithmetic, comparison, I/O, and the immediate-
 * word back-patching mechanism push and pop tagged Vals from here. */
static Val ds[DSTACK]; static int dsp = 0;

/* The return stack. The inner interpreter uses it to remember where to
 * resume after a colon definition finishes. The user can also push to it
 * via >r and pop back via r>. These two roles share one stack by Forth
 * tradition; the user must keep >r and r> balanced within a word. */
static Val rs[RSTACK]; static int rsp = 0;

/* The instruction pointer used by the inner interpreter. It always points
 * at the NEXT body cell to be processed. Modified by `docol`, `p_exit`,
 * the branch primitives, and the trampoline mechanism described below. */
static cell *ip = NULL;

/* Heads of the dictionary's linked list and the state of the outer
 * interpreter (compile mode vs. execute mode). */
static int  latest_cfa = 0;
static int  compiling = 0;

/* Sticky error flag; reset at the top of each input chunk. */
static int  err = 0;

/* Cleared by the EXIT primitive when it sees an empty return stack; this
 * breaks the inner-interpreter loop and returns control to the outer. */
static int  running = 0;

#define LINK(c)    mem[(c) - 3]
#define FLAGS(c)   mem[(c) - 2]
#define NAMEIDX(c) mem[(c) - 1]
#define IS_IMM(c)  (FLAGS(c) & 1)


/* ---- object registry -------------------------------------------------- */
/* Strings, sets, and arrays don't fit in a single 8-byte cell, so they
 * live in heap-allocated Obj structs. The stack carries small integer
 * handles (indices into objs[]). This separation lets the dictionary
 * remain a uniform cell array while permitting variable-size first-class
 * compound values.
 *
 * Strings are immutable byte arrays. Arrays are immutable in size but
 * mutable in contents. Sets are fully mutable: `add` modifies them in
 * place, so two stack copies of the same set handle refer to the same
 * underlying storage. */

typedef enum { OBJ_STR, OBJ_SET, OBJ_ARR } ObjKind;
typedef struct {
    ObjKind kind;
    int len, cap;
    char *bytes;     /* OBJ_STR */
    Val  *items;     /* OBJ_SET, OBJ_ARR */
} Obj;

static Obj *objs[MAXOBJS];
static int  nobjs = 0;

/* GC roots beyond the two stacks and the dictionary: a small fixed stack
 * that C-level primitives use to keep heap objects alive across calls
 * that might trigger collection. `map` is the canonical example — it
 * allocates an intermediate result set whose handle is only held in a C
 * local; without rooting, a GC triggered by the user's xt would free the
 * partially-built result and leave us writing into a dangling Obj. */
#define GC_ROOTS_MAX 16
static Val gc_roots[GC_ROOTS_MAX];
static int n_gc_roots = 0;

static void gc_root_push(Val v) {
    if (n_gc_roots < GC_ROOTS_MAX) gc_roots[n_gc_roots++] = v;
}
static void gc_root_pop(void) { if (n_gc_roots > 0) n_gc_roots--; }

/* The collector itself is defined later in the file — it needs to know
 * about the inline-data primitives (lit, branch, dostr) to walk colon
 * bodies, and their CFAs are declared after this section. */
static void gc(void);

/* Pick a slot in objs[]. Fast path: extend nobjs upward. If the array is
 * full, look for a NULL hole left by a previous sweep. Only when no hole
 * is available do we actually run the collector, then look again. A
 * return of -1 means every slot is genuinely reachable. */
static int obj_alloc_slot(void) {
    if (nobjs < MAXOBJS) return nobjs++;
    for (int i = 0; i < MAXOBJS; i++) if (objs[i] == NULL) return i;
    gc();
    for (int i = 0; i < MAXOBJS; i++) if (objs[i] == NULL) return i;
    return -1;
}

static int obj_new_str(const char *s, int n) {
    int idx = obj_alloc_slot();
    if (idx < 0) { err = 1; return 0; }
    Obj *o = calloc(1, sizeof(*o));
    o->kind = OBJ_STR; o->len = n; o->cap = n;
    o->bytes = malloc((size_t)n + 1);
    memcpy(o->bytes, s, (size_t)n); o->bytes[n] = 0;
    objs[idx] = o;
    return idx;
}
static int obj_new_set(void) {
    int idx = obj_alloc_slot();
    if (idx < 0) { err = 1; return 0; }
    Obj *o = calloc(1, sizeof(*o));
    o->kind = OBJ_SET; o->cap = 4;
    o->items = malloc(sizeof(Val) * (size_t)o->cap);
    objs[idx] = o;
    return idx;
}
static int obj_new_arr(int n) {
    int idx = obj_alloc_slot();
    if (idx < 0) { err = 1; return 0; }
    Obj *o = calloc(1, sizeof(*o));
    o->kind = OBJ_ARR; o->len = n; o->cap = n;
    o->items = malloc(sizeof(Val) * (size_t)(n > 0 ? n : 1));
    objs[idx] = o;
    return idx;
}


/* ---- stack helpers ---------------------------------------------------- */
/* Bounds-checked push and pop. Errors are sticky: a single bad operation
 * sets `err`, the outer interpreter notices, and the current input is
 * abandoned. */

static void push(Val x)  { if (dsp < DSTACK) ds[dsp++] = x; else err = 1; }
static Val  pop(void)    { if (dsp > 0) return ds[--dsp]; err = 1; Val z = {T_NONE,0}; return z; }
static void rpush(Val x) { if (rsp < RSTACK) rs[rsp++] = x; else err = 1; }
static Val  rpop(void)   { if (rsp > 0) return rs[--rsp]; err = 1; Val z = {T_NONE,0}; return z; }


/* ---- value comparison ------------------------------------------------- */
/* We need a total order over Vals so sets can stay sorted (which makes
 * insertion, membership, and ordered iteration all simple). Tags order
 * each other by their enum value; within a tag, the natural ordering
 * applies. This lets sets contain heterogeneous values without breaking
 * the binary-search invariants in set_add and set_member. */

static int val_cmp(Val a, Val b) {
    if (a.tag != b.tag) return (int)a.tag - (int)b.tag;
    switch (a.tag) {
        case T_FLOAT: {
            double da = val_f(a), db = val_f(b);
            return da < db ? -1 : (da > db ? 1 : 0);
        }
        case T_SYM: case T_XT: case T_ADDR:
            return a.v < b.v ? -1 : (a.v > b.v ? 1 : 0);
        case T_STR: {
            Obj *oa = objs[a.v], *ob = objs[b.v];
            int n = oa->len < ob->len ? oa->len : ob->len;
            int c = memcmp(oa->bytes, ob->bytes, (size_t)n);
            if (c) return c;
            return oa->len - ob->len;
        }
        case T_SET: case T_ARR: {
            Obj *oa = objs[a.v], *ob = objs[b.v];
            int n = oa->len < ob->len ? oa->len : ob->len;
            for (int i = 0; i < n; i++) {
                int c = val_cmp(oa->items[i], ob->items[i]);
                if (c) return c;
            }
            return oa->len - ob->len;
        }
        default: return 0;
    }
}


/* ---- set operations --------------------------------------------------- */
/* Sets are kept sorted by val_cmp. Insertion does binary search to find
 * the slot; if the element is already present, the call is a no-op
 * (silent dedup). Union, intersection, and difference all build new sets
 * by walking the sorted backing arrays. */

static void set_add(int h, Val v) {
    Obj *s = objs[h];
    int lo = 0, hi = s->len;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int c = val_cmp(s->items[mid], v);
        if (c == 0) return;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    if (s->len >= s->cap) {
        s->cap *= 2;
        s->items = realloc(s->items, sizeof(Val) * (size_t)s->cap);
    }
    memmove(&s->items[lo + 1], &s->items[lo], sizeof(Val) * (size_t)(s->len - lo));
    s->items[lo] = v;
    s->len++;
}

static int set_member(int h, Val v) {
    Obj *s = objs[h];
    int lo = 0, hi = s->len;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        int c = val_cmp(s->items[mid], v);
        if (c == 0) return 1;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}

static int set_union(int a, int b) {
    int h = obj_new_set();
    Obj *oa = objs[a], *ob = objs[b];
    for (int i = 0; i < oa->len; i++) set_add(h, oa->items[i]);
    for (int i = 0; i < ob->len; i++) set_add(h, ob->items[i]);
    return h;
}
static int set_intersect(int a, int b) {
    int h = obj_new_set();
    Obj *oa = objs[a];
    for (int i = 0; i < oa->len; i++)
        if (set_member(b, oa->items[i])) set_add(h, oa->items[i]);
    return h;
}
static int set_diff(int a, int b) {
    int h = obj_new_set();
    Obj *oa = objs[a];
    for (int i = 0; i < oa->len; i++)
        if (!set_member(b, oa->items[i])) set_add(h, oa->items[i]);
    return h;
}


/* ---- printing --------------------------------------------------------- */
/* The `.` primitive uses this. Type-aware: strings unquoted, symbols as
 * names, sets and arrays in their literal form, integers-as-floats as
 * integers when they happen to be whole. */

static void print_val(Val v);
static void print_double(double d) {
    if (d == (double)(int64_t)d && d > -1e15 && d < 1e15)
        printf("%lld", (long long)d);
    else
        printf("%g", d);
}
static void print_val(Val v) {
    switch (v.tag) {
        case T_FLOAT: print_double(val_f(v)); break;
        case T_SYM:   fputs(&namepool[NAMEIDX(v.v)], stdout); break;
        case T_STR:   fputs(objs[v.v]->bytes, stdout); break;
        case T_SET: {
            Obj *s = objs[v.v];
            fputs("{ ", stdout);
            for (int i = 0; i < s->len; i++) { print_val(s->items[i]); putchar(' '); }
            putchar('}');
            break;
        }
        case T_ARR: {
            Obj *s = objs[v.v];
            fputs("[ ", stdout);
            for (int i = 0; i < s->len; i++) { print_val(s->items[i]); putchar(' '); }
            putchar(']');
            break;
        }
        case T_XT:   printf("<xt %lld>",   (long long)v.v); break;
        case T_ADDR: printf("<addr %lld>", (long long)v.v); break;
        default:     printf("<?>"); break;
    }
}


/* ---- dictionary search ------------------------------------------------ */
/* `find` walks the linked list of headers, newest-first, comparing names.
 * Because new definitions are prepended, a redefinition shadows the
 * previous version: `find` returns the newer CFA. Old compiled references
 * to the previous version still call it — they were resolved at compile
 * time to a specific CFA index, not to a name. */

static int find(const char *name) {
    int c = latest_cfa;
    while (c != 0) {
        if (strcmp(&namepool[NAMEIDX(c)], name) == 0) return c;
        c = (int)LINK(c);
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
    /* Save the current ip as a T_ADDR tagged Val on the return stack. We
     * store the offset into mem[] rather than the raw pointer so the
     * return stack contains only valid Vals. */
    Val r; r.tag = T_ADDR; r.v = (cell)(ip - mem);
    rpush(r);
    ip = cfa + 1;
}

static void dosym(cell *cfa) { push(mk_sym((int)(cfa - mem))); }
static void dovar(cell *cfa) { push(mk_addr((int)((cfa + 1) - mem))); }


/* Cached CFAs of internal primitives we'll need to reference during
 * compilation, plus (stop) which is used as the trampoline terminator in
 * execute_cfa. Resolved in main() at startup. */
static int exit_cfa = 0, lit_cfa = 0, branch_cfa = 0, zbranch_cfa = 0, dostr_cfa = 0, stop_cfa = 0;


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
 * modify ip itself (docol, p_exit, branches), or clear `running` to stop
 * the loop. This is "NEXT" in classical Forth terminology. */

typedef void (*cfa_fn)(cell *cfa);

static void run_inner(void) {
    while (running && !err) {
        cell *cfa = (cell *)*ip++;
        cfa_fn fn = (cfa_fn)*cfa;
        fn(cfa);
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
 * (stop) primitive there just clears `running` and returns; it does NOT
 * pop the return stack. That distinction is the whole point of having a
 * separate terminator: when execute_cfa is called reentrantly — for
 * example by `map` from inside a colon definition that's itself inside
 * another colon definition — the enclosing words' return markers are
 * already on rs, and we must not consume them. An EXIT-as-terminator
 * would walk right up the stack, prematurely returning from words that
 * have plenty left to do.
 *
 * Pure primitives are still dispatched directly, without going through
 * the trampoline. There's no need: a primitive runs and returns. The
 * trampoline only exists to give the inner-interpreter loop something to
 * stop on, and only colon definitions need that loop. */

static cell trampoline[2];

static void execute_cfa(int cfa) {
    cfa_fn fn = (cfa_fn)mem[cfa];
    if (fn != &docol) { fn(&mem[cfa]); return; }
    cell *saved_ip = ip;
    int saved_running = running;
    trampoline[0] = (cell)&mem[cfa];
    trampoline[1] = (cell)&mem[stop_cfa];
    ip = trampoline;
    running = 1;
    run_inner();
    running = saved_running;
    ip = saved_ip;
}


/* ---- defining new words: headers and code fields --------------------- */
/* alloc_name copies a name into the byte pool. create_header lays down
 * the three header cells (link, flags, name index) and returns the CFA —
 * the index of the cell that will become the code field. The caller
 * fills in that cell with whichever handler is appropriate.
 *
 * def_prim is the convenience wrapper that does both: create a header,
 * then write a function pointer into the code field. Returns the CFA so
 * we can cache it for internal primitives we reference during
 * compilation. */

static int alloc_name(const char *s) {
    int len = (int)strlen(s) + 1;
    if (names_here + len > NAMEPOOL) { err = 1; return 0; }
    int idx = names_here;
    memcpy(&namepool[names_here], s, (size_t)len);
    names_here += len;
    return idx;
}
static int create_header(const char *name, int immediate) {
    if (here + 3 >= MEMSZ) { err = 1; return 0; }
    int link = latest_cfa;
    int n = alloc_name(name);
    mem[here++] = link;
    mem[here++] = immediate ? 1 : 0;
    mem[here++] = n;
    int cfa = here;
    latest_cfa = cfa;
    return cfa;
}
static int def_prim(const char *name, cfa_fn fn, int immediate) {
    int cfa = create_header(name, immediate);
    if (here < MEMSZ) mem[here++] = (cell)fn; else err = 1;
    return cfa;
}

/* `emit` appends a single raw cell to the dictionary at `here`. Used
 * during compilation of colon definitions and for inline data (literals
 * and branch offsets). */
static void emit(cell x) { if (here < MEMSZ) mem[here++] = x; else err = 1; }

/* Compiling a tagged literal: we emit a (lit) reference, then two raw
 * cells encoding the Val (tag, then payload). At runtime (lit) reads
 * those two cells and reconstructs the Val.
 *
 * This is the one place where the body of a colon definition contains
 * something other than CFA references: the two raw cells immediately
 * after a (lit) reference are inline data. The inner interpreter never
 * sees them as separate dispatch steps — (lit)'s handler consumes them. */
static void emit_val_literal(Val v) {
    emit((cell)&mem[lit_cfa]);
    emit((cell)v.tag);
    emit(v.v);
}


/* ---- core primitives -------------------------------------------------- */
/* The primitives below implement the basic Forth-level operations. Each
 * one is a C function matching the cfa_fn signature. They ignore the
 * cfa argument unless they're a handler for a kind of word (docol,
 * dosym, dovar already covered).
 *
 * We start with EXIT and the inline-data primitives, which are the
 * machinery that makes colon definitions work, then move outward to the
 * user-visible operations. */

static void type_err(const char *op) { printf("? type error in %s\n", op); err = 1; }

/* EXIT: pop the saved instruction pointer from the return stack and jump
 * to it. When the return stack is empty, we're at the top level; clear
 * `running` and let the inner-interpreter loop terminate cleanly. */
static void p_exit(cell *c) {
    (void)c;
    if (rsp <= 0) { running = 0; return; }
    Val r = rs[--rsp];
    ip = (cell *)(mem + r.v);
}

/* (stop): used only as the second cell of execute_cfa's trampoline. The
 * call sequence is: trampoline[0] dispatches the target word, the target
 * runs to its EXIT, EXIT pops its own saved ip and lands on (stop), and
 * (stop) breaks the inner loop without touching the return stack. See
 * execute_cfa for why a different terminator from EXIT is necessary. */
static void p_stop(cell *c) { (void)c; running = 0; }

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
static void p_lit(cell *c) {
    (void)c;
    Val r; r.tag = (Tag)*ip++; r.v = *ip++;
    push(r);
}
static void p_branch(cell *c)  { (void)c; ip += *ip; }
static void p_0branch(cell *c) {
    (void)c;
    cell o = *ip++;
    Val v = pop();
    int zero = (v.tag == T_FLOAT) ? (val_f(v) == 0.0) : (v.v == 0);
    if (zero) ip += o - 1;
}

/* (dostr): string-literal handler. The cell immediately after this in
 * the body holds the handle of a template string. We perform
 * interpolation against the current data stack at run time, push the
 * resulting string, and advance past the handle. */
static int interpolate(int template_handle);   /* forward                  */

static void p_dostr(cell *c) {
    (void)c;
    int h = (int)*ip++;
    int r = interpolate(h);
    push(mk_str(r));
}


/* ---- polymorphic arithmetic and comparison ---------------------------- */
/* `+`, `-`, `*` are overloaded across the types where overloading makes
 * sense. `+` adds numbers, concatenates strings, unions sets. `-` does
 * numeric subtraction and set difference. `*` does multiplication and
 * set intersection. The remaining type combinations raise a type error.
 *
 * Equality is universal: val_cmp gives us a total ordering and equality
 * falls out for free. */

static int string_concat(int a, int b) {
    Obj *oa = objs[a], *ob = objs[b];
    int total = oa->len + ob->len;
    char *buf = malloc((size_t)total + 1);
    memcpy(buf, oa->bytes, (size_t)oa->len);
    memcpy(buf + oa->len, ob->bytes, (size_t)ob->len);
    int h = obj_new_str(buf, total);
    free(buf);
    return h;
}

static void p_add(cell *c) {
    (void)c; Val b = pop(), a = pop();
    if (a.tag == T_FLOAT && b.tag == T_FLOAT)  push(mk_float(val_f(a) + val_f(b)));
    else if (a.tag == T_STR && b.tag == T_STR) push(mk_str(string_concat((int)a.v, (int)b.v)));
    else if (a.tag == T_SET && b.tag == T_SET) push(mk_set(set_union((int)a.v, (int)b.v)));
    else type_err("+");
}
static void p_sub(cell *c) {
    (void)c; Val b = pop(), a = pop();
    if (a.tag == T_FLOAT && b.tag == T_FLOAT) push(mk_float(val_f(a) - val_f(b)));
    else if (a.tag == T_SET && b.tag == T_SET) push(mk_set(set_diff((int)a.v, (int)b.v)));
    else type_err("-");
}
static void p_mul(cell *c) {
    (void)c; Val b = pop(), a = pop();
    if (a.tag == T_FLOAT && b.tag == T_FLOAT) push(mk_float(val_f(a) * val_f(b)));
    else if (a.tag == T_SET && b.tag == T_SET) push(mk_set(set_intersect((int)a.v, (int)b.v)));
    else type_err("*");
}
static void p_div(cell *c) {
    (void)c; Val b = pop(), a = pop();
    if (a.tag == T_FLOAT && b.tag == T_FLOAT && val_f(b) != 0.0)
        push(mk_float(val_f(a) / val_f(b)));
    else type_err("/");
}
static void p_neg(cell *c) {
    (void)c; Val a = pop();
    if (a.tag == T_FLOAT) push(mk_float(-val_f(a)));
    else type_err("negate");
}

/* Booleans live in T_FLOAT: -1.0 is true, 0.0 is false. The choice of
 * -1 over 1 follows Forth tradition — bitwise AND on flag values then
 * coincides with logical AND. */
static Val mk_bool(int t) { return mk_float(t ? -1.0 : 0.0); }
static int truthy(Val v)  { return (v.tag == T_FLOAT) ? (val_f(v) != 0.0) : (v.v != 0); }

static void p_eq(cell *c)  { (void)c; Val b = pop(), a = pop(); push(mk_bool(val_cmp(a, b) == 0)); }
static void p_lt(cell *c)  { (void)c; Val b = pop(), a = pop(); push(mk_bool(val_cmp(a, b) <  0)); }
static void p_gt(cell *c)  { (void)c; Val b = pop(), a = pop(); push(mk_bool(val_cmp(a, b) >  0)); }
static void p_zeq(cell *c) { (void)c; Val a = pop(); push(mk_bool(!truthy(a))); }


/* ---- stack manipulation, I/O, return-stack access -------------------- */

static void p_dup(cell *c)  { (void)c; Val a = pop(); push(a); push(a); }
static void p_drop(cell *c) { (void)c; (void)pop(); }
static void p_swap(cell *c) { (void)c; Val b = pop(), a = pop(); push(b); push(a); }
static void p_over(cell *c) { (void)c; Val b = pop(), a = pop(); push(a); push(b); push(a); }
static void p_rot(cell *c)  { (void)c; Val z = pop(), y = pop(), x = pop(); push(y); push(z); push(x); }

static void p_dot(cell *c)  { (void)c; print_val(pop()); putchar(' '); fflush(stdout); }
static void p_cr(cell *c)   { (void)c; putchar('\n'); fflush(stdout); }
static void p_emit_(cell *c){
    (void)c; Val a = pop();
    if (a.tag == T_FLOAT) { putchar((int)val_f(a)); fflush(stdout); }
    else type_err("emit");
}
static void p_dots(cell *c) {
    (void)c;
    printf("<%d> ", dsp);
    for (int i = 0; i < dsp; i++) { print_val(ds[i]); putchar(' '); }
    fflush(stdout);
}
static void p_bye(cell *c) { (void)c; exit(0); }

static void p_tor(cell *c)    { (void)c; rpush(pop()); }
static void p_rfrom(cell *c)  { (void)c; push(rpop()); }
static void p_rfetch(cell *c) { (void)c; if (rsp > 0) push(rs[rsp - 1]); else err = 1; }


/* ---- variables: @ and ! ---------------------------------------------- */
/* `variable foo` reserves a body of two raw cells (tag, payload) — enough
 * to hold one Val. dovar pushes the address (cell index) of the tag
 * cell. The user accesses the stored value via @ and !, which read and
 * write the two-cell pair. */

static void p_fetch(cell *c) {
    (void)c; Val a = pop();
    if (a.tag != T_ADDR) { type_err("@"); return; }
    int idx = (int)a.v;
    Val r; r.tag = (Tag)mem[idx]; r.v = mem[idx + 1];
    push(r);
}
static void p_store(cell *c) {
    (void)c; Val addr = pop(), v = pop();
    if (addr.tag != T_ADDR) { type_err("!"); return; }
    int idx = (int)addr.v;
    mem[idx] = (cell)v.tag;
    mem[idx + 1] = v.v;
}


/* ---- set and array literals via stack markers ------------------------ */
/* `{` and `[` push a special T_MARK value. The user then types elements,
 * which push themselves. `}` and `]` scan down the stack to find the
 * marker, collect everything above it into a new set or array, drop the
 * marker, and push the resulting object handle.
 *
 * These are deliberately NOT immediate. In interpret mode the outer
 * interpreter executes them directly and the data stack acts as the
 * collection buffer. In compile mode they get compiled into the body
 * like any other primitive reference, so at run time `{` pushes the
 * mark, the compiled element literals push themselves, and `}` collects
 * — exactly the same mechanism, just deferred to run time. */

static void p_setopen(cell *c)  { (void)c; push(mk_mark()); }
static void p_setclose(cell *c) {
    (void)c;
    int base = dsp;
    while (base > 0 && ds[base - 1].tag != T_MARK) base--;
    if (base == 0) { type_err("}"); return; }
    int h = obj_new_set();
    for (int i = base; i < dsp; i++) set_add(h, ds[i]);
    dsp = base - 1;
    push(mk_set(h));
}
static void p_arropen(cell *c)  { (void)c; push(mk_mark()); }
static void p_arrclose(cell *c) {
    (void)c;
    int base = dsp;
    while (base > 0 && ds[base - 1].tag != T_MARK) base--;
    if (base == 0) { type_err("]"); return; }
    int n = dsp - base;
    int h = obj_new_arr(n);
    for (int i = 0; i < n; i++) objs[h]->items[i] = ds[base + i];
    dsp = base - 1;
    push(mk_arr(h));
}


/* ---- set, array, and higher-order operations ------------------------- */

static void p_cardinality(cell *c) {
    (void)c; Val a = pop();
    if (a.tag == T_SET || a.tag == T_ARR || a.tag == T_STR)
        push(mk_float((double)objs[a.v]->len));
    else type_err("cardinality");
}
static void p_member(cell *c) {
    (void)c; Val v = pop(), s = pop();
    if (s.tag != T_SET) { type_err("member?"); return; }
    push(mk_bool(set_member((int)s.v, v)));
}
static void p_at(cell *c) {
    (void)c; Val iv = pop(), av = pop();
    if (av.tag != T_ARR || iv.tag != T_FLOAT) { type_err("at"); return; }
    Obj *o = objs[av.v];
    int i = (int)val_f(iv);
    if (i < 0 || i >= o->len) { type_err("at: out of bounds"); return; }
    push(o->items[i]);
}
/* set: ( v1 v2 ... vN N -- set ) build a set from the top N stack items.
 * The count goes on top so the items can be pushed in their natural left-
 * to-right order: `1 2 3 4 3 set` consumes 2 3 4 and leaves 1 below. */
static void p_set(cell *c) {
    (void)c; Val nv = pop();
    if (nv.tag != T_FLOAT) { type_err("set"); return; }
    int n = (int)val_f(nv);
    if (n < 0 || n > dsp) { type_err("set"); return; }
    int h = obj_new_set();
    if (err) return;
    int base = dsp - n;
    for (int i = 0; i < n; i++) set_add(h, ds[base + i]);
    dsp = base;
    push(mk_set(h));
}

static void p_union(cell *c) {
    (void)c; Val b = pop(), a = pop();
    if (a.tag != T_SET || b.tag != T_SET) { type_err("union"); return; }
    push(mk_set(set_union((int)a.v, (int)b.v)));
}
static void p_intersect(cell *c) {
    (void)c; Val b = pop(), a = pop();
    if (a.tag != T_SET || b.tag != T_SET) { type_err("intersection"); return; }
    push(mk_set(set_intersect((int)a.v, (int)b.v)));
}
static void p_difference(cell *c) {
    (void)c; Val b = pop(), a = pop();
    if (a.tag != T_SET || b.tag != T_SET) { type_err("difference"); return; }
    push(mk_set(set_diff((int)a.v, (int)b.v)));
}

/* `execute` takes an execution token (xt) — the handle for a word — and
 * runs it. Together with `'` (tick), this gives us higher-order
 * operations: a word that takes a word as data. */
static void p_execute(cell *c) {
    (void)c; Val v = pop();
    if (v.tag != T_XT) { type_err("execute"); return; }
    execute_cfa((int)v.v);
}

/* `map`: apply an xt to each element of a set, collect the results into
 * an array. The result is an array — not a set — so the element count
 * and the source's sorted iteration order are both preserved. (A set
 * would silently dedup `f x = f y` cases, which is rarely what the user
 * wants when they say "apply f to each.") We snapshot the source's
 * elements first because the user's mapping word might do anything to
 * the registry.
 *
 * Two GC roots are needed during the loop: `sv` is the source set, which
 * is no longer on the data stack and whose items we read from across xt
 * calls; and the partially-built result array, whose handle is only held
 * in a C local until we push it at the end. We also zero-initialize the
 * array's items[] before the loop, because obj_new_arr leaves them
 * uninitialized — and a GC triggered mid-loop would walk those slots as
 * Vals during marking. Zero bytes deserialize as `{T_NONE, 0}`, which
 * mark_val ignores. */
static void p_map(cell *c) {
    (void)c; Val xt = pop(), sv = pop();
    if (xt.tag != T_XT || sv.tag != T_SET) { type_err("map"); return; }
    Obj *s = objs[sv.v];
    int srclen = s->len;
    Val *src = malloc(sizeof(Val) * (size_t)(srclen > 0 ? srclen : 1));
    memcpy(src, s->items, sizeof(Val) * (size_t)srclen);
    gc_root_push(sv);
    int h = obj_new_arr(srclen);
    if (err) { gc_root_pop(); free(src); return; }
    Obj *result = objs[h];
    memset(result->items, 0, sizeof(Val) * (size_t)(srclen > 0 ? srclen : 1));
    gc_root_push(mk_arr(h));
    for (int i = 0; i < srclen && !err; i++) {
        push(src[i]);
        execute_cfa((int)xt.v);
        if (err) break;
        result->items[i] = pop();
    }
    gc_root_pop();
    gc_root_pop();
    free(src);
    push(mk_arr(h));
}

/* `words`: list every name in the dictionary. Walks the linked list
 * newest-to-oldest. */
static void p_words(cell *c) {
    (void)c;
    int cnt = 0;
    for (int cf = latest_cfa; cf != 0; cf = (int)LINK(cf)) {
        fputs(&namepool[NAMEIDX(cf)], stdout);
        putchar(' ');
        if (++cnt % 8 == 0) putchar('\n');
    }
    if (cnt % 8) putchar('\n');
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

static void p_semi(cell *c)  { (void)c; emit((cell)&mem[exit_cfa]); compiling = 0; }
static void p_if(cell *c)    { (void)c; emit((cell)&mem[zbranch_cfa]); push(mk_float((double)here)); emit(0); }
static void p_then(cell *c)  {
    (void)c; int slot = (int)val_f(pop());
    mem[slot] = (here - slot);
}
static void p_else(cell *c)  {
    (void)c; int slot = (int)val_f(pop());
    emit((cell)&mem[branch_cfa]);
    push(mk_float((double)here));
    emit(0);
    mem[slot] = (here - slot);
}
static void p_begin(cell *c) { (void)c; push(mk_float((double)here)); }
static void p_until(cell *c) {
    (void)c; int back = (int)val_f(pop());
    emit((cell)&mem[zbranch_cfa]);
    emit(back - here);
}
static void p_again(cell *c) {
    (void)c; int back = (int)val_f(pop());
    emit((cell)&mem[branch_cfa]);
    emit(back - here);
}

/* [: and :] — anonymous colon definitions ("quotations"). [: starts
 * compiling a nameless word; :] closes it and the resulting xt is the
 * payload. Used for higher-order code: `{ 1 2 3 } [: 2 * :] map`.
 *
 * The anon has no dictionary header — it's never looked up by name and
 * skipping the link/flags/name fields means the dictionary chain stays
 * clean (FORGET works as before, and the GC's chain-based body walker
 * doesn't get confused by anon headers sitting in the middle of an
 * enclosing word's body). We just lay down a docol cell and a body.
 *
 * Nested case (inside a colon definition): we need to skip over the anon
 * at run time so the enclosing code reaches it only via `execute`. So
 * we emit a forward (branch) in the enclosing body, then lay down the
 * anon inline, then (after :]) patch the branch to land past the anon
 * and emit a (lit) that pushes the anon's xt. The compile-time data
 * stack carries the anon's CFA and the branch slot to be patched.
 *
 * Top-level case: there's no enclosing body to skip past, so we just
 * lay down the anon. The compile-time data stack still carries the
 * anon's CFA; the slot field is -1 as a flag meaning "no patch needed,
 * push the xt for the user instead of compiling a literal." */
static void p_qcolon(cell *c) {
    (void)c;
    int branch_slot = -1;
    if (compiling) {
        emit((cell)&mem[branch_cfa]);
        branch_slot = here;
        emit(0);
    }
    int anon_cfa = here;
    emit((cell)&docol);
    compiling = 1;
    push(mk_float((double)anon_cfa));
    push(mk_float((double)branch_slot));
}
static void p_qsemi(cell *c) {
    (void)c;
    emit((cell)&mem[exit_cfa]);
    int branch_slot = (int)val_f(pop());
    int anon_cfa    = (int)val_f(pop());
    if (branch_slot < 0) {
        compiling = 0;
        push(mk_xt(anon_cfa));
    } else {
        mem[branch_slot] = (here - branch_slot);
        emit_val_literal(mk_xt(anon_cfa));
    }
}


/* ---- defining words: : variable symbol ' forget---------------------- */
/* All defining words follow the same pattern: read the next source
 * token (the new word's name), create a header for it, and lay down the
 * appropriate code field handler. They differ only in which handler.
 *
 * `:` writes docol and switches to compile mode. The subsequent tokens,
 * up to `;`, get compiled into the body.
 *
 * `variable` writes dovar and reserves two cells for the storage.
 *
 * `symbol` writes dosym and reserves nothing — the symbol's identity is
 * its own CFA, no body needed. */

static char *next_token(void);

static void p_tick(cell *c) {
    (void)c;
    char *t = next_token();
    if (!t) { type_err("'"); return; }
    int cf = find(t);
    if (!cf) { printf("? %s\n", t); err = 1; return; }
    Val v = mk_xt(cf);
    if (compiling) emit_val_literal(v);
    else push(v);
}

static void p_colon(cell *c) {
    (void)c; char *t = next_token();
    if (!t) { printf("? : needs a name\n"); err = 1; return; }
    create_header(t, 0);
    emit((cell)&docol);
    compiling = 1;
}
static void p_variable(cell *c) {
    (void)c; char *t = next_token();
    if (!t) { printf("? variable needs a name\n"); err = 1; return; }
    create_header(t, 0);
    emit((cell)&dovar);
    emit((cell)T_FLOAT);
    double zero = 0.0; cell zb; memcpy(&zb, &zero, 8);
    emit(zb);
}
static void p_symbol(cell *c) {
    (void)c; char *t = next_token();
    if (!t) { printf("? symbol needs a name\n"); err = 1; return; }
    create_header(t, 0);
    emit((cell)&dosym);
}

/* forget: discard the named word and everything defined after it. We
 * roll back here (so the dictionary memory above is available again),
 * names_here (so the name pool reclaims any names of forgotten words),
 * and latest_cfa (so the linked list's head moves back). Objects in the
 * registry that were referenced only from forgotten code are no longer
 * reachable from any root, so the next GC will sweep them up. */
static void p_forget(cell *c) {
    (void)c; char *t = next_token();
    if (!t) { printf("? forget needs a name\n"); err = 1; return; }
    int target = find(t);
    if (!target) { printf("? %s\n", t); err = 1; return; }
    here       = target - 3;
    names_here = (int)NAMEIDX(target);
    latest_cfa = (int)LINK(target);
}


/* ---- input buffer and tokeniser -------------------------------------- */
/* The outer interpreter reads lines and accumulates them in `inbuf`. A
 * pointer `inp` walks through the buffer producing tokens. Most tokens
 * are whitespace-delimited and trivial. Two situations need special
 * handling:
 *
 *   · String literals starting with `"` are read up to the matching `"`,
 *     not whitespace. If end-of-buffer is reached before the closing
 *     quote, the parser sets `need_more` and returns; main() reads
 *     another line and appends.
 *
 *   · `(` and `\` introduce Forth-style comments — paren comments end at
 *     `)`, line comments end at newline. These are handled inline by the
 *     dispatcher rather than the tokenizer. */

static char inbuf[INBUFSZ];
static int  inbuf_len = 0;
static int  inp = 0;
static int  need_more = 0;

static void inbuf_reset(void) { inbuf_len = 0; inp = 0; inbuf[0] = 0; need_more = 0; }

static char tokbuf[INBUFSZ];

static int read_string_literal(void) {
    int start = inp + 1;
    int i = start;
    while (i < inbuf_len && inbuf[i] != '"') i++;
    if (i >= inbuf_len) { need_more = 1; return -1; }
    int n = i - start;
    memcpy(tokbuf, inbuf + start, (size_t)n);
    tokbuf[n] = 0;
    inp = i + 1;
    return n;
}

static char *next_token(void) {
    while (inp < inbuf_len && isspace((unsigned char)inbuf[inp])) inp++;
    if (inp >= inbuf_len) return NULL;
    int start = inp;
    while (inp < inbuf_len && !isspace((unsigned char)inbuf[inp])) inp++;
    int n = inp - start;
    if (n >= (int)sizeof(tokbuf)) n = sizeof(tokbuf) - 1;
    memcpy(tokbuf, inbuf + start, (size_t)n);
    tokbuf[n] = 0;
    return tokbuf;
}

static int parse_float(const char *s, double *out) {
    if (!*s) return 0;
    char *end;
    double v = strtod(s, &end);
    if (*end != 0) return 0;
    *out = v;
    return 1;
}


/* ---- string interpolation -------------------------------------------- */
/* When a string literal is encountered, we examine its content for `{n}`
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
    Obj *tmpl = objs[template_handle];
    int maxref = -1, seen_any = 0;
    for (int i = 0; i < tmpl->len; ) {
        if (tmpl->bytes[i] == '{') {
            int j = i + 1, n = 0, any = 0;
            while (j < tmpl->len && isdigit((unsigned char)tmpl->bytes[j])) {
                n = n * 10 + (tmpl->bytes[j] - '0'); j++; any = 1;
            }
            if (any && j < tmpl->len && tmpl->bytes[j] == '}') {
                if (n > maxref) maxref = n;
                seen_any = 1;
                i = j + 1;
                continue;
            }
        }
        i++;
    }
    char *out = malloc((size_t)tmpl->len * 4 + 64);
    int olen = 0;
    for (int i = 0; i < tmpl->len; ) {
        if (tmpl->bytes[i] == '{') {
            int j = i + 1, n = 0, any = 0;
            while (j < tmpl->len && isdigit((unsigned char)tmpl->bytes[j])) {
                n = n * 10 + (tmpl->bytes[j] - '0'); j++; any = 1;
            }
            if (any && j < tmpl->len && tmpl->bytes[j] == '}') {
                int idx = dsp - 1 - n;
                if (idx < 0) {
                    type_err("string interpolation: stack too shallow");
                    free(out);
                    return obj_new_str("", 0);
                }
                char render[256];
                int rlen = 0;
                Val v = ds[idx];
                switch (v.tag) {
                    case T_FLOAT: {
                        double d = val_f(v);
                        if (d == (double)(int64_t)d && d > -1e15 && d < 1e15)
                            rlen = snprintf(render, sizeof(render), "%lld", (long long)d);
                        else
                            rlen = snprintf(render, sizeof(render), "%g", d);
                        break;
                    }
                    case T_SYM:
                        rlen = snprintf(render, sizeof(render), "%s", &namepool[NAMEIDX(v.v)]);
                        break;
                    case T_STR: {
                        Obj *so = objs[v.v];
                        int copy = so->len < (int)sizeof(render) - 1 ? so->len : (int)sizeof(render) - 1;
                        memcpy(render, so->bytes, (size_t)copy);
                        render[copy] = 0;
                        rlen = copy;
                        break;
                    }
                    default:
                        rlen = snprintf(render, sizeof(render), "<?>");
                        break;
                }
                memcpy(out + olen, render, (size_t)rlen);
                olen += rlen;
                i = j + 1;
                continue;
            }
        }
        out[olen++] = tmpl->bytes[i++];
    }
    if (seen_any && maxref >= 0) {
        int to_drop = maxref + 1;
        if (to_drop > dsp) to_drop = dsp;
        dsp -= to_drop;
    }
    int h = obj_new_str(out, olen);
    free(out);
    return h;
}


/* ---- the outer interpreter ------------------------------------------- */
/* The dispatcher: for each token (or special character) decide what to
 * do.
 *
 *   · String literals (`"..."`) are handled specially: we read until the
 *     closing quote (spanning lines if necessary), then either push the
 *     resulting string at interpret time (after interpolation) or emit a
 *     (dostr) reference plus the template handle at compile time.
 *
 *   · Comments `( ... )` and `\ to end of line` are dropped.
 *
 *   · A known word: if we're compiling and it isn't immediate, emit a
 *     reference; otherwise execute it now.
 *
 *   · A number: if compiling, emit a tagged literal; otherwise push.
 *
 *   · Anything else: report and abort the current input. */

static void process(void) {
    while (!err) {
        while (inp < inbuf_len && isspace((unsigned char)inbuf[inp])) inp++;
        if (inp >= inbuf_len) return;
        char ch = inbuf[inp];
        if (ch == '"') {
            int n = read_string_literal();
            if (n < 0) return;
            int h = obj_new_str(tokbuf, n);
            if (compiling) {
                emit((cell)&mem[dostr_cfa]);
                emit((cell)h);
            } else {
                int r = interpolate(h);
                push(mk_str(r));
            }
            continue;
        }
        if (ch == '(' && (inp + 1 >= inbuf_len || isspace((unsigned char)inbuf[inp + 1]))) {
            while (inp < inbuf_len && inbuf[inp] != ')') inp++;
            if (inp < inbuf_len) inp++;
            continue;
        }
        if (ch == '\\' && (inp + 1 >= inbuf_len || isspace((unsigned char)inbuf[inp + 1]))) {
            while (inp < inbuf_len && inbuf[inp] != '\n') inp++;
            continue;
        }
        char *tok = next_token();
        if (!tok) return;
        int cf = find(tok);
        if (cf) {
            if (compiling && !IS_IMM(cf)) emit((cell)&mem[cf]);
            else execute_cfa(cf);
            continue;
        }
        double dv;
        if (parse_float(tok, &dv)) {
            Val v = mk_float(dv);
            if (compiling) emit_val_literal(v);
            else push(v);
            continue;
        }
        printf("? %s\n", tok);
        err = 1;
        return;
    }
}


/* ---- garbage collection: mark and sweep ------------------------------ */
/* Strings, sets, and arrays accumulate in objs[] as side effects of
 * normal execution — every `+` on strings, every set operation, every
 * `map` registers new Objs. Without reclamation the array fills and
 * allocation fails. We run a stop-the-world mark-and-sweep when
 * obj_alloc_slot can't find a free slot.
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
 * before walking. The latest word's body extends up to `here`. */

static int obj_mark[MAXOBJS];

static void mark_val(Val v) {
    if (v.tag != T_STR && v.tag != T_SET && v.tag != T_ARR) return;
    int h = (int)v.v;
    if (h < 0 || h >= MAXOBJS || !objs[h] || obj_mark[h]) return;
    obj_mark[h] = 1;
    Obj *o = objs[h];
    if (o->kind == OBJ_SET || o->kind == OBJ_ARR) {
        for (int i = 0; i < o->len; i++) mark_val(o->items[i]);
    }
}

static void mark_body(int start, int end) {
    int i = start;
    while (i < end) {
        cell ref = mem[i];
        if (ref == (cell)&mem[lit_cfa] && i + 2 < end) {
            Tag t = (Tag)mem[i + 1];
            Val v; v.tag = t; v.v = mem[i + 2];
            mark_val(v);
            i += 3;
        } else if (ref == (cell)&mem[dostr_cfa] && i + 1 < end) {
            Val v; v.tag = T_STR; v.v = mem[i + 1];
            mark_val(v);
            i += 2;
        } else if ((ref == (cell)&mem[branch_cfa]
                 || ref == (cell)&mem[zbranch_cfa]) && i + 1 < end) {
            i += 2;
        } else {
            i++;
        }
    }
}

static void gc(void) {
    memset(obj_mark, 0, sizeof(obj_mark));

    for (int i = 0; i < dsp; i++)         mark_val(ds[i]);
    for (int i = 0; i < rsp; i++)         mark_val(rs[i]);
    for (int i = 0; i < n_gc_roots; i++)  mark_val(gc_roots[i]);

    /* Collect CFAs into ascending order so we know each body's end. */
    static int cfas[MEMSZ / 4];
    int ncfas = 0;
    for (int c = latest_cfa; c != 0; c = (int)LINK(c)) {
        if (ncfas < (int)(sizeof cfas / sizeof cfas[0])) cfas[ncfas++] = c;
    }
    for (int i = 1; i < ncfas; i++) {
        int x = cfas[i], j = i - 1;
        while (j >= 0 && cfas[j] > x) { cfas[j + 1] = cfas[j]; j--; }
        cfas[j + 1] = x;
    }
    for (int i = 0; i < ncfas; i++) {
        int cfa = cfas[i];
        int body_start = cfa + 1;
        int body_end = (i + 1 < ncfas) ? cfas[i + 1] - 3 : here;
        cfa_fn fn = (cfa_fn)mem[cfa];
        if (fn == docol) {
            mark_body(body_start, body_end);
        } else if (fn == dovar && body_start + 1 < body_end) {
            Val v; v.tag = (Tag)mem[body_start]; v.v = mem[body_start + 1];
            mark_val(v);
        }
        /* primitives, dosym: no body to scan */
    }

    for (int h = 0; h < nobjs; h++) {
        if (objs[h] && !obj_mark[h]) {
            if (objs[h]->bytes) free(objs[h]->bytes);
            if (objs[h]->items) free(objs[h]->items);
            free(objs[h]);
            objs[h] = NULL;
        }
    }
}


/* ---- main: bootstrap the dictionary, run the REPL -------------------- */
/* We install every primitive into the dictionary. Ordering matters only
 * for the cached CFAs (exit, lit, branch, 0branch, dostr, stop) — they
 * must be defined before any immediate word that emits a reference to
 * them, since those references are resolved at definition time. */

int main(void) {
    def_prim("+",      p_add,   0);
    def_prim("-",      p_sub,   0);
    def_prim("*",      p_mul,   0);
    def_prim("/",      p_div,   0);
    def_prim("negate", p_neg,   0);
    def_prim("dup",    p_dup,   0);
    def_prim("drop",   p_drop,  0);
    def_prim("swap",   p_swap,  0);
    def_prim("over",   p_over,  0);
    def_prim("rot",    p_rot,   0);
    def_prim("=",      p_eq,    0);
    def_prim("<",      p_lt,    0);
    def_prim(">",      p_gt,    0);
    def_prim("0=",     p_zeq,   0);
    def_prim(".",      p_dot,   0);
    def_prim("cr",     p_cr,    0);
    def_prim("emit",   p_emit_, 0);
    def_prim(".s",     p_dots,  0);
    def_prim("bye",    p_bye,   0);
    def_prim(">r",     p_tor,    0);
    def_prim("r>",     p_rfrom,  0);
    def_prim("r@",     p_rfetch, 0);
    def_prim("@",      p_fetch, 0);
    def_prim("!",      p_store, 0);

    def_prim("{",      p_setopen,  0);
    def_prim("}",      p_setclose, 0);
    def_prim("[",      p_arropen,  0);
    def_prim("]",      p_arrclose, 0);

    def_prim("cardinality",  p_cardinality, 0);
    def_prim("member?",      p_member,      0);
    def_prim("set",          p_set,         0);
    def_prim("union",        p_union,       0);
    def_prim("intersection", p_intersect,   0);
    def_prim("difference",   p_difference,  0);
    def_prim("at",           p_at,          0);
    def_prim("execute",      p_execute,     0);
    def_prim("map",          p_map,         0);
    def_prim("words",        p_words,       0);

    exit_cfa    = def_prim("exit",      p_exit,    0);
    lit_cfa     = def_prim("(lit)",     p_lit,     0);
    branch_cfa  = def_prim("(branch)",  p_branch,  0);
    zbranch_cfa = def_prim("(0branch)", p_0branch, 0);
    dostr_cfa   = def_prim("(dostr)",   p_dostr,   0);
    stop_cfa    = def_prim("(stop)",    p_stop,    0);

    def_prim(":",        p_colon,    0);
    def_prim("variable", p_variable, 0);
    def_prim("symbol",   p_symbol,   0);
    def_prim("forget",   p_forget,   0);
    def_prim("'",        p_tick,     1);

    def_prim(";",     p_semi,  1);
    def_prim("if",    p_if,    1);
    def_prim("then",  p_then,  1);
    def_prim("else",  p_else,  1);
    def_prim("begin", p_begin, 1);
    def_prim("until", p_until, 1);
    def_prim("again", p_again, 1);
    def_prim("[:",    p_qcolon, 1);
    def_prim(":]",    p_qsemi,  1);

    printf("logicforth\n");
    char line[1024];
    while (fgets(line, sizeof(line), stdin)) {
        int line_len = (int)strlen(line);
        if (inbuf_len + line_len < INBUFSZ - 1) {
            memcpy(inbuf + inbuf_len, line, (size_t)line_len + 1);
            inbuf_len += line_len;
        }
        err = 0;
        need_more = 0;
        process();
        if (need_more) continue;
        if (err) { compiling = 0; dsp = 0; rsp = 0; }
        if (!err && !compiling) { printf("ok\n"); fflush(stdout); }
        inbuf_reset();
    }
    return 0;
}
