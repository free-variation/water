#ifndef LOGICFORTH_H
#define LOGICFORTH_H

#define VERSION "0.4.0"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <limits.h>
#include <regex.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

typedef int64_t cell;

#define VOCABULARY_INIT_SIZE    1048576
#define NAME_POOL   			262144
#define DATA_STACK_DEPTH		65536
#define RETURN_STACK_DEPTH  	65536
#define MAX_OBJECTS    			4194304
#define INPUT_BUFFER_SIZE    	1048576
#define SOURCE_POOL 			4194304
#define SYMBOL_POOL 			4194304
#define SYMBOL_HASH_SIZE 		1048576
#define SIDESTACK_DEPTH 		1024
#define BIND_TRAIL_DEPTH		65536
#define MAX_LOADED_FILES 		64
#define MAX_GC_ROOTS 			64
#define LOCAL_NAMES_POOL_SIZE	8192
#define MAX_LOCAL_NAMES			256
#define MAX_LOCAL_SCOPES		64
#define TRAMPOLINE_SLOT			0
#define DICT_RESERVED			3
#define PROMPT_EXCEPTION		0
#define PROMPT_CHOICE			1
#define LVAR_STACK_DEPTH		65536
#define PAIR_TABLE_DEPTH		1 << 20
#define COPY_SPINE_MAX			(1 << 24)

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
	T_UNBOUND
} Tag;

typedef union {
	uint64_t bits;
	double number;
} Val;

#define NAN_BOX_PREFIX 0x7FF8000000000000ULL
#define NAN_BOX_MASK 0xFFFF000000000000ULL
#define VAL_TAG_SHIFT 44
#define VAL_TAG_MASK 0x000F000000000000ULL
#define VAL_DATA_MASK 0x00000FFFFFFFFFFFULL

#define VAL_IS_FLOAT(v) (((v).bits & NAN_BOX_MASK) != NAN_BOX_PREFIX)
#define VAL_TAG(v) (VAL_IS_FLOAT(v) ? T_FLOAT : (Tag)(((v).bits >> VAL_TAG_SHIFT) & 0xF))
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
	OBJECT_CONTINUATION
} ObjectKind;

typedef struct {
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
		} continuation;
	};
} Object;

typedef struct {
	Val head;
	Val tail;
} Pair;

typedef struct {
	int slot;
	Val value;
} VarMapEntry;

typedef struct {
	int reify;
	VarMapEntry *entries;
	int count, cap;
} VarMap;

#define MAT(m, i, j) ((m)->matrix.elements[(i) * (m)->matrix.columns + (j)])
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define GROW_IF_FULL(count, cap, arr) do { \
	if ((count) == (cap)) { \
		(cap) = (cap) ? (cap) * 2 : 8; \
		(arr) = realloc((arr), sizeof(*(arr)) * (size_t)(cap)); \
	} \
} while (0)

typedef struct Vocabulary {
	cell *dict;
	int dict_cap;
	int here;
	int latest_cfa;
	char name_pool[NAME_POOL];
	int names_here;
	char source_pool[SOURCE_POOL];
	int source_here;
	char symbol_pool[SYMBOL_POOL];
	int symbol_pool_here;
	int symbol_hash[SYMBOL_HASH_SIZE];

	int exit_cfa, literal_cfa, branch_cfa, zbranch_cfa, dostr_cfa, stop_cfa, to_var_cfa;
	int enter_locals_cfa, enter_locals_to_cfa, enter_locals_mixed_cfa, leave_locals_cfa, local_fetch_cfa, local_store_cfa;
	int local_fetch_0depth_cfa, local_store_0depth_cfa;
	int local_incr_0depth_cfa, local_decr_0depth_cfa, inc_cfa, dec_cfa;
	int local_finc_0depth_cfa, local_fdec_0depth_cfa, finc_cfa, fdec_cfa;
	int qzbranch_cfa;
	int false_symbol, true_symbol;

	int init_here, init_latest_cfa, init_names_here;
	int init_source_here, init_symbol_pool_here;
	int lib_end_latest_cfa;
} Vocabulary;

typedef struct Interpreter {
	Vocabulary *vocab;

	Val data_stack[DATA_STACK_DEPTH];
	int dsp;
	Val return_stack[RETURN_STACK_DEPTH];
	int rsp;
	Val side_stack[SIDESTACK_DEPTH];
	int side_dsp;
	int local_base;
	int *bind_trail;
	int bind_trail_top, bind_trail_cap;
	Val *lvar_stack;
	int lvar_top, lvar_cap;

	int ip;
	int running;
	int compiling;
	int error_flag;

	char input_buffer[INPUT_BUFFER_SIZE];
	int input_buffer_len, input_buffer_pos, need_more;
	int compiling_src_start;
	int fuse_prev_var, fuse_prev2_var;

	char local_names_pool[LOCAL_NAMES_POOL_SIZE];
	int local_names_pool_here;
	int local_name_offsets[MAX_LOCAL_NAMES];
	int n_local_names;
	int local_scope_starts[MAX_LOCAL_SCOPES];
	int local_scope_dict_starts[MAX_LOCAL_SCOPES];
	int n_local_scopes;

	Object *objects[MAX_OBJECTS];
	int n_objects;
	unsigned char object_mark[MAX_OBJECTS];
	int free_slots[MAX_OBJECTS];
	int n_free_slots;

	Pair *pairs;
	int n_pairs, pairs_cap;
	unsigned char *pair_mark;
	int *pair_free_list;
	int pair_free_count;
	int gc_disabled;

	Val gc_roots[MAX_GC_ROOTS];
	int n_gc_roots;

	char *loaded_files[MAX_LOADED_FILES];
	int n_loaded_files, load_depth;

	int unwinding, unwind_target, next_mark_id, next_lvar_id;

	char error_message[256];
	char token_buffer[INPUT_BUFFER_SIZE];
} Interpreter;

typedef void (*cfa_handler)(Interpreter *interp);

#define DISPATCH(interp) do { \
	if ((interp)->unwinding || (interp)->error_flag) \
		return; \
	__attribute__((musttail)) \
	return ((cfa_handler)(interp)->vocab->dict[(interp)->ip++])(interp); \
} while (0)

typedef double (*scalar_operator)(double, double);

#define WORD_LINK(v, cfa) ((v)->dict[(cfa) - 4])
#define WORD_FLAGS(v, cfa) ((v)->dict[(cfa) - 3])
#define WORD_NAME(v, cfa) ((v)->dict[(cfa) - 2])
#define WORD_SOURCE(v, cfa) ((v)->dict[(cfa) - 1])
#define WORD_IS_IMMEDIATE(v, cfa) (WORD_FLAGS(v, cfa) & 1)
#define WORD_IS_INLINE(v, cfa) (WORD_FLAGS(v, cfa) & 2)
#define WORD_IS_INTERNAL(v, cfa) (WORD_FLAGS(v, cfa) & 4)

extern int print_truncate;
void fail(Interpreter *interp, const char *fmt, ...);

void p_sum(Interpreter *interp);
void p_max(Interpreter *interp);
void p_min(Interpreter *interp);
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

#define POP_INT(name, op, what) \
	Val name##_val = pop(interp); \
	if (interp->error_flag) return; \
	if (VAL_TAG(name##_val) != T_FLOAT) { \
		fail(interp, "%s: expected a float %s; got %s", (op), (what), tag_name(VAL_TAG(name##_val))); \
		return; \
	} \
	int name = (int)VAL_NUMBER(name##_val)

#define POP_XT(name, op) \
	Val name##_val = pop(interp); \
	if (interp->error_flag) return; \
	if (VAL_TAG(name##_val) != T_XT) { \
		fail(interp, "%s: expected an execution token; got %s", (op), tag_name(VAL_TAG(name##_val))); \
		return; \
	} \
	int name = (int)VAL_DATA(name##_val)

#define POP_MATRIX(name, op) \
	Val name##_val = pop(interp); \
	if (interp->error_flag) return; \
	if (VAL_TAG(name##_val) != T_MATRIX) { \
		fail(interp, "%s: expected a matrix; got %s", (op), tag_name(VAL_TAG(name##_val))); \
		return; \
	} \
	Object *name = interp->objects[VAL_DATA(name##_val)]

#define POP_STRING(name, op) \
	Val name##_val = pop(interp); \
	if (interp->error_flag) return; \
	if (VAL_TAG(name##_val) != T_STRING) { \
		fail(interp, "%s: expected a string; got %s", (op), tag_name(VAL_TAG(name##_val))); \
		return; \
	} \
	Object *name = interp->objects[VAL_DATA(name##_val)]

#define POP_COLLECTION(name, op) \
	Val name##_val = pop(interp); \
	if (interp->error_flag) return; \
	if (VAL_TAG(name##_val) != T_ARRAY && VAL_TAG(name##_val) != T_SET) { \
		fail(interp, "%s: expected an array or set; got %s", (op), tag_name(VAL_TAG(name##_val))); \
		return; \
	} \
	Object *name = interp->objects[VAL_DATA(name##_val)]

#define NEW_MATRIX(handle, obj, rows, cols) \
	int handle = object_new_matrix(interp, (rows), (cols)); \
	if (interp->error_flag) return; \
	Object *obj = interp->objects[handle]

#define NEW_ARRAY(handle, obj, len) \
	int handle = object_new_array(interp, (len)); \
	if (interp->error_flag) return; \
	Object *obj = interp->objects[handle]

#define NEW_FRAME(handle, obj) \
	int handle = object_new_frame(interp); \
	if (interp->error_flag) return; \
	Object *obj = interp->objects[handle]

#define NEW_OBJECT(obj, kind) \
	int slot; \
	Object *obj = object_new(interp, (kind), &slot); \
	if (!obj) return -1

#define INIT_PAIR(slot) \
	interp->pairs[slot].head = make_tagged(T_NONE, 0); \
	interp->pairs[slot].tail = make_tagged(T_NONE, 0)

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
	Object *name = interp->objects[VAL_DATA(name##_val)]

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
int object_new_string(Interpreter *interp, const char *bytes, int length);
int object_new_string_uninit(Interpreter *interp, int length);
int object_new_set(Interpreter *interp);
int object_new_array(Interpreter *interp, int num_elements);
int object_new_frame(Interpreter *interp);
int object_new_matrix(Interpreter *interp, int num_rows, int num_columns);
int object_new_logic_var(Interpreter *interp);
int object_new_pair(Interpreter *interp);
int object_new_continuation(Interpreter *interp, const Val *frames, int return_len, int resume_ip);
int val_cmp(Interpreter *interp, Val left, Val right);
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
int find(Interpreter *interp, const char *name);
const char *name_of(Interpreter *interp, int cfa);
void docol(Interpreter *interp);
void dosym(Interpreter *interp);
void dovar(Interpreter *interp);
void run_inner(Interpreter *interp);
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
int find_local(Interpreter *interp, const char *token, int *depth_out, int *slot_out);
int string_concat(Interpreter *interp, int left_handle, int right_handle);
int string_matches(Interpreter *interp, Object *subject, Object *pattern);
void p_match(Interpreter *interp);
void p_match_all(Interpreter *interp);
void p_split(Interpreter *interp);
void p_replace(Interpreter *interp);
void p_substring(Interpreter *interp);
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
void p_array_close(Interpreter *interp);
void p_list_close(Interpreter *interp);
void p_cons(Interpreter *interp);
void p_head_tail(Interpreter *interp);
void p_array_to_cons(Interpreter *interp);
void p_cons_to_array(Interpreter *interp);
void p_array(Interpreter *interp);
void p_size(Interpreter *interp);
void p_member(Interpreter *interp);
void p_at_i(Interpreter *interp);
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
int push_prompt(Interpreter *interp, int kind);
void p_reset(Interpreter *interp);
void p_fail(Interpreter *interp);
int capture_continuation(Interpreter *interp, int what_kind, int *out_mark_index);
void backtrack(Interpreter *interp);
void p_shift(Interpreter *interp);
void p_shift_with(Interpreter *interp);
void p_resume(Interpreter *interp);
void p_map(Interpreter *interp);
void p_mapn(Interpreter *interp);
void p_filter(Interpreter *interp);
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
void p_to(Interpreter *interp);
void p_to_var(Interpreter *interp);
void p_bar(Interpreter *interp);
void p_bar_to(Interpreter *interp);
void p_symbol(Interpreter *interp);
void p_string_to_symbol(Interpreter *interp);
void p_forget(Interpreter *interp);
void inbuf_reset(Interpreter *interp);
int read_string_literal(Interpreter *interp);
char *next_token(Interpreter *interp);
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
void p_take(Interpreter *interp);
void p_reverse(Interpreter *interp);
void p_reverse_slice(Interpreter *interp);
void p_concat(Interpreter *interp);
void p_destruct(Interpreter *interp);
void p_destruct_to(Interpreter *interp);
void p_slice_store(Interpreter *interp);
void p_to_slice(Interpreter *interp);
void p_range(Interpreter *interp);
void frame_put(Object *frame, cell key, Val value);
int frame_delete(Object *frame, cell key);
void p_to_frame(Interpreter *interp);
void p_json_to_frame(Interpreter *interp);
void p_frame_to_json(Interpreter *interp);
void p_transpose(Interpreter *interp);
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
void p_now(Interpreter *interp);
void p_sleep(Interpreter *interp);
void p_env(Interpreter *interp);
void p_env_set(Interpreter *interp);
void p_read_file(Interpreter *interp);
void p_write_file(Interpreter *interp);
void p_append_file(Interpreter *interp);
void p_format(Interpreter *interp);
void p_start_process(Interpreter *interp);
void p_write(Interpreter *interp);
void p_read(Interpreter *interp);
void p_close(Interpreter *interp);
void p_wait(Interpreter *interp);
void p_stop_process(Interpreter *interp);
void p_running(Interpreter *interp);
Interpreter *interp_new(void);
int interp_bootstrap(Interpreter *interp);

typedef enum { WALK_ERROR, WALK_VIVIFY, WALK_PROBE } FrameWalkMode;

static inline __attribute__((always_inline))
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

int frame_find(Object *frame, cell key) {
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
		Object *frame = interp->objects[VAL_DATA(node)];
		FRAME_LOOKUP(frame, key, at, present);
		if (present && (mode != WALK_VIVIFY || VAL_TAG(frame->frame.values[at]) == T_FRAME)) {
			node = frame->frame.values[at];
		} else if (mode == WALK_VIVIFY) {
			int child = object_new_frame(interp);
			frame_put(interp->objects[VAL_DATA(node)], key, make_frame(child));
			node = make_frame(child);
		} else {
			if (found) *found = 0;
			if (mode != WALK_PROBE)
				fail(interp, "%s: no key :%s", op, &interp->vocab->symbol_pool[key]);
			return node;
		}
	}
	if (found) *found = 1;
	return node;
}

#endif

 
