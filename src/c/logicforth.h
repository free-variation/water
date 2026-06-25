#ifndef LOGICFORTH_H
#define LOGICFORTH_H

#define VERSION "0.9.0"

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
#include <signal.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <pthread.h>
#include <ffi/ffi.h>

typedef int64_t cell;

#define VOCABULARY_INIT_SIZE (1 << 20)
#define NAME_POOL (1 << 18)
#define DATA_STACK_DEPTH (1 << 16)
#define RETURN_STACK_DEPTH (1 << 16)
#define MAX_OBJECTS (1 << 26)
#define ARENA_RESERVE ((size_t)1 << 34)
#define ARENA_ALIGNMENT 16
#define ARENA_SIZE_CLASSES (2 << 5)
#define OBJECTS_INIT_CAP (1 << 16)
#define SLAB_BYTES (1 << 16)
#define SLOTS_PER_CLAIM (1 << 10)
#define HEAP_GC_FLOOR ((size_t)1 << 28)
#define INPUT_BUFFER_SIZE (1 << 20)
#define SOURCE_POOL (1 << 22)
#define SYMBOL_POOL (1 << 22)
#define SYMBOL_HASH_SIZE (1 << 20)
#define SIDESTACK_DEPTH (1 << 10)
#define BIND_TRAIL_DEPTH (1 << 16)
#define MAX_LOADED_FILES (1 << 6)
#define MAX_GC_ROOTS (1 << 6)
#define LOCAL_NAMES_POOL_SIZE (1 << 13)
#define MAX_LOCAL_NAMES (1 << 8)
#define MAX_LOCAL_SCOPES (1 << 6)
#define MAX_HANDLERS (1 << 10)
#define MAX_DATABASES (1 << 8)
#define TRAMPOLINE_SLOT 0
#define DICT_RESERVED (3 * (MAX_WORKER_THREADS + 1))
#define PROMPT_EXCEPTION 0
#define PROMPT_CHOICE 1
#define LVAR_STACK_DEPTH (1 << 16)
#define PAIR_TABLE_DEPTH (1 << 20)
#define COPY_SPINE_MAX (1 << 24)
#define REGEX_CACHE_SIZE (1 << 10)
#define JSON_MAX_DEPTH (1 << 10)
#define SELECT_MAX_DEPTH JSON_MAX_DEPTH
#define MAX_WORKER_THREADS (1 << 6)
#define MAX_NESTING_DEPTH (1 << 8)
#define MAX_CALL_DEPTH (1 << 12)
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
	T_SEGMENT
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
		value.bits = NAN_BOX_PREFIX | VAL_DATA_MASK;
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
static inline Val make_continuation(int handle) { return make_tagged(T_CONT, handle); }
static inline Val make_logic_var(int handle) { return make_tagged(T_LOGIC_VAR, handle); }
static inline Val make_mark(void) { return make_tagged(T_MARK, 0); }
static inline Val make_bool(int is_true) { return make_float(is_true ? 1.0 : 0.0); }


static inline int truthy(Val value) {
	if (VAL_TAG(value) == T_FLOAT)
		return VAL_NUMBER(value) != 0.0;
	return VAL_DATA(value) != 0;
}

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

typedef struct {
	size_t used;
	int n_objects;
	int n_pairs;
} RegionSnapshot;


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
		fprintf(stderr, "logicforth GC invariant violated: %s\n", msg); \
		abort(); \
	} \
} while (0)
#else
#define GC_ASSERT(cond, msg) ((void)0)
#endif


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
	int local_incr_0depth_cfa, local_decr_0depth_cfa, inc_cfa, dec_cfa;
	int local_finc_0depth_cfa, local_fdec_0depth_cfa, finc_cfa, fdec_cfa;
	int qzbranch_cfa;
	int eq_cfa, lt_cfa, gt_cfa, zeq_cfa;
	int at_i_cfa;
	int eq_zbranch_cfa, lt_zbranch_cfa, gt_zbranch_cfa, zeq_zbranch_cfa;
	int false_symbol, true_symbol;
	int wildcard_symbol, descendant_symbol, self_symbol;

	int init_here, init_latest_cfa, init_names_here;
	int init_source_here, init_symbol_pool_here;
	int lib_end_latest_cfa;
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
	int run_floor;
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
		void *re;
		int in_use;
	} regex_cache[REGEX_CACHE_SIZE];
	int regex_cache_next;

	void *databases[MAX_DATABASES];
	int n_databases;

	int unwinding, unwind_target, next_mark_id;
	int call_depth;

	char error_message[256];
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
	int compiling_src_start;
	
	int fuse_prev_var, fuse_prev2_var;
	int fuse_prev_cmp;
	
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


typedef void (*cfa_handler)(Interpreter *interp);

#define DISPATCH(interp) do { \
	if ((interp)->unwinding || (interp)->error_flag || (interp)->gc_pending) \
		return; \
	__attribute__((musttail)) \
	return ((cfa_handler)vocab.dict[(interp)->ip++])(interp); \
} while (0)

typedef double (*scalar_operator)(double, double);

#define WORD_LINK(cfa) (vocab.dict[(cfa) - 4])
#define WORD_FLAGS(cfa) (vocab.dict[(cfa) - 3])
#define WORD_NAME(cfa) (vocab.dict[(cfa) - 2])
#define WORD_SOURCE(cfa) (vocab.dict[(cfa) - 1])
#define WORD_IS_IMMEDIATE(cfa) (WORD_FLAGS(cfa) & 1)
#define WORD_IS_INLINE(cfa) (WORD_FLAGS(cfa) & 2)
#define WORD_IS_INTERNAL(cfa) (WORD_FLAGS(cfa) & 4)

extern int print_truncate;
void fail(Interpreter *interp, const char *fmt, ...);

void p_sum(Interpreter *interp);
void p_variance(Interpreter *interp);
void p_quantile(Interpreter *interp);
void p_max(Interpreter *interp);
void p_min(Interpreter *interp);
void p_argmax(Interpreter *interp);
void p_argmin(Interpreter *interp);
void p_row_sums(Interpreter *interp);
void p_row_maxes(Interpreter *interp);
void p_row_mins(Interpreter *interp);
void p_column_sums(Interpreter *interp);
void p_column_maxes(Interpreter *interp);
void p_column_mins(Interpreter *interp);

void define_superwords(Interpreter *interp);
int superword_cell_count(cell handler);
int superword_try_fuse(Interpreter *interp, int op_cfa);
int superword_try_fuse_store(Interpreter *interp, int dst_cfa);
int try_fuse_local_acc(Interpreter *interp, int depth, int slot);
int try_fuse_at_i_local(Interpreter *interp);
int try_fuse_at_i_lit(Interpreter *interp);
int try_fuse_local_arith(Interpreter *interp, cfa_handler op_handler);

static inline void push(Interpreter *interp, Val value) {
	if (interp->dsp < DATA_STACK_DEPTH) {
		interp->data_stack[interp->dsp++] = value;
	} else {
		fail(interp, "data stack overflow");
	}
}

static inline Val pop(Interpreter *interp) {
	if (interp->dsp > 0) {
		return interp->data_stack[--interp->dsp];
	}
	fail(interp, "data stack underflow");
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

#define POP(name) Val name = pop(interp); if (interp->error_flag) return

#define POP_TYPED(name, op, type) \
	POP(name##_val); \
	if (VAL_TAG(name##_val) != (type)) { \
		fail(interp, "%s: expected %s; got %s", (op), tag_name(type), tag_name(VAL_TAG(name##_val))); \
		return; \
	}

#define POP_INT(name, op, what) \
	POP(name##_val); \
	if (VAL_TAG(name##_val) != T_FLOAT) { \
		fail(interp, "%s: expected a float %s; got %s", (op), (what), tag_name(VAL_TAG(name##_val))); \
		return; \
	} \
	int name = (int)VAL_NUMBER(name##_val)

#define POP_FLOAT(name, op, what) \
	POP(name##_val); \
	if (VAL_TAG(name##_val) != T_FLOAT) { \
		fail(interp, "%s: expected a float %s; got %s", (op), (what), tag_name(VAL_TAG(name##_val))); \
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
#define POP_CONT(name, op)    POP_TYPED(name, op, T_CONT);    Object *name = OBJECT_AT(VAL_DATA(name##_val))
#define POP_SYMBOL(name, op)  POP_TYPED(name, op, T_SYMBOL);  cell name = VAL_DATA(name##_val)
#define POP_PTR(name, op)     POP_TYPED(name, op, T_PTR);     int name = (int)VAL_DATA(name##_val)

#define POP_COLLECTION(name, op) \
	POP(name##_val); \
	if (VAL_TAG(name##_val) != T_ARRAY && VAL_TAG(name##_val) != T_SET) { \
		fail(interp, "%s: expected an array or set; got %s", (op), tag_name(VAL_TAG(name##_val))); \
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
		fail(interp, "%s: stack too shallow", (op)); \
		return; \
	} \
	Val var = interp->data_stack[interp->dsp - 1 - (depth)]

#define PEEK_TYPE_AT(var, depth, op, type) \
	PEEK_AT(var, depth, op); \
	if (VAL_TAG(var) != (type)) { \
		fail(interp, "%s: expected %s; got %s", (op), tag_name(type), tag_name(VAL_TAG(var))); \
		return; \
	}

#define PEEK_STRING_AT(name, depth, op) \
	PEEK_TYPE_AT(name##_val, depth, op, T_STRING); \
	Object *name = OBJECT_AT(VAL_DATA(name##_val))

#define PEEK_SEQUENCE_AT(var, depth, op) \
	PEEK_AT(var, depth, op); \
	if (VAL_TAG(var) != T_ARRAY && VAL_TAG(var) != T_SET) { \
		fail(interp, "%s: expected array or set; got %s", (op), tag_name(VAL_TAG(var))); \
		return; \
	}


#define PEEK_COLLECTION_AT(var, depth, op) \
	PEEK_AT(var, depth, op); \
	if (VAL_TAG(var) != T_ARRAY && VAL_TAG(var) != T_SET && VAL_TAG(var) != T_FRAME) { \
		fail(interp, "%s: expected array, set, or frame; got %s", (op), tag_name(VAL_TAG(var))); \
		return; \
	}


int create_variable(Interpreter *interp, const char *name);
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
int object_alloc_slot(Interpreter *interp);
void *arena_malloc(size_t bytes);
void *arena_realloc(void *payload, size_t bytes);
void arena_free(void *payload);
int object_new_string(Interpreter *interp, const char *bytes, int length);
int object_new_string_uninit(Interpreter *interp, int length);
int utf8_codepoint_count(const char *bytes, int length);
int utf8_encode(int codepoint, char *out);
int object_new_set(Interpreter *interp);
int build_set_from_values(Interpreter *interp, const Val *values, int count);
int object_new_array(Interpreter *interp, int num_elements);
int object_new_frame(Interpreter *interp);
int object_new_matrix(Interpreter *interp, int num_rows, int num_columns);
int object_new_segment(Interpreter *interp, int length, SegmentType element_type);
int object_new_logic_var(Interpreter *interp);
int object_new_pair(Interpreter *interp);
int object_new_continuation(Interpreter *interp, const Val *frames, int return_len, int resume_ip);
int val_cmp(Interpreter *interp, Val left, Val right);
int double_cmp(const void *left, const void *right);
void set_add(Interpreter *interp, int set_handle, Val value);
void set_remove(Interpreter *interp, int set_handle, Val value);
int set_member(Interpreter *interp, int set_handle, Val value);
int set_union(Interpreter *interp, int handle_a, int handle_b);
int set_intersect(Interpreter *interp, int handle_a, int handle_b);
int set_difference(Interpreter *interp, int handle_a, int handle_b);
void print_double(double number);
void print_items(Interpreter *interp, Object *collection);
void print_corners(Object *matrix);
void print_matrix_cell(double value);
void print_matrix_grid(Object *m);
void print_val(Interpreter *interp, Val value);
void print_val_inspect(Interpreter *interp, Val value);
void pretty_print_array(Interpreter *interp, Val value);
void print_val_compact(Interpreter *interp, Val value);
void print_frame_pretty(Interpreter *interp, Object *frame, int indent);
void print_prompt_state(Interpreter *interp);
int find(const char *name);
const char *name_of(int cfa);
void docol(Interpreter *interp);
void dosym(Interpreter *interp);
void dovar(Interpreter *interp);
void run_inner(Interpreter *interp, int floor);
void execute_cfa(Interpreter *interp, int cfa);

typedef struct {
	int saved_ip;
	int saved_running;
	cell saved_slot_0, saved_slot_1, saved_slot_2;
	int fast;
} CallContext;

void call_open(Interpreter *interp, int cfa, CallContext *ctx);
void call_invoke(Interpreter *interp);
void call_close(Interpreter *interp, CallContext *ctx);

typedef struct {
	const char *name;
	const char *effect;
	const char *summary;
	const char *ops;
	const char *alloc;
	const char *order;
} HelpEntry;

extern const HelpEntry help_entries[];
extern const int help_entry_count;

static inline void call_step(Interpreter *interp, CallContext *ctx, int cfa) {
	if (ctx->fast)
		call_invoke(interp);
	else
		execute_cfa(interp, cfa);
}

int alloc_name(Interpreter *interp, const char *name);
int intern_symbol(Interpreter *interp, const char *name);
void dict_ensure(Interpreter *interp, int extra);
int create_header(Interpreter *interp, const char *name, int flags);
int define_primitive(Interpreter *interp, const char *name, cfa_handler handler, int flags);
void emit(Interpreter *interp, cell value);
void emit_call(Interpreter *interp, int target_cfa);
void emit_val_literal(Interpreter *interp, Val value);
const char *tag_name(Tag t);
void p_exit(Interpreter *interp);
void p_stop(Interpreter *interp);
void p_literal(Interpreter *interp);
void p_branch(Interpreter *interp);
void p_0branch(Interpreter *interp);
void p_qzbranch(Interpreter *interp);
void p_eq_zbranch(Interpreter *interp);
void p_lt_zbranch(Interpreter *interp);
void p_gt_zbranch(Interpreter *interp);
void p_zeq_zbranch(Interpreter *interp);
void p_dostr(Interpreter *interp);
void p_enter_locals(Interpreter *interp);
void p_enter_locals_to(Interpreter *interp);
void p_enter_locals_mixed(Interpreter *interp);
void p_leave_locals(Interpreter *interp);
void p_local_fetch(Interpreter *interp);
void p_local_store(Interpreter *interp);
void p_local_fetch_0depth(Interpreter *interp);
void p_local_store_0depth(Interpreter *interp);
void p_local_incr_0depth(Interpreter *interp);
void p_local_decr_0depth(Interpreter *interp);
void p_local_finc_0depth(Interpreter *interp);
void p_local_fdec_0depth(Interpreter *interp);
void p_increment(Interpreter *interp);
void p_decrement(Interpreter *interp);
void p_f_increment(Interpreter *interp);
void p_f_decrement(Interpreter *interp);
void p_inline(Interpreter *interp);
void inline_word_body(Interpreter *interp, int target_cfa);
int find_local(const char *token, int *depth_out, int *slot_out);
int string_concat(Interpreter *interp, int left_handle, int right_handle);
int string_matches(Interpreter *interp, Object *subject, Object *pattern);
void p_match(Interpreter *interp);
void p_match_all(Interpreter *interp);
void p_split(Interpreter *interp);
void p_replace(Interpreter *interp);
void p_substring(Interpreter *interp);
void p_byte_substring(Interpreter *interp);
void p_char_at(Interpreter *interp);
void p_codepoint_at(Interpreter *interp);
void p_string_to_chars(Interpreter *interp);
void p_string_to_codepoints(Interpreter *interp);
void p_codepoint_to_char(Interpreter *interp);
void p_codepoints_to_string(Interpreter *interp);
void p_trim(Interpreter *interp);
void p_join(Interpreter *interp);
int matrix_add(Interpreter *interp, Val left_val, Val right_val);
int matrix_sub(Interpreter *interp, Val left_val, Val right_val);
int matrix_mul(Interpreter *interp, Val left_val, Val right_val);
int matrix_div(Interpreter *interp, Val left_val, Val right_val);
void p_add(Interpreter *interp);
void p_sub(Interpreter *interp);
void p_mul(Interpreter *interp);
void p_div(Interpreter *interp);
void p_add_inplace(Interpreter *interp);
void p_sub_inplace(Interpreter *interp);
void p_mul_inplace(Interpreter *interp);
void p_div_inplace(Interpreter *interp);
void p_add_f(Interpreter *interp);
void p_sub_f(Interpreter *interp);
void p_mul_f(Interpreter *interp);
void p_eq_f(Interpreter *interp);
void p_lt_f(Interpreter *interp);
void p_gt_f(Interpreter *interp);
void p_bit_and(Interpreter *interp);
void p_bit_or(Interpreter *interp);
void p_bit_xor(Interpreter *interp);
void p_lshift(Interpreter *interp);
void p_rshift(Interpreter *interp);
void p_bit_not(Interpreter *interp);
void p_lowest_bit(Interpreter *interp);
void p_div_f(Interpreter *interp);
void p_neg(Interpreter *interp);
void p_inc(Interpreter *interp);
void p_dec(Interpreter *interp);
void p_sq(Interpreter *interp);
void p_eq(Interpreter *interp);
void p_lt(Interpreter *interp);
void p_gt(Interpreter *interp);
void p_zeq(Interpreter *interp);
void p_and(Interpreter *interp);
void p_or(Interpreter *interp);
void p_not(Interpreter *interp);
void p_null(Interpreter *interp);
void p_symbol_q(Interpreter *interp);
void p_lvar(Interpreter *interp);
void p_wildcard(Interpreter *interp);
void p_unify(Interpreter *interp);
void p_matches(Interpreter *interp);
void p_deref(Interpreter *interp);
Val deref(Interpreter *interp, Val value);
void p_amb(Interpreter *interp);
void p_dup(Interpreter *interp);
void p_drop(Interpreter *interp);
void p_swap(Interpreter *interp);
void p_over(Interpreter *interp);
void p_rot(Interpreter *interp);
void p_depth(Interpreter *interp);
void p_roll(Interpreter *interp);
void p_dot(Interpreter *interp);
void p_dot_all(Interpreter *interp);
void p_cr(Interpreter *interp);
void p_emit_(Interpreter *interp);
void p_dots(Interpreter *interp);
void p_bye(Interpreter *interp);
void p_tor(Interpreter *interp);
void p_rfrom(Interpreter *interp);
void p_rfetch(Interpreter *interp);
void p_to_side(Interpreter *interp);
void p_side_to(Interpreter *interp);
void p_side_drop(Interpreter *interp);
void p_side_depth(Interpreter *interp);
void p_frame_get(Interpreter *interp);
void p_frame_set(Interpreter *interp);
void p_frame_delete_at(Interpreter *interp);
void p_has(Interpreter *interp);
void p_update_at(Interpreter *interp);
void p_merge(Interpreter *interp);
void p_copy(Interpreter *interp);
void p_reify(Interpreter *interp);
void p_frame_keys(Interpreter *interp);
void p_frame_values(Interpreter *interp);
void p_frame(Interpreter *interp);
void p_setopen(Interpreter *interp);
void p_setclose(Interpreter *interp);
void p_frameopen(Interpreter *interp);
void p_frameclose(Interpreter *interp);
void p_array_open(Interpreter *interp);
void p_list_open(Interpreter *interp);
void p_array_close(Interpreter *interp);
void p_list_close(Interpreter *interp);
void p_cons(Interpreter *interp);
void p_head_tail(Interpreter *interp);
void p_array_to_cons(Interpreter *interp);
void p_cons_to_array(Interpreter *interp);
void p_array_to_set(Interpreter *interp);
void p_group_by(Interpreter *interp);
void p_array(Interpreter *interp);
void p_size(Interpreter *interp);
void p_byte_size(Interpreter *interp);
void p_member(Interpreter *interp);
void p_at_i(Interpreter *interp);
void p_at_i_local0(Interpreter *interp);
void p_at_i_lit(Interpreter *interp);
void p_at_i_lit_local0(Interpreter *interp);
void p_store_i(Interpreter *interp);
void p_at_j(Interpreter *interp);
void p_at_ij(Interpreter *interp);
int dgemm_kernel(Interpreter *interp, int transpose_a, int transpose_b,
		double alpha,
		int a_handle, int b_handle,
		double beta, int c_handle);
void p_dgemm_helper(Interpreter *interp, int transpose_a, int transpose_b);
void p_dgemm_nn(Interpreter *interp);
void p_dgemm_tn(Interpreter *interp);
void p_dgemm_nt(Interpreter *interp);
void p_dgemm_tt(Interpreter *interp);
double matrix_sum_overall(Object *source);
double matrix_variance_overall(Object *source);
double matrix_max_overall(Object *source);
double matrix_min_overall(Object *source);
int matrix_sum_rows(Interpreter *interp, Object *source);
int matrix_max_rows(Interpreter *interp, Object *source);
int matrix_min_rows(Interpreter *interp, Object *source);
int matrix_sum_columns(Interpreter *interp, Object *source);
int matrix_max_columns(Interpreter *interp, Object *source);
int matrix_min_columns(Interpreter *interp, Object *source);
void p_set(Interpreter *interp);
void p_union(Interpreter *interp);
void p_intersect(Interpreter *interp);
void p_difference(Interpreter *interp);
void p_set_add(Interpreter *interp);
void p_set_remove(Interpreter *interp);
void p_execute(Interpreter *interp);
void p_execute_catching(Interpreter *interp);
int push_prompt(Interpreter *interp, int kind);
void p_reset(Interpreter *interp);
void p_fail(Interpreter *interp);
int capture_continuation(Interpreter *interp, int what_kind, int *out_mark_index);
void backtrack(Interpreter *interp);
void trail_undo_to(Interpreter *interp, int mark);
void p_shift(Interpreter *interp);
void p_shift_with(Interpreter *interp);
void p_resume(Interpreter *interp);
void p_map(Interpreter *interp);
void parallel_for(int n_items, int n_threads, int items_per_claim,
		void (*kernel)(int start_index, int end_index, void *context), void *context);
void p_mapn(Interpreter *interp);
void p_filter(Interpreter *interp);
void p_pmap(Interpreter *interp);
void p_pfilter(Interpreter *interp);
void p_pmap_reduce(Interpreter *interp);
void abort_parallel_region(size_t saved_used, int saved_n_objects, int saved_n_pairs);
void reset_thread_alloc(void);
void p_num_cores(Interpreter *interp);
void p_reduce(Interpreter *interp);
void p_times(Interpreter *interp);
void p_i_times(Interpreter *interp);
void p_words(Interpreter *interp);
void p_see(Interpreter *interp);
void p_man(Interpreter *interp);
void p_semicolon(Interpreter *interp);
void p_if(Interpreter *interp);
void p_qif(Interpreter *interp);
void p_then(Interpreter *interp);
void p_else(Interpreter *interp);
void p_begin(Interpreter *interp);
void p_until(Interpreter *interp);
void p_again(Interpreter *interp);
void p_while(Interpreter *interp);
void p_repeat(Interpreter *interp);
void p_qcolon(Interpreter *interp);
void p_qsemi(Interpreter *interp);
void p_tick(Interpreter *interp);
void p_lookup(Interpreter *interp);
void p_colon(Interpreter *interp);
void p_variable(Interpreter *interp);
void p_constant(Interpreter *interp);
void p_to(Interpreter *interp);
void p_to_var(Interpreter *interp);
void p_bar(Interpreter *interp);
void p_bar_to(Interpreter *interp);
void p_symbol(Interpreter *interp);
void p_string_to_symbol(Interpreter *interp);
void p_forget(Interpreter *interp);
void inbuf_reset(void);
int read_string_literal(void);
char *next_token(void);
int parse_float(const char *text, double *out);
int interpolate(Interpreter *interp, int template_handle);
void run_outer(Interpreter *interp);
void record_loaded_file(Interpreter *interp, const char *filename);
void load_file(Interpreter *interp, const char *filename);
void p_load(Interpreter *interp);
void p_reload(Interpreter *interp);
void mark_value(Interpreter *interp, Val value);
void mark_body(Interpreter *interp, int body_start, int body_end);
void gc(Interpreter *interp);
void worker_local_gc(Interpreter *interp);
void p_gc(Interpreter *interp);
void p_clear(Interpreter *interp);
void p_save(Interpreter *interp);
void w_u8 (FILE *f, uint8_t v);
void w_i32(FILE *f, int32_t v);
void w_i64(FILE *f, int64_t v);
void w_val(FILE *f, Val value);
int r_u8 (FILE *f, uint8_t *v);
int r_u32(FILE *f, uint32_t *v);
int r_i32(FILE *f, int32_t *v);
int r_i64(FILE *f, int64_t *v);
int r_val(FILE *f, Val *v);
void p_save_image(Interpreter *interp);
void free_one_object(Object *obj);
void forget_user(Interpreter *interp);
void p_load_image(Interpreter *interp);
int create_matrix(Interpreter *interp);
void p_0_matrix(Interpreter *interp);
void p_diagonal_matrix(Interpreter *interp);
void p_diagonal(Interpreter *interp);
void p_reshape(Interpreter *interp);
void p_matrix_range(Interpreter *interp);
void p_matrix(Interpreter *interp);
void p_dim(Interpreter *interp);
void p_array_of(Interpreter *interp);
void p_int_segment(Interpreter *interp);
void p_double_segment(Interpreter *interp);
void p_take(Interpreter *interp);
void p_reverse(Interpreter *interp);
void p_reverse_slice(Interpreter *interp);
void p_concat(Interpreter *interp);
void p_flatten_array(Interpreter *interp);
void p_sort(Interpreter *interp);
void p_sample(Interpreter *interp);
void p_destruct(Interpreter *interp);
void p_destruct_to(Interpreter *interp);
void p_slice_store(Interpreter *interp);
void p_to_slice(Interpreter *interp);
void p_range(Interpreter *interp);
void frame_reserve(Object *frame, int needed);
void frame_put(Object *frame, cell key, Val value);
int frame_delete(Object *frame, cell key);
void p_array_to_frame(Interpreter *interp);
void p_frame_to_array(Interpreter *interp);
void p_select_values(Interpreter *interp);
void p_select_keys(Interpreter *interp);
void p_json_to_frame(Interpreter *interp);
void p_frame_to_json(Interpreter *interp);
void p_transpose(Interpreter *interp);
void p_submatrix(Interpreter *interp);
void p_select_rows(Interpreter *interp);
void p_augment(Interpreter *interp);
void unary_op(Interpreter *interp, Val operand, double (*function)(double), const char *name);
void binary_op(Interpreter *interp, Val left, Val right, scalar_operator function, const char *name);
void p_abs(Interpreter *interp);
void p_sqrt(Interpreter *interp);
void p_exp(Interpreter *interp);
void p_log(Interpreter *interp);
void p_ln(Interpreter *interp);
void p_power(Interpreter *interp);
void p_divmod(Interpreter *interp);
void p_fabs(Interpreter *interp);
void p_fsqrt(Interpreter *interp);
void p_fexp(Interpreter *interp);
void p_flog(Interpreter *interp);
void p_fln(Interpreter *interp);
void p_fsin(Interpreter *interp);
void p_fcos(Interpreter *interp);
void p_ftan(Interpreter *interp);
void p_ftanh(Interpreter *interp);
void p_fasin(Interpreter *interp);
void p_facos(Interpreter *interp);
void p_fatan(Interpreter *interp);
void p_fround(Interpreter *interp);
void p_ftruncate(Interpreter *interp);
void p_fnegate(Interpreter *interp);
void p_fround_up(Interpreter *interp);
void p_fround_down(Interpreter *interp);
void p_fpow(Interpreter *interp);
void p_fmodop(Interpreter *interp);
void p_sin(Interpreter *interp);
void p_cos(Interpreter *interp);
void p_tan(Interpreter *interp);
void p_tanh(Interpreter *interp);
void p_asin(Interpreter *interp);
void p_acos(Interpreter *interp);
void p_atan(Interpreter *interp);
void p_round(Interpreter *interp);
void p_truncate(Interpreter *interp);
void p_round_up(Interpreter *interp);
void p_round_down(Interpreter *interp);
void p_inc_poly(Interpreter *interp);
void p_dec_poly(Interpreter *interp);
void p_sq_poly(Interpreter *interp);
void p_seed(Interpreter *interp);
void p_random(Interpreter *interp);
void p_random_int(Interpreter *interp);
int random_below(int bound);
void p_now(Interpreter *interp);
void p_sleep(Interpreter *interp);
void p_env(Interpreter *interp);
void p_env_set(Interpreter *interp);
void p_cd(Interpreter *interp);
void p_cwd(Interpreter *interp);
void p_read_file(Interpreter *interp);
void p_write_file(Interpreter *interp);
void p_append_file(Interpreter *interp);
void p_read_tsv(Interpreter *interp);
void p_write_tsv(Interpreter *interp);
void p_format(Interpreter *interp);
void p_start_process(Interpreter *interp);
void p_write(Interpreter *interp);
void p_read(Interpreter *interp);
void p_close(Interpreter *interp);
void p_db_open(Interpreter *interp);
void p_db_close(Interpreter *interp);
void p_ffi_open(Interpreter *interp);
void p_ffi_function(Interpreter *interp);
void p_ffi_variadic(Interpreter *interp);
void p_ffi_call(Interpreter *interp);
void p_ffi_free(Interpreter *interp);
void p_matrix_to_pointer(Interpreter *interp);
void p_segment_to_pointer(Interpreter *interp);
int ffi_register_call_cfa(int cfa);
void p_db_exec(Interpreter *interp);
void p_db_query(Interpreter *interp);
void p_wait(Interpreter *interp);
void p_stop_process(Interpreter *interp);
void p_running(Interpreter *interp);
void interp_init(Interpreter *interp);
Interpreter *main_init(void);
Interpreter *worker_init(int worker_index);
int construct_vocabulary(Interpreter *interp, int load_lib);

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

static inline __attribute__((always_inline))
Val frame_walk(Interpreter *interp, Val node, Object *path,
		int count, FrameWalkMode mode, int *found, const char *op) {
	for (int i = 0; i < count; i++) {
		if (VAL_TAG(node) != T_FRAME) {
			if (found) *found = 0;
			if (mode != WALK_PROBE)
				fail(interp, "%s: cannot descend into %s", op, tag_name(VAL_TAG(node)));
			return node;
		}

		cell key = VAL_DATA(path->items[i]);
		Object *frame = OBJECT_AT(VAL_DATA(node));
		FRAME_LOOKUP(frame, key, at, present);
		if (present && (mode != WALK_VIVIFY || VAL_TAG(frame->frame.values[at]) == T_FRAME)) {
			node = frame->frame.values[at];
		} else if (mode == WALK_VIVIFY) {
			if (key == (cell)vocab.wildcard_symbol || key == (cell)vocab.descendant_symbol) {
				fail(interp, "%s: path has a wildcard or descendant; use select-keys/select-values", op);
				return node;
			}
			int child = object_new_frame(interp);
			frame_put(OBJECT_AT(VAL_DATA(node)), key, make_frame(child));
			node = make_frame(child);
		} else {
			if (found) *found = 0;
			if (mode != WALK_PROBE) {
				if (key == (cell)vocab.wildcard_symbol || key == (cell)vocab.descendant_symbol)
					fail(interp, "%s: path has a wildcard or descendant; use select-keys/select-values", op);
				else
					fail(interp, "%s: no key :%s", op, &vocab.symbol_pool[key]);
			}
			return node;
		}
	}
	if (found) *found = 1;
	return node;
}

#endif

 
