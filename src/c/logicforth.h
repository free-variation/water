#ifndef LOGICFORTH_H
#define LOGICFORTH_H

#define VERSION "0.1.0"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <limits.h>

typedef int64_t cell;

#define VOCABULARY_INIT_SIZE    1048576
#define NAME_POOL   			32768
#define DATA_STACK_DEPTH		256
#define RETURN_STACK_DEPTH  	256
#define MAX_OBJECTS    			65536
#define INPUT_BUFFER_SIZE    			16384
#define SOURCE_POOL 			1048576
#define SYMBOL_POOL 			32768
#define SIDESTACK_DEPTH 		256
#define MAX_LOADED_FILES 		64
#define MAX_GC_ROOTS 			16

#define TRAMPOLINE_SLOT			0
#define DICT_RESERVED			2

typedef enum {
	T_NONE = 0,
	T_SYM,
	T_FLOAT,
	T_STRING,
	T_SET,
	T_ARRAY,
	T_MATRIX,
	T_XT,
	T_ADDR,
	T_CONT,
	T_MARK
} Tag;

typedef struct {
	Tag tag;
	int64_t data;
} Val;

static inline double unpack_float(Val packed) {
	double number;
	memcpy(&number, &packed.data, 8);
	return number;
}

static inline Val make_float(double number) {
	Val value;
	value.tag = T_FLOAT;
	memcpy(&value.data, &number, 8);
	return value;
}

static inline Val make_symbol(int cfa) {
	Val value;
	value.tag = T_SYM;
	value.data = cfa;
	return value;
}

static inline Val make_string(int handle) {
	Val value;
	value.tag = T_STRING;
	value.data = handle;
	return value;
}

static inline Val make_set(int handle) {
	Val value;
	value.tag = T_SET;
	value.data = handle;
	return value;
}

static inline Val make_array(int handle) {
	Val value;
	value.tag = T_ARRAY;
	value.data = handle;
	return value;
}

static inline Val make_matrix(int handle) {
	Val value;
	value.tag = T_MATRIX;
	value.data = handle;
	return value;
}

static inline Val make_xt(int cfa) {
	Val value;
	value.tag = T_XT;
	value.data = cfa;
	return value;
}

static inline Val make_addr(int cell_index) {
	Val value;
	value.tag = T_ADDR;
	value.data = cell_index;
	return value;
}

static inline Val make_continuation(int handle) {
	Val value;
	value.tag = T_CONT;
	value.data = handle;
	return value;
}

static inline Val make_mark(void) {
	Val value;
	value.tag = T_MARK;
	value.data = 0;
	return value;
}

typedef enum {
	OBJECT_STRING = 0,
	OBJECT_SET,
	OBJECT_ARRAY,
	OBJECT_MATRIX,
	OBJECT_CONTINUATION
} ObjectKind;

typedef struct {
	ObjectKind kind;
	int len, cap;
	union {
		char *bytes;
		Val  *items;
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

#define MAT(m, i, j) ((m)->matrix.elements[(i) * (m)->matrix.columns + (j)])
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

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

	int exit_cfa, literal_cfa, branch_cfa, zbranch_cfa, dostr_cfa, stop_cfa, to_var_cfa;

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

	int ip;
	int running;
	int compiling;
	int error_flag;

	char input_buffer[INPUT_BUFFER_SIZE];
	int input_buffer_len, input_buffer_pos, need_more;
	int compiling_src_start;

	Object *objects[MAX_OBJECTS];
	int n_objects;
	int object_mark[MAX_OBJECTS];
	Val gc_roots[MAX_GC_ROOTS];
	int n_gc_roots;

	char *loaded_files[MAX_LOADED_FILES];
	int n_loaded_files, load_depth;

	int unwinding, unwind_target, next_mark_id;

	char error_message[256];
	char token_buffer[INPUT_BUFFER_SIZE];
} Interpreter;

typedef void (*cfa_handler)(Interpreter *interp, cell *cfa);
typedef double (*scalar_operator)(double, double);
typedef double (*reducer)(double accumulator, double element);

#define WORD_LINK(v, cfa) ((v)->dict[(cfa) - 4])
#define WORD_FLAGS(v, cfa) ((v)->dict[(cfa) - 3])
#define WORD_NAME(v, cfa) ((v)->dict[(cfa) - 2])
#define WORD_SOURCE(v, cfa) ((v)->dict[(cfa) - 1])
#define WORD_IS_IMMEDIATE(v, cfa) (WORD_FLAGS(v, cfa) & 1)

extern int print_truncate;
void fail(Interpreter *interp, const char *fmt, ...);

void p_sum(Interpreter *interp, cell *cfa);
void p_max(Interpreter *interp, cell *cfa);
void p_min(Interpreter *interp, cell *cfa);
void p_row_sums(Interpreter *interp, cell *cfa);
void p_row_maxes(Interpreter *interp, cell *cfa);
void p_row_mins(Interpreter *interp, cell *cfa);
void p_column_sums(Interpreter *interp, cell *cfa);
void p_column_maxes(Interpreter *interp, cell *cfa);
void p_column_mins(Interpreter *interp, cell *cfa);

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
	Val none = { T_NONE, 0 };
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
	Val none = { T_NONE, 0 };
	return none;
}

#define POP(name) Val name = pop(interp); if (interp->error_flag) return

#define POP_INT(name, op, what) \
	Val name##_val = pop(interp); \
	if (interp->error_flag) return; \
	if (name##_val.tag != T_FLOAT) { \
		fail(interp, "%s: expected a float %s, got %s", (op), (what), tag_name(name##_val.tag)); \
		return; \
	} \
	int name = (int)unpack_float(name##_val)

#define POP_XT(name, op) \
	Val name##_val = pop(interp); \
	if (interp->error_flag) return; \
	if (name##_val.tag != T_XT) { \
		fail(interp, "%s: expected an execution token, got %s", (op), tag_name(name##_val.tag)); \
		return; \
	} \
	int name = (int)name##_val.data

#define POP_MATRIX(name, op) \
	Val name##_val = pop(interp); \
	if (interp->error_flag) return; \
	if (name##_val.tag != T_MATRIX) { \
		fail(interp, "%s: expected a matrix, got %s", (op), tag_name(name##_val.tag)); \
		return; \
	} \
	Object *name = interp->objects[name##_val.data]

#define POP_STRING(name, op) \
	Val name##_val = pop(interp); \
	if (interp->error_flag) return; \
	if (name##_val.tag != T_STRING) { \
		fail(interp, "%s: expected a string, got %s", (op), tag_name(name##_val.tag)); \
		return; \
	} \
	Object *name = interp->objects[name##_val.data]

#define NEW_MATRIX(handle, obj, rows, cols) \
	int handle = object_new_matrix(interp, (rows), (cols)); \
	if (interp->error_flag) return; \
	Object *obj = interp->objects[handle]

#define NEW_ARRAY(handle, obj, len) \
	int handle = object_new_array(interp, (len)); \
	if (interp->error_flag) return; \
	Object *obj = interp->objects[handle]

#define PEEK_COLLECTION_AT(name, depth, op) \
	if (interp->dsp <= (depth)) { \
		fail(interp, "%s: stack too shallow; expected array or set", (op)); \
		return; \
	} \
	Val name##_val = interp->data_stack[interp->dsp - 1 - (depth)]; \
	if (name##_val.tag != T_ARRAY && name##_val.tag != T_SET) { \
		fail(interp, "%s: expected array or set, got %s", (op), tag_name(name##_val.tag)); \
		return; \
	} \
	Object *name = interp->objects[name##_val.data]



/* ---- internal cross-file prototypes ---- */
void gc_root_push(Interpreter *interp, Val value);
void gc_root_pop(Interpreter *interp);
int object_alloc_slot(Interpreter *interp);
int object_new_string(Interpreter *interp, const char *bytes, int length);
int object_new_set(Interpreter *interp);
int object_new_array(Interpreter *interp, int num_elements);
int object_new_matrix(Interpreter *interp, int num_rows, int num_columns);
int object_new_continuation(Interpreter *interp, const Val *frames, int return_len, int resume_ip);
int val_cmp(Interpreter *interp, Val left, Val right);
void set_add(Interpreter *interp, int set_handle, Val value);
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
void print_val_compact(Interpreter *interp, Val value);
void print_prompt_state(Interpreter *interp);
int find(Interpreter *interp, const char *name);
void docol(Interpreter *interp, cell *cfa);
void dosym(Interpreter *interp, cell *cfa);
void dovar(Interpreter *interp, cell *cfa);
void run_inner(Interpreter *interp);
void execute_cfa(Interpreter *interp, int cfa);
int alloc_name(Interpreter *interp, const char *name);
int intern_symbol(Interpreter *interp, const char *name);
void dict_ensure(Interpreter *interp, int extra);
int create_header(Interpreter *interp, const char *name, int immediate);
int define_primitive(Interpreter *interp, const char *name, cfa_handler handler, int immediate);
void emit(Interpreter *interp, cell value);
void emit_val_literal(Interpreter *interp, Val value);
const char *tag_name(Tag t);
void p_exit(Interpreter *interp, cell *cfa);
void p_stop(Interpreter *interp, cell *cfa);
void p_literal(Interpreter *interp, cell *cfa);
void p_branch(Interpreter *interp, cell *cfa);
void p_0branch(Interpreter *interp, cell *cfa);
void p_dostr(Interpreter *interp, cell *cfa);
int string_concat(Interpreter *interp, int left_handle, int right_handle);
double scalar_add(double a, double b);
double scalar_subtract(double a, double b);
double scalar_multiply(double a, double b);
double scalar_divide(double a, double b);
int matrix_scalar_op(Interpreter *interp, Val left_val, Val right_val, scalar_operator op);
void p_add(Interpreter *interp, cell *cfa);
void p_sub(Interpreter *interp, cell *cfa);
void p_mul(Interpreter *interp, cell *cfa);
void p_div(Interpreter *interp, cell *cfa);
void p_neg(Interpreter *interp, cell *cfa);
Val make_bool(int is_true);
int truthy(Val value);
void p_eq(Interpreter *interp, cell *cfa);
void p_lt(Interpreter *interp, cell *cfa);
void p_gt(Interpreter *interp, cell *cfa);
void p_zeq(Interpreter *interp, cell *cfa);
void p_dup(Interpreter *interp, cell *cfa);
void p_drop(Interpreter *interp, cell *cfa);
void p_swap(Interpreter *interp, cell *cfa);
void p_over(Interpreter *interp, cell *cfa);
void p_rot(Interpreter *interp, cell *cfa);
void p_depth(Interpreter *interp, cell *cfa);
void p_roll(Interpreter *interp, cell *cfa);
void p_dot(Interpreter *interp, cell *cfa);
void p_dot_all(Interpreter *interp, cell *cfa);
void p_cr(Interpreter *interp, cell *cfa);
void p_emit_(Interpreter *interp, cell *cfa);
void p_dots(Interpreter *interp, cell *cfa);
void p_bye(Interpreter *interp, cell *cfa);
void p_tor(Interpreter *interp, cell *cfa);
void p_rfrom(Interpreter *interp, cell *cfa);
void p_rfetch(Interpreter *interp, cell *cfa);
void p_to_side(Interpreter *interp, cell *cfa);
void p_side_to(Interpreter *interp, cell *cfa);
void p_side_drop(Interpreter *interp, cell *cfa);
void p_side_depth(Interpreter *interp, cell *cfa);
void p_fetch(Interpreter *interp, cell *cfa);
void p_store(Interpreter *interp, cell *cfa);
void p_setopen(Interpreter *interp, cell *cfa);
void p_setclose(Interpreter *interp, cell *cfa);
void p_array_open(Interpreter *interp, cell *cfa);
void p_array_close(Interpreter *interp, cell *cfa);
void p_array(Interpreter *interp, cell *cfa);
void p_cardinality(Interpreter *interp, cell *cfa);
void p_member(Interpreter *interp, cell *cfa);
void p_at_i(Interpreter *interp, cell *cfa);
void p_at_j(Interpreter *interp, cell *cfa);
void p_at_ij(Interpreter *interp, cell *cfa);
int dgemm_kernel(Interpreter *interp, int transpose_a, int transpose_b,
		double alpha,
		int a_handle, int b_handle,
		double beta, int c_handle);
void p_dgemm_helper(Interpreter *interp, int transpose_a, int transpose_b);
void p_dgemm_nn(Interpreter *interp, cell *cfa);
void p_dgemm_tn(Interpreter *interp, cell *cfa);
void p_dgemm_nt(Interpreter *interp, cell *cfa);
void p_dgemm_tt(Interpreter *interp, cell *cfa);
double reduce_add(double accumulator, double element);
double reduce_max(double accumulator, double element);
double reduce_min(double accumulator, double element);
double matrix_reduce_overall(Object *source, reducer fn, double identity);
int matrix_reduce_rows(Interpreter *interp, Object *source, reducer fn, double identity);
int matrix_reduce_columns(Interpreter *interp, Object *source, reducer fn, double identity);
void p_set(Interpreter *interp, cell *cfa);
void p_union(Interpreter *interp, cell *cfa);
void p_intersect(Interpreter *interp, cell *cfa);
void p_difference(Interpreter *interp, cell *cfa);
void p_execute(Interpreter *interp, cell *cfa);
void p_reset(Interpreter *interp, cell *cfa);
int capture_continuation(Interpreter *interp, int *out_mark_index);
void p_shift(Interpreter *interp, cell *cfa);
void p_shift_with(Interpreter *interp, cell *cfa);
void p_resume(Interpreter *interp, cell *cfa);
void p_map(Interpreter *interp, cell *cfa);
void p_mapn(Interpreter *interp, cell *cfa);
void p_filter(Interpreter *interp, cell *cfa);
void p_words(Interpreter *interp, cell *cfa);
void p_see(Interpreter *interp, cell *cfa);
void p_semi(Interpreter *interp, cell *cfa);
void p_if(Interpreter *interp, cell *cfa);
void p_then(Interpreter *interp, cell *cfa);
void p_else(Interpreter *interp, cell *cfa);
void p_begin(Interpreter *interp, cell *cfa);
void p_until(Interpreter *interp, cell *cfa);
void p_again(Interpreter *interp, cell *cfa);
void p_qcolon(Interpreter *interp, cell *cfa);
void p_qsemi(Interpreter *interp, cell *cfa);
void p_tick(Interpreter *interp, cell *cfa);
void p_colon(Interpreter *interp, cell *cfa);
void p_variable(Interpreter *interp, cell *cfa);
void p_to(Interpreter *interp, cell *cfa);
void p_to_var(Interpreter *interp, cell *cfa);
void p_symbol(Interpreter *interp, cell *cfa);
void p_string_to_symbol(Interpreter *interp, cell *cfa);
void p_forget(Interpreter *interp, cell *cfa);
void inbuf_reset(Interpreter *interp);
int read_string_literal(Interpreter *interp);
char *next_token(Interpreter *interp);
int parse_float(const char *text, double *out);
int interpolate(Interpreter *interp, int template_handle);
void run_outer(Interpreter *interp);
void record_loaded_file(Interpreter *interp, const char *filename);
void load_file(Interpreter *interp, const char *filename);
void p_load(Interpreter *interp, cell *cfa);
void p_reload(Interpreter *interp, cell *cfa);
void mark_val(Interpreter *interp, Val value);
void mark_body(Interpreter *interp, int body_start, int body_end);
void gc(Interpreter *interp);
void p_gc(Interpreter *interp, cell *cfa);
void p_clear(Interpreter *interp, cell *cfa);
void p_save(Interpreter *interp, cell *cfa);
void w_u8 (FILE *f, uint8_t v);
void w_i32(FILE *f, int32_t v);
void w_i64(FILE *f, int64_t v);
void w_val(FILE *f, Val v);
int r_u8 (FILE *f, uint8_t *v);
int r_u32(FILE *f, uint32_t *v);
int r_i32(FILE *f, int32_t *v);
int r_i64(FILE *f, int64_t *v);
int r_val(FILE *f, Val *v);
void p_save_image(Interpreter *interp, cell *cfa);
void free_one_object(Object *obj);
void forget_user(Interpreter *interp);
void p_load_image(Interpreter *interp, cell *cfa);
int create_matrix(Interpreter *interp);
void p_0_matrix(Interpreter *interp, cell *cfa);
void p_diagonal_matrix(Interpreter *interp, cell *cfa);
void p_diagonal(Interpreter *interp, cell *cfa);
void p_reshape(Interpreter *interp, cell *cfa);
void p_matrix(Interpreter *interp, cell *cfa);
void p_dim(Interpreter *interp, cell *cfa);
void p_array_of(Interpreter *interp, cell *cfa);
void p_take(Interpreter *interp, cell *cfa);
void p_reverse(Interpreter *interp, cell *cfa);
void p_concat(Interpreter *interp, cell *cfa);
void p_range(Interpreter *interp, cell *cfa);
void p_transpose(Interpreter *interp, cell *cfa);
void unary_op(Interpreter *interp, Val operand, double (*function)(double), const char *name);
void p_abs(Interpreter *interp, cell *cfa);
void p_sqrt(Interpreter *interp, cell *cfa);
void p_exp(Interpreter *interp, cell *cfa);
void p_log(Interpreter *interp, cell *cfa);
void p_sin(Interpreter *interp, cell *cfa);
void p_cos(Interpreter *interp, cell *cfa);
void p_tan(Interpreter *interp, cell *cfa);
void p_tanh(Interpreter *interp, cell *cfa);
Interpreter *interp_new(void);

#endif

 
