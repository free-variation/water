#include "logicforth.h"

static void enter_compile_scope(Interpreter *interp);
static void leave_compile_scope(Interpreter *interp);

int string_concat(Interpreter *interp, int left_handle, int right_handle) {
	Object *left = OBJECT_AT(left_handle);
	Object *right = OBJECT_AT(right_handle);
	int combined_length = left->len + right->len;

	char *buffer = malloc((size_t)combined_length + 1);
	memcpy(buffer, left->bytes, (size_t)left->len);
	memcpy(buffer + left->len, right->bytes, (size_t)right->len);
	int result_handle = object_new_string(interp, buffer, combined_length);
	free(buffer);

	return result_handle;
}

#define BROADCAST_SCALAR_OP_MATRIX(op) \
	do { \
		double scalar = VAL_NUMBER(left); \
		Object *matrix_source = OBJECT_AT(VAL_DATA(right)); \
		int target_handle = object_new_matrix(interp, matrix_source->matrix.rows, matrix_source->matrix.columns); \
		if (interp->error_flag) return; \
		Object *target = OBJECT_AT(target_handle); \
		size_t num_elements = (size_t)matrix_source->matrix.rows * (size_t)matrix_source->matrix.columns; \
		const double * restrict source_elements = matrix_source->matrix.elements; \
		double * restrict target_elements = target->matrix.elements; \
		for (size_t i = 0; i < num_elements; i++) \
			target_elements[i] = scalar op source_elements[i]; \
		interp->data_stack[interp->dsp - 2] = make_matrix(target_handle); \
		interp->dsp--; \
	} while (0)

#define BROADCAST_MATRIX_OP_SCALAR(op) \
	do { \
		double scalar = VAL_NUMBER(right); \
		Object *matrix_source = OBJECT_AT(VAL_DATA(left)); \
		int target_handle = object_new_matrix(interp, matrix_source->matrix.rows, matrix_source->matrix.columns); \
		if (interp->error_flag) return; \
		Object *target = OBJECT_AT(target_handle); \
		size_t num_elements = (size_t)matrix_source->matrix.rows * (size_t)matrix_source->matrix.columns; \
		const double * restrict source_elements = matrix_source->matrix.elements; \
		double * restrict target_elements = target->matrix.elements; \
		for (size_t i = 0; i < num_elements; i++) \
			target_elements[i] = source_elements[i] op scalar; \
		interp->data_stack[interp->dsp - 2] = make_matrix(target_handle); \
		interp->dsp--; \
	} while (0)

void p_add(Interpreter *interp) {
	PEEK_AT(left, 1, "+");
	PEEK_AT(right, 0, "+");

	if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT) {
		interp->data_stack[interp->dsp - 2] = make_float(VAL_NUMBER(left) + VAL_NUMBER(right));
		interp->dsp--;
	}
	else if (VAL_TAG(left) == T_STRING && VAL_TAG(right) == T_STRING) {
		int handle = string_concat(interp, (int)VAL_DATA(left), (int)VAL_DATA(right));
		if (interp->error_flag)
			return;
		interp->data_stack[interp->dsp - 2] = make_string(handle);
		interp->dsp--;
	}
	else if (VAL_TAG(left) == T_SET && VAL_TAG(right) == T_SET) {
		int handle = set_union(interp, (int)VAL_DATA(left), (int)VAL_DATA(right));
		if (interp->error_flag)
			return;
		interp->data_stack[interp->dsp - 2] = make_set(handle);
		interp->dsp--;
	}
	else if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_MATRIX) {
		int target_handle = matrix_add(interp, left, right);
		if (target_handle < 0)
			return;
		interp->data_stack[interp->dsp - 2] = make_matrix(target_handle);
		interp->dsp--;
	}
	else if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_MATRIX)
		BROADCAST_SCALAR_OP_MATRIX(+);
	else if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_FLOAT)
		BROADCAST_MATRIX_OP_SCALAR(+);
	else if (VAL_TAG(left) == T_ARRAY && VAL_TAG(right) == T_ARRAY)
		execute_cfa(interp, find("concat"));
	else
		fail(interp, "+ : expected two floats, two strings, two sets, two matrices, scalar/matrix, or two arrays; got %s and %s", tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));

	DISPATCH(interp);
}

void p_sub(Interpreter *interp) {
	PEEK_AT(left, 1, "-");
	PEEK_AT(right, 0, "-");

	if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT) {
		interp->data_stack[interp->dsp - 2] = make_float(VAL_NUMBER(left) - VAL_NUMBER(right));
		interp->dsp--;
	}
	else if (VAL_TAG(left) == T_SET && VAL_TAG(right) == T_SET) {
		int handle = set_difference(interp, (int)VAL_DATA(left), (int)VAL_DATA(right));
		if (interp->error_flag)
			return;
		interp->data_stack[interp->dsp - 2] = make_set(handle);
		interp->dsp--;
	}
	else if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_MATRIX) {
		int target_handle = matrix_sub(interp, left, right);
		if (target_handle < 0)
			return;
		interp->data_stack[interp->dsp - 2] = make_matrix(target_handle);
		interp->dsp--;
	}
	else if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_MATRIX)
		BROADCAST_SCALAR_OP_MATRIX(-);
	else if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_FLOAT)
		BROADCAST_MATRIX_OP_SCALAR(-);
	else
		fail(interp, "- : expected two floats, two sets, two matrices, or scalar/matrix; got %s and %s", tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));

	DISPATCH(interp);
}

void p_mul(Interpreter *interp) {
	PEEK_AT(left, 1, "*");
	PEEK_AT(right, 0, "*");

	if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT) {
		interp->data_stack[interp->dsp - 2] = make_float(VAL_NUMBER(left) * VAL_NUMBER(right));
		interp->dsp--;
	}
	else if (VAL_TAG(left) == T_SET && VAL_TAG(right) == T_SET) {
		int handle = set_intersect(interp, (int)VAL_DATA(left), (int)VAL_DATA(right));
		if (interp->error_flag)
			return;
		interp->data_stack[interp->dsp - 2] = make_set(handle);
		interp->dsp--;
	}
	else if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_MATRIX) {
		int target_handle = matrix_mul(interp, left, right);
		if (target_handle < 0)
			return;
		interp->data_stack[interp->dsp - 2] = make_matrix(target_handle);
		interp->dsp--;
	}
	else if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_MATRIX)
		BROADCAST_SCALAR_OP_MATRIX(*);
	else if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_FLOAT)
		BROADCAST_MATRIX_OP_SCALAR(*);
	else
		fail(interp, "* : expected two floats, two sets, two matrices, or scalar/matrix; got %s and %s", tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));

	DISPATCH(interp);
}

void p_div(Interpreter *interp) {
	PEEK_AT(left, 1, "/");
	PEEK_AT(right, 0, "/");
	if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT) {
		if (VAL_NUMBER(right) == 0.0) {
			fail(interp, "/ : division by zero");
			return;
		}
		interp->data_stack[interp->dsp - 2] = make_float(VAL_NUMBER(left) / VAL_NUMBER(right));
		interp->dsp--;
	}
	else if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_MATRIX) {
		Object *divisor = OBJECT_AT(VAL_DATA(right));
		int n = divisor->matrix.rows * divisor->matrix.columns;
		for (int i = 0; i < n; i++) {
			if (divisor->matrix.elements[i] == 0.0) {
				fail(interp, "/ : division by zero (matrix element %d)", i);
				return;
			}
		}
		int target_handle = matrix_div(interp, left, right);
		if (target_handle < 0)
			return;
		interp->data_stack[interp->dsp - 2] = make_matrix(target_handle);
		interp->dsp--;
	}
	else if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_MATRIX)
		BROADCAST_SCALAR_OP_MATRIX(/);
	else if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_FLOAT) {
		if (VAL_NUMBER(right) == 0.0) {
			fail(interp, "/ : division by zero");
			return;
		}
		BROADCAST_MATRIX_OP_SCALAR(/);
	}
	else
		fail(interp, "/ : expected two floats, two matrices, or scalar/matrix; got %s and %s", tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));

	DISPATCH(interp);
}

#define INPLACE_OP(name, word, op) \
	void name(Interpreter *interp) { \
		POP(right); \
		POP(left); \
		if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_MATRIX) { \
			Object *left_matrix = OBJECT_AT(VAL_DATA(left)); \
			Object *right_matrix = OBJECT_AT(VAL_DATA(right)); \
			if (left_matrix->matrix.rows != right_matrix->matrix.rows || left_matrix->matrix.columns != right_matrix->matrix.columns) { \
				fail(interp, word ": matrix shapes differ (%dx%d vs %dx%d)", left_matrix->matrix.rows, left_matrix->matrix.columns, right_matrix->matrix.rows, right_matrix->matrix.columns); \
				return; \
			} \
			size_t num_elements = (size_t)left_matrix->matrix.rows * (size_t)left_matrix->matrix.columns; \
			double * restrict left_elements = left_matrix->matrix.elements; \
			const double * restrict right_elements = right_matrix->matrix.elements; \
			for (size_t i = 0; i < num_elements; i++) \
				left_elements[i] = left_elements[i] op right_elements[i]; \
			push(interp, left); \
		} else if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_FLOAT) { \
			double scalar = VAL_NUMBER(right); \
			Object *matrix = OBJECT_AT(VAL_DATA(left)); \
			size_t num_elements = (size_t)matrix->matrix.rows * (size_t)matrix->matrix.columns; \
			double * restrict elements = matrix->matrix.elements; \
			for (size_t i = 0; i < num_elements; i++) \
				elements[i] = elements[i] op scalar; \
			push(interp, left); \
		} else if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_MATRIX) { \
			double scalar = VAL_NUMBER(left); \
			Object *matrix = OBJECT_AT(VAL_DATA(right)); \
			size_t num_elements = (size_t)matrix->matrix.rows * (size_t)matrix->matrix.columns; \
			double * restrict elements = matrix->matrix.elements; \
			for (size_t i = 0; i < num_elements; i++) \
				elements[i] = scalar op elements[i]; \
			push(interp, right); \
		} else { \
			fail(interp, word ": expected a matrix operand; got %s and %s", tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right))); \
		} \
		DISPATCH(interp); \
	}

INPLACE_OP(p_add_inplace, "+!", +)
INPLACE_OP(p_sub_inplace, "-!", -)
INPLACE_OP(p_mul_inplace, "*!", *)
INPLACE_OP(p_div_inplace, "/!", /)

#define BINARY_FLOAT_OP(name, opname, op) \
	void name(Interpreter *interp) { \
		if (interp->dsp < 2) { \
			fail(interp, "%s: data stack underflow", opname); \
			return; \
		} \
		Val *left = &interp->data_stack[interp->dsp - 2]; \
		Val *right = &interp->data_stack[interp->dsp - 1]; \
		left->number = left->number op right->number; \
		interp->dsp--; \
		DISPATCH(interp); \
	}

BINARY_FLOAT_OP(p_add_f, "f+", +)
BINARY_FLOAT_OP(p_sub_f, "f-", -)
BINARY_FLOAT_OP(p_mul_f, "f*", *)

BINARY_FLOAT_OP(p_eq_f, "feq", ==)
BINARY_FLOAT_OP(p_lt_f, "flt", <)
BINARY_FLOAT_OP(p_gt_f, "fgt", >)

#define BITWISE_BINARY_OP(name, opname, op) \
	void name(Interpreter *interp) { \
		if (interp->dsp < 2) { \
			fail(interp, "%s: data stack underflow", opname); \
			return; \
		} \
		Val *left = &interp->data_stack[interp->dsp - 2]; \
		Val *right = &interp->data_stack[interp->dsp - 1]; \
		int64_t a = (int64_t)left->number; \
		int64_t b = (int64_t)right->number; \
		left->number = (double)(a op b); \
		interp->dsp--; \
		DISPATCH(interp); \
	}
BITWISE_BINARY_OP(p_bit_and, "bit-and", &)
BITWISE_BINARY_OP(p_bit_or, "bit-or", |)
BITWISE_BINARY_OP(p_bit_xor, "bit-xor", ^)
BITWISE_BINARY_OP(p_lshift, "lshift", <<)
BITWISE_BINARY_OP(p_rshift, "rshift", >>)

void p_bit_not(Interpreter *interp) {
	if (interp->dsp < 1) {
		fail(interp, "bit-not: data stack underflow");
		return;
	}
	Val *top = &interp->data_stack[interp->dsp - 1];
	top->number = (double)(~(int64_t)top->number);
	DISPATCH(interp);
}

void p_lowest_bit(Interpreter *interp) {
	if (interp->dsp < 1) {
		fail(interp, "lowest-bit: data stack underflow");
		return;
	}
	Val *top = &interp->data_stack[interp->dsp - 1];
	uint64_t bits = (uint64_t)(int64_t)top->number;
	top->number = bits ? (double)__builtin_ctzll(bits) : -1.0;
	DISPATCH(interp);
}

void p_div_f(Interpreter *interp) {
	if (interp->dsp < 2) {
		fail(interp, "f/: data stack underflow");
		return;
	}
	Val *left = &interp->data_stack[interp->dsp - 2];
	Val *right = &interp->data_stack[interp->dsp - 1];
	if (right->number == 0.0) {
		fail(interp, "f/: division by zero");
		return;
	}
	left->number = left->number / right->number;
	interp->dsp--;

	DISPATCH(interp);
}

static double scalar_negate(double x) { return -x; }
static double scalar_inc(double x) { return x + 1.0; }
static double scalar_dec(double x) { return x - 1.0; }
static double scalar_sq(double x) { return x * x; }

void p_neg(Interpreter *interp) {
	POP(operand);
	unary_op(interp, operand, scalar_negate, "negate");

	DISPATCH(interp);
}

#define COMPARISON_OP(name, op) \
	void name(Interpreter *interp) { \
		POP(right); \
		POP(left); \
		if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT) \
			push(interp, make_bool(VAL_NUMBER(left) op VAL_NUMBER(right))); \
		else \
			push(interp, make_bool(val_cmp(interp, left, right) op 0)); \
		DISPATCH(interp); \
	}

COMPARISON_OP(p_eq, ==)
COMPARISON_OP(p_lt, <)
COMPARISON_OP(p_gt, >)

#define UNARY_FLOAT_OP(name, opname, expr) \
	void name(Interpreter *interp) { \
		if (interp->dsp < 1) { \
			fail(interp, "%s: data stack underflow", opname); \
			return; \
		} \
		Val *top = &interp->data_stack[interp->dsp - 1]; \
		double n = top->number; \
		top->number = (expr); \
		DISPATCH(interp); \
	}

void p_zeq(Interpreter *interp) {
	POP(operand);
	push(interp, make_bool(!truthy(operand)));

	DISPATCH(interp);
}

#define COMPARISON_ZBRANCH(name, op) \
	void name(Interpreter *interp) { \
		cell branch_distance = vocab.dict[interp->ip++]; \
		POP(right); \
		POP(left); \
		int is_true = (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT) \
			? (VAL_NUMBER(left) op VAL_NUMBER(right)) \
			: (val_cmp(interp, left, right) op 0); \
		if (!is_true) \
			interp->ip += branch_distance -1; \
		DISPATCH(interp); \
	}

COMPARISON_ZBRANCH(p_eq_zbranch, ==);
COMPARISON_ZBRANCH(p_lt_zbranch, <);
COMPARISON_ZBRANCH(p_gt_zbranch, >);

/* Pure-float counterparts: feq/flt/fgt assert float operands, so these skip the
   tag check and val_cmp fallback entirely — the speed feq is chosen for. */
#define FLOAT_COMPARISON_ZBRANCH(name, op) \
	void name(Interpreter *interp) { \
		cell branch_distance = vocab.dict[interp->ip++]; \
		POP(right); \
		POP(left); \
		if (!(VAL_NUMBER(left) op VAL_NUMBER(right))) \
			interp->ip += branch_distance - 1; \
		DISPATCH(interp); \
	}

FLOAT_COMPARISON_ZBRANCH(p_eq_f_zbranch, ==);
FLOAT_COMPARISON_ZBRANCH(p_lt_f_zbranch, <);
FLOAT_COMPARISON_ZBRANCH(p_gt_f_zbranch, >);

void p_zeq_zbranch(Interpreter *interp) {
	cell branch_distance = vocab.dict[interp->ip++];
	POP(operand);

	if (truthy(operand))
		interp->ip += branch_distance - 1;

	DISPATCH(interp);
}

void p_and(Interpreter *interp) {
	POP(right);
	POP(left);
	push(interp, make_bool(truthy(left) && truthy(right)));

	DISPATCH(interp);
}

void p_or(Interpreter *interp) {
	POP(right);
	POP(left);
	push(interp, make_bool(truthy(left) || truthy(right)));

	DISPATCH(interp);
}

void p_not(Interpreter *interp) {
	POP(operand);
	push(interp, make_bool(!truthy(operand)));

	DISPATCH(interp);
}

void p_null(Interpreter *interp) {
	push(interp, make_tagged(T_NONE,0));

	DISPATCH(interp);
}

void p_symbol_q(Interpreter *interp) {
	POP(value);
	push(interp, make_bool(VAL_TAG(value) == T_SYMBOL));

	DISPATCH(interp);
}

void p_dup(Interpreter *interp) {
	POP(top);
	push(interp, top);
	push(interp, top);

	DISPATCH(interp);
}

void p_drop(Interpreter *interp) {
	(void)pop(interp);

	DISPATCH(interp);
}

void p_swap(Interpreter *interp) {
	POP(top);
	POP(second);
	push(interp, top);
	push(interp, second);

	DISPATCH(interp);
}

void p_over(Interpreter *interp) {
	POP(top);
	POP(second);
	push(interp, second);
	push(interp, top);
	push(interp, second);

	DISPATCH(interp);
}

void p_rot(Interpreter *interp) {
	POP(top);
	POP(middle);
	POP(bottom);
	push(interp, middle);
	push(interp, top);
	push(interp, bottom);

	DISPATCH(interp);
}

void p_depth(Interpreter *interp) {
	push(interp, make_float((double)interp->dsp));

	DISPATCH(interp);
}

void p_roll(Interpreter *interp) {
	POP_INT(n, "roll", "depth");
	if (n < 0 || n >= interp->dsp) {
		fail(interp, "roll: depth %d out of range (stack has %d below it)", n, interp->dsp);
		return;
	}

	int src = interp->dsp - 1 - n;
	Val value = interp->data_stack[src];
	memmove(&interp->data_stack[src], &interp->data_stack[src + 1], (size_t)n * sizeof(Val));
	interp->data_stack[interp->dsp - 1] = value;

	DISPATCH(interp);
}

void p_dot(Interpreter *interp) {
	POP(value);
	if (VAL_TAG(value) == T_MATRIX) {
		print_matrix_grid(OBJECT_AT(VAL_DATA(value)));
	} else if (VAL_TAG(value) == T_FRAME) {
		print_frame_pretty(interp, OBJECT_AT(VAL_DATA(value)), 0);
		putchar('\n');
	} else if (VAL_TAG(value) == T_ARRAY) {
		pretty_print_array(interp, value);
	} else {
		print_val(interp, value);
		putchar(' ');
	}
	fflush(stdout);

	DISPATCH(interp);
}

void p_dot_all(Interpreter *interp) {
	int saved = print_truncate;
	print_truncate = 0;
	POP(value);
	if (VAL_TAG(value) == T_MATRIX) {
		print_matrix_grid(OBJECT_AT(VAL_DATA(value)));
	} else {
		print_val(interp, value);
		putchar(' ');
	}
	fflush(stdout);
	print_truncate = saved;

	DISPATCH(interp);
}

void p_cr(Interpreter *interp) {
	(void)interp;

	putchar('\n');
	fflush(stdout);

	DISPATCH(interp);
}

void p_emit_(Interpreter *interp) {
	POP_INT(char_code, "emit", "character code");

	if (char_code < 0 || char_code > 0x10FFFF) {
		fail(interp, "emit: codepoint %d out of range", char_code);
		return;
	}

	if (char_code < 0x80)
		putchar(char_code);
	else {
		char encoded[4];
		int length = utf8_encode(char_code, encoded);
		fwrite(encoded, 1, (size_t)length, stdout);
	}

	fflush(stdout);

	DISPATCH(interp);
}

void p_dots(Interpreter *interp) {
	for (int i = 0; i < interp->dsp; i++) {
		print_val_inspect(interp, interp->data_stack[i]);
		putchar(' ');
	}
	fflush(stdout);

	DISPATCH(interp);
}

void p_bye(Interpreter *interp) {
	(void)interp;

	exit(0);
}

void p_tor(Interpreter *interp) {
	POP(value);
	rpush(interp, value);

	DISPATCH(interp);
}

void p_rfrom(Interpreter *interp) {
	push(interp, rpop(interp));

	DISPATCH(interp);
}

void p_rfetch(Interpreter *interp) {
	if (interp->rsp > 0)
		push(interp, interp->return_stack[interp->rsp - 1]);
	else
		fail(interp, "r@: return stack is empty");

	DISPATCH(interp);
}

void p_to_side(Interpreter *interp) {
	POP(value);
	if (interp->side_dsp >= SIDESTACK_DEPTH) {
		fail(interp, ">side: side stack overflow (max %d)", SIDESTACK_DEPTH);
		return;
	}
	interp->side_stack[interp->side_dsp++] = value;

	DISPATCH(interp);
}

void p_side_to(Interpreter *interp) {
	if (interp->side_dsp <= 0) {
		fail(interp, "side>: side stack is empty");
		return;
	}
	push(interp, interp->side_stack[--interp->side_dsp]);

	DISPATCH(interp);
}

void p_side_drop(Interpreter *interp) {
	if (interp->side_dsp <= 0) {
		fail(interp, "side-drop: side stack is empty");
		return;
	}
	interp->side_dsp--;

	DISPATCH(interp);
}

void p_side_depth(Interpreter *interp) {
	push(interp, make_float((double)interp->side_dsp));

	DISPATCH(interp);
}

void p_execute(Interpreter *interp) {
	POP_XT(value, "execute");
	execute_cfa(interp, value);

	DISPATCH(interp);
}

int push_prompt(Interpreter *interp, int kind) {
	Val mark = make_tagged(T_MARK, (interp->next_mark_id++ << 1) | kind);
	rpush(interp, mark);

	return (int)VAL_DATA(mark);
}

void p_reset(Interpreter *interp) {
	push_prompt(interp, PROMPT_EXCEPTION);

	DISPATCH(interp);
}


static int find_prompt(Interpreter *interp, int kind) {
	int mark_index = interp->rsp - 1;
	
	while (mark_index >= 0 && 
			!(VAL_TAG(interp->return_stack[mark_index]) == T_MARK
				&& (VAL_DATA(interp->return_stack[mark_index]) & 1) == kind)) {
		mark_index--;
	}
	if (mark_index < 0) {
		fail(interp, kind == PROMPT_CHOICE
				? "no enclosing amb to backtrack to"
				: "shift/shift-with: no enclosing reset on the return stack");
		return -1;
	}

	return mark_index;
}

static void restore_local_base_below(Interpreter *interp, int mark_index) {
	int base = interp->local_base;
	while (base > mark_index)
		base = saved_local_base(interp->return_stack[base - 1]);
	interp->local_base = base;
}


static void unwind_to(Interpreter *interp, int mark_index) {
	interp->unwind_target = (int)VAL_DATA(interp->return_stack[mark_index]);
	interp->rsp = mark_index + 1;
	restore_local_base_below(interp, mark_index);
}

int capture_continuation(Interpreter *interp, int what_kind, int *out_mark_index) {
	int mark_index = find_prompt(interp, what_kind);
	if (mark_index < 0)
		return -1;

	int return_len = interp->rsp - mark_index - 1;
	int resume_ip = interp->ip;
	int slot = object_new_continuation(interp, &interp->return_stack[mark_index + 1],
			return_len, resume_ip);
	if (interp->error_flag)
		return -1;

	OBJECT_AT(slot)->continuation.local_base_offset =
		interp->local_base - (mark_index + 1);

	*out_mark_index = mark_index;
	return slot;
}

void backtrack(Interpreter *interp) {
	int mark_index = find_prompt(interp, PROMPT_CHOICE);
	if (mark_index < 0)
		return;
	
	unwind_to(interp, mark_index);
	interp->unwinding = 1;
}

void p_fail(Interpreter *interp) {
	backtrack(interp);

	DISPATCH(interp);
}

void p_shift(Interpreter *interp) {
	int mark_index;
	int cont_slot = capture_continuation(interp, PROMPT_EXCEPTION, &mark_index);
	if (cont_slot < 0)
		return;

	interp->rsp = mark_index;
	restore_local_base_below(interp, mark_index);
	push(interp, make_continuation(cont_slot));

	DISPATCH(interp);
}

void p_shift_with(Interpreter *interp) {
	POP_XT(handler, "shift-with");

	int mark_index;
	int cont_slot = capture_continuation(interp, PROMPT_EXCEPTION, &mark_index);
	if (cont_slot < 0)
		return;

	unwind_to(interp, mark_index);
	push(interp, make_continuation(cont_slot));

	execute_cfa(interp, handler);
	if (interp->error_flag)
		return;

	interp->unwinding = 1;
}

void p_execute_catching(Interpreter *interp) {
	POP_XT(xt, "(execute-catching)");
	int base_dsp = interp->dsp;

	execute_cfa(interp, xt);

	if (interp->error_flag) {
		interp->error_flag = 0;
		int handle = object_new_string(interp, interp->error_message,
				(int)strlen(interp->error_message));
		interp->dsp = base_dsp;
		push(interp, make_string(handle));
		push(interp, make_float(1));

		int mark_index = find_prompt(interp, PROMPT_EXCEPTION);
		if (mark_index >= 0) {
			unwind_to(interp, mark_index);
			interp->unwinding = 1;
		}
	}

	DISPATCH(interp);
}

void p_resume(Interpreter *interp) {
	POP_CONT(continuation, "resume");
	if (continuation->continuation.capture_generation != vocab.forget_generation) {
		fail(interp, "resume: continuation outlived its defining word");
		return;
	}
	int saved_ip = interp->ip;
	int saved_running = interp->running;
	int saved_local_base = interp->local_base;
	int resume_base = interp->rsp;

	rpush(interp, make_addr(interp->trampoline_base + 2));

	rpush(interp, make_mark());

	int slice_base = interp->rsp;
	for (int i = 0; i < continuation->continuation.return_len; i++)
		rpush(interp, continuation->continuation.return_slice[i]);

	if (continuation->continuation.local_base_offset >= 0)
		interp->local_base = slice_base + continuation->continuation.local_base_offset;

	interp->ip = continuation->continuation.resume_ip;
	interp->running = 1;
	run_inner(interp, resume_base);

	interp->running = saved_running;
	interp->ip = saved_ip;
	interp->local_base = saved_local_base;

	DISPATCH(interp);
}


void p_words(Interpreter *interp) {
	int printed_count = 0;
	for (int cfa = vocab.latest_cfa; cfa != 0; cfa = (int)WORD_LINK(cfa)) {
		if (WORD_IS_INTERNAL(cfa))
			continue;
		fputs(&vocab.name_pool[WORD_NAME(cfa)], stdout);
		putchar(' ');
		if (++printed_count % 8 == 0)
			putchar('\n');
	}
	if (printed_count % 8)
		putchar('\n');
	fflush(stdout);

	DISPATCH(interp);
}

void p_see(Interpreter *interp) {
	POP_XT(target_cfa, "see");

	const char *name = name_of(target_cfa);

	cfa_handler handler = (cfa_handler)vocab.dict[target_cfa];
	if (handler == docol) {
		if (!name) {

			printf("[: ... :]  \\ anonymous, no source\n");
		} else {
			int src_idx = (int)WORD_SOURCE(target_cfa);
			if (src_idx > 0)
				printf(": %s%s;\n", name, &vocab.source_pool[src_idx]);
			else
				printf(": %s ... ;  \\ no source captured\n", name);
		}
	} else if (handler == dovar) {
		Val value;
		value.bits = (uint64_t)vocab.dict[target_cfa + 1];
		printf("variable %s  \\ current value: ", name ? name : "?");
		print_val(interp, value);
		putchar('\n');
	} else if (handler == dosym) {
		printf("symbol %s\n", name ? name : "?");
	} else {
		printf("%s is a primitive\n", name ? name : "?");
	}
	fflush(stdout);

	DISPATCH(interp);
}

static void help_put(Interpreter *interp, int frame_handle, const char *key, const char *text) {
	int string_handle = object_new_string(interp, text, (int)strlen(text));
	if (interp->error_flag) {
		return;
	}
	frame_put(OBJECT_AT(frame_handle), intern_symbol(interp, key), make_string(string_handle));
}

void p_man(Interpreter *interp) {
	POP_XT(target_cfa, "man");

	const char *name = name_of(target_cfa);
	const HelpEntry *entry = NULL;
	if (name) {
		LOWER_BOUND(help_entry_count, mid, strcmp(help_entries[mid].name, name) < 0, at);
		if (at < help_entry_count && strcmp(help_entries[at].name, name) == 0) {
			entry = &help_entries[at];
		}
	}

	if (!entry) {
		push(interp, make_tagged(T_NONE, 0));
		DISPATCH(interp);
	}

	NEW_FRAME(frame_handle, frame);
	(void)frame;
	gc_root_push(interp, make_frame(frame_handle));
	help_put(interp, frame_handle, "word", entry->name);
	help_put(interp, frame_handle, "effect", entry->effect);
	help_put(interp, frame_handle, "summary", entry->summary);
	if (entry->ops) {
		help_put(interp, frame_handle, "ops", entry->ops);
		help_put(interp, frame_handle, "alloc", entry->alloc);
		help_put(interp, frame_handle, "order", entry->order);
	}
	gc_root_pop(interp);

	push(interp, make_frame(frame_handle));

	DISPATCH(interp);
}

void p_semicolon(Interpreter *interp) {
	leave_compile_scope(interp);
	emit_call(interp, vocab.exit_cfa);
	if (compiler.compiling_src_start > 0 && vocab.latest_cfa != 0) {
		int src_end = compiler.input_buffer_pos - 1;
		int src_len = src_end - compiler.compiling_src_start;
		src_len = MAX(src_len, 0);
		if (vocab.source_here + src_len + 1 > SOURCE_POOL) {
			fail(interp, "source pool full (max %d bytes); definition source too large to store", SOURCE_POOL);
		} else {
			int source_offset = vocab.source_here;
			memcpy(&vocab.source_pool[vocab.source_here],
					&compiler.input_buffer[compiler.compiling_src_start],
					(size_t)src_len);
			vocab.source_pool[vocab.source_here + src_len] = 0;
			vocab.source_here += src_len + 1;
			WORD_SOURCE(vocab.latest_cfa) = source_offset;
		}
	}
	compiler.compiling = 0;
	compiler.compiling_src_start = 0;

	DISPATCH(interp);
}

static int try_fuse_cmp_branch(Interpreter *interp) {
	int fused_cfa;

	if (compiler.fuse_prev_cmp == vocab.eq_cfa)
		fused_cfa = vocab.eq_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.lt_cfa)
		fused_cfa = vocab.lt_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.gt_cfa)
		fused_cfa = vocab.gt_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.zeq_cfa)
		fused_cfa = vocab.zeq_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.eq_f_cfa)
		fused_cfa = vocab.eq_f_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.lt_f_cfa)
		fused_cfa = vocab.lt_f_zbranch_cfa;
	else if (compiler.fuse_prev_cmp == vocab.gt_f_cfa)
		fused_cfa = vocab.gt_f_zbranch_cfa;
	else
		return 0;

	vocab.here--;
	emit_call(interp, fused_cfa);
	return 1;
}


void p_if(Interpreter *interp) {
	if (!try_fuse_cmp_branch(interp))
		emit_call(interp, vocab.zbranch_cfa);
	push(interp, make_float((double)vocab.here));
	emit(interp, 0);

	DISPATCH(interp);
}

void p_qif(Interpreter *interp) {
	emit_call(interp, vocab.qzbranch_cfa);
	push(interp, make_float((double)vocab.here));
	emit(interp, 0);

	DISPATCH(interp);
}

static int valid_patch_slot(Interpreter *interp, int slot, const char *op) {
	if (slot < DICT_RESERVED || slot > vocab.here) {
		fail(interp, "%s: no matching control-flow opener", op);
		return 0;
	}
	return 1;
}

void p_then(Interpreter *interp) {
	POP(slot_val);
	int slot = (int)VAL_NUMBER(slot_val);
	if (!valid_patch_slot(interp, slot, "then"))
		return;
	vocab.dict[slot] = (vocab.here - slot);
	compiler.fuse_floor = vocab.here;

	DISPATCH(interp);
}

void p_else(Interpreter *interp) {
	POP(slot_val);
	int slot = (int)VAL_NUMBER(slot_val);
	if (!valid_patch_slot(interp, slot, "else"))
		return;
	emit_call(interp, vocab.branch_cfa);
	push(interp, make_float((double)vocab.here));
	emit(interp, 0);
	vocab.dict[slot] = (vocab.here - slot);

	DISPATCH(interp);
}

void p_begin(Interpreter *interp) {
	push(interp, make_float((double)vocab.here));
	compiler.fuse_floor = vocab.here;

	DISPATCH(interp);
}

void p_until(Interpreter *interp) {
	POP(back_val);
	int back = (int)VAL_NUMBER(back_val);
	if (!valid_patch_slot(interp, back, "until"))
		return;
	if (!try_fuse_cmp_branch(interp))
		emit_call(interp, vocab.zbranch_cfa);
	emit(interp, back - vocab.here);

	DISPATCH(interp);
}

void p_again(Interpreter *interp) {
	POP(back_val);
	int back = (int)VAL_NUMBER(back_val);
	if (!valid_patch_slot(interp, back, "again"))
		return;
	emit_call(interp, vocab.branch_cfa);
	emit(interp, back - vocab.here);

	DISPATCH(interp);
}

void p_while(Interpreter *interp) {
	if (!try_fuse_cmp_branch(interp))
		emit_call(interp, vocab.zbranch_cfa);
	push(interp, make_float((double)vocab.here));
	emit(interp, 0);

	DISPATCH(interp);
}

void p_repeat(Interpreter *interp) {
	POP(exit_slot_val);
	POP(back_val);
	int exit_slot = (int)VAL_NUMBER(exit_slot_val);
	int back = (int)VAL_NUMBER(back_val);
	if (!valid_patch_slot(interp, back, "repeat") || !valid_patch_slot(interp, exit_slot, "repeat"))
		return;
	emit_call(interp, vocab.branch_cfa);
	emit(interp, back - vocab.here);
	vocab.dict[exit_slot] = (vocab.here - exit_slot);

	DISPATCH(interp);
}

static void open_quotation(Interpreter *interp) {
	int branch_slot = -1;
	if (compiler.compiling) {
		emit_call(interp, vocab.branch_cfa);
		branch_slot = vocab.here;
		emit(interp, 0);
	}
	int anon_cfa = vocab.here;
	emit(interp, (cell)&docol);
	compiler.fuse_floor = vocab.here;
	compiler.loadn_at = -1;
	enter_compile_scope(interp);
	compiler.compiling = 1;
	push(interp, make_float((double)anon_cfa));
	push(interp, make_float((double)branch_slot));
}

void p_qcolon(Interpreter *interp) {
	open_quotation(interp);

	DISPATCH(interp);
}

void p_qsemi(Interpreter *interp) {
	leave_compile_scope(interp);
	emit_call(interp, vocab.exit_cfa);
	POP(branch_slot_val);
	POP(anon_cfa_val);
	int branch_slot = (int)VAL_NUMBER(branch_slot_val);
	int anon_cfa = (int)VAL_NUMBER(anon_cfa_val);
	if (branch_slot < 0) {
		compiler.compiling = 0;
		push(interp, make_xt(anon_cfa));
	} else {
		vocab.dict[branch_slot] = (vocab.here - branch_slot);
		emit_val_literal(interp, make_xt(anon_cfa));
	}

	DISPATCH(interp);
}

static int parse_word_cfa(Interpreter *interp, const char *op) {
	char *token = next_token();
	
	if (!token) {
		fail(interp, "%s : expected a word name", op);
		return 0;
	}
	int target_cfa = find(token);
	if (!target_cfa) {
		fail(interp, "%s : unknown word: %s", op, token);
		return 0;
	}

	return target_cfa;
}
	

void p_tick(Interpreter *interp) {
	int target_cfa = parse_word_cfa(interp, "'");
	if (!target_cfa)
		return;

	Val value = make_xt(target_cfa);
	if (compiler.compiling)
		emit_val_literal(interp, value);
	else
		push(interp, value);

	DISPATCH(interp);
}

void p_lookup(Interpreter *interp) {
	int target_cfa = parse_word_cfa(interp, "lookup");
	if (!target_cfa)
		return;

	push(interp, make_xt(target_cfa));

	DISPATCH(interp);
}
static void enter_compile_scope(Interpreter *interp) {
	if (compiler.n_local_scopes >= MAX_LOCAL_SCOPES) {
		fail(interp, "compile: locals nesting deeper than %d", MAX_LOCAL_SCOPES);
		return;
	}

	compiler.local_scope_starts[compiler.n_local_scopes] = compiler.n_local_names;
	compiler.local_scope_dict_starts[compiler.n_local_scopes] = vocab.here;
	compiler.n_local_scopes++;
}

static void leave_compile_scope(Interpreter *interp) {
	if (compiler.n_local_scopes <= 0)
		return;

	compiler.n_local_scopes--;
	int saved_n_names = compiler.local_scope_starts[compiler.n_local_scopes];
	int n_locals_in_scope = compiler.n_local_names - saved_n_names;

	if (n_locals_in_scope > 0) {
		emit_call(interp, vocab.leave_locals_cfa);
		emit(interp, (cell)n_locals_in_scope);
	}

	if (saved_n_names == 0) {
		compiler.local_names_pool_here = 0;
	} else {
		int last_offset = compiler.local_name_offsets[saved_n_names - 1];
		compiler.local_names_pool_here = last_offset +
			(int)strlen(&compiler.local_names_pool[last_offset]) + 1;
	}
	compiler.n_local_names = saved_n_names;
}

void p_colon(Interpreter *interp) {
	char *token = next_token();
	if (!token) {
		fail(interp, ": expected a name for the new definition");
		return;
	}

	create_header(interp, token, 0);
	emit(interp, (cell)&docol);
	compiler.fuse_floor = vocab.here;
	compiler.loadn_at = -1;
	enter_compile_scope(interp);
	compiler.compiling = 1;

	compiler.compiling_src_start = compiler.input_buffer_pos;

	DISPATCH(interp);
}

int create_variable(Interpreter *interp, const char *name) {
	create_header(interp, name, 0);
	emit(interp, (cell)&dovar);
	emit(interp, (cell)make_float(0.0).bits);

	return vocab.latest_cfa;
}


void p_variable(Interpreter *interp) {
	char *token = next_token();
	if (!token) {
		fail(interp, "variable: expected a name");
		return;
	}

	create_variable(interp, token);

	DISPATCH(interp);
}

void p_constant(Interpreter *interp) {
	POP(value);
	char *token = next_token();
	if (!token) {
		fail(interp, "constant: expected a name");
		return;
	}
	create_header(interp, token, 2);
	emit(interp, (cell)&docol);
	emit_val_literal(interp, value);
	emit_call(interp, vocab.exit_cfa);
	DISPATCH(interp);
}

static void compile_locals_decl(Interpreter *interp, const char *opener, int force_all_receive) {
	if (!compiler.compiling || compiler.n_local_scopes <= 0) {
		fail(interp, "%s: only valid inside a colon definition or quotation", opener);
		return;
	}

	int scope_idx = compiler.n_local_scopes - 1;
	if (vocab.here != compiler.local_scope_dict_starts[scope_idx]) {
		fail(interp, "%s: locals must be declared at the head of the body", opener);
		return;
	}

	int scope_start = compiler.local_scope_starts[scope_idx];
	int receive_slots[MAX_LOCAL_NAMES];
	int n_received = 0;

	while (1) {
		skip_whitespace_and_comments();
		char *token = next_token();
		if (!token) {
			if (refill_input())
				continue;
			fail(interp, "%s: unterminated locals declaration (no closing |)", opener);
			return;
		}
		if (strcmp(token, "|") == 0)
			break;

		int has_receive_marker = 0;
		if (token[0] == '>' && token[1] != 0) {
			has_receive_marker = 1;
			token++;
		}

		for (int i = scope_start; i < compiler.n_local_names; i++) {
			if (strcmp(token, &compiler.local_names_pool[compiler.local_name_offsets[i]]) == 0) {
				fail(interp, "%s: local '%s' declared twice", opener, token);
				return;
			}
		}

		int name_len = (int)strlen(token);
		if (compiler.local_names_pool_here + name_len + 1 > LOCAL_NAMES_POOL_SIZE) {
			fail(interp, "%s: local names pool full", opener);
			return;
		}
		if (compiler.n_local_names >= MAX_LOCAL_NAMES) {
			fail(interp, "%s: too many local names (max %d)", opener, MAX_LOCAL_NAMES);
			return;
		}

		int slot = compiler.n_local_names - scope_start;

		int offset = compiler.local_names_pool_here;
		memcpy(&compiler.local_names_pool[offset], token, (size_t)name_len);
		compiler.local_names_pool[offset + name_len] = 0;
		compiler.local_names_pool_here += name_len + 1;
		compiler.local_name_offsets[compiler.n_local_names++] = offset;

		if (has_receive_marker)
			receive_slots[n_received++] = slot;
	}

	int n_declared = compiler.n_local_names - scope_start;
	if (n_declared == 0)
		return;

	if (force_all_receive && n_received > 0) {
		fail(interp, "%s: do not use > markers; %s already implies all-receive", opener, opener);
		return;
	}

	if (force_all_receive || n_received == n_declared) {
		emit_call(interp, vocab.enter_locals_to_cfa);
		emit(interp, (cell)n_declared);
	} else if (n_received == 0) {
		emit_call(interp, vocab.enter_locals_cfa);
		emit(interp, (cell)n_declared);
	} else {
		emit_call(interp, vocab.enter_locals_mixed_cfa);
		emit(interp, (cell)n_declared);
		emit(interp, (cell)n_received);
		for (int i = 0; i < n_received; i++)
			emit(interp, (cell)receive_slots[i]);
	}

	int lvar_cfa = find("lvar");
	for (int i = scope_start; i < compiler.n_local_names; i++) {
		const char *name = &compiler.local_names_pool[compiler.local_name_offsets[i]];
		if (name[0] < 'A' || name[0] > 'Z')
			continue;
		int slot = i - scope_start;
		int received = 0;
		for (int r = 0; r < n_received; r++)
			if (receive_slots[r] == slot) {
				received = 1;
				break;
			}
		if (received)
			continue;
		emit_call(interp, lvar_cfa);
		emit_call(interp, vocab.local_store_0depth_cfa);
		emit(interp, (cell)slot);
	}
}

void p_bar(Interpreter *interp) {
	compile_locals_decl(interp, "|", 0);

	DISPATCH(interp);
}

void p_bar_to(Interpreter *interp) {
	compile_locals_decl(interp, "|>", 1);

	DISPATCH(interp);
}

void p_bracket_bar(Interpreter *interp) {
	open_quotation(interp);
	compile_locals_decl(interp, "[|", 0);

	DISPATCH(interp);
}

void p_bracket_bar_to(Interpreter *interp) {
	open_quotation(interp);
	compile_locals_decl(interp, "[>", 1);

	DISPATCH(interp);
}

void p_to_var(Interpreter *interp) {
	int var_cfa = (int)vocab.dict[interp->ip++];
	POP(value);
	vocab.dict[var_cfa + 1] = (cell)value.bits;

	DISPATCH(interp);
}

void p_to(Interpreter *interp) {
	char *token = next_token();
	if (!token) {
		fail(interp, "to: expected a name"); 
		return;
	}

	if (compiler.compiling) {
		int local_depth, local_slot_idx;
		if (find_local(token, &local_depth, &local_slot_idx)) {
			if (try_fuse_local_acc(interp, local_depth, local_slot_idx))
				return;
			if (local_depth == 0) {
				emit_call(interp, vocab.local_store_0depth_cfa);
				emit(interp, (cell)local_slot_idx);
			} else {
				emit_call(interp, vocab.local_store_cfa);
				emit(interp, (cell)local_depth);
				emit(interp, (cell)local_slot_idx);
			}
			return;
		}
	}

	int target_cfa = find(token);
	if (!target_cfa) {
		if (compiler.compiling) {
			fail(interp, "to: unknown variable: %s; declare it with variable", token); 
		return; 
		}
		target_cfa = create_variable(interp, token);
	}

	cfa_handler h = (cfa_handler)vocab.dict[target_cfa];
	if (h != dovar) {
		fail(interp, "to: %s is not a variable", token); 
		return; 
	}

	if (compiler.compiling) {
		if (!superword_try_fuse_store(interp, target_cfa)) {
			emit_call(interp, vocab.to_var_cfa);
			emit(interp, (cell)target_cfa);
		}
	} else {
		POP(value);
		vocab.dict[target_cfa + 1] = (cell)value.bits;
	}

	DISPATCH(interp);
}

static void compile_local_unary(Interpreter *interp, const char *op,
                                int depth0_cfa, int fallback_cfa) {
	char *token = next_token();
	if (!token) {
		fail(interp, "%s: expected a local name", op);
		return;
	}
	if (!compiler.compiling) {
		fail(interp, "%s: only valid inside a colon definition", op);
		return;
	}
	int depth, slot;
	if (!find_local(token, &depth, &slot)) {
		fail(interp, "%s: %s is not a local", op, token);
		return;
	}
	if (depth == 0) {
		emit_call(interp, depth0_cfa);
		emit(interp, (cell)slot);
	} else {
		emit_call(interp, vocab.local_fetch_cfa);
		emit(interp, (cell)depth);
		emit(interp, (cell)slot);
		emit_call(interp, fallback_cfa);
		emit_call(interp, vocab.local_store_cfa);
		emit(interp, (cell)depth);
		emit(interp, (cell)slot);
	}
}

void p_increment(Interpreter *interp) {
	compile_local_unary(interp, "++",
	                    vocab.local_incr_0depth_cfa,
	                    vocab.inc_cfa);

	DISPATCH(interp);
}

void p_decrement(Interpreter *interp) {
	compile_local_unary(interp, "--",
	                    vocab.local_decr_0depth_cfa,
	                    vocab.dec_cfa);

	DISPATCH(interp);
}

void p_f_increment(Interpreter *interp) {
	compile_local_unary(interp, "f++",
	                    vocab.local_finc_0depth_cfa,
	                    vocab.finc_cfa);

	DISPATCH(interp);
}

void p_f_decrement(Interpreter *interp) {
	compile_local_unary(interp, "f--",
	                    vocab.local_fdec_0depth_cfa,
	                    vocab.fdec_cfa);

	DISPATCH(interp);
}

void p_inline(Interpreter *interp) {
	int latest = vocab.latest_cfa;
	if (!latest) {
		fail(interp, "inline: no recent definition");
		return;
	}

	WORD_FLAGS(latest) |= 2;

	DISPATCH(interp);
}

void p_symbol(Interpreter *interp) {
	char *token = next_token();
	if (!token) {
		fail(interp, "symbol: expected a name");
		return;
	}
	if (token[0] == ':')
		token++;

	create_header(interp, token, 0);
	emit(interp, (cell)&dosym);

	emit(interp, (cell)intern_symbol(interp, token));

	DISPATCH(interp);
}

void p_string_to_symbol(Interpreter *interp) {
	POP_STRING(string, "string>symbol");
	push(interp, make_symbol(intern_symbol(interp, string->bytes)));

	DISPATCH(interp);
}

void p_forget(Interpreter *interp) {
	char *token = next_token();
	if (!token) {
		fail(interp, "forget: expected a name");
		return;
	}
	int target_cfa = find(token);
	if (!target_cfa) {
		fail(interp, "forget: unknown word: %s", token);
		return;
	}
	vocab.here = target_cfa - 4;
	vocab.forget_generation++;
	vocab.names_here = (int)WORD_NAME(target_cfa);
	vocab.latest_cfa = (int)WORD_LINK(target_cfa);

	int max_src_end = 1;
	for (int surviving_cfa = vocab.latest_cfa; surviving_cfa != 0; surviving_cfa = (int)WORD_LINK(surviving_cfa)) {
		int src_offset = (int)WORD_SOURCE(surviving_cfa);
		if (src_offset > 0) {
			int src_end = src_offset + (int)strlen(&vocab.source_pool[src_offset]) + 1;
			max_src_end = MAX(max_src_end, src_end);
		}
	}
	vocab.source_here = max_src_end;

	DISPATCH(interp);
}

int read_string_literal(void) {
	int cursor = compiler.input_buffer_pos + 1;
	int length = 0;
	while (cursor < compiler.input_buffer_len) {
		char c = compiler.input_buffer[cursor];
		if (c == '"') {
			if (cursor + 1 >= compiler.input_buffer_len) {
				compiler.need_more = 1;
				return -1;
			}
			if (compiler.input_buffer[cursor + 1] == '"') {
				compiler.token_buffer[length++] = '"';
				cursor += 2;
				continue;
			}
			compiler.token_buffer[length] = 0;
			compiler.input_buffer_pos = cursor + 1;
			return length;
		}
		compiler.token_buffer[length++] = c;
		cursor++;
	}
	compiler.need_more = 1;
	return -1;
}

static void interp_append(Interpreter *interp, char **buffer, int *capacity, int *length, const char *src, int n) {
	if (interp->error_flag)
		return;
	if (*length + n > *capacity) {
		while (*length + n > *capacity) {
			if (*capacity > INT_MAX / 2) {
				free(*buffer);
				*buffer = NULL;
				fail(interp, "format: result too large");
				return;
			}
			*capacity *= 2;
		}
		char *grown = realloc(*buffer, (size_t)*capacity);
		if (!grown) {
			free(*buffer);
			*buffer = NULL;
			fail(interp, "format: out of memory");
			return;
		}
		*buffer = grown;
	}
	memcpy(*buffer + *length, src, (size_t)n);
	*length += n;
}

static void interp_render_val(Interpreter *interp, Val value, char **out_buffer, int *capacity, int *out_length) {
	switch (VAL_TAG(value)) {
		case T_FLOAT: {
			char rendered[64];
			double number = VAL_NUMBER(value);
			int n;
			if (number == (double)(int64_t)number && number > -1e15 && number < 1e15)
				n = snprintf(rendered, sizeof(rendered), "%lld", (long long)number);
			else
				n = snprintf(rendered, sizeof(rendered), "%g", number);
			interp_append(interp, out_buffer, capacity, out_length, rendered, n);
			break;
		}
		case T_SYMBOL: {
			const char *name = &vocab.symbol_pool[VAL_DATA(value)];
			interp_append(interp, out_buffer, capacity, out_length, name, (int)strlen(name));
			break;
		}
		case T_STRING: {
			Object *string_obj = OBJECT_AT(VAL_DATA(value));
			interp_append(interp, out_buffer, capacity, out_length, string_obj->bytes, string_obj->len);
			break;
		}
		case T_PTR: {
			char rendered[32];
			int n = snprintf(rendered, sizeof(rendered), "<ptr %lld>", (long long)VAL_DATA(value));
			interp_append(interp, out_buffer, capacity, out_length, rendered, n);
			break;
		}
		default:
			interp_append(interp, out_buffer, capacity, out_length, "<?>", 3);
			break;
	}
}

int interpolate(Interpreter *interp, int template_handle) {
	Object *template = OBJECT_AT(template_handle);
	int capacity = template->len + 64;
	char *out_buffer = malloc((size_t)capacity);
	if (!out_buffer) {
		fail(interp, "format: out of memory");
		return object_new_string(interp, "", 0);
	}
	int out_length = 0;
	int refs[64];
	int ref_count = 0;

	for (int cursor = 0; cursor < template->len; ) {
		if (template->bytes[cursor] == '{') {
			int scan = cursor + 1, digit_value = 0, saw_digit = 0;
			while (scan < template->len && isdigit((unsigned char)template->bytes[scan])) {
				digit_value = digit_value * 10 + (template->bytes[scan] - '0');
				scan++;
				saw_digit = 1;
			}
			if (saw_digit && scan < template->len && template->bytes[scan] == '}') {
				int stack_index = interp->dsp - 1 - digit_value;
				if (stack_index < 0) {
					fail(interp, "format: {%d} needs %d stack value(s) but only %d present",
							digit_value, digit_value + 1, interp->dsp);
					free(out_buffer);
					return object_new_string(interp, "", 0);
				}
				int already = 0;
				for (int i = 0; i < ref_count; i++)
					if (refs[i] == digit_value) {
						already = 1;
						break;
					}
				if (!already && ref_count < (int)(sizeof(refs) / sizeof(refs[0])))
					refs[ref_count++] = digit_value;
				interp_render_val(interp, interp->data_stack[stack_index], &out_buffer, &capacity, &out_length);
				cursor = scan + 1;
				continue;
			}
		}
		interp_append(interp, &out_buffer, &capacity, &out_length, &template->bytes[cursor], 1);
		cursor++;
	}

	if (interp->error_flag) {
		free(out_buffer);
		return object_new_string(interp, "", 0);
	}

	if (ref_count > 0) {
		int original_dsp = interp->dsp;
		int write = 0;
		for (int read = 0; read < original_dsp; read++) {
			int depth = original_dsp - 1 - read;
			int referenced = 0;
			for (int i = 0; i < ref_count; i++)
				if (refs[i] == depth) {
					referenced = 1;
					break;
				}
			if (!referenced)
				interp->data_stack[write++] = interp->data_stack[read];
		}
		interp->dsp = write;
	}

	int result_handle = object_new_string(interp, out_buffer, out_length);
	free(out_buffer);
	return result_handle;
}

void p_format(Interpreter *interp) {
	POP(template_val);
	if (VAL_TAG(template_val) != T_STRING) {
		fail(interp, "format: expected a template string; got %s", tag_name(VAL_TAG(template_val)));
		return;
	}
	int result = interpolate(interp, (int)VAL_DATA(template_val));
	if (interp->error_flag)
		return;
	push(interp, make_string(result));

	DISPATCH(interp);
}

void p_gc(Interpreter *interp) {
	gc(interp);

	DISPATCH(interp);
}

void p_clear(Interpreter *interp) {
	interp->dsp = 0;

	DISPATCH(interp);
}

void unary_op(Interpreter *interp, Val operand, double (*function)(double), const char *name) {
	if (VAL_TAG(operand) == T_FLOAT) {
		push(interp, make_float(function(VAL_NUMBER(operand))));
	} else if (VAL_TAG(operand) == T_MATRIX) {
		Object *source = OBJECT_AT(VAL_DATA(operand));
		int target_handle = object_new_matrix(interp, source->matrix.rows, source->matrix.columns);
		if (interp->error_flag)
			return;

		Object *target = OBJECT_AT(target_handle);
		int num_elements = source->matrix.rows * source->matrix.columns;
		for (int i = 0; i < num_elements; i++)
			target->matrix.elements[i] = function(source->matrix.elements[i]);

		push(interp, make_matrix(target_handle));
	} else {
		fail(interp, "%s: expected a float or a matrix; got %s", name, tag_name(VAL_TAG(operand)));
	}
}

#define UNARY_MATH_OP_NAMED(cname, func, word) \
	void p_##cname(Interpreter *interp) { \
		POP(operand); \
		unary_op(interp, operand, func, word); \
		DISPATCH(interp); \
	}
#define UNARY_MATH_OP(name, func) UNARY_MATH_OP_NAMED(name, func, #name)

UNARY_MATH_OP(abs, fabs)
UNARY_MATH_OP(sqrt, sqrt)
UNARY_MATH_OP(exp, exp)
UNARY_MATH_OP(log, log10)
UNARY_MATH_OP(ln, log)
UNARY_MATH_OP(sin, sin)
UNARY_MATH_OP(cos, cos)
UNARY_MATH_OP(tan, tan)
UNARY_MATH_OP(tanh, tanh)
UNARY_MATH_OP(asin, asin)
UNARY_MATH_OP(acos, acos)
UNARY_MATH_OP(atan, atan)
UNARY_MATH_OP(round, round)
UNARY_MATH_OP(truncate, trunc)
UNARY_MATH_OP_NAMED(round_up, ceil, "round-up")
UNARY_MATH_OP_NAMED(round_down, floor, "round-down")
UNARY_MATH_OP_NAMED(inc_poly, scalar_inc, "1+")
UNARY_MATH_OP_NAMED(dec_poly, scalar_dec, "1-")
UNARY_MATH_OP_NAMED(sq_poly, scalar_sq, "sq")

void binary_op(Interpreter *interp, Val left, Val right, scalar_operator function, const char *name) {
	if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT) {
		push(interp, make_float(function(VAL_NUMBER(left), VAL_NUMBER(right))));
		return;
	}

	if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_MATRIX) {
		Object *a = OBJECT_AT(VAL_DATA(left));
		Object *b = OBJECT_AT(VAL_DATA(right));
		if (a->matrix.rows != b->matrix.rows || a->matrix.columns != b->matrix.columns) {
			fail(interp, "%s: matrix shapes differ (%dx%d vs %dx%d)", name,
			     a->matrix.rows, a->matrix.columns, b->matrix.rows, b->matrix.columns);
			return;
		}
		int target_handle = object_new_matrix(interp, a->matrix.rows, a->matrix.columns);
		if (interp->error_flag) return;
		Object *target = OBJECT_AT(target_handle);
		int num_elements = a->matrix.rows * a->matrix.columns;
		for (int i = 0; i < num_elements; i++)
			target->matrix.elements[i] = function(a->matrix.elements[i], b->matrix.elements[i]);
		push(interp, make_matrix(target_handle));
		return;
	}

	if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_FLOAT) {
		double scalar = VAL_NUMBER(right);
		Object *source = OBJECT_AT(VAL_DATA(left));
		int target_handle = object_new_matrix(interp, source->matrix.rows, source->matrix.columns);
		if (interp->error_flag) return;
		Object *target = OBJECT_AT(target_handle);
		int num_elements = source->matrix.rows * source->matrix.columns;
		for (int i = 0; i < num_elements; i++)
			target->matrix.elements[i] = function(source->matrix.elements[i], scalar);
		push(interp, make_matrix(target_handle));
		return;
	}

	if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_MATRIX) {
		double scalar = VAL_NUMBER(left);
		Object *source = OBJECT_AT(VAL_DATA(right));
		int target_handle = object_new_matrix(interp, source->matrix.rows, source->matrix.columns);
		if (interp->error_flag) return;
		Object *target = OBJECT_AT(target_handle);
		int num_elements = source->matrix.rows * source->matrix.columns;
		for (int i = 0; i < num_elements; i++)
			target->matrix.elements[i] = function(scalar, source->matrix.elements[i]);
		push(interp, make_matrix(target_handle));
		return;
	}

	fail(interp, "%s: expected floats or matrices; got %s and %s", name,
	     tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));
}

void p_power(Interpreter *interp) {
	POP(right);
	POP(left);
	binary_op(interp, left, right, pow, "^");

	DISPATCH(interp);
}

void p_divmod(Interpreter *interp) {
	POP(right);
	POP(left);
	if (VAL_TAG(left) != T_FLOAT || VAL_TAG(right) != T_FLOAT) {
		fail(interp, "%%: expected two floats; got %s and %s", tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));
		return;
	}

	double dividend = VAL_NUMBER(left);
	double divisor = VAL_NUMBER(right);
	if (divisor == 0.0) {
		fail(interp, "%%: division by zero");
		return;
	}

	double quotient = trunc(dividend / divisor);
	double remainder = dividend - quotient * divisor;
	push(interp, make_float(remainder));
	push(interp, make_float(quotient));

	DISPATCH(interp);
}

UNARY_FLOAT_OP(p_fabs, "fabs", fabs(n))
UNARY_FLOAT_OP(p_fsqrt, "fsqrt", sqrt(n))
UNARY_FLOAT_OP(p_fexp, "fexp", exp(n))
UNARY_FLOAT_OP(p_flog, "flog", log10(n))
UNARY_FLOAT_OP(p_fln, "fln", log(n))
UNARY_FLOAT_OP(p_fsin, "fsin", sin(n))
UNARY_FLOAT_OP(p_fcos, "fcos", cos(n))
UNARY_FLOAT_OP(p_ftan, "ftan", tan(n))
UNARY_FLOAT_OP(p_ftanh, "ftanh", tanh(n))
UNARY_FLOAT_OP(p_fasin, "fasin", asin(n))
UNARY_FLOAT_OP(p_facos, "facos", acos(n))
UNARY_FLOAT_OP(p_fatan, "fatan", atan(n))
UNARY_FLOAT_OP(p_fround, "fround", round(n))
UNARY_FLOAT_OP(p_ftruncate, "ftruncate", trunc(n))
UNARY_FLOAT_OP(p_fnegate, "fnegate", -n)
UNARY_FLOAT_OP(p_fround_up, "fround-up", ceil(n))
UNARY_FLOAT_OP(p_fround_down, "fround-down", floor(n))
UNARY_FLOAT_OP(p_inc, "f1+", n + 1.0)
UNARY_FLOAT_OP(p_dec, "f1-", n - 1.0)
UNARY_FLOAT_OP(p_sq, "fsq", n * n)

void p_fpow(Interpreter *interp) {
	if (interp->dsp < 2) {
		fail(interp, "f^: data stack underflow");
		return;
	}
	Val *left = &interp->data_stack[interp->dsp - 2];
	Val *right = &interp->data_stack[interp->dsp - 1];
	left->number = pow(left->number, right->number);
	interp->dsp--;

	DISPATCH(interp);
}

void p_fmodop(Interpreter *interp) {
	if (interp->dsp < 2) {
		fail(interp, "fmod: data stack underflow");
		return;
	}
	Val *left = &interp->data_stack[interp->dsp - 2];
	Val *right = &interp->data_stack[interp->dsp - 1];
	left->number = fmod(left->number, right->number);
	interp->dsp--;

	DISPATCH(interp);
}

static _Atomic uint64_t random_base_seed = 0x2545F4914F6CDD1DULL;
static _Atomic uint64_t random_stream_counter = 0;
static _Thread_local uint64_t random_state[4];
static _Thread_local int random_seeded = 0;

static uint64_t splitmix64(uint64_t *state) {
	uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}

static void random_ensure_seeded(void) {
	if (random_seeded)
		return;
	uint64_t stream = atomic_fetch_add(&random_stream_counter, 1);
	uint64_t expand = atomic_load(&random_base_seed) + stream * 0x9E3779B97F4A7C15ULL;
	for (int i = 0; i < 4; i++)
		random_state[i] = splitmix64(&expand);
	random_seeded = 1;
}

static inline uint64_t random_rotl(uint64_t x, int k) {
	return (x << k) | (x >> (64 - k));
}

static uint64_t random_next(void) {
	random_ensure_seeded();
	uint64_t result = random_rotl(random_state[1] * 5, 7) * 9;
	uint64_t t = random_state[1] << 17;
	random_state[2] ^= random_state[0];
	random_state[3] ^= random_state[1];
	random_state[1] ^= random_state[2];
	random_state[0] ^= random_state[3];
	random_state[2] ^= t;
	random_state[3] = random_rotl(random_state[3], 45);
	return result;
}

void p_seed(Interpreter *interp) {
	POP_INT(seed_value, "seed", "seed");
	atomic_store(&random_base_seed, (uint64_t)seed_value);
	atomic_store(&random_stream_counter, 0);
	random_seeded = 0;
	random_ensure_seeded();

	DISPATCH(interp);
}

void p_random(Interpreter *interp) {
	uint64_t bits = random_next() >> 11;
	push(interp, make_float((double)bits * 0x1.0p-53));

	DISPATCH(interp);
}

int random_below(int bound) {
	uint64_t range = (uint64_t)bound;
	uint64_t threshold = (0 - range) % range;
	uint64_t value;

	do {
		value = random_next();
	} while (value < threshold);

	return (int)(value % range);
}

void p_random_int(Interpreter *interp) {
	POP_INT(bound, "random-int", "bound");
	if (bound <= 0) {
		fail(interp, "random-int: bound must be positive; got %d", bound);
		return;
	}
	push(interp, make_float((double)random_below(bound)));

	DISPATCH(interp);
}

void p_now(Interpreter *interp) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	push(interp, make_float((double)ts.tv_sec + (double)ts.tv_nsec / 1e9));

	DISPATCH(interp);
}

void p_sleep(Interpreter *interp) {
	Val seconds_val = pop(interp);
	if (interp->error_flag)
		return;
	if (VAL_TAG(seconds_val) != T_FLOAT) {
		fail(interp, "sleep: expected a float; got %s", tag_name(VAL_TAG(seconds_val)));
		return;
	}

	double seconds = VAL_NUMBER(seconds_val);
	if (seconds > 0) {
		struct timespec request;
		request.tv_sec = (time_t)seconds;
		request.tv_nsec = (long)((seconds - (double)(time_t)seconds) * 1e9);
		nanosleep(&request, NULL);
	}

	DISPATCH(interp);
}

void p_env(Interpreter *interp) {
	POP_STRING(name, "env");
	const char *value = getenv(name->bytes);
	if (value == NULL)
		push(interp, make_tagged(T_NONE, 0));
	else
		push(interp, make_string(object_new_string(interp, value, (int)strlen(value))));

	DISPATCH(interp);
}

void p_env_set(Interpreter *interp) {
	POP_STRING(value, "env!");
	POP_STRING(name, "env!");
	if (setenv(name->bytes, value->bytes, 1) != 0)
		fail(interp, "env!: could not set %s", name->bytes);

	DISPATCH(interp);
}

void p_cd(Interpreter *interp) {
	POP_STRING(path, "cd");
	if (chdir(path->bytes) != 0)
		fail(interp, "cd: cannot change to %s", path->bytes);

	DISPATCH(interp);
}

void p_cwd(Interpreter *interp) {
	char buffer[PATH_MAX];
	if (getcwd(buffer, sizeof buffer) == NULL) {
		fail(interp, "cwd: cannot read working directory");
		return;
	}
	push(interp, make_string(object_new_string(interp, buffer, (int)strlen(buffer))));

	DISPATCH(interp);
}

void p_read_file(Interpreter *interp) {
	POP_STRING(path, "read-file");

	FILE *file = fopen(path->bytes, "rb");
	if (file == NULL) {
		fail(interp, "read-file: cannot open %s", path->bytes);
		return;
	}

	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	rewind(file);
	if (size < 0 || size > INT_MAX) {
		fclose(file);
		fail(interp, "read-file: cannot size %s", path->bytes);
		return;
	}

	int handle = object_new_string_uninit(interp, (int)size);
	if (interp->error_flag) {
		fclose(file);
		return;
	}

	Object *string = OBJECT_AT(handle);
	size_t got = fread(string->bytes, 1, (size_t)size, file);
	fclose(file);

	string->len = (int)got;
	string->bytes[got] = 0;
	push(interp, make_string(handle));

	DISPATCH(interp);
}

static void write_file(Interpreter *interp, const char *mode, const char *op) {
	POP_STRING(path, op);
	POP_STRING(content, op);

	FILE *file = fopen(path->bytes, mode);
	if (file == NULL) {
		fail(interp, "%s: cannot open %s", op, path->bytes);
		return;
	}

	size_t written = fwrite(content->bytes, 1, (size_t)content->len, file);
	fclose(file);

	if (written != (size_t)content->len)
		fail(interp, "%s: short write to %s", op, path->bytes);
}

void p_write_file(Interpreter *interp) {
	write_file(interp, "wb", "write-file");

	DISPATCH(interp);
}

void p_append_file(Interpreter *interp) {
	write_file(interp, "ab", "append-file");

	DISPATCH(interp);
}

static int tsv_row_to_array(Interpreter *interp, char *row, int row_length) {
	int cell_count = 1;
	for (int i = 0; i < row_length; i++)
		if (row[i] == '\t')
			cell_count++;

	int array_handle = object_new_array(interp, cell_count);
	if (interp->error_flag) return -1;

	Object *array = OBJECT_AT(array_handle);
	memset(array->items, 0, sizeof(Val) * (size_t)cell_count);
	gc_root_push(interp, make_array(array_handle));

	int cell_index = 0;
	int cell_start = 0;
	for (int i = 0; i <= row_length; i++) {
			if (i < row_length && row[i] != '\t')
				continue;

			row[i] = 0;
			char *cell = row + cell_start;
			int cell_length = i - cell_start;
			double number;

			if (cell_length == 0)
				array->items[cell_index] = make_tagged(T_NONE, 0);
			else if (parse_float(cell, &number))
				array->items[cell_index] = make_float(number);
			else
				array->items[cell_index] = make_string(object_new_string(interp, cell, cell_length));

			if (interp->error_flag) {
				gc_root_pop(interp);
				return -1;
			}

			cell_index++;
			cell_start = i + 1;
	}

	gc_root_pop(interp);
	return array_handle;
}

void p_read_tsv(Interpreter *interp) {
	POP_STRING(path, "read-tsv");

	FILE *file = fopen(path->bytes, "rb");
	if (file == NULL) {
		fail(interp, "read-tsv: cannot open %s", path->bytes);
		return;
	}

	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	rewind(file);
	if (size < 0 || size > INT_MAX) {
		fclose(file);
		fail(interp, "read-tsv: cannot size %s", path->bytes);
		return;
	}

	char *buffer = malloc((size_t)size + 1);
	if (buffer == NULL) {
		fclose(file);
		fail(interp, "read-tsv: out of memory");
		return;
	}
	int length = (int)fread(buffer, 1, (size_t)size, file);
	fclose(file);
	buffer[length] = 0;

	int row_count = 0;
	if (length > 0) {
		row_count = 1;
		for (int i = 0; i < length; i++)
			if (buffer[i] == '\n')
				row_count++;
		if (buffer[length - 1] == '\n')
			row_count--;
	}

	int outer_handle = object_new_array(interp, row_count);
	if (interp->error_flag) {
		free(buffer);
		return;
	}
	Object *outer = OBJECT_AT(outer_handle);
	memset(outer->items, 0, sizeof(Val) * (size_t)row_count);
	gc_root_push(interp, make_array(outer_handle));

	int row_index = 0;
	int row_start = 0;
	for (int i = 0; i <= length; i++) {
		if (i < length && buffer[i] != '\n')
			continue;
		if (i == length && i == row_start)
			break;
		int row_length = i - row_start;
		if (row_length > 0 && buffer[row_start + row_length - 1] == '\r')
			row_length--;
		buffer[row_start + row_length] = 0;
		int row_handle = tsv_row_to_array(interp, buffer + row_start, row_length);
		if (interp->error_flag) {
			gc_root_pop(interp);
			free(buffer);
			return;
		}
		outer->items[row_index++] = make_array(row_handle);
		row_start = i + 1;
	}

	gc_root_pop(interp);
	free(buffer);
	push(interp, make_array(outer_handle));

	DISPATCH(interp);
}

void p_write_tsv(Interpreter *interp) {
	POP_STRING(path, "write-tsv");
	POP_ARRAY(rows, "write-tsv");

	FILE *file = fopen(path->bytes, "wb");
	if (file == NULL) {
		fail(interp, "write-tsv: cannot open %s", path->bytes);
		return;
	}

	for (int r = 0; r < rows->len; r++) {
		Val row_val = rows->items[r];
		if (VAL_TAG(row_val) != T_ARRAY) {
			fclose(file);
			fail(interp, "write-tsv: row %d is %s, expected an array", r, tag_name(VAL_TAG(row_val)));
			return;
		}
		Object *row = OBJECT_AT(VAL_DATA(row_val));
		for (int c = 0; c < row->len; c++) {
			if (c > 0)
				fputc('\t', file);
			Val cell = row->items[c];
			switch (VAL_TAG(cell)) {
				case T_NONE:
					break;
				case T_FLOAT: {
					double number = VAL_NUMBER(cell);
					if (number == (double)(int64_t)number && number > -1e15 && number < 1e15)
						fprintf(file, "%lld", (long long)number);
					else
						fprintf(file, "%g", number);
					break;
				}
				case T_STRING: {
					Object *string = OBJECT_AT(VAL_DATA(cell));
					for (int b = 0; b < string->len; b++)
						if (string->bytes[b] == '\t' || string->bytes[b] == '\n') {
							fclose(file);
							fail(interp, "write-tsv: row %d cell %d contains a tab or newline", r, c);
							return;
						}
					fwrite(string->bytes, 1, (size_t)string->len, file);
					break;
				}
				default:
					fclose(file);
					fail(interp, "write-tsv: row %d cell %d is %s, cannot represent in TSV", r, c, tag_name(VAL_TAG(cell)));
					return;
			}
		}
		fputc('\n', file);
	}

	fclose(file);

	DISPATCH(interp);
}

static char *resolve_program_path(const char *name) {
	if (strchr(name, '/')) {
		size_t length = strlen(name);
		char *copy = malloc(length + 1);
		memcpy(copy, name, length + 1);
		return copy;
	}

	const char *path = getenv("PATH");
	if (!path || !*path)
		path = "/usr/bin:/bin";

	size_t name_len = strlen(name);
	for (const char *segment = path; ; ) {
		const char *colon = strchr(segment, ':');
		const char *dir = segment;
		size_t dir_len = colon ? (size_t)(colon - segment) : strlen(segment);
		if (dir_len == 0) {
			dir = ".";
			dir_len = 1;
		}

		char *candidate = malloc(dir_len + 1 + name_len + 1);
		memcpy(candidate, dir, dir_len);
		candidate[dir_len] = '/';
		memcpy(candidate + dir_len + 1, name, name_len + 1);

		if (access(candidate, X_OK) == 0)
			return candidate;
		free(candidate);

		if (!colon)
			return NULL;
		segment = colon + 1;
	}
}

void p_start_process(Interpreter *interp) {
	PEEK_TYPE_AT(argv_val, 0, "start-process", T_ARRAY);
	Object *argv_array = OBJECT_AT(VAL_DATA(argv_val));
	int argc = argv_array->len;
	if (argc < 1) {
		fail(interp, "start-process: argv needs at least the program name");
		return;
	}

	char **argv = malloc(sizeof(char *) * (size_t)(argc + 1));
	for (int i = 0; i < argc; i++) {
		if (VAL_TAG(argv_array->items[i]) != T_STRING) {
			free(argv);
			fail(interp, "start-process: argv element %d is %s, expected a string",
					i, tag_name(VAL_TAG(argv_array->items[i])));
			return;
		}
		argv[i] = OBJECT_AT(VAL_DATA(argv_array->items[i]))->bytes;
	}
	argv[argc] = NULL;

	char *program_path = resolve_program_path(argv[0]);
	if (!program_path) {
		const char *name = argv[0];
		free(argv);
		fail(interp, "start-process: %s: command not found", name);
		return;
	}

	int in_pipe[2];
	int out_pipe[2];
	int err_pipe[2];
	if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0 || pipe(err_pipe) < 0) {
		free(argv);
		free(program_path);
		fail(interp, "start-process: pipe failed");
		return;
	}

	int pipe_fds[6] = { in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1] };
	for (int i = 0; i < 6; i++)
		fcntl(pipe_fds[i], F_SETFD, FD_CLOEXEC);

	pid_t pid = fork();
	if (pid < 0) {
		free(argv);
		free(program_path);
		fail(interp, "start-process: fork failed");
		return;
	}

	if (pid == 0) {
		dup2(in_pipe[0], 0);
		dup2(out_pipe[1], 1);
		dup2(err_pipe[1], 2);
		close(in_pipe[0]);
		close(in_pipe[1]);
		close(out_pipe[0]);
		close(out_pipe[1]);
		close(err_pipe[0]);
		close(err_pipe[1]);
		execv(program_path, argv);
		_exit(127);
	}

	close(in_pipe[0]);
	close(out_pipe[1]);
	close(err_pipe[1]);
	free(argv);
	free(program_path);

	NEW_FRAME(proc_handle, proc);
	frame_put(proc, intern_symbol(interp, "pid"), make_float((double)pid));
	frame_put(proc, intern_symbol(interp, "in"), make_stream(in_pipe[1]));
	frame_put(proc, intern_symbol(interp, "out"), make_stream(out_pipe[0]));
	frame_put(proc, intern_symbol(interp, "err"), make_stream(err_pipe[0]));

	interp->data_stack[interp->dsp - 1] = make_frame(proc_handle);

	DISPATCH(interp);
}

void p_write(Interpreter *interp) {
	PEEK_AT(stream_val, 0, "write");
	if (VAL_TAG(stream_val) != T_STREAM) {
		fail(interp, "write: expected a stream; got %s", tag_name(VAL_TAG(stream_val)));
		return;
	}
	PEEK_TYPE_AT(string_val, 1, "write", T_STRING);
	int file_descriptor = (int)VAL_DATA(stream_val);
	Object *string = OBJECT_AT(VAL_DATA(string_val));

	int total_written = 0;
	while (total_written < string->len) {
		ssize_t bytes_written = write(file_descriptor, string->bytes + total_written, (size_t)(string->len - total_written));
		if (bytes_written < 0) {
			if (errno == EINTR)
				continue;
			fail(interp, "write: %s", strerror(errno));
			return;
		}
		total_written += (int)bytes_written;
	}

	interp->dsp -= 2;

	DISPATCH(interp);
}

void p_read(Interpreter *interp) {
	PEEK_AT(stream_val, 0, "read");
	if (VAL_TAG(stream_val) != T_STREAM) {
		fail(interp, "read: expected a stream; got %s", tag_name(VAL_TAG(stream_val)));
		return;
	}
	int file_descriptor = (int)VAL_DATA(stream_val);

	int length = 0;
	int capacity = 1 << 16;
	char *buffer = malloc((size_t)capacity);
	if (!buffer) {
		fail(interp, "read: out of memory");
		return;
	}
	while (1) {
		if (length == capacity) {
			if (capacity > INT_MAX / 2) {
				free(buffer);
				fail(interp, "read: stream exceeds %d bytes", INT_MAX);
				return;
			}
			capacity *= 2;
			char *grown = realloc(buffer, (size_t)capacity);
			if (!grown) {
				free(buffer);
				fail(interp, "read: out of memory");
				return;
			}
			buffer = grown;
		}

		ssize_t bytes_read = read(file_descriptor, buffer + length, (size_t)(capacity - length));
		if (bytes_read < 0) {
			if (errno == EINTR)
				continue;
			free(buffer);
			fail(interp, "read: %s", strerror(errno));
			return;
		}

		if (bytes_read == 0)
			break;
		length += (int)bytes_read;
	}

	int handle = object_new_string(interp, buffer ? buffer : "", length);
	free(buffer);
	if (interp->error_flag) return;

	interp->data_stack[interp->dsp - 1] = make_string(handle);

	DISPATCH(interp);
}

void p_close(Interpreter *interp) {
	PEEK_AT(stream_val, 0, "close");
	if (VAL_TAG(stream_val) != T_STREAM) {
		fail(interp, "close: expected a stream; got %s", tag_name(VAL_TAG(stream_val)));
		return;
	}
	close((int)VAL_DATA(stream_val));
	interp->dsp -= 1;

	DISPATCH(interp);
}

void p_wait(Interpreter *interp) {
	POP_INT(pid, "wait", "pid");
	if (pid <= 0) {
		fail(interp, "wait: invalid pid %d (expected a spawned process id)", pid);
		return;
	}

	int status;
	pid_t result;
	do {
		result = waitpid((pid_t)pid, &status, 0);
	} while (result < 0 && errno == EINTR);

	if (result < 0) {
		fail(interp, "wait: %s", strerror(errno));
		return;
	}

	int code;
	if (WIFEXITED(status))
		code = WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		code = 128 + WTERMSIG(status);
	else
		code = -1;
	push(interp, make_float((double)code));

	DISPATCH(interp);
}

void p_stop_process(Interpreter *interp) {
	POP_INT(pid, "stop", "pid");
	if (pid <= 0) {
		fail(interp, "stop: invalid pid %d (expected a spawned process id)", pid);
		return;
	}

	kill((pid_t)pid, SIGKILL);

	int status;
	pid_t result;
	do {
		result = waitpid((pid_t)pid, &status, 0);
	} while (result < 0 && errno == EINTR);

	if (result < 0) {
		fail(interp, "stop: %s", strerror(errno));
		return;
	}

	int code;
	if (WIFEXITED(status))
		code = WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		code = 128 + WTERMSIG(status);
	else
		code = -1;
	push(interp, make_float((double)code));

	DISPATCH(interp);
}

void p_running(Interpreter *interp) {
	POP_INT(pid, "running?", "pid");

	siginfo_t info;
	info.si_pid = 0;
	int result;
	do {
		result = waitid(P_PID, (id_t)pid, &info, WEXITED | WNOHANG | WNOWAIT);
	} while (result < 0 && errno == EINTR);

	push(interp, make_bool(result == 0 && info.si_pid == 0));

	DISPATCH(interp);
}

