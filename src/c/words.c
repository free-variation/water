#include "water.h"


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

static double scalar_negate(double x) { return -x; }
static double scalar_inc(double x) { return x + 1.0; }
static double scalar_dec(double x) { return x - 1.0; }
static double scalar_sq(double x) { return x * x; }
static double scalar_lt(double a, double b) { return a < b; }
static double scalar_gt(double a, double b) { return a > b; }
static double scalar_add(double a, double b) { return a + b; }
static double scalar_sub(double a, double b) { return a - b; }
static double scalar_mul(double a, double b) { return a * b; }
static double scalar_div(double a, double b) { return a / b; }

static void unwrap_quantity(Val value, int is_quantity, Val *magnitude, int *unit) {
	if (is_quantity) {
		int slot = (int)VAL_DATA(value);
		*magnitude = pairs.table[slot].head;
		*unit = (int)pairs.table[slot].tail.bits;
	} else {
		*magnitude = value;
		*unit = 0;
	}
}

static void unary_quantity_op(Interpreter *interp, Val quantity, double (*function)(double), const char *name, int result_unit) {
	int slot = (int)VAL_DATA(quantity);

	gc_root_push(interp, quantity);
	unary_op(interp, pairs.table[slot].head, function, name);
	gc_root_pop(interp);

	if (interp->error_flag)
		return;

	push_quantity(interp, pop(interp), result_unit);
}

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
	else {
		int left_is_quantity  = VAL_TAG(left)  == T_QUANTITY;
		int right_is_quantity = VAL_TAG(right) == T_QUANTITY;

		if (left_is_quantity && right_is_quantity) {
			Val left_magnitude, right_magnitude;
			int left_unit, right_unit;
			unwrap_quantity(left,  1, &left_magnitude,  &left_unit);
			unwrap_quantity(right, 1, &right_magnitude, &right_unit);
			int base = interp->dsp - 2;

			if (left_unit != right_unit) {
				double factor;
				if (!unit_conversion(right_unit, left_unit, &factor)) {
					fail(interp, "+ : unit mismatch");
					return;
				}
				binary_op(interp, right_magnitude, make_float(factor), scalar_mul, "*");
				if (interp->error_flag) return;
				right_magnitude = interp->data_stack[interp->dsp - 1];
			}

			binary_op(interp, left_magnitude, right_magnitude, scalar_add, "+");
			if (interp->error_flag) return;

			Val sum = pop(interp);
			interp->dsp = base;
			push_quantity(interp, sum, left_unit);
		}
		else if (left_is_quantity || right_is_quantity)
			fail(interp, "+ : cannot add a quantity and a plain number");
		else
			fail(interp, "+ : expected two floats, two strings, two sets, two matrices, scalar/matrix, or two arrays; got %s and %s",
					tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));
	}

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
	else {
		int left_is_quantity  = VAL_TAG(left)  == T_QUANTITY;
		int right_is_quantity = VAL_TAG(right) == T_QUANTITY;

		if (left_is_quantity && right_is_quantity) {
			Val left_magnitude, right_magnitude;
			int left_unit, right_unit;
			unwrap_quantity(left,  1, &left_magnitude,  &left_unit);
			unwrap_quantity(right, 1, &right_magnitude, &right_unit);
			int base = interp->dsp - 2;

			if (left_unit != right_unit) {
				double factor;
				if (!unit_conversion(right_unit, left_unit, &factor)) {
					fail(interp, "- : unit mismatch");
					return;
				}
				binary_op(interp, right_magnitude, make_float(factor), scalar_mul, "*");
				if (interp->error_flag) return;
				right_magnitude = interp->data_stack[interp->dsp - 1];
			}

			binary_op(interp, left_magnitude, right_magnitude, scalar_sub, "-");
			if (interp->error_flag) return;

			Val difference = pop(interp);
			interp->dsp = base;
			push_quantity(interp, difference, left_unit);
		}
		else if (left_is_quantity || right_is_quantity)
			fail(interp, "- : cannot subtract a quantity and a plain number");
		else
			fail(interp, "- : expected two floats, two sets, two matrices, or scalar/matrix; got %s and %s",
					tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));
	}

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
	else {
		int left_is_quantity  = VAL_TAG(left)  == T_QUANTITY;
		int right_is_quantity = VAL_TAG(right) == T_QUANTITY;

		if (left_is_quantity || right_is_quantity) {
			Val left_magnitude, right_magnitude;
			int left_unit, right_unit;
			unwrap_quantity(left,  left_is_quantity,  &left_magnitude,  &left_unit);
			unwrap_quantity(right, right_is_quantity, &right_magnitude, &right_unit);

			binary_op(interp, left_magnitude, right_magnitude, scalar_mul, "*");
			if (interp->error_flag) return;

			Val product = pop(interp);
			interp->dsp -= 2;
			push_quantity(interp, product, unit_multiply(left_unit, right_unit));
		}
		else
			fail(interp, "* : expected two floats, two sets, two matrices, or scalar/matrix; got %s and %s",
					tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));
	}

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
	else {
		int left_is_quantity  = VAL_TAG(left)  == T_QUANTITY;
		int right_is_quantity = VAL_TAG(right) == T_QUANTITY;

		if (left_is_quantity || right_is_quantity) {
			Val left_magnitude, right_magnitude;
			int left_unit, right_unit;
			unwrap_quantity(left,  left_is_quantity,  &left_magnitude,  &left_unit);
			unwrap_quantity(right, right_is_quantity, &right_magnitude, &right_unit);

			if (VAL_TAG(right_magnitude) == T_FLOAT && VAL_NUMBER(right_magnitude) == 0.0) {
				fail(interp, "/ : division by zero");
				return;
			}

			binary_op(interp, left_magnitude, right_magnitude, scalar_div, "/");
			if (interp->error_flag) return;

			Val quotient = pop(interp);
			interp->dsp -= 2;
			push_quantity(interp, quotient, unit_divide(left_unit, right_unit));
		}
		else
			fail(interp, "/ : expected two floats, two matrices, or scalar/matrix; got %s and %s",
					tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));
	}

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

/* lt/gt are element-wise on matrices (a 1.0/0.0 matrix, same shape or scalar
   broadcast, via binary_op) after the float fast path; other types keep the
   structural val_cmp bool. `=` stays structural so matrices order for set
   membership. */
#define MATRIX_COMPARISON_OP(name, op, sfn, word) \
	void name(Interpreter *interp) { \
		POP(right); \
		POP(left); \
		if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT) \
			push(interp, make_bool(VAL_NUMBER(left) op VAL_NUMBER(right))); \
		else if (VAL_TAG(left) == T_MATRIX || VAL_TAG(right) == T_MATRIX) \
			binary_op(interp, left, right, sfn, word); \
		else \
			push(interp, make_bool(val_cmp(interp, left, right) op 0)); \
		DISPATCH(interp); \
	}
MATRIX_COMPARISON_OP(p_lt, <, scalar_lt, "lt")
MATRIX_COMPARISON_OP(p_gt, >, scalar_gt, "gt")

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

static int grid_if_matrix(FILE *out, Val value) {
	if (VAL_TAG(value) == T_MATRIX) {
		print_matrix_grid(out, OBJECT_AT(VAL_DATA(value)), 0);
		return 1;
	}
	if (VAL_TAG(value) == T_QUANTITY) {
		int slot = (int)VAL_DATA(value);
		if (VAL_TAG(pairs.table[slot].head) == T_MATRIX) {
			print_matrix_grid(out, OBJECT_AT(VAL_DATA(pairs.table[slot].head)), (int)pairs.table[slot].tail.bits);
			return 1;
		}
	}
	return 0;
}

void p_dot(Interpreter *interp) {
	POP(value);
	if (!grid_if_matrix(stdout, value)) {
		if (VAL_TAG(value) == T_FRAME) {
			print_frame_pretty(stdout, interp, OBJECT_AT(VAL_DATA(value)), 0);
			putchar('\n');
		} else if (VAL_TAG(value) == T_ARRAY) {
			pretty_print_array(stdout, interp, value);
			putchar(' ');
		} else {
			print_val(stdout, interp, value);
			putchar(' ');
		}
	}
	fflush(stdout);

	DISPATCH(interp);
}

void p_dot_all(Interpreter *interp) {
	int saved = print_truncate;
	print_truncate = 0;
	POP(value);
	if (!grid_if_matrix(stdout, value)) {
		print_val(stdout, interp, value);
		putchar(' ');
	}
	fflush(stdout);
	print_truncate = saved;

	DISPATCH(interp);
}

void p_render(Interpreter *interp) {
	PEEK_AT(value, 0, "render");

	char *buffer = NULL;
	size_t size = 0;
	FILE *out = open_memstream(&buffer, &size);
	if (!out) {
		fail(interp, "render: out of memory");
		return;
	}

	int saved_truncate = print_truncate;
	print_truncate = 0;
	int gridded = grid_if_matrix(out, value);
	if (!gridded) {
		if (VAL_TAG(value) == T_FRAME)
			print_frame_pretty(out, interp, OBJECT_AT(VAL_DATA(value)), 0);
		else if (VAL_TAG(value) == T_ARRAY)
			pretty_print_array(out, interp, value);
		else
			print_val(out, interp, value);
	}
	print_truncate = saved_truncate;

	fclose(out);

	int length = (int)size;
	if (gridded && length > 0 && buffer[length - 1] == '\n')
		length--;

	int handle = object_new_string(interp, buffer ? buffer : "", length);
	free(buffer);
	if (interp->error_flag)
		return;

	interp->data_stack[interp->dsp - 1] = make_string(handle);

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
		print_val_inspect(stdout, interp, interp->data_stack[i]);
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

static void see_source_render(FILE *out, Interpreter *interp, int target_cfa) {
	const char *name = name_of(target_cfa);

	cfa_handler handler = (cfa_handler)vocab.dict[target_cfa];
	if (handler == docol) {
		if (!name) {
			fprintf(out, "[: ... :]  \\ anonymous, no source\n");
		} else {
			int src_idx = (int)WORD_SOURCE(target_cfa);
			if (src_idx > 0)
				fprintf(out, ": %s%s;\n", name, &vocab.source_pool[src_idx]);
			else
				fprintf(out, ": %s ... ;  \\ no source captured\n", name);
		}
	} else if (handler == dovar) {
		Val value;
		value.bits = (uint64_t)vocab.dict[target_cfa + 1];
		fprintf(out, "variable %s  \\ current value: ", name ? name : "?");
		print_val(out, interp, value);
		putc('\n', out);
	} else if (handler == dosym) {
		fprintf(out, "symbol %s\n", name ? name : "?");
	} else {
		fprintf(out, "%s is a primitive\n", name ? name : "?");
	}
}

void p_see(Interpreter *interp) {
	POP_XT(target_cfa, "see");
	see_source_render(stdout, interp, target_cfa);
	fflush(stdout);

	DISPATCH(interp);
}

void p_see_to_string(Interpreter *interp) {
	POP_XT(target_cfa, "see>string");
	int handle = capture_render(interp, see_source_render, target_cfa);
	if (interp->error_flag)
		return;
	push(interp, make_string(handle));

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

static int parse_format_spec(const char *spec, int len, char *conv_out) {
	int i = 0;
	while(i < len && (spec[i] == '-' || spec[i] == '+' || spec[i] == ' '
				|| spec[i] == '#' || spec[i] == '0'))
		i++;

	while(i < len && isdigit((unsigned char)spec[i]))
		i++;

	if (i < len && spec[i] == '.') {
		i++;
		while (i < len && isdigit((unsigned char)spec[i]))
			i++;
	}

	char conv = 0;
	if (i < len && strchr("fFeEgGdis", spec[i])) {
		conv = spec[i];
		i++;
	}

	if (i != len)
		return -1;

	*conv_out = conv;
	return 0;
}


static void interp_render_with_spec(Interpreter *interp, Val value,
		const char *spec, int spec_len,
		char **out_buffer, int *capacity, int *out_length) {
	if (spec_len >= 64) {
		fail(interp, "format: spec too long");
		return;
	}
	char cspec[64];
	memcpy(cspec, spec, (size_t)spec_len);
	cspec[spec_len] = 0;

	char conv;
	if (parse_format_spec(cspec, spec_len, &conv) != 0) {
		fail(interp, "format: bad spec '%s'", cspec);
		return;
	}

	char fmt[80];
	char stackbuf[256];
	char *rendered = stackbuf;
	int n;

	if (conv == 'd' || conv == 'i') {
		if (VAL_TAG(value) != T_FLOAT) {
			fail(interp, "format: {%s} needs a float; got %s", cspec, tag_name(VAL_TAG(value)));
			return;
		}
		cspec[spec_len - 1] = 0;
		snprintf(fmt, sizeof fmt, "%%%slld", cspec);
		long long as_int = (long long)VAL_NUMBER(value);
		n = snprintf(stackbuf, sizeof stackbuf, fmt, as_int);
		if (n >= (int)sizeof stackbuf) {
			rendered = malloc((size_t)n + 1);
			snprintf(rendered, (size_t)n + 1, fmt, as_int);
		}
	} else if (conv && conv != 's') {
		if (VAL_TAG(value) != T_FLOAT) {
			fail(interp, "format: {%s} needs a float; got %s", cspec, tag_name(VAL_TAG(value)));
			return;
		}
		double as_float = VAL_NUMBER(value);
		snprintf(fmt, sizeof fmt, "%%%s", cspec);
		n = snprintf(stackbuf, sizeof stackbuf, fmt, as_float);
		if (n >= (int)sizeof stackbuf) {
			rendered = malloc((size_t)n + 1);
			snprintf(rendered, (size_t)n + 1, fmt, as_float);
		}
	} else {
		int text_cap = 64;
		char *text = malloc(text_cap);
		int text_len = 0;
		interp_render_val(interp, value, &text, &text_cap, &text_len);
		interp_append(interp, &text, &text_cap, &text_len, "", 1);
		if (interp->error_flag) {
			free(text);
			return;
		}
		if (conv == 's')
			snprintf(fmt, sizeof fmt, "%%%s", cspec);
		else
			snprintf(fmt, sizeof fmt, "%%%ss", cspec);
		n = snprintf(stackbuf, sizeof stackbuf, fmt, text);
		if (n >= (int)sizeof stackbuf) {
			rendered = malloc((size_t)n + 1);
			snprintf(rendered, (size_t)n + 1, fmt, text);
		}
		free(text);
	}

	if (n < 0)
		n = 0;
	interp_append(interp, out_buffer, capacity, out_length, rendered, n);
	if (rendered != stackbuf)
		free(rendered);
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
			if (saw_digit && scan < template->len
					&& (template->bytes[scan] == '}' || template->bytes[scan] == ':')) {
				const char *spec = NULL;
				int spec_len = 0;
				int close = scan;
				if (template->bytes[scan] == ':') {
					int spec_start = scan + 1;
					close = spec_start;
					while (close < template->len && template->bytes[close] != '}')
						close++;
					spec = &template->bytes[spec_start];
					spec_len = close - spec_start;
				}
				if (close < template->len && template->bytes[close] == '}') {
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
					Val value = interp->data_stack[stack_index];
					if (spec)
						interp_render_with_spec(interp, value, spec, spec_len, &out_buffer, &capacity, &out_length);
					else
						interp_render_val(interp, value, &out_buffer, &capacity, &out_length);
					cursor = close + 1;
					continue;
				}
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

#define UNARY_QUANTITY_OP(cname, func, word, result_unit) \
	void p_##cname(Interpreter *interp) { \
		if (interp->dsp >= 1 && VAL_TAG(interp->data_stack[interp->dsp - 1]) == T_FLOAT) { \
			Val *top = &interp->data_stack[interp->dsp - 1]; \
			*top = make_float(func(top->number)); \
			DISPATCH(interp); \
		} \
		POP(operand); \
		if (VAL_TAG(operand) == T_QUANTITY) { \
			int unit = (result_unit); \
			if (interp->error_flag) return; \
			unary_quantity_op(interp, operand, func, word, unit); \
		} else \
			unary_op(interp, operand, func, word); \
		DISPATCH(interp); \
	}

UNARY_QUANTITY_OP(neg,  scalar_negate, "negate", (int)pairs.table[VAL_DATA(operand)].tail.bits)
UNARY_QUANTITY_OP(abs,  fabs,          "abs",    (int)pairs.table[VAL_DATA(operand)].tail.bits)
UNARY_QUANTITY_OP(sqrt, sqrt,          "sqrt",   unit_pow(interp, (int)pairs.table[VAL_DATA(operand)].tail.bits, 1, 2))
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

static int rational_of_double(double value, int *numerator, int *denominator) {
	for (int candidate = 1; candidate <= 64; candidate++) {
		double scaled = value * candidate;
		double rounded = round(scaled);
		if (fabs(scaled - rounded) < 1e-9) {
			*numerator = (int)rounded;
			*denominator = candidate;
			return 1;
		}
	}
	return 0;
}

void p_power(Interpreter *interp) {
	POP(right);
	POP(left);

	if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT) {
		push(interp, make_float(pow(VAL_NUMBER(left), VAL_NUMBER(right))));
		DISPATCH(interp);
		return;
	}

	if (VAL_TAG(left) == T_QUANTITY) {
		if (VAL_TAG(right) != T_FLOAT) {
			fail(interp, "^ : exponent must be a number; got %s", tag_name(VAL_TAG(right)));
			return;
		}

		int numerator, denominator;
		if (!rational_of_double(VAL_NUMBER(right), &numerator, &denominator)) {
			fail(interp, "^ : exponent must be a simple rational");
			return;
		}

		int slot = (int)VAL_DATA(left);
		int unit = unit_pow(interp, (int)pairs.table[slot].tail.bits, numerator, denominator);
		if (interp->error_flag) return;

		gc_root_push(interp, left);
		binary_op(interp, pairs.table[slot].head, right, pow, "^");
		gc_root_pop(interp);
		if (interp->error_flag) return;

		push_quantity(interp, pop(interp), unit);
		DISPATCH(interp);
		return;
	}

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

