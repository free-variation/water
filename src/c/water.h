#ifndef WATER_H
#define WATER_H

#define VERSION "0.19.6"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include "platform.h"

typedef int64_t cell;

#define VOCABULARY_INIT_SIZE (1 << 20)
#define NAME_POOL (1 << 18)
#define SOURCE_POOL (1 << 22)
#define SYMBOL_POOL (1 << 22)
#define SYMBOL_HASH_SIZE (1 << 20)
#define MAX_QUOTATION_SPANS (1 << 14)
#define MAX_HANDLERS (1 << 10)
#define MAX_LOADED_FILES (1 << 6)
#define TRAMPOLINE_SLOT 0
#define DICT_RESERVED (3 * (MAX_WORKER_THREADS + 1))
#define DATA_STACK_DEPTH (1 << 16)
#define RETURN_STACK_DEPTH (1 << 16)
#define SIDESTACK_DEPTH (1 << 10)
#define INPUT_BUFFER_SIZE (1 << 20)
#define LOCAL_NAMES_POOL_SIZE (1 << 13)
#define MAX_LOCAL_NAMES (1 << 8)
#define MAX_LOCAL_SCOPES (1 << 6)
#define MAX_NESTING_DEPTH (1 << 8)
#define MAX_CALL_DEPTH (1 << 12)
#define MAX_OBJECTS (1 << 26)
#define OBJECTS_INIT_CAP (1 << 16)
#define ARENA_RESERVE ((size_t)1 << 34)
#define ARENA_BYTES_PER_HANDLE 256
#define ARENA_ALIGNMENT 16
#define ARENA_SIZE_CLASSES (2 << 5)
#define SLAB_BYTES (1 << 16)
#define SLOTS_PER_CLAIM (1 << 10)
#define HEAP_GC_FLOOR ((size_t)1 << 28)
#define MAX_GC_ROOTS (1 << 6)
#define COPY_SPINE_MAX (1 << 24)
#define PAIR_TABLE_DEPTH (1 << 20)
#define MAX_WORKER_THREADS (1 << 6)
#define BIND_TRAIL_DEPTH (1 << 16)
#define LVAR_STACK_DEPTH (1 << 16)
#define PROMPT_EXCEPTION 0
#define PROMPT_CHOICE 1
#define REGEX_CACHE_SIZE (1 << 10)
#define JSON_MAX_DEPTH (1 << 10)
#define SELECT_MAX_DEPTH JSON_MAX_DEPTH
#define MAX_DATABASES (1 << 8)
#define ERROR_TRACE_SIZE 1024
#define TRACE_SNIPPET_MAX 48
#define TRACE_FRAMES_FIRST 10
#define TRACE_FRAMES_LAST 3
#define TRACE_FRAMES_MAX 512
#define PRINT_FIRST 10
#define PRINT_LAST 3
#define LIST_PRINT_MAX 100000

typedef enum {
	T_NONE = 0,
	T_SYMBOL,
	T_FLOAT,
	T_STRING,
	T_SET,
	T_ARRAY,
	T_PAIR,
	T_FRAME,
	T_MATRIX,
	T_XT,
	T_ADDR,
	T_CONT,
	T_MARK,
	T_STREAM,
	T_LOGIC_VAR,
	T_UNBOUND,
	T_DB,
	T_PTR,
	T_SEGMENT,
	T_QUANTITY
} Tag;

typedef union {
	uint64_t bits;
	double number;
} Val;

#define NAN_BOX_PREFIX 0x7FF8000000000000ULL
#define NAN_BOX_MASK 0x7FF8000000000000ULL
#define VAL_TAG_SHIFT 44
#define VAL_TAG_MASK 0x0007F00000000000ULL
#define VAL_DATA_MASK 0x00000FFFFFFFFFFFULL
#define VAL_IS_FLOAT(v) (((v).bits & NAN_BOX_MASK) != NAN_BOX_PREFIX)
#define VAL_TAG(v) (VAL_IS_FLOAT(v) ? T_FLOAT : (Tag)(((v).bits >> VAL_TAG_SHIFT) & (VAL_TAG_MASK >> VAL_TAG_SHIFT)))
#define VAL_NUMBER(v) ((v).number)
#define VAL_DATA(v) ((int64_t)(VAL_IS_FLOAT(v) ? (v).bits : ((v).bits & VAL_DATA_MASK)))

static inline Val make_tagged(Tag tag, int64_t data) {
	Val value;
	if (tag == T_FLOAT) {
		value.bits = (uint64_t)data;
	} else {
		value.bits = NAN_BOX_PREFIX
			| ((uint64_t)tag << VAL_TAG_SHIFT)
			| ((uint64_t)data & VAL_DATA_MASK);
	}
	return value;
}

#define LOCAL_BASE_BITS 24

static inline Val make_locals_header(int local_base, int n_locals) {
	return make_tagged(T_ADDR, ((int64_t)n_locals << LOCAL_BASE_BITS | local_base));
}

static inline int saved_local_base(Val locals_header) {
	return (int)(VAL_DATA(locals_header) & ((1 << LOCAL_BASE_BITS) - 1));
}

static inline int saved_n_locals(Val locals_header) {
	return (int)(VAL_DATA(locals_header) >> LOCAL_BASE_BITS);
}

static inline Val make_float(double number) {
	Val value;
	value.number = number;
	if ((value.bits & NAN_BOX_MASK) == NAN_BOX_PREFIX) {
		value.bits = NAN_BOX_PREFIX;
	}
	return value;
}

static inline Val make_symbol(int cfa) { return make_tagged(T_SYMBOL, cfa); }
static inline Val make_string(int handle) { return make_tagged(T_STRING, handle); }
static inline Val make_set(int handle) { return make_tagged(T_SET, handle); }
static inline Val make_array(int handle) { return make_tagged(T_ARRAY, handle); }
static inline Val make_pair(int handle) { return make_tagged(T_PAIR, handle); }
static inline Val make_frame(int handle) { return make_tagged(T_FRAME, handle); }
static inline Val make_matrix(int handle) { return make_tagged(T_MATRIX, handle); }
static inline Val make_xt(int cfa) { return make_tagged(T_XT, cfa); }
static inline Val make_addr(int cell_index) { return make_tagged(T_ADDR, cell_index); }
static inline Val make_stream(int file_descriptor) {return make_tagged(T_STREAM, file_descriptor); }
static inline Val make_db(int handle) { return make_tagged(T_DB, handle); }
static inline Val make_pointer(int handle) { return make_tagged(T_PTR, handle); }
static inline Val make_segment(int handle) { return make_tagged(T_SEGMENT, handle); }
static inline Val make_quantity(int handle) { return make_tagged(T_QUANTITY, handle); }
static inline Val make_continuation(int handle) { return make_tagged(T_CONT, handle); }
static inline Val make_logic_var(int handle) { return make_tagged(T_LOGIC_VAR, handle); }
static inline Val make_mark(void) { return make_tagged(T_MARK, 0); }
static inline Val make_bool(int is_true) { return make_float(is_true ? 1.0 : 0.0); }

typedef enum {
	OBJECT_STRING = 0,
	OBJECT_SET,
	OBJECT_ARRAY,
	OBJECT_FRAME,
	OBJECT_MATRIX,
	OBJECT_CONTINUATION,
	OBJECT_SEGMENT
} ObjectKind;

typedef enum {
	SEGMENT_INT = 0,
	SEGMENT_DOUBLE
} SegmentType;

typedef struct Object {
	ObjectKind kind;
	int len, capacity;
	union {
		char *bytes;
		Val  *items;
		struct {
			cell *keys;
			Val *values;
		} frame;
		struct {
			int rows;
			int columns;
			double *elements;
		} matrix;
		struct {
			Val *return_slice;
			int return_len;
			int resume_ip;
			int local_base_offset;
			int capture_generation;
		} continuation;
		struct {
			int element_type;
			int length;
			void *data;
		} segment;
	};

	cell mark_epoch;
} Object;

static inline size_t segment_element_size(SegmentType element_type) {
	switch (element_type) {
		case SEGMENT_DOUBLE: return sizeof(double);
		case SEGMENT_INT:    return sizeof(int);
	}
	return 0;
}

static inline double segment_get(Object *segment, int index) {
	switch (segment->segment.element_type) {
		case SEGMENT_INT:    return ((int *)segment->segment.data)[index];
		case SEGMENT_DOUBLE: return ((double *)segment->segment.data)[index];
	}
	return 0;
}

static inline void segment_set(Object *segment, int index, double value) {
	switch (segment->segment.element_type) {
		case SEGMENT_INT:    ((int *)segment->segment.data)[index] = (int)value; break;
		case SEGMENT_DOUBLE: ((double *)segment->segment.data)[index] = value; break;
	}
}

typedef struct {
	Val head;
	Val tail;
} Pair;

typedef struct {
	_Atomic int n;
	int cap, max, init;
	int *free;
	int n_free;
} HandleSpace;

typedef struct {
	Pair *table;
	cell *mark_epoch;
	HandleSpace space;
} PairPool;

extern PairPool pairs;
typedef struct {
	char *base;
	_Atomic size_t used;
	size_t reserved;

	_Atomic size_t heap_bytes_live;
	size_t heap_gc_threshold;

	_Atomic cell current_epoch;
	Object **objects;
	HandleSpace object_space;
} Arena;

extern Arena arena;
extern int in_parallel;
extern int parallel_region_collected;
extern int parallel_region_object_base;
extern int parallel_region_pair_base;

typedef struct {
	int next, end;
	int *free;
	int n_free, free_cap;
	int *chunks;
	int n_chunks, chunks_cap;
} LocalHandles;

typedef struct {
	char *slab_next, *slab_end;
	void *size_class_free[ARENA_SIZE_CLASSES];
	void *freed_object_structs;
	size_t heap_bytes_live, heap_gc_threshold;
	LocalHandles objects, pairs;
} AllocContext;
extern AllocContext main_alloc;

typedef struct {
	size_t used;
	int n_objects;
	int n_pairs;
} ParallelRegion;

typedef struct {
	int slot;
	Val value;
} VarMapEntry;

typedef struct {
	int reify;
	VarMapEntry *entries;
	int count, cap;
} VarMap;

typedef enum {
	PRED_EXISTS = 0,
	PRED_EQ,
	PRED_LT,
	PRED_GT
} PredicateOp;

#define MAT(m, i, j) ((m)->matrix.elements[(i) * (m)->matrix.columns + (j)])
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define CLAMP(value, lo, hi) do { \
	if ((value) < (lo)) \
		(value) = (lo); \
	if ((value) > (hi)) \
		(value) = (hi); \
} while (0)

#define GROW_IF_FULL(count, cap, arr) do { \
	if ((count) == (cap)) { \
		(cap) = (cap) ? (cap) * 2 : 8; \
		(arr) = arena_realloc((arr), sizeof(*(arr)) * (size_t)(cap)); \
	} \
} while (0)

#define GROW_IF_FULL_SYS(count, cap, arr) do { \
	if ((count) == (cap)) { \
		(cap) = (cap) ? (cap) * 2 : 8; \
		(arr) = realloc((arr), sizeof(*(arr)) * (size_t)(cap)); \
	} \
} while (0)

#ifdef GC_DEBUG
#define GC_ASSERT(cond, msg) do { \
	if (!(cond)) { \
		fprintf(stderr, "water GC invariant violated: %s\n", msg); \
		abort(); \
	} \
} while (0)
#else
#define GC_ASSERT(cond, msg) ((void)0)
#define GROW_OBJECT_TABLE(new_cap) do { \
	int grow_old = arena.object_space.cap; \
	int grow_new = (new_cap); \
	arena.objects = realloc(arena.objects, sizeof(Object *) * (size_t)grow_new); \
	arena.object_space.free = realloc(arena.object_space.free, sizeof(int) * (size_t)grow_new); \
	memset(arena.objects + grow_old, 0, sizeof(Object *) * (size_t)(grow_new - grow_old)); \
	arena.object_space.cap = grow_new; \
} while (0)

#define GROW_PAIR_TABLE(new_cap) do { \
	int grow_new = (new_cap); \
	pairs.table = realloc(pairs.table, sizeof(Pair) * (size_t)grow_new); \
	pairs.mark_epoch = realloc(pairs.mark_epoch, sizeof(cell) * (size_t)grow_new); \
	pairs.space.free = realloc(pairs.space.free, sizeof(int) * (size_t)grow_new); \
	pairs.space.cap = grow_new; \
} while (0)

#define OBJECT_AT(handle) (arena.objects[handle])
typedef struct {
	int start_cfa;
	int end_cfa;
	int source_offset;
} QuotationSpan;

typedef struct Vocabulary {
	cell dict[VOCABULARY_INIT_SIZE];
	int here;
	int latest_cfa;
	int forget_generation;
	char name_pool[NAME_POOL];
	int names_here;
	char source_pool[SOURCE_POOL];
	int source_here;
	char symbol_pool[SYMBOL_POOL];
	int symbol_pool_here;
	_Atomic int symbol_hash[SYMBOL_HASH_SIZE];

	int exit_cfa, literal_cfa, branch_cfa, zbranch_cfa, dostr_cfa, stop_cfa, to_var_cfa;
	int enter_locals_cfa, enter_locals_to_cfa, enter_locals_mixed_cfa, leave_locals_cfa, local_fetch_cfa, local_store_cfa;
	int local_fetch_0depth_cfa, local_store_0depth_cfa;
	int local_fetch_1depth_cfa;
	int local_incr_0depth_cfa, local_decr_0depth_cfa, inc_cfa, dec_cfa;
	int local_finc_0depth_cfa, local_fdec_0depth_cfa, finc_cfa, fdec_cfa;
	int qzbranch_cfa;
	int eq_cfa, lt_cfa, gt_cfa, zeq_cfa;
	int eq_f_cfa, lt_f_cfa, gt_f_cfa;
	int at_i_cfa;
	int at_e_cfa;
	int eq_zbranch_cfa, lt_zbranch_cfa, gt_zbranch_cfa, zeq_zbranch_cfa;
	int eq_f_zbranch_cfa, lt_f_zbranch_cfa, gt_f_zbranch_cfa;
	int false_symbol, true_symbol;
	int wildcard_symbol, descendant_symbol, self_symbol;

	int init_here, init_latest_cfa, init_names_here;
	int init_source_here, init_symbol_pool_here;
	int lib_end_latest_cfa;

	QuotationSpan quotation_spans[MAX_QUOTATION_SPANS];
	int n_quotation_spans;
} Vocabulary;

extern Vocabulary vocab;

typedef struct Interpreter {
	Val data_stack[DATA_STACK_DEPTH];
	int dsp;
	Val return_stack[RETURN_STACK_DEPTH];
	int rsp;
	Val side_stack[SIDESTACK_DEPTH];
	int side_dsp;

	int local_base;
	int loop_local_base;
	int loop_local_refill;
	int run_floor;
	int loop_body_start;
	int loop_n;
	int loop_slots_ip;

	int *bind_trail;
	int bind_trail_top, bind_trail_cap;
	Val *lvar_stack;
	int lvar_top, lvar_cap;

	int ip;
	int trampoline_base;
	int running;
	int error_flag;
	int gc_disabled;
	int gc_pending;
	cell gc_epoch;
	int gc_object_base, gc_pair_base;

	Val gc_roots[MAX_GC_ROOTS];
	int n_gc_roots;

	struct {
		char *pattern;
		int pattern_len;
		unsigned pattern_hash;
		void *re;
		int in_use;
	} regex_cache[REGEX_CACHE_SIZE];
	int regex_cache_next;

	void *databases[MAX_DATABASES];
	int n_databases;

	int unwinding, unwind_target, next_mark_id;
	int call_depth;

	char error_message[256];
	char error_trace[ERROR_TRACE_SIZE];
} Interpreter;

typedef struct {
	int n_items;
	int items_per_claim;
	_Atomic int next_index;
	void (*kernel)(int start_index, int end_index, void *context);
	void *context;
} ParallelTask;

typedef struct {
	int compiling;

	char input_buffer[INPUT_BUFFER_SIZE];
	int input_buffer_len, input_buffer_pos, need_more;
	int interactive;
	int compiling_src_start;

	int fuse_prev_var, fuse_prev2_var;
	int fuse_prev_cmp;
	int fuse_floor;
	int loadn_at;
	int loop_begin;
	int leave_chain;

	char local_names_pool[LOCAL_NAMES_POOL_SIZE];

	int local_names_pool_here;
	int local_name_offsets[MAX_LOCAL_NAMES];
	int n_local_names;
	int local_scope_starts[MAX_LOCAL_SCOPES];
	int local_scope_dict_starts[MAX_LOCAL_SCOPES];
	int n_local_scopes;
	void *handler_registry[MAX_HANDLERS];
	int n_handlers;
	char *loaded_files[MAX_LOADED_FILES];
	int n_loaded_files, load_depth;

	char token_buffer[INPUT_BUFFER_SIZE];
} Compiler;

extern Compiler compiler;

#define DISPATCH_ARGS Interpreter *interp, cell *chain_ip __attribute__((unused)), Val *chain_sp __attribute__((unused))

typedef void (*cfa_handler)(DISPATCH_ARGS);

typedef double (*scalar_operator)(double, double);

extern unsigned char dict_is_handler[];

static inline int dict_op_is(int pos, cfa_handler h) {
	return pos >= 0 && dict_is_handler[pos] && (cfa_handler)vocab.dict[pos] == h;
}

#define WORD_LINK(cfa) (vocab.dict[(cfa) - 4])
#define WORD_FLAGS(cfa) (vocab.dict[(cfa) - 3])
#define WORD_NAME(cfa) (vocab.dict[(cfa) - 2])
#define WORD_SOURCE(cfa) (vocab.dict[(cfa) - 1])
#define WORD_IS_IMMEDIATE(cfa) (WORD_FLAGS(cfa) & 1)
#define WORD_IS_INLINE(cfa) (WORD_FLAGS(cfa) & 2)
#define WORD_IS_INTERNAL(cfa) (WORD_FLAGS(cfa) & 4)

typedef struct {
	int saved_ip;
	int saved_running;
	cell saved_slot_0, saved_slot_1, saved_slot_2;
	int fast;
	int reuses_locals;
	int saved_loop_local_base;

	int saved_loop_body_start;
	int saved_loop_n;
	int saved_loop_slots_ip;
	int leave_ip;
	cell saved_leave;
} CallContext;

typedef struct {
	const char *name;
	const char *effect;
	const char *summary;
	const char *ops;
	const char *alloc;
	const char *order;
	int section;
} HelpEntry;

extern const HelpEntry help_entries[];
extern const int help_entry_count;
extern const char *const help_section_names[];
extern const int help_section_count;

extern int print_truncate;

#define POP(name) Val name = pop(interp); if (interp->error_flag) return
#define POP_TYPED(name, op, type) \
	POP(name##_val); \
	if (VAL_TAG(name##_val) != (type)) { \
		fail(interp, "expected %s; got %s", tag_name(type), tag_name(VAL_TAG(name##_val))); \
		return; \
	}

#define POP_INT(name, op, what) \
	POP(name##_val); \
	if (VAL_TAG(name##_val) != T_FLOAT) { \
		fail(interp, "expected a float %s; got %s", (what), tag_name(VAL_TAG(name##_val))); \
		return; \
	} \
	int name = (int)VAL_NUMBER(name##_val)

#define POP_FLOAT(name, op, what) \
	POP(name##_val); \
	if (VAL_TAG(name##_val) != T_FLOAT) { \
		fail(interp, "expected a float %s; got %s", (what), tag_name(VAL_TAG(name##_val))); \
		return; \
	} \
	double name = VAL_NUMBER(name##_val)

#define POP_XT(name, op)      POP_TYPED(name, op, T_XT);      int name = (int)VAL_DATA(name##_val)
#define POP_MATRIX(name, op)  POP_TYPED(name, op, T_MATRIX);  Object *name = OBJECT_AT(VAL_DATA(name##_val))
#define POP_SEGMENT(name, op) POP_TYPED(name, op, T_SEGMENT); Object *name = OBJECT_AT(VAL_DATA(name##_val))
#define POP_STRING(name, op)  POP_TYPED(name, op, T_STRING);  Object *name = OBJECT_AT(VAL_DATA(name##_val))
#define POP_ARRAY(name, op)   POP_TYPED(name, op, T_ARRAY);   Object *name = OBJECT_AT(VAL_DATA(name##_val))
#define POP_SET(name, op)     POP_TYPED(name, op, T_SET);     int name = (int)VAL_DATA(name##_val)
#define POP_PAIR(name, op)    POP_TYPED(name, op, T_PAIR);    Pair *name = &pairs.table[VAL_DATA(name##_val)]
#define POP_QUANTITY(name, op) POP_TYPED(name, op, T_QUANTITY); Pair *name = &pairs.table[VAL_DATA(name##_val)]
#define POP_CONT(name, op)    POP_TYPED(name, op, T_CONT);    Object *name = OBJECT_AT(VAL_DATA(name##_val))
#define POP_SYMBOL(name, op)  POP_TYPED(name, op, T_SYMBOL);  cell name = VAL_DATA(name##_val)
#define POP_PTR(name, op)     POP_TYPED(name, op, T_PTR);     int name = (int)VAL_DATA(name##_val)
#define POP_COLLECTION(name, op) \
	POP(name##_val); \
	if (VAL_TAG(name##_val) != T_ARRAY && VAL_TAG(name##_val) != T_SET) { \
		fail(interp, "expected an array or set; got %s", tag_name(VAL_TAG(name##_val))); \
		return; \
	} \
	Object *name = OBJECT_AT(VAL_DATA(name##_val))

#define NEW_MATRIX(handle, obj, rows, cols) \
	int handle = object_new_matrix(interp, (rows), (cols)); \
	if (interp->error_flag) return; \
	Object *obj = OBJECT_AT(handle)

#define NEW_ARRAY(handle, obj, len) \
	int handle = object_new_array(interp, (len)); \
	if (interp->error_flag) return; \
	Object *obj = OBJECT_AT(handle)

#define NEW_FRAME(handle, obj) \
	int handle = object_new_frame(interp); \
	if (interp->error_flag) return; \
	Object *obj = OBJECT_AT(handle)

#define NEW_OBJECT(obj, kind) \
	int slot; \
	Object *obj = object_new(interp, (kind), &slot); \
	if (!obj) return -1

#define INIT_PAIR(slot) \
	pairs.table[slot].head = make_tagged(T_NONE, 0); \
	pairs.table[slot].tail = make_tagged(T_NONE, 0)

#define PEEK_AT(var, depth, op) \
	if (interp->dsp <= (depth)) { \
		fail(interp, "stack underflow"); \
		return; \
	} \
	Val var = interp->data_stack[interp->dsp - 1 - (depth)]

#define REQUIRE_CHAIN_TAG(value, tag, op, expected_phrase) \
	do { \
		if (VAL_TAG(value) != (tag)) { \
			fail(interp, "expected " expected_phrase "; got %s", tag_name(VAL_TAG(value))); \
			return; \
		} \
	} while (0)

#define PEEK_TYPE_AT(var, depth, op, type) \
	PEEK_AT(var, depth, op); \
	if (VAL_TAG(var) != (type)) { \
		fail(interp, "expected %s; got %s", tag_name(type), tag_name(VAL_TAG(var))); \
		return; \
	}

#define PEEK_STRING_AT(name, depth, op) \
	PEEK_TYPE_AT(name##_val, depth, op, T_STRING); \
	Object *name = OBJECT_AT(VAL_DATA(name##_val))

#define PEEK_SEQUENCE_AT(var, depth, op) \
	PEEK_AT(var, depth, op); \
	if (VAL_TAG(var) != T_ARRAY && VAL_TAG(var) != T_SET) { \
		fail(interp, "expected array or set; got %s", tag_name(VAL_TAG(var))); \
		return; \
	}

#define PEEK_COLLECTION_AT(var, depth, op) \
	PEEK_AT(var, depth, op); \
	if (VAL_TAG(var) != T_ARRAY && VAL_TAG(var) != T_SET && VAL_TAG(var) != T_FRAME) { \
		fail(interp, "expected array, set, or frame; got %s", tag_name(VAL_TAG(var))); \
		return; \
	}

int dgemm_kernel(Interpreter *interp, int transpose_a, int transpose_b,
		double alpha,
		int a_handle, int b_handle,
		double beta, int c_handle);

void parallel_for(int n_items, int n_threads, int items_per_claim,
		void (*kernel)(int start_index, int end_index, void *context), void *context);

void write_u8 (FILE *f, uint8_t v);
int read_u8 (FILE *f, uint8_t *v);

typedef enum { WALK_ERROR, WALK_VIVIFY, WALK_PROBE } FrameWalkMode;

#define LOWER_BOUND(count, probe, less, at) \
	int at = 0; \
	for (int bsearch_high = (count); at < bsearch_high; ) { \
		int probe = (at + bsearch_high) / 2; \
		if (less) { \
			at = probe + 1; \
		} else { \
			bsearch_high = probe; \
		} \
	}

static inline __attribute__((always_inline)) int frame_find(Object *frame, cell key) {
	LOWER_BOUND(frame->len, mid, frame->frame.keys[mid] < key, at);
	return at;
}
#define FRAME_LOOKUP(obj, key, at, present) \
	int at = frame_find((obj), (key)); \
	int present = (at) < (obj)->len && (obj)->frame.keys[at] == (key)

int alloc_name(Interpreter *interp, const char *name);
Object *arena_alloc_object(void);
void arena_free(void *payload);
void *arena_malloc(size_t bytes);
void *arena_realloc(void *payload, size_t bytes);
void call_close(Interpreter *interp, CallContext *ctx);
void call_invoke(Interpreter *interp);
void call_open(Interpreter *interp, int cfa, CallContext *ctx);
int capture_render(Interpreter *interp, void (*render)(FILE *, Interpreter *, int), int target_cfa);
int construct_vocabulary(Interpreter *interp, int load_lib);
int create_header(Interpreter *interp, const char *name, int flags);
int define_primitive(Interpreter *interp, const char *name, cfa_handler handler, int flags);
void dict_ensure(Interpreter *interp, int extra);
int double_cmp(const void *left, const void *right);
void emit(Interpreter *interp, cell value);
void emit_call(Interpreter *interp, int target_cfa);
void emit_val_literal(Interpreter *interp, Val value);
void execute_cfa(Interpreter *interp, int cfa);
void execute_xt(Interpreter *interp, int cfa);
void fail(Interpreter *interp, const char *fmt, ...);
int find(const char *name);
int find_local(const char *token, int *depth_out, int *slot_out);
void forget_user(Interpreter *interp);
void free_one_object(Object *obj);
int fuse_rewrite(Interpreter *interp, int n_replaced_cells, int fused_cfa, cell operand);
int fuse_rewrite_pair(Interpreter *interp, int n_replaced_cells, int fused_cfa, cell operand_a, cell operand_b);
void gc(Interpreter *interp);
void heap_bytes_add(size_t bytes);
void inbuf_reset(void);
void inline_word_body(Interpreter *interp, int target_cfa);
int intern_symbol(Interpreter *interp, const char *name);
void interp_init(Interpreter *interp);
void load_file(Interpreter *interp, const char *filename);
Interpreter *main_init(void);
void mark_body(Interpreter *interp, int body_start, int body_end);
void mark_value(Interpreter *interp, Val value);
const char *name_of(int cfa);
char *next_token(void);
int object_alloc_slot(Interpreter *interp);
int object_new_array(Interpreter *interp, int num_elements);
int object_new_continuation(Interpreter *interp, const Val *frames, int return_len, int resume_ip);
int object_new_frame(Interpreter *interp);
int object_new_logic_var(Interpreter *interp);
int object_new_matrix(Interpreter *interp, int num_rows, int num_columns);
int object_new_pair(Interpreter *interp);
int object_new_segment(Interpreter *interp, int length, SegmentType element_type);
int object_new_set(Interpreter *interp);
int object_new_string(Interpreter *interp, const char *bytes, int length);
int object_new_string_uninit(Interpreter *interp, int length);
int op_cell_count(int cursor);
int parse_float(const char *text, double *out);
void pretty_print_array(FILE *out, Interpreter *interp, Val value);
void print_corners(FILE *out, Object *matrix);
void print_double(FILE *out, double number);
void print_frame_pretty(FILE *out, Interpreter *interp, Object *frame, int indent);
void print_items(FILE *out, Interpreter *interp, Object *collection);
void print_matrix_cell(FILE *out, double value);
void print_matrix_grid(FILE *out, Object *m, int unit);
void print_prompt_state(Interpreter *interp);
void print_val(FILE *out, Interpreter *interp, Val value);
void print_val_compact(FILE *out, Interpreter *interp, Val value);
void print_val_inspect(FILE *out, Interpreter *interp, Val value);
void rebuild_symbol_hash(void);
void record_loaded_file(Interpreter *interp, const char *filename);
int refill_input(void);
void region_abort(ParallelRegion *region);
void region_begin(ParallelRegion *region, int domain_len, int worker_count);
void region_commit(ParallelRegion *region);
void reset_thread_alloc(void);
void run_inner(Interpreter *interp, int floor);
void run_outer(Interpreter *interp);
void skip_whitespace_and_comments(void);
int stdout_is_tty(void);
const char *tag_name(Tag t);
int try_fuse_at_e_lit(Interpreter *interp);
int try_fuse_at_e_ll(Interpreter *interp);
int try_fuse_at_e_local(Interpreter *interp);
int try_fuse_at_i_lit(Interpreter *interp);
int try_fuse_at_i_ll(Interpreter *interp);
int try_fuse_at_i_local(Interpreter *interp);
int try_fuse_gather_local(Interpreter *interp);
int try_fuse_local_acc(Interpreter *interp, int depth, int slot);
int try_fuse_local_arith(Interpreter *interp, cfa_handler op_handler);
int val_cmp(Interpreter *interp, Val left, Val right);
Interpreter *worker_init(int worker_index);
void worker_local_gc(Interpreter *interp);

void backtrack(Interpreter *interp);
void binary_op(Interpreter *interp, Val left, Val right, scalar_operator function, const char *name);
int capture_continuation(Interpreter *interp, int what_kind, int *out_mark_index);
int interpolate(Interpreter *interp, int template_handle);
int push_prompt(Interpreter *interp, int kind);
int random_below(int bound);
int read_string_literal(void);
int string_concat(Interpreter *interp, int left_handle, int right_handle);
void type_of_intern_names(Interpreter *interp);
void unary_op(Interpreter *interp, Val operand, double (*function)(double));

int create_variable(Interpreter *interp, const char *name);
void rollback_partial_definition(void);
void truncate_quotation_spans(void);

int read_i32(FILE *f, int32_t *v);
int read_i64(FILE *f, int64_t *v);
int read_u32(FILE *f, uint32_t *v);
int read_val(FILE *f, Val *v);
void write_i32(FILE *f, int32_t v);
void write_i64(FILE *f, int64_t v);
void write_val(FILE *f, Val value);

int array_sorted_copy(Interpreter *interp, Object *source);
int build_set_from_values(Interpreter *interp, const Val *values, int count);
int frame_delete(Object *frame, cell key);
void frame_put(Object *frame, cell key, Val value);
void frame_reserve(Object *frame, int needed);
void set_add(Interpreter *interp, int set_handle, Val value);
int set_difference(Interpreter *interp, int handle_a, int handle_b);
int set_elements_copy(Interpreter *interp, Object *source);
int set_intersect(Interpreter *interp, int handle_a, int handle_b);
int set_member(Interpreter *interp, int set_handle, Val value);
void set_remove(Interpreter *interp, int set_handle, Val value);
int set_union(Interpreter *interp, int handle_a, int handle_b);

int create_matrix(Interpreter *interp);
int matrix_add(Interpreter *interp, Val left_val, Val right_val);
int matrix_div(Interpreter *interp, Val left_val, Val right_val);
int matrix_max_columns(Interpreter *interp, Object *source);
double matrix_max_overall(Object *source);
int matrix_max_rows(Interpreter *interp, Object *source);
int matrix_min_columns(Interpreter *interp, Object *source);
double matrix_min_overall(Object *source);
int matrix_min_rows(Interpreter *interp, Object *source);
int matrix_mul(Interpreter *interp, Val left_val, Val right_val);
int matrix_nonzero_indices(Interpreter *interp, Object *source);
int matrix_sub(Interpreter *interp, Val left_val, Val right_val);
int matrix_sum_columns(Interpreter *interp, Object *source);
double matrix_sum_overall(Object *source);
int matrix_sum_rows(Interpreter *interp, Object *source);
int vector_argsort_copy(Interpreter *interp, Object *source);
int vector_sorted_copy(Interpreter *interp, Object *source);

void define_superwords(Interpreter *interp);
int superword_cell_count(cell handler);
int superword_is_lit_fold(cell handler);
int superword_try_fuse(Interpreter *interp, int op_cfa);
int superword_try_fuse_store(Interpreter *interp, int dst_cfa);

int string_codepoint_count(Object *string);
int string_matches(Interpreter *interp, Object *subject, Object *pattern);
int utf8_codepoint_count(const char *bytes, int length);
int utf8_encode(int codepoint, char *out);

Val deref(Interpreter *interp, Val value);
void trail_undo_to(Interpreter *interp, int mark);

int ffi_register_call_cfa(int cfa);

void apply_unit(Interpreter *interp, int cfa);
void dimension_freeze(void);
void dimension_init();
int dimension_load(FILE *file);
void dimension_save(FILE *file);
void push_quantity(Interpreter *interp, Val magnitude, int unit);
int quantity_truthy(Val quantity);
void render_unit(FILE *out, int unit);
void render_unit_description(FILE *out, Interpreter *interp, int word_cfa);
int unit_conversion(int from, int to, double *factor);
int unit_divide(Interpreter *interp, int left, int right, double *collapse_factor);
int unit_id_valid(int unit);
int unit_is_named(int unit);
int unit_multiply(Interpreter *interp, int left, int right, double *collapse_factor);
int unit_pow(Interpreter *interp, int unit, int numerator, int denominator);
double unit_scale_value(int unit);

void docol(DISPATCH_ARGS);
void dosym(DISPATCH_ARGS);
void dovar(DISPATCH_ARGS);
void p_0branch(DISPATCH_ARGS);
void p_branch(DISPATCH_ARGS);
void p_copy(DISPATCH_ARGS);
void p_dostr(DISPATCH_ARGS);
void p_enter_locals(DISPATCH_ARGS);
void p_enter_locals_mixed(DISPATCH_ARGS);
void p_enter_locals_to(DISPATCH_ARGS);
void p_exit(DISPATCH_ARGS);
void p_leave_locals(DISPATCH_ARGS);
void p_literal(DISPATCH_ARGS);
void p_load(DISPATCH_ARGS);
void p_load2(DISPATCH_ARGS);
void p_load3(DISPATCH_ARGS);
void p_local_decr_0depth(DISPATCH_ARGS);
void p_local_fdec_0depth(DISPATCH_ARGS);
void p_local_fetch(DISPATCH_ARGS);
void p_local_fetch_0depth(DISPATCH_ARGS);
void p_local_fetch_1depth(DISPATCH_ARGS);
void p_local_finc_0depth(DISPATCH_ARGS);
void p_local_incr_0depth(DISPATCH_ARGS);
void p_local_store(DISPATCH_ARGS);
void p_local_store_0depth(DISPATCH_ARGS);
void p_qzbranch(DISPATCH_ARGS);
void p_reify(DISPATCH_ARGS);
void p_reload(DISPATCH_ARGS);
void p_save(DISPATCH_ARGS);
void p_see_compiled_to_string(DISPATCH_ARGS);
void p_see_tree_to_string(DISPATCH_ARGS);
void p_set(DISPATCH_ARGS);
void p_stop(DISPATCH_ARGS);

void p_abs(DISPATCH_ARGS);
void p_acos(DISPATCH_ARGS);
void p_add(DISPATCH_ARGS);
void p_add_f(DISPATCH_ARGS);
void p_add_inplace(DISPATCH_ARGS);
void p_and(DISPATCH_ARGS);
void p_apropos(DISPATCH_ARGS);
void p_argsort(DISPATCH_ARGS);
void p_asin(DISPATCH_ARGS);
void p_atan(DISPATCH_ARGS);
void p_bit_and(DISPATCH_ARGS);
void p_bit_not(DISPATCH_ARGS);
void p_bit_or(DISPATCH_ARGS);
void p_bit_xor(DISPATCH_ARGS);
void p_bye(DISPATCH_ARGS);
void p_clear(DISPATCH_ARGS);
void p_cos(DISPATCH_ARGS);
void p_cr(DISPATCH_ARGS);
void p_dec(DISPATCH_ARGS);
void p_dec_poly(DISPATCH_ARGS);
void p_depth(DISPATCH_ARGS);
void p_div(DISPATCH_ARGS);
void p_div_f(DISPATCH_ARGS);
void p_div_inplace(DISPATCH_ARGS);
void p_divmod(DISPATCH_ARGS);
void p_dot(DISPATCH_ARGS);
void p_dot_all(DISPATCH_ARGS);
void p_dots(DISPATCH_ARGS);
void p_drop(DISPATCH_ARGS);
void p_dup(DISPATCH_ARGS);
void p_emit_(DISPATCH_ARGS);
void p_eq(DISPATCH_ARGS);
void p_eq_elements(DISPATCH_ARGS);
void p_eq_f(DISPATCH_ARGS);
void p_eq_f_zbranch(DISPATCH_ARGS);
void p_eq_string(DISPATCH_ARGS);
void p_eq_symbol(DISPATCH_ARGS);
void p_eq_zbranch(DISPATCH_ARGS);
void p_execute(DISPATCH_ARGS);
void p_execute_catching(DISPATCH_ARGS);
void p_exp(DISPATCH_ARGS);
void p_fabs(DISPATCH_ARGS);
void p_facos(DISPATCH_ARGS);
void p_fail(DISPATCH_ARGS);
void p_fasin(DISPATCH_ARGS);
void p_fatan(DISPATCH_ARGS);
void p_fcos(DISPATCH_ARGS);
void p_fexp(DISPATCH_ARGS);
void p_fln(DISPATCH_ARGS);
void p_flog(DISPATCH_ARGS);
void p_fmodop(DISPATCH_ARGS);
void p_fnegate(DISPATCH_ARGS);
void p_format(DISPATCH_ARGS);
void p_fpow(DISPATCH_ARGS);
void p_fround(DISPATCH_ARGS);
void p_fround_down(DISPATCH_ARGS);
void p_fround_up(DISPATCH_ARGS);
void p_fsin(DISPATCH_ARGS);
void p_fsqrt(DISPATCH_ARGS);
void p_ftan(DISPATCH_ARGS);
void p_ftanh(DISPATCH_ARGS);
void p_ftruncate(DISPATCH_ARGS);
void p_gc(DISPATCH_ARGS);
void p_gt(DISPATCH_ARGS);
void p_gt_f(DISPATCH_ARGS);
void p_gt_f_zbranch(DISPATCH_ARGS);
void p_gt_zbranch(DISPATCH_ARGS);
void p_inc(DISPATCH_ARGS);
void p_inc_poly(DISPATCH_ARGS);
void p_ln(DISPATCH_ARGS);
void p_log(DISPATCH_ARGS);
void p_lowest_bit(DISPATCH_ARGS);
void p_lshift(DISPATCH_ARGS);
void p_lt(DISPATCH_ARGS);
void p_lt_f(DISPATCH_ARGS);
void p_lt_f_zbranch(DISPATCH_ARGS);
void p_lt_zbranch(DISPATCH_ARGS);
void p_man(DISPATCH_ARGS);
void p_mul(DISPATCH_ARGS);
void p_mul_f(DISPATCH_ARGS);
void p_mul_inplace(DISPATCH_ARGS);
void p_nan(DISPATCH_ARGS);
void p_neg(DISPATCH_ARGS);
void p_not(DISPATCH_ARGS);
void p_null(DISPATCH_ARGS);
void p_or(DISPATCH_ARGS);
void p_over(DISPATCH_ARGS);
void p_power(DISPATCH_ARGS);
void p_random(DISPATCH_ARGS);
void p_random_int(DISPATCH_ARGS);
void p_render(DISPATCH_ARGS);
void p_reset(DISPATCH_ARGS);
void p_resume(DISPATCH_ARGS);
void p_rfetch(DISPATCH_ARGS);
void p_rfrom(DISPATCH_ARGS);
void p_roll(DISPATCH_ARGS);
void p_rot(DISPATCH_ARGS);
void p_round(DISPATCH_ARGS);
void p_round_down(DISPATCH_ARGS);
void p_round_up(DISPATCH_ARGS);
void p_rshift(DISPATCH_ARGS);
void p_see(DISPATCH_ARGS);
void p_see_to_string(DISPATCH_ARGS);
void p_seed(DISPATCH_ARGS);
void p_shift(DISPATCH_ARGS);
void p_shift_with(DISPATCH_ARGS);
void p_side_depth(DISPATCH_ARGS);
void p_side_drop(DISPATCH_ARGS);
void p_side_to(DISPATCH_ARGS);
void p_sin(DISPATCH_ARGS);
void p_size(DISPATCH_ARGS);
void p_size_len(DISPATCH_ARGS);
void p_sleep(DISPATCH_ARGS);
void p_sort(DISPATCH_ARGS);
void p_sq(DISPATCH_ARGS);
void p_sq_poly(DISPATCH_ARGS);
void p_sqrt(DISPATCH_ARGS);
void p_sub(DISPATCH_ARGS);
void p_sub_f(DISPATCH_ARGS);
void p_sub_inplace(DISPATCH_ARGS);
void p_swap(DISPATCH_ARGS);
void p_tan(DISPATCH_ARGS);
void p_tanh(DISPATCH_ARGS);
void p_to_side(DISPATCH_ARGS);
void p_tor(DISPATCH_ARGS);
void p_truncate(DISPATCH_ARGS);
void p_type_of(DISPATCH_ARGS);
void p_water(DISPATCH_ARGS);
void p_words(DISPATCH_ARGS);
void p_zeq(DISPATCH_ARGS);
void p_zeq_zbranch(DISPATCH_ARGS);

void p_again(DISPATCH_ARGS);
void p_bar(DISPATCH_ARGS);
void p_bar_to(DISPATCH_ARGS);
void p_begin(DISPATCH_ARGS);
void p_bracket_bar(DISPATCH_ARGS);
void p_bracket_bar_to(DISPATCH_ARGS);
void p_colon(DISPATCH_ARGS);
void p_constant(DISPATCH_ARGS);
void p_continue(DISPATCH_ARGS);
void p_decrement(DISPATCH_ARGS);
void p_else(DISPATCH_ARGS);
void p_f_decrement(DISPATCH_ARGS);
void p_f_increment(DISPATCH_ARGS);
void p_forget(DISPATCH_ARGS);
void p_if(DISPATCH_ARGS);
void p_increment(DISPATCH_ARGS);
void p_inline(DISPATCH_ARGS);
void p_internal(DISPATCH_ARGS);
void p_leave(DISPATCH_ARGS);
void p_lookup(DISPATCH_ARGS);
void p_qcolon(DISPATCH_ARGS);
void p_qif(DISPATCH_ARGS);
void p_qsemi(DISPATCH_ARGS);
void p_repeat(DISPATCH_ARGS);
void p_semicolon(DISPATCH_ARGS);
void p_string_to_symbol(DISPATCH_ARGS);
void p_symbol(DISPATCH_ARGS);
void p_then(DISPATCH_ARGS);
void p_tick(DISPATCH_ARGS);
void p_to(DISPATCH_ARGS);
void p_to_var(DISPATCH_ARGS);
void p_until(DISPATCH_ARGS);
void p_variable(DISPATCH_ARGS);
void p_while(DISPATCH_ARGS);

void p_append_file(DISPATCH_ARGS);
void p_cd(DISPATCH_ARGS);
void p_close(DISPATCH_ARGS);
void p_cwd(DISPATCH_ARGS);
void p_env(DISPATCH_ARGS);
void p_env_set(DISPATCH_ARGS);
void p_read(DISPATCH_ARGS);
void p_read_file(DISPATCH_ARGS);
void p_read_tsv(DISPATCH_ARGS);
void p_stderr(DISPATCH_ARGS);
void p_stdin(DISPATCH_ARGS);
void p_stdout(DISPATCH_ARGS);
void p_write(DISPATCH_ARGS);
void p_write_file(DISPATCH_ARGS);
void p_write_tsv(DISPATCH_ARGS);

void p_load_image(DISPATCH_ARGS);
void p_save_image(DISPATCH_ARGS);

void p_add_last(DISPATCH_ARGS);
void p_array(DISPATCH_ARGS);
void p_array_close(DISPATCH_ARGS);
void p_array_of(DISPATCH_ARGS);
void p_array_open(DISPATCH_ARGS);
void p_array_to_cons(DISPATCH_ARGS);
void p_array_to_frame(DISPATCH_ARGS);
void p_array_to_set(DISPATCH_ARGS);
void p_byte_size(DISPATCH_ARGS);
void p_concat(DISPATCH_ARGS);
void p_cons(DISPATCH_ARGS);
void p_cons_to_array(DISPATCH_ARGS);
void p_destruct(DISPATCH_ARGS);
void p_destruct_to(DISPATCH_ARGS);
void p_difference(DISPATCH_ARGS);
void p_double_segment(DISPATCH_ARGS);
void p_flatten_array(DISPATCH_ARGS);
void p_frame(DISPATCH_ARGS);
void p_frame_delete_at(DISPATCH_ARGS);
void p_frame_get(DISPATCH_ARGS);
void p_frame_get_or(DISPATCH_ARGS);
void p_frame_get_symbol(DISPATCH_ARGS);
void p_frame_keys(DISPATCH_ARGS);
void p_frame_set(DISPATCH_ARGS);
void p_frame_set_symbol(DISPATCH_ARGS);
void p_frame_to_array(DISPATCH_ARGS);
void p_frame_to_json(DISPATCH_ARGS);
void p_frame_values(DISPATCH_ARGS);
void p_frameclose(DISPATCH_ARGS);
void p_frameopen(DISPATCH_ARGS);
void p_group_by(DISPATCH_ARGS);
void p_has(DISPATCH_ARGS);
void p_head_tail(DISPATCH_ARGS);
void p_int_segment(DISPATCH_ARGS);
void p_intersect(DISPATCH_ARGS);
void p_json_to_frame(DISPATCH_ARGS);
void p_list_close(DISPATCH_ARGS);
void p_list_open(DISPATCH_ARGS);
void p_member(DISPATCH_ARGS);
void p_merge(DISPATCH_ARGS);
void p_range(DISPATCH_ARGS);
void p_remove_last(DISPATCH_ARGS);
void p_reverse(DISPATCH_ARGS);
void p_sample(DISPATCH_ARGS);
void p_select_keys(DISPATCH_ARGS);
void p_select_values(DISPATCH_ARGS);
void p_set_add(DISPATCH_ARGS);
void p_set_remove(DISPATCH_ARGS);
void p_setclose(DISPATCH_ARGS);
void p_setopen(DISPATCH_ARGS);
void p_slice_store(DISPATCH_ARGS);
void p_take(DISPATCH_ARGS);
void p_to_slice(DISPATCH_ARGS);
void p_union(DISPATCH_ARGS);
void p_update_at(DISPATCH_ARGS);

void p_0_matrix(DISPATCH_ARGS);
void p_argmax(DISPATCH_ARGS);
void p_argmin(DISPATCH_ARGS);
void p_at_e(DISPATCH_ARGS);
void p_at_e_l1l0(DISPATCH_ARGS);
void p_at_e_lit(DISPATCH_ARGS);
void p_at_e_ll0(DISPATCH_ARGS);
void p_at_e_local0(DISPATCH_ARGS);
void p_at_ij(DISPATCH_ARGS);
void p_at_j(DISPATCH_ARGS);
void p_augment(DISPATCH_ARGS);
void p_column_maxes(DISPATCH_ARGS);
void p_column_mins(DISPATCH_ARGS);
void p_column_sums(DISPATCH_ARGS);
void p_dgemm_helper(Interpreter *interp, int transpose_a, int transpose_b);
void p_dgemm_nn(DISPATCH_ARGS);
void p_dgemm_nt(DISPATCH_ARGS);
void p_dgemm_tn(DISPATCH_ARGS);
void p_dgemm_tt(DISPATCH_ARGS);
void p_diagonal(DISPATCH_ARGS);
void p_diagonal_matrix(DISPATCH_ARGS);
void p_dim(DISPATCH_ARGS);
void p_frobenius_norm(DISPATCH_ARGS);
void p_matrix(DISPATCH_ARGS);
void p_matrix_range(DISPATCH_ARGS);
void p_max(DISPATCH_ARGS);
void p_min(DISPATCH_ARGS);
void p_norm(DISPATCH_ARGS);
void p_reshape(DISPATCH_ARGS);
void p_row_maxes(DISPATCH_ARGS);
void p_row_mins(DISPATCH_ARGS);
void p_row_sums(DISPATCH_ARGS);
void p_select_rows(DISPATCH_ARGS);
void p_store_e(DISPATCH_ARGS);
void p_store_e_drop(DISPATCH_ARGS);
void p_store_e_lll0(DISPATCH_ARGS);
void p_store_ij(DISPATCH_ARGS);
void p_store_ij_drop(DISPATCH_ARGS);
void p_submatrix(DISPATCH_ARGS);
void p_sum(DISPATCH_ARGS);
void p_transpose(DISPATCH_ARGS);
void p_vstack(DISPATCH_ARGS);
void p_where(DISPATCH_ARGS);

double matrix_variance_overall(Object *source);
void p_quantile(DISPATCH_ARGS);
void p_variance(DISPATCH_ARGS);

void p_add_store_i(DISPATCH_ARGS);
void p_at_i(DISPATCH_ARGS);
void p_at_i_array(DISPATCH_ARGS);
void p_at_i_l1l0(DISPATCH_ARGS);
void p_at_i_lit(DISPATCH_ARGS);
void p_at_i_lit_local0(DISPATCH_ARGS);
void p_at_i_ll0(DISPATCH_ARGS);
void p_at_i_local0(DISPATCH_ARGS);
void p_at_i_segment(DISPATCH_ARGS);
void p_dec_store_i(DISPATCH_ARGS);
void p_div_store_i(DISPATCH_ARGS);
void p_gather_local0(DISPATCH_ARGS);
void p_inc_store_i(DISPATCH_ARGS);
void p_mul_store_i(DISPATCH_ARGS);
void p_store_i(DISPATCH_ARGS);
void p_store_i_array(DISPATCH_ARGS);
void p_store_i_drop(DISPATCH_ARGS);
void p_store_i_drop_array(DISPATCH_ARGS);
void p_sub_store_i(DISPATCH_ARGS);

void p_filter(DISPATCH_ARGS);
void p_i_times(DISPATCH_ARGS);
void p_map(DISPATCH_ARGS);
void p_mapn(DISPATCH_ARGS);
void p_num_cores(DISPATCH_ARGS);
void p_pfilter(DISPATCH_ARGS);
void p_pmap(DISPATCH_ARGS);
void p_pmap_reduce(DISPATCH_ARGS);
void p_reduce(DISPATCH_ARGS);
void p_times(DISPATCH_ARGS);

void p_byte_substring(DISPATCH_ARGS);
void p_char_at(DISPATCH_ARGS);
void p_codepoint_at(DISPATCH_ARGS);
void p_codepoint_to_char(DISPATCH_ARGS);
void p_codepoints_to_string(DISPATCH_ARGS);
void p_join(DISPATCH_ARGS);
void p_match(DISPATCH_ARGS);
void p_match_all(DISPATCH_ARGS);
void p_replace(DISPATCH_ARGS);
void p_split(DISPATCH_ARGS);
void p_string_to_chars(DISPATCH_ARGS);
void p_string_to_codepoints(DISPATCH_ARGS);
void p_string_to_number(DISPATCH_ARGS);
void p_substring(DISPATCH_ARGS);
void p_trim(DISPATCH_ARGS);

void p_amb(DISPATCH_ARGS);
void p_deref(DISPATCH_ARGS);
void p_lvar(DISPATCH_ARGS);
void p_matches(DISPATCH_ARGS);
void p_unify(DISPATCH_ARGS);
void p_unify_cons(DISPATCH_ARGS);
void p_wildcard(DISPATCH_ARGS);

void p_db_close(DISPATCH_ARGS);
void p_db_exec(DISPATCH_ARGS);
void p_db_open(DISPATCH_ARGS);
void p_db_query(DISPATCH_ARGS);

void p_ffi_call(DISPATCH_ARGS);
void p_ffi_free(DISPATCH_ARGS);
void p_ffi_function(DISPATCH_ARGS);
void p_ffi_open(DISPATCH_ARGS);
void p_ffi_variadic(DISPATCH_ARGS);
void p_matrix_to_pointer(DISPATCH_ARGS);
void p_segment_to_pointer(DISPATCH_ARGS);

void p_running(DISPATCH_ARGS);
void p_start_process(DISPATCH_ARGS);
void p_stop_process(DISPATCH_ARGS);
void p_wait(DISPATCH_ARGS);

void dounit(DISPATCH_ARGS);
void p_base(DISPATCH_ARGS);
void p_unit(DISPATCH_ARGS);

void p_date_to_epoch(DISPATCH_ARGS);
void p_date_to_epoch_local(DISPATCH_ARGS);
void p_epoch_to_date(DISPATCH_ARGS);
void p_epoch_to_date_local(DISPATCH_ARGS);
void p_format_time(DISPATCH_ARGS);
void p_format_time_local(DISPATCH_ARGS);
void p_now(DISPATCH_ARGS);
void p_parse_time(DISPATCH_ARGS);
void p_wall_now(DISPATCH_ARGS);

static inline int truthy(Val value) {
	if (VAL_TAG(value) == T_FLOAT)
		return VAL_NUMBER(value) != 0.0;
	if (VAL_TAG(value) == T_QUANTITY)
		return quantity_truthy(value);
	return VAL_DATA(value) != 0;
}

static inline void gc_root_push(Interpreter *interp, Val value) {
	if (interp->n_gc_roots >= MAX_GC_ROOTS) {
		fail(interp, "gc roots exhausted");
		return;
	}
	interp->gc_roots[interp->n_gc_roots++] = value;
}

static inline void gc_root_pop(Interpreter *interp) {
	if (interp->n_gc_roots > 0) {
		interp->n_gc_roots--;
	}
}

static inline void push(Interpreter *interp, Val value) {
	if (interp->dsp < DATA_STACK_DEPTH) {
		interp->data_stack[interp->dsp++] = value;
	} else {
		fail(interp, "stack overflow");
	}
}

static inline Val pop(Interpreter *interp) {
	if (interp->dsp > 0) {
		return interp->data_stack[--interp->dsp];
	}
	fail(interp, "stack underflow");
	Val none = make_tagged(T_NONE, 0);
	return none;
}

static inline void rpush(Interpreter *interp, Val value) {
	if (interp->rsp < RETURN_STACK_DEPTH) {
		interp->return_stack[interp->rsp++] = value;
	} else {
		fail(interp, "return stack overflow");
	}
}

static inline Val rpop(Interpreter *interp) {
	if (interp->rsp > 0) {
		return interp->return_stack[--interp->rsp];
	}
	fail(interp, "return stack underflow");
	Val none = make_tagged(T_NONE, 0);
	return none;
}

static inline void call_step(Interpreter *interp, CallContext *context, int cfa) {
	if (context->fast) {
		if (context->reuses_locals)
			interp->loop_local_refill = 1;
		call_invoke(interp);
	} else
		execute_cfa(interp, cfa);
}

static inline __attribute__((always_inline))
Val frame_walk(Interpreter *interp, Val node, Object *path,
		int count, FrameWalkMode mode, int *found) {
	for (int i = 0; i < count; i++) {
		if (VAL_TAG(node) != T_FRAME) {
			if (found) *found = 0;
			if (mode != WALK_PROBE)
				fail(interp, "cannot descend into %s", tag_name(VAL_TAG(node)));
			return node;
		}

		cell key = VAL_DATA(path->items[i]);
		Object *frame = OBJECT_AT(VAL_DATA(node));
		FRAME_LOOKUP(frame, key, at, present);
		if (present && (mode != WALK_VIVIFY || VAL_TAG(frame->frame.values[at]) == T_FRAME)) {
			node = frame->frame.values[at];
		} else if (mode == WALK_VIVIFY) {
			if (key == (cell)vocab.wildcard_symbol || key == (cell)vocab.descendant_symbol) {
				fail(interp, "path has a wildcard or descendant; use select-keys/select-values");
				return node;
			}
			int child = object_new_frame(interp);
			frame_put(OBJECT_AT(VAL_DATA(node)), key, make_frame(child));
			node = make_frame(child);
		} else {
			if (found) *found = 0;
			if (mode != WALK_PROBE) {
				if (key == (cell)vocab.wildcard_symbol || key == (cell)vocab.descendant_symbol)
					fail(interp, "path has a wildcard or descendant; use select-keys/select-values");
				else
					fail(interp, "no key :%s", &vocab.symbol_pool[key]);
			}
			return node;
		}
	}
	if (found) *found = 1;
	return node;
}

#endif

#endif
