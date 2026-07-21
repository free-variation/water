#include "water.h"


int string_concat(Interpreter *interp, int left_handle, int right_handle) {
	Object *left = OBJECT_AT(left_handle);
	Object *right = OBJECT_AT(right_handle);
	int combined_length = left->len + right->len;

	char *buffer;
	MALLOC_OR_FAIL_RETURNING(interp, buffer, (size_t)combined_length + 1, -1);
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
static double scalar_eq(double a, double b) { return a == b; }
static double scalar_nan(double element) { return isnan(element) ? 1.0 : 0.0; }
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

static Val scale_collapsed_magnitude(Val magnitude, double factor) {
	if (VAL_TAG(magnitude) == T_MATRIX) {
		Object *result = OBJECT_AT(VAL_DATA(magnitude));
		int n = result->matrix.rows * result->matrix.columns;
		for (int i = 0; i < n; i++)
			result->matrix.elements[i] *= factor;
		return magnitude;
	}

	return make_float(VAL_NUMBER(magnitude) * factor);
}

static int quantity_additive_op(Interpreter *interp, Val left, Val right,
		scalar_operator scalar_op, const char *op, const char *verb) {
	int left_is_quantity  = VAL_TAG(left)  == T_QUANTITY;
	int right_is_quantity = VAL_TAG(right) == T_QUANTITY;
	if (!left_is_quantity && !right_is_quantity)
		return 0;

	if (!left_is_quantity || !right_is_quantity) {
		fail(interp, "cannot %s a quantity and a plain number", verb);
		return 1;
	}

	Val left_magnitude, right_magnitude;
	int left_unit, right_unit;
	unwrap_quantity(left,  1, &left_magnitude,  &left_unit);
	unwrap_quantity(right, 1, &right_magnitude, &right_unit);
	int base = interp->dsp - 2;

	if (left_unit != right_unit) {
		double factor;
		if (!unit_conversion(right_unit, left_unit, &factor)) {
			fail(interp, "unit mismatch");
			return 1;
		}
		binary_op(interp, right_magnitude, make_float(factor), scalar_mul, "*");
		if (interp->error_flag) return 1;
		right_magnitude = interp->data_stack[interp->dsp - 1];
	}

	binary_op(interp, left_magnitude, right_magnitude, scalar_op, op);
	if (interp->error_flag) return 1;

	Val combined = pop(interp);
	interp->dsp = base;
	push_quantity(interp, combined, left_unit);
	return 1;
}

static int quantity_multiplicative_op(Interpreter *interp, Val left, Val right,
		scalar_operator scalar_op, int (*unit_op)(Interpreter *, int, int, double *),
		const char *op, int guards_zero) {
	int left_is_quantity  = VAL_TAG(left)  == T_QUANTITY;
	int right_is_quantity = VAL_TAG(right) == T_QUANTITY;
	if (!left_is_quantity && !right_is_quantity)
		return 0;

	Val left_magnitude, right_magnitude;
	int left_unit, right_unit;
	unwrap_quantity(left,  left_is_quantity,  &left_magnitude,  &left_unit);
	unwrap_quantity(right, right_is_quantity, &right_magnitude, &right_unit);

	if (guards_zero && VAL_TAG(right_magnitude) == T_FLOAT && VAL_NUMBER(right_magnitude) == 0.0) {
		fail(interp, "division by zero");
		return 1;
	}

	binary_op(interp, left_magnitude, right_magnitude, scalar_op, op);
	if (interp->error_flag) return 1;

	Val combined = pop(interp);
	interp->dsp -= 2;
	double collapse_factor;
	int combined_unit = unit_op(interp, left_unit, right_unit, &collapse_factor);
	if (interp->error_flag) return 1;
	if (collapse_factor != 1.0)
		combined = scale_collapsed_magnitude(combined, collapse_factor);
	push_quantity(interp, combined, combined_unit);
	return 1;
}

static void unary_quantity_op(Interpreter *interp, Val quantity, double (*function)(double), int result_unit) {
	int slot = (int)VAL_DATA(quantity);

	gc_root_push(interp, quantity);
	unary_op(interp, pairs.table[slot].head, function);
	gc_root_pop(interp);

	if (interp->error_flag)
		return;

	push_quantity(interp, pop(interp), result_unit);
}

void p_add(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val left = chain_sp[-2];
	Val right = chain_sp[-1];

	if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT) {
		chain_sp[-2] = make_float(VAL_NUMBER(left) + VAL_NUMBER(right));
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
	}

	SYNC_REGISTERS(interp, chain_ip, chain_sp);
	if (VAL_TAG(left) == T_STRING && VAL_TAG(right) == T_STRING) {
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
	else if (!quantity_additive_op(interp, left, right, scalar_add, "+", "add"))
		fail(interp, "expected two floats, two strings, two sets, two matrices, scalar/matrix, or two arrays; got %s and %s",
				tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));

	DISPATCH(interp);
}

void p_sub(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val left = chain_sp[-2];
	Val right = chain_sp[-1];

	if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT) {
		chain_sp[-2] = make_float(VAL_NUMBER(left) - VAL_NUMBER(right));
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
	}

	SYNC_REGISTERS(interp, chain_ip, chain_sp);
	if (VAL_TAG(left) == T_SET && VAL_TAG(right) == T_SET) {
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
	else if (!quantity_additive_op(interp, left, right, scalar_sub, "-", "subtract"))
		fail(interp, "expected two floats, two sets, two matrices, or scalar/matrix; got %s and %s",
				tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));

	DISPATCH(interp);
}

void p_mul(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val left = chain_sp[-2];
	Val right = chain_sp[-1];

	if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT) {
		chain_sp[-2] = make_float(VAL_NUMBER(left) * VAL_NUMBER(right));
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
	}

	SYNC_REGISTERS(interp, chain_ip, chain_sp);
	if (VAL_TAG(left) == T_SET && VAL_TAG(right) == T_SET) {
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
		if (!quantity_multiplicative_op(interp, left, right, scalar_mul, unit_multiply, "*", 0))
			fail(interp, "expected two floats, two sets, two matrices, or scalar/matrix; got %s and %s",
					tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));
	}

	DISPATCH(interp);
}

void p_div(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val left = chain_sp[-2];
	Val right = chain_sp[-1];
	if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT) {
		if (VAL_NUMBER(right) == 0.0) {
			SYNC_REGISTERS(interp, chain_ip, chain_sp);
			fail(interp, "division by zero");
			return;
		}
		chain_sp[-2] = make_float(VAL_NUMBER(left) / VAL_NUMBER(right));
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
	}

	SYNC_REGISTERS(interp, chain_ip, chain_sp);
	if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_MATRIX) {
		Object *divisor = OBJECT_AT(VAL_DATA(right));
		int n = divisor->matrix.rows * divisor->matrix.columns;
		for (int i = 0; i < n; i++) {
			if (divisor->matrix.elements[i] == 0.0) {
				fail(interp, "division by zero (matrix element %d)", i);
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
			fail(interp, "division by zero");
			return;
		}
		BROADCAST_MATRIX_OP_SCALAR(/);
	}
	else {
		if (!quantity_multiplicative_op(interp, left, right, scalar_div, unit_divide, "/", 1))
			fail(interp, "expected two floats, two matrices, or scalar/matrix; got %s and %s",
					tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));
	}

	DISPATCH(interp);
}

#define INPLACE_OP(name, word, op) \
	void name(DISPATCH_ARGS) { \
		POP(right); \
		POP(left); \
		if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_MATRIX) { \
			Object *left_matrix = OBJECT_AT(VAL_DATA(left)); \
			Object *right_matrix = OBJECT_AT(VAL_DATA(right)); \
			if (left_matrix->matrix.rows != right_matrix->matrix.rows || left_matrix->matrix.columns != right_matrix->matrix.columns) { \
				fail(interp, "matrix shapes differ (%dx%d vs %dx%d)", left_matrix->matrix.rows, left_matrix->matrix.columns, right_matrix->matrix.rows, right_matrix->matrix.columns); \
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
			fail(interp, "expected a matrix operand; got %s and %s", tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right))); \
		} \
		DISPATCH(interp); \
	}

INPLACE_OP(p_add_inplace, "+!", +)
INPLACE_OP(p_sub_inplace, "-!", -)
INPLACE_OP(p_mul_inplace, "*!", *)
INPLACE_OP(p_div_inplace, "/!", /)

#define BINARY_FLOAT_OP(name, opname, expr) \
	void name(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2); \
		double a = chain_sp[-2].number; \
		double b = chain_sp[-1].number; \
		chain_sp[-2].number = (expr); \
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1); \
	}

BINARY_FLOAT_OP(p_add_f, "f+", a + b)
BINARY_FLOAT_OP(p_sub_f, "f-", a - b)
BINARY_FLOAT_OP(p_mul_f, "f*", a * b)
BINARY_FLOAT_OP(p_fpow, "f^", pow(a, b))
BINARY_FLOAT_OP(p_fmodop, "fmod", fmod(a, b))

BINARY_FLOAT_OP(p_eq_f, "feq", a == b)
BINARY_FLOAT_OP(p_lt_f, "flt", a < b)
BINARY_FLOAT_OP(p_gt_f, "fgt", a > b)

#define BITWISE_BINARY_OP(name, opname, op) \
	void name(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2); \
		int64_t a = (int64_t)chain_sp[-2].number; \
		int64_t b = (int64_t)chain_sp[-1].number; \
		chain_sp[-2].number = (double)(a op b); \
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1); \
	}
BITWISE_BINARY_OP(p_bit_and, "bit-and", &)
BITWISE_BINARY_OP(p_bit_or, "bit-or", |)
BITWISE_BINARY_OP(p_bit_xor, "bit-xor", ^)
BITWISE_BINARY_OP(p_lshift, "lshift", <<)
BITWISE_BINARY_OP(p_rshift, "rshift", >>)

void p_bit_not(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	chain_sp[-1].number = (double)(~(int64_t)chain_sp[-1].number);
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_lowest_bit(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	uint64_t bits = (uint64_t)(int64_t)chain_sp[-1].number;
	chain_sp[-1].number = bits ? (double)__builtin_ctzll(bits) : -1.0;
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_div_f(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	if (chain_sp[-1].number == 0.0) {
		fail(interp, "division by zero");
		return;
	}
	chain_sp[-2].number = chain_sp[-2].number / chain_sp[-1].number;
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_eq(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val right = chain_sp[-1];
	Val left = chain_sp[-2];
	int is_true;

	if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT) {
		is_true = VAL_NUMBER(left) == VAL_NUMBER(right);
	} else {
		if (VAL_TAG(left) == T_SYMBOL && VAL_TAG(right) == T_SYMBOL)
			RETARGET_OP(p_eq_symbol);
		else if (VAL_TAG(left) == T_STRING && VAL_TAG(right) == T_STRING)
			RETARGET_OP(p_eq_string);
		SYNC_REGISTERS(interp, chain_ip, chain_sp - 2);
		is_true = val_cmp(interp, left, right) == 0;
		if (interp->error_flag)
			return;
	}

	chain_sp[-2] = make_bool(is_true);
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_eq_symbol(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val right = chain_sp[-1];
	Val left = chain_sp[-2];
	if (VAL_TAG(left) != T_SYMBOL || VAL_TAG(right) != T_SYMBOL) {
		RETARGET_OP(p_eq);
		MUSTTAIL return p_eq(interp, chain_ip, chain_sp);
	}

	chain_sp[-2] = make_bool(VAL_DATA(left) == VAL_DATA(right));
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_eq_string(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val right = chain_sp[-1];
	Val left = chain_sp[-2];
	if (VAL_TAG(left) != T_STRING || VAL_TAG(right) != T_STRING) {
		RETARGET_OP(p_eq);
		MUSTTAIL return p_eq(interp, chain_ip, chain_sp);
	}

	Object *left_string = OBJECT_AT(VAL_DATA(left));
	Object *right_string = OBJECT_AT(VAL_DATA(right));
	int is_true = left_string->len == right_string->len
		&& memcmp(left_string->bytes, right_string->bytes, (size_t)left_string->len) == 0;

	chain_sp[-2] = make_bool(is_true);
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}


static int quantity_comparison_mask(Interpreter *interp, Val left, Val right,
		scalar_operator scalar_op, const char *word) {
	int left_unit;
	int right_unit;
	Val left_magnitude = quantity_unwrap(left, &left_unit);
	Val right_magnitude = quantity_unwrap(right, &right_unit);

	if (VAL_TAG(left_magnitude) != T_MATRIX && VAL_TAG(right_magnitude) != T_MATRIX)
		return 0;

	if ((left_unit == 0) != (right_unit == 0)) {
		fail(interp, "cannot compare a quantity and a plain number");
		return 1;
	}

	int base = interp->dsp - 2;

	if (left_unit != right_unit) {
		double conversion;
		if (!unit_conversion(right_unit, left_unit, &conversion)) {
			fail(interp, "unit mismatch");
			return 1;
		}
		binary_op(interp, right_magnitude, make_float(conversion), scalar_mul, "*");
		if (interp->error_flag)
			return 1;
		right_magnitude = interp->data_stack[interp->dsp - 1];
	}

	binary_op(interp, left_magnitude, right_magnitude, scalar_op, word);
	if (interp->error_flag)
		return 1;

	Val mask = pop(interp);
	interp->dsp = base;
	push(interp, mask);
	return 1;
}

static void array_comparison_mask(Interpreter *interp, Val left, Val right,
		scalar_operator scalar_op) {
	Object *left_array = VAL_TAG(left) == T_ARRAY ? OBJECT_AT(VAL_DATA(left)) : NULL;
	Object *right_array = VAL_TAG(right) == T_ARRAY ? OBJECT_AT(VAL_DATA(right)) : NULL;

	if (left_array && right_array && left_array->len != right_array->len) {
		fail(interp, "length mismatch (%d vs %d)", left_array->len, right_array->len);
		return;
	}

	int n_elements = left_array ? left_array->len : right_array->len;

	NEW_MATRIX(mask_handle, mask, n_elements, 1);
	for (int i = 0; i < n_elements; i++) {
		Val left_element = left_array ? left_array->items[i] : left;
		Val right_element = right_array ? right_array->items[i] : right;
		int ordering = val_cmp(interp, left_element, right_element);
		if (interp->error_flag)
			return;
		mask->matrix.elements[i] = scalar_op((double)ordering, 0.0);
	}

	interp->dsp -= 2;
	push(interp, make_matrix(mask_handle));
}

#define MATRIX_COMPARISON_OP(name, op, sfn, word) \
	void name(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2); \
		Val right = chain_sp[-1]; \
		Val left = chain_sp[-2]; \
		if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT) { \
			chain_sp[-2] = make_bool(VAL_NUMBER(left) op VAL_NUMBER(right)); \
			DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1); \
		} \
		if (VAL_TAG(left) == T_QUANTITY || VAL_TAG(right) == T_QUANTITY) { \
			SYNC_REGISTERS(interp, chain_ip, chain_sp); \
			if (quantity_comparison_mask(interp, left, right, sfn, word)) { \
				if (interp->error_flag) \
					return; \
				DISPATCH(interp); \
			} \
		} \
		if (VAL_TAG(left) == T_ARRAY || VAL_TAG(right) == T_ARRAY) { \
			SYNC_REGISTERS(interp, chain_ip, chain_sp); \
			array_comparison_mask(interp, left, right, sfn); \
			if (interp->error_flag) \
				return; \
			DISPATCH(interp); \
		} \
		SYNC_REGISTERS(interp, chain_ip, chain_sp - 2); \
		if (VAL_TAG(left) == T_MATRIX || VAL_TAG(right) == T_MATRIX) { \
			binary_op(interp, left, right, sfn, word); \
			if (interp->error_flag) \
				return; \
		} else { \
			int ordering = val_cmp(interp, left, right); \
			if (interp->error_flag) \
				return; \
			push(interp, make_bool(ordering op 0)); \
		} \
		DISPATCH(interp); \
	}
MATRIX_COMPARISON_OP(p_lt, <, scalar_lt, "lt")
MATRIX_COMPARISON_OP(p_gt, >, scalar_gt, "gt")
MATRIX_COMPARISON_OP(p_eq_elements, ==, scalar_eq, "eq")

void p_nan(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	if (VAL_TAG(chain_sp[-1]) == T_NONE) {
			chain_sp[-1] = make_bool(1);
			DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
	}

	if (VAL_TAG(chain_sp[-1]) == T_ARRAY) {
		Object *source = OBJECT_AT(VAL_DATA(chain_sp[-1]));
		int n_elements = source->len;

		NEW_MATRIX(mask_handle, mask, n_elements, 1);
		for (int i = 0; i < n_elements; i++)
			mask->matrix.elements[i] = VAL_TAG(source->items[i]) == T_NONE ? 1.0 : 0.0;

		chain_sp[-1] = make_matrix(mask_handle);
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
	}

	SYNC_REGISTERS(interp, chain_ip, chain_sp - 1);
	unary_op(interp, chain_sp[-1], scalar_nan);
	if (interp->error_flag) return;

	DISPATCH(interp);
}
			

#define UNARY_FLOAT_OP(name, opname, expr) \
	void name(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1); \
		double n = chain_sp[-1].number; \
		chain_sp[-1].number = (expr); \
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp); \
	}

void p_zeq(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	chain_sp[-1] = make_bool(!truthy(chain_sp[-1]));

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

#define COMPARISON_ZBRANCH(name, op, word) \
	void name(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip + 1, chain_sp, 2); \
		Val right = chain_sp[-1]; \
		Val left = chain_sp[-2]; \
		int is_true; \
		if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT) { \
			is_true = VAL_NUMBER(left) op VAL_NUMBER(right); \
		} else { \
			SYNC_REGISTERS(interp, chain_ip + 1, chain_sp - 2); \
			is_true = val_cmp(interp, left, right) op 0; \
			if (interp->error_flag) \
				return; \
		} \
		cell *continue_ip = is_true ? chain_ip + 1 : chain_ip + (int)*chain_ip; \
		DISPATCH_REGISTERS(interp, continue_ip, chain_sp - 2); \
	}

COMPARISON_ZBRANCH(p_eq_zbranch, ==, "(=0branch)");
COMPARISON_ZBRANCH(p_lt_zbranch, <, "(lt0branch)");
COMPARISON_ZBRANCH(p_gt_zbranch, >, "(gt0branch)");


#define FLOAT_COMPARISON_ZBRANCH(name, op, word) \
	void name(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip + 1, chain_sp, 2); \
		double left = chain_sp[-2].number; \
		double right = chain_sp[-1].number; \
		cell *continue_ip = (left op right) ? chain_ip + 1 : chain_ip + (int)*chain_ip; \
		DISPATCH_REGISTERS(interp, continue_ip, chain_sp - 2); \
	}

FLOAT_COMPARISON_ZBRANCH(p_eq_f_zbranch, ==, "(feq0branch)");
FLOAT_COMPARISON_ZBRANCH(p_lt_f_zbranch, <, "(flt0branch)");
FLOAT_COMPARISON_ZBRANCH(p_gt_f_zbranch, >, "(fgt0branch)");

void p_zeq_zbranch(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip + 1, chain_sp, 1);
	cell *continue_ip = truthy(chain_sp[-1]) ? chain_ip + (int)*chain_ip : chain_ip + 1;

	DISPATCH_REGISTERS(interp, continue_ip, chain_sp - 1);
}

void p_and(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	chain_sp[-2] = make_bool(truthy(chain_sp[-2]) && truthy(chain_sp[-1]));

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_or(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	chain_sp[-2] = make_bool(truthy(chain_sp[-2]) || truthy(chain_sp[-1]));

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_not(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	chain_sp[-1] = make_bool(!truthy(chain_sp[-1]));

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_null(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	*chain_sp = make_tagged(T_NONE, 0);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

int type_of_symbols[T_QUANTITY + 1];

void type_of_intern_names(Interpreter *interp) {
	static const char *names[T_QUANTITY + 1] = {
		[T_NONE] = "none",        [T_SYMBOL] = "symbol",  [T_FLOAT] = "float",
		[T_STRING] = "string",    [T_SET] = "set",        [T_ARRAY] = "array",
		[T_PAIR] = "pair",        [T_FRAME] = "frame",    [T_MATRIX] = "matrix",
		[T_XT] = "xt",            [T_ADDR] = "addr",      [T_CONT] = "continuation",
		[T_MARK] = "mark",        [T_STREAM] = "stream",  [T_LOGIC_VAR] = "lvar",
		[T_UNBOUND] = "wildcard", [T_DB] = "db",          [T_PTR] = "ptr",
		[T_SEGMENT] = "segment",  [T_QUANTITY] = "quantity"
	};
	for (int tag = 0; tag <= T_QUANTITY; tag++)
		type_of_symbols[tag] = intern_symbol(interp, names[tag]);
}

void p_type_of(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);

	Val value = chain_sp[-1];
	if (VAL_TAG(value) == T_LOGIC_VAR)
		value = deref(interp, value);

	chain_sp[-1] = make_symbol(type_of_symbols[VAL_TAG(value)]);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_dup(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	chain_sp[0] = chain_sp[-1];
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

void p_drop(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_swap(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val top = chain_sp[-1];
	chain_sp[-1] = chain_sp[-2];
	chain_sp[-2] = top;
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_over(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	chain_sp[0] = chain_sp[-2];
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

void p_rot(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3);
	Val bottom = chain_sp[-3];
	chain_sp[-3] = chain_sp[-2];
	chain_sp[-2] = chain_sp[-1];
	chain_sp[-1] = bottom;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_depth(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	*chain_sp = make_float((double)(chain_sp - interp->data_stack));

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);

}

#define REQUIRE_ENTRY_VALUES(interp, needed) do { \
	if ((interp)->entry_snapshot_depth < (needed)) { \
		fail(interp, "%d value(s) on the stack at the start of the line (need %d)", \
				(interp)->entry_snapshot_depth, (needed)); \
		return; \
	} \
} while (0)

void p_it(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	REQUIRE_ENTRY_VALUES(interp, 1);
	
	*chain_sp = interp->entry_snapshot[interp->entry_snapshot_depth - 1];

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

void p_them(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 2);
	REQUIRE_ENTRY_VALUES(interp, 2);

	Val *snapshot_top = interp->entry_snapshot + interp->entry_snapshot_depth;
	chain_sp[0] = snapshot_top[-2];
	chain_sp[1] = snapshot_top[-1];

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 2);
}

void p_other(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	REQUIRE_ENTRY_VALUES(interp, 2);

	Val *snapshot_top = interp->entry_snapshot + interp->entry_snapshot_depth;
	*chain_sp = snapshot_top[-2];

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

void p_roll(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val depth_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(depth_val, T_FLOAT, "roll", "a float depth");
	int n = (int)VAL_NUMBER(depth_val);
	Val *rolled_top = chain_sp - 1;
	if (n < 0 || rolled_top - n < interp->data_stack) {
		SYNC_REGISTERS(interp, chain_ip, rolled_top);
		fail(interp, "depth %d out of range (stack has %d below it)", n, (int)(rolled_top - interp->data_stack));
		return;
	}

	Val rolled = rolled_top[-1 - n];
	memmove(rolled_top - 1 - n, rolled_top - n, (size_t)n * sizeof(Val));
	rolled_top[-1] = rolled;

	DISPATCH_REGISTERS(interp, chain_ip, rolled_top);
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

void p_dot(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val value = chain_sp[-1];
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

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_dot_all(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	int saved = print_truncate;
	print_truncate = 0;
	Val value = chain_sp[-1];
	if (!grid_if_matrix(stdout, value)) {
		print_val(stdout, interp, value);
		putchar(' ');
	}
	fflush(stdout);
	print_truncate = saved;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_render(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val value = chain_sp[-1];

	char *buffer = NULL;
	size_t size = 0;
	FILE *out = open_memstream(&buffer, &size);
	if (!out) {
		fail(interp, "out of memory");
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

	chain_sp[-1] = make_string(handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_cr(DISPATCH_ARGS) {
	putchar('\n');
	fflush(stdout);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_emit_(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val code_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(code_val, T_FLOAT, "emit", "a float character code");
	int char_code = (int)VAL_NUMBER(code_val);

	if (char_code < 0 || char_code > 0x10FFFF) {
		fail(interp, "codepoint %d out of range", char_code);
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

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_dots(DISPATCH_ARGS) {
	for (Val *slot = interp->data_stack; slot < chain_sp; slot++) {
		print_val_inspect(stdout, interp, *slot);
		putchar(' ');
	}
	fflush(stdout);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_bye(DISPATCH_ARGS) {
	(void)interp;

	exit(0);
}

void p_tor(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	if (interp->rsp >= RETURN_STACK_DEPTH) {
		fail(interp, "return stack overflow");
		return;
	}
	interp->return_stack[interp->rsp++] = chain_sp[-1];

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_rfrom(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	if (interp->rsp <= 0) {
		fail(interp, "return stack underflow");
		return;
	}
	*chain_sp = interp->return_stack[--interp->rsp];

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

void p_rfetch(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	if (interp->rsp <= 0) {
		fail(interp, "return stack is empty");
		return;
	}
	*chain_sp = interp->return_stack[interp->rsp - 1];

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

void p_to_side(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	if (interp->side_dsp >= SIDESTACK_DEPTH) {
		fail(interp, "side stack overflow (max %d)", SIDESTACK_DEPTH);
		return;
	}
	interp->side_stack[interp->side_dsp++] = chain_sp[-1];

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_side_to(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	if (interp->side_dsp <= 0) {
		fail(interp, "side stack is empty");
		return;
	}
	*chain_sp = interp->side_stack[--interp->side_dsp];

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

void p_side_drop(DISPATCH_ARGS) {
	if (interp->side_dsp <= 0) {
		fail(interp, "side stack is empty");
		return;
	}
	interp->side_dsp--;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_side_depth(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	*chain_sp = make_float((double)interp->side_dsp);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

void p_execute(DISPATCH_ARGS) {
	POP_XT(value, "execute");
	execute_xt(interp, value);

	DISPATCH(interp);
}

void p_curry(DISPATCH_ARGS) {
	if (in_parallel) {
		fail(interp, "cannot curry inside a parallel region");
		return;
	}

	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val xt_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(xt_val, T_XT, "curry", "an execution token");
	Val bound_value = chain_sp[-2];

	int curried_cfa = create_header(interp, "(curried)", 4);
	if (interp->error_flag) return;

	emit(interp, (cell)&docol);
	emit_val_literal(interp, bound_value);
	emit_call(interp, (int)VAL_DATA(xt_val));
	emit_call(interp, vocab.exit_cfa);
	if (interp->error_flag) return;

	chain_sp[-2] = make_xt(curried_cfa);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

int push_prompt(Interpreter *interp, int kind) {
	Val mark = make_tagged(T_MARK, (interp->next_mark_id++ << 1) | kind);
	rpush(interp, mark);

	return (int)VAL_DATA(mark);
}

void p_reset(DISPATCH_ARGS) {
	push_prompt(interp, PROMPT_EXCEPTION);
	if (interp->error_flag)
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}


int prompt_index(Interpreter *interp, int kind) {
	int scope_base = interp->local_base;
	int scope_top = scope_base > 0
		? scope_base + saved_n_locals(interp->return_stack[scope_base - 1]) : -1;

	for (int i = interp->rsp - 1; i >= 0; i--) {
		if (scope_base > 0 && i >= scope_base && i < scope_top)
			continue;
		if (scope_base > 0 && i == scope_base - 1) {
			scope_base = saved_local_base(interp->return_stack[i]);
			scope_top = scope_base > 0
				? scope_base + saved_n_locals(interp->return_stack[scope_base - 1]) : -1;
			continue;
		}

		Val frame = interp->return_stack[i];
		if (VAL_TAG(frame) == T_MARK && (VAL_DATA(frame) & 1) == kind)
			return i;
	}

	return -1;
}

static int find_prompt(Interpreter *interp, int kind) {
	int mark_index = prompt_index(interp, kind);
	if (mark_index < 0)
		fail(interp, kind == PROMPT_CHOICE
				? "no enclosing amb to backtrack to"
				: "no enclosing reset on the return stack");

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

void p_fail(DISPATCH_ARGS) {
	backtrack(interp);

	DISPATCH(interp);
}

void p_shift(DISPATCH_ARGS) {
	int mark_index;
	int cont_slot = capture_continuation(interp, PROMPT_EXCEPTION, &mark_index);
	if (cont_slot < 0)
		return;

	interp->rsp = mark_index;
	restore_local_base_below(interp, mark_index);
	push(interp, make_continuation(cont_slot));

	DISPATCH(interp);
}

void p_shift_with(DISPATCH_ARGS) {
	POP_XT(handler, "shift-with");

	int mark_index;
	int cont_slot = capture_continuation(interp, PROMPT_EXCEPTION, &mark_index);
	if (cont_slot < 0)
		return;

	unwind_to(interp, mark_index);
	push(interp, make_continuation(cont_slot));

	execute_xt(interp, handler);
	if (interp->error_flag)
		return;

	interp->unwinding = 1;
}

void p_throw(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	SYNC_REGISTERS(interp, chain_ip, chain_sp);

	int mark_index = prompt_index(interp, PROMPT_EXCEPTION);
	if (mark_index < 0) {
		char *rendered = NULL;
		size_t rendered_len = 0;
		FILE *out = open_memstream(&rendered, &rendered_len);
		if (out) {
			print_val_inspect(out, interp, chain_sp[-1]);
			fclose(out);
		}
		fail(interp, "uncaught exception: %s", rendered ? rendered : "?");
		free(rendered);
		return;
	}

	unwind_to(interp, mark_index);
	push(interp, make_float(1.0));
	interp->unwinding = 1;
}

void p_execute_catching(DISPATCH_ARGS) {
	POP_XT(xt, "(execute-catching)");
	int base_dsp = interp->dsp;

	execute_xt(interp, xt);

	if (interp->error_flag) {
		interp->error_flag = 0;
		int message_handle = object_new_string(interp, interp->error_message,
				(int)strlen(interp->error_message));
		gc_root_push(interp, make_string(message_handle));
		int trace_handle = object_new_string(interp, interp->error_trace,
				(int)strlen(interp->error_trace));
		gc_root_push(interp, make_string(trace_handle));

		NEW_FRAME(error_handle, error_frame);
		gc_root_push(interp, make_frame(error_handle));
		frame_put(error_frame, intern_symbol(interp, "message"), make_string(message_handle));
		frame_put(error_frame, intern_symbol(interp, "trace"), make_string(trace_handle));
		gc_root_pop(interp);
		gc_root_pop(interp);
		gc_root_pop(interp);

		interp->dsp = base_dsp;
		push(interp, make_frame(error_handle));
		push(interp, make_float(1));

		int mark_index = find_prompt(interp, PROMPT_EXCEPTION);
		if (mark_index >= 0) {
			unwind_to(interp, mark_index);
			interp->unwinding = 1;
		}
	}

	DISPATCH(interp);
}

void p_resume(DISPATCH_ARGS) {
	POP_CONT(continuation, "resume");
	if (continuation->continuation.capture_generation != vocab.forget_generation) {
		fail(interp, "continuation outlived its defining word");
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


static const HelpEntry *help_lookup(const char *name) {
	LOWER_BOUND(help_entry_count, mid, strcmp(help_entries[mid].name, name) < 0, at);
	if (at < help_entry_count && strcmp(help_entries[at].name, name) == 0)
		return &help_entries[at];
	return NULL;
}

static int name_cmp(const void *left, const void *right) {
	return strcmp(*(const char *const *)left, *(const char *const *)right);
}

static void print_word_columns(const char **names, int n_names) {
	size_t widest = 0;
	for (int i = 0; i < n_names; i++) {
		size_t length = strlen(names[i]);
		widest = length > widest ? length : widest;
	}
	int columns = (int)(78 / (widest + 2));
	if (columns < 1)
		columns = 1;
	for (int i = 0; i < n_names; i++) {
		if ((i + 1) % columns == 0 || i + 1 == n_names)
			printf("  %s\n", names[i]);
		else
			printf("  %-*s", (int)widest, names[i]);
	}
}

static void print_word_group(const char **names, const int *groups, int n_collected,
		int group, const char *label, const char **scratch) {
	int n_in_group = 0;
	for (int i = 0; i < n_collected; i++)
		if (groups[i] == group)
			scratch[n_in_group++] = names[i];
	if (n_in_group == 0)
		return;
	qsort(scratch, (size_t)n_in_group, sizeof(char *), name_cmp);
	printf("%s%s:%s\n", term_bold(), label, term_plain());
	print_word_columns(scratch, n_in_group);
}

#include "logo_embed.h"

void p_water(DISPATCH_ARGS) {
	fwrite(water_logo_txt, 1, water_logo_txt_len, stdout);
	printf("\n%*swater %s\n", 42, "", VERSION);
	printf("%*shttps://github.com/free-variation/water\n", 30, "");
	fflush(stdout);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_size(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	int unit;
	Val collection = quantity_unwrap(chain_sp[-1], &unit);
	(void)unit;
	double elements;

	if (VAL_TAG(collection) == T_STRING) {
		Object *string = OBJECT_AT(VAL_DATA(collection));
		elements = (double)string_codepoint_count(string);
	} else if (VAL_TAG(collection) == T_SEGMENT) {
		elements = (double)OBJECT_AT(VAL_DATA(collection))->segment.length;
	} else if (VAL_TAG(collection) == T_MATRIX) {
		Object *matrix = OBJECT_AT(VAL_DATA(collection));
		elements = (double)(matrix->matrix.rows * matrix->matrix.columns);
	} else if (VAL_TAG(collection) == T_SET ||
			VAL_TAG(collection) == T_ARRAY ||
			VAL_TAG(collection) == T_FRAME) {
		RETARGET_OP(p_size_len);
		elements = (double)OBJECT_AT(VAL_DATA(collection))->len;
	} else {
		fail(interp, "expected a set, array, matrix, string, segment, or frame; got %s", tag_name(VAL_TAG(collection)));
		return;
	}
	chain_sp[-1] = make_float(elements);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_sort(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val collection = chain_sp[-1];

	int sorted_handle;
	if (VAL_TAG(collection) == T_ARRAY)
		sorted_handle = array_sorted_copy(interp, OBJECT_AT(VAL_DATA(collection)));
	else if (VAL_TAG(collection) == T_SET)
		sorted_handle = set_elements_copy(interp, OBJECT_AT(VAL_DATA(collection)));
	else if (VAL_TAG(collection) == T_MATRIX)
		sorted_handle = vector_sorted_copy(interp, OBJECT_AT(VAL_DATA(collection)));
	else {
		fail(interp, "expected an array, set, or vector; got %s", tag_name(VAL_TAG(collection)));
		return;
	}
	if (interp->error_flag) return;

	chain_sp[-1] = VAL_TAG(collection) == T_MATRIX ? make_matrix(sorted_handle) : make_array(sorted_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_argsort(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	int unit;
	Val collection = quantity_unwrap(chain_sp[-1], &unit);
	(void)unit;

	REQUIRE_CHAIN_TAG(collection, T_MATRIX, "argsort", "a vector (nx1 or 1xn)");

	int permutation_handle = vector_argsort_copy(interp, OBJECT_AT(VAL_DATA(collection)));
	if (interp->error_flag) return;

	chain_sp[-1] = make_matrix(permutation_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_size_len(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val collection = chain_sp[-1];
	Tag tag = VAL_TAG(collection);
	if (tag != T_ARRAY && tag != T_SET && tag != T_FRAME) {
		RETARGET_OP(p_size);
		MUSTTAIL return p_size(interp, chain_ip, chain_sp);
	}

	chain_sp[-1] = make_float((double)OBJECT_AT(VAL_DATA(collection))->len);
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_globals(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);

	int n_globals = 0;
	for (int cfa = vocab.latest_cfa; cfa != 0; cfa = (int)WORD_LINK(cfa))
		if ((cfa_handler)vocab.dict[cfa] == dovar && !WORD_IS_INTERNAL(cfa))
			n_globals++;

	NEW_ARRAY(bindings_handle, bindings, n_globals);
	memset(bindings->items, 0, sizeof(Val) * (size_t)n_globals);
	gc_root_push(interp, make_array(bindings_handle));

	int slot = n_globals;
	for (int cfa = vocab.latest_cfa; cfa != 0; cfa = (int)WORD_LINK(cfa)) {
		if ((cfa_handler)vocab.dict[cfa] != dovar || WORD_IS_INTERNAL(cfa))
			continue;

		int symbol_cfa = intern_symbol(interp, &vocab.name_pool[WORD_NAME(cfa)]);
		if (interp->error_flag) {
			gc_root_pop(interp);
			return;
		}

		int binding_handle = object_new_array(interp, 2);
		if (interp->error_flag) {
			gc_root_pop(interp);
			return;
		}
		Object *binding = OBJECT_AT(binding_handle);
		binding->items[0] = make_symbol(symbol_cfa);
		binding->items[1].bits = (uint64_t)vocab.dict[cfa + 1];

		slot--;
		bindings->items[slot] = make_array(binding_handle);
	}

	gc_root_pop(interp);
	chain_sp[0] = make_array(bindings_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

void p_words(DISPATCH_ARGS) {
	int word_count = 0;
	for (int cfa = vocab.latest_cfa; cfa != 0; cfa = (int)WORD_LINK(cfa))
		if (!WORD_IS_INTERNAL(cfa))
			word_count++;

	const char **names;
	MALLOC_OR_FAIL(interp, names, sizeof(char *) * (size_t)word_count);
	int *groups;
	MALLOC_OR_FAIL_CLEANUP(interp, groups, sizeof(int) * (size_t)word_count, free(names));
	int session_group = help_section_count;
	int undocumented_group = help_section_count + 1;
	int units_group = help_section_count + 2;
	int n_collected = 0;
	for (int cfa = vocab.latest_cfa; cfa != 0; cfa = (int)WORD_LINK(cfa)) {
		if (WORD_IS_INTERNAL(cfa))
			continue;
		const char *name = &vocab.name_pool[WORD_NAME(cfa)];
		const HelpEntry *entry = help_lookup(name);
		names[n_collected] = name;
		if ((cfa_handler)vocab.dict[cfa] == dounit)
			groups[n_collected] = units_group;
		else if (cfa > vocab.lib_end_latest_cfa)
			groups[n_collected] = session_group;
		else
			groups[n_collected] = entry && entry->section >= 0
				? entry->section : undocumented_group;
		n_collected++;
	}

	const char **group_names;
	MALLOC_OR_FAIL_CLEANUP(interp, group_names, sizeof(char *) * (size_t)word_count, { free(groups); free(names); });
	print_word_group(names, groups, n_collected, session_group, "this session", group_names);
	for (int s = 0; s < help_section_count; s++)
		print_word_group(names, groups, n_collected, s, help_section_names[s], group_names);
	print_word_group(names, groups, n_collected, units_group, "units", group_names);
	print_word_group(names, groups, n_collected, undocumented_group, "undocumented", group_names);

	free(group_names);
	free(groups);
	free(names);
	fflush(stdout);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

static int contains_case_insensitive(const char *haystack, const char *needle) {
	size_t needle_length = strlen(needle);
	for (const char *cursor = haystack; *cursor; cursor++)
		if (strncasecmp(cursor, needle, needle_length) == 0)
			return 1;
	return 0;
}

void p_apropos(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val query_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(query_val, T_STRING, "apropos", "a string");
	const char *query = OBJECT_AT(VAL_DATA(query_val))->bytes;

	for (int i = 0; i < help_entry_count; i++) {
		const HelpEntry *entry = &help_entries[i];
		if (!contains_case_insensitive(entry->name, query)
				&& !contains_case_insensitive(entry->summary, query))
			continue;
		if (!find(entry->name))
			continue;
		printf("%-16s %-24s %s\n", entry->name,
				entry->effect ? entry->effect : "", entry->summary);
	}
	for (int cfa = vocab.latest_cfa; cfa != 0 && cfa > vocab.lib_end_latest_cfa;
			cfa = (int)WORD_LINK(cfa)) {
		if (WORD_IS_INTERNAL(cfa))
			continue;
		const char *name = &vocab.name_pool[WORD_NAME(cfa)];
		if (contains_case_insensitive(name, query) && !help_lookup(name))
			printf("%-16s %-24s defined this session\n", name, "");
	}
	fflush(stdout);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
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
	} else if (handler == dounit) {
		fprintf(out, "unit %s  \\ ", name ? name : "?");
		render_unit_description(out, interp, target_cfa);
		putc('\n', out);
	} else if (handler == dosym) {
		fprintf(out, "symbol %s\n", name ? name : "?");
	} else {
		fprintf(out, "%s is a primitive\n", name ? name : "?");
	}
}

void p_see(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val target_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(target_val, T_XT, "see", "an execution token");
	see_source_render(stdout, interp, (int)VAL_DATA(target_val));
	fflush(stdout);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_see_to_string(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val target_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(target_val, T_XT, "see>string", "an execution token");

	int handle = capture_render(interp, see_source_render, (int)VAL_DATA(target_val));
	if (interp->error_flag)
		return;
	chain_sp[-1] = make_string(handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

static void help_put(Interpreter *interp, int frame_handle, const char *key, const char *text) {
	int string_handle = object_new_string(interp, text, (int)strlen(text));
	if (interp->error_flag) {
		return;
	}
	frame_put(OBJECT_AT(frame_handle), intern_symbol(interp, key), make_string(string_handle));
}

void p_man(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val target_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(target_val, T_XT, "man", "an execution token");
	int target_cfa = (int)VAL_DATA(target_val);

	const char *name = name_of(target_cfa);
	const HelpEntry *entry = NULL;
	if (name) {
		LOWER_BOUND(help_entry_count, mid, strcmp(help_entries[mid].name, name) < 0, at);
		if (at < help_entry_count && strcmp(help_entries[at].name, name) == 0) {
			entry = &help_entries[at];
		}
	}

	if (!entry && name && (cfa_handler)vocab.dict[target_cfa] == dounit) {
		int description_handle = capture_render(interp, render_unit_description, target_cfa);
		if (interp->error_flag)
			return;
		gc_root_push(interp, make_string(description_handle));

		NEW_FRAME(unit_frame_handle, unit_frame);
		(void)unit_frame;
		gc_root_push(interp, make_frame(unit_frame_handle));
		help_put(interp, unit_frame_handle, "word", name);
		help_put(interp, unit_frame_handle, "effect", "( n -- q )");
		char unit_summary[512];
		snprintf(unit_summary, sizeof(unit_summary), "unit: %s",
				OBJECT_AT(description_handle)->bytes);
		help_put(interp, unit_frame_handle, "summary", unit_summary);
		gc_root_pop(interp);
		gc_root_pop(interp);

		chain_sp[-1] = make_frame(unit_frame_handle);
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
	}

	if (!entry) {
		chain_sp[-1] = make_tagged(T_NONE, 0);
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
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

	chain_sp[-1] = make_frame(frame_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
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
				fail(interp, "result too large");
				return;
			}
			*capacity *= 2;
		}
		char *grown = realloc(*buffer, (size_t)*capacity);
		if (!grown) {
			free(*buffer);
			*buffer = NULL;
			fail(interp, "out of memory");
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
		fail(interp, "spec too long");
		return;
	}
	char cspec[64];
	memcpy(cspec, spec, (size_t)spec_len);
	cspec[spec_len] = 0;

	char conv;
	if (parse_format_spec(cspec, spec_len, &conv) != 0) {
		fail(interp, "bad spec '%s'", cspec);
		return;
	}

	char fmt[80];
	char stackbuf[256];
	char *rendered = stackbuf;
	int n;

	if (conv == 'd' || conv == 'i') {
		if (VAL_TAG(value) != T_FLOAT) {
			fail(interp, "{%s} needs a float; got %s", cspec, tag_name(VAL_TAG(value)));
			return;
		}
		cspec[spec_len - 1] = 0;
		snprintf(fmt, sizeof fmt, "%%%slld", cspec);
		long long as_int = (long long)VAL_NUMBER(value);
		n = snprintf(stackbuf, sizeof stackbuf, fmt, as_int);
		if (n >= (int)sizeof stackbuf) {
			MALLOC_OR_FAIL(interp, rendered, (size_t)n + 1);
			snprintf(rendered, (size_t)n + 1, fmt, as_int);
		}
	} else if (conv && conv != 's') {
		if (VAL_TAG(value) != T_FLOAT) {
			fail(interp, "{%s} needs a float; got %s", cspec, tag_name(VAL_TAG(value)));
			return;
		}
		double as_float = VAL_NUMBER(value);
		snprintf(fmt, sizeof fmt, "%%%s", cspec);
		n = snprintf(stackbuf, sizeof stackbuf, fmt, as_float);
		if (n >= (int)sizeof stackbuf) {
			MALLOC_OR_FAIL(interp, rendered, (size_t)n + 1);
			snprintf(rendered, (size_t)n + 1, fmt, as_float);
		}
	} else {
		int text_cap = 64;
		char *text;
		MALLOC_OR_FAIL(interp, text, text_cap);
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
			MALLOC_OR_FAIL_CLEANUP(interp, rendered, (size_t)n + 1, free(text));
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
	char *out_buffer;
	MALLOC_OR_FAIL_RETURNING(interp, out_buffer, (size_t)capacity, -1);
	int out_length = 0;
	int refs[64];
	int ref_count = 0;

	for (int cursor = 0; cursor < template->len; ) {
		if (template->bytes[cursor] == '{') {
			if (template->len - cursor >= 4 && memcmp(&template->bytes[cursor], "{nl}", 4) == 0) {
				interp_append(interp, &out_buffer, &capacity, &out_length, "\n", 1);
				cursor += 4;
				continue;
			}
			if (template->len - cursor >= 5 && memcmp(&template->bytes[cursor], "{tab}", 5) == 0) {
				interp_append(interp, &out_buffer, &capacity, &out_length, "\t", 1);
				cursor += 5;
				continue;
			}
			int scan = cursor + 1, saw_digit = 0;
			long long digit_value = 0;
			while (scan < template->len && isdigit((unsigned char)template->bytes[scan])) {
				if (digit_value <= INT_MAX)
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
					long long stack_index = (long long)interp->dsp - 1 - digit_value;
					if (stack_index < 0) {
						fail(interp, "{%lld} needs %lld stack value(s) but only %d present",
								digit_value, digit_value + 1, interp->dsp);
						free(out_buffer);
						return object_new_string(interp, "", 0);
					}
					int reference = (int)digit_value;
					int already = 0;
					for (int i = 0; i < ref_count; i++)
						if (refs[i] == reference) {
							already = 1;
							break;
						}
					if (!already && ref_count < (int)(sizeof(refs) / sizeof(refs[0])))
						refs[ref_count++] = reference;
					Val value = interp->data_stack[(int)stack_index];
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

void p_format(DISPATCH_ARGS) {
	POP(template_val);
	REQUIRE_CHAIN_TAG(template_val, T_STRING, "format", "a template string");
	int result = interpolate(interp, (int)VAL_DATA(template_val));
	if (interp->error_flag)
		return;
	push(interp, make_string(result));

	DISPATCH(interp);
}

void p_gc(DISPATCH_ARGS) {
	SYNC_REGISTERS(interp, chain_ip, chain_sp);
	gc(interp);
	if (interp->error_flag)
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_clear(DISPATCH_ARGS) {
	DISPATCH_REGISTERS(interp, chain_ip, interp->data_stack);
}

void unary_op(Interpreter *interp, Val operand, double (*function)(double)) {
	if (VAL_TAG(operand) == T_FLOAT) {
		push(interp, make_float(function(VAL_NUMBER(operand))));
	} else if (VAL_TAG(operand) == T_MATRIX) {
		gc_root_push(interp, operand);
		Object *source = OBJECT_AT(VAL_DATA(operand));
		int target_handle = object_new_matrix(interp, source->matrix.rows, source->matrix.columns);
		if (interp->error_flag) {
			gc_root_pop(interp);
			return;
		}

		Object *target = OBJECT_AT(target_handle);
		int num_elements = source->matrix.rows * source->matrix.columns;
		for (int i = 0; i < num_elements; i++)
			target->matrix.elements[i] = function(source->matrix.elements[i]);

		gc_root_pop(interp);
		push(interp, make_matrix(target_handle));
	} else {
		fail(interp, "expected a float or a matrix; got %s", tag_name(VAL_TAG(operand)));
	}
}

#define UNARY_MATH_OP_NAMED(cname, func, word) \
	void p_##cname(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1); \
		if (VAL_TAG(chain_sp[-1]) == T_FLOAT) { \
			chain_sp[-1] = make_float(func(chain_sp[-1].number)); \
			DISPATCH_REGISTERS(interp, chain_ip, chain_sp); \
		} \
		\
		SYNC_REGISTERS(interp, chain_ip, chain_sp - 1); \
		unary_op(interp, chain_sp[-1], func); \
		DISPATCH(interp); \
	}
#define UNARY_MATH_OP(name, func) UNARY_MATH_OP_NAMED(name, func, #name)

#define UNARY_QUANTITY_OP(cname, func, word, result_unit) \
	void p_##cname(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1); \
		if (VAL_TAG(chain_sp[-1]) == T_FLOAT) { \
			chain_sp[-1] = make_float(func(chain_sp[-1].number)); \
			DISPATCH_REGISTERS(interp, chain_ip, chain_sp); \
		} \
		\
		SYNC_REGISTERS(interp, chain_ip, chain_sp - 1); \
		Val operand = chain_sp[-1]; \
		if (VAL_TAG(operand) == T_QUANTITY) { \
			int unit = (result_unit); \
			if (interp->error_flag) return; \
			unary_quantity_op(interp, operand, func, unit); \
		} else \
			unary_op(interp, operand, func); \
		DISPATCH(interp); \
	}

UNARY_QUANTITY_OP(neg, scalar_negate, "negate", (int)pairs.table[VAL_DATA(operand)].tail.bits)
UNARY_QUANTITY_OP(abs, fabs, "abs", (int)pairs.table[VAL_DATA(operand)].tail.bits)
UNARY_QUANTITY_OP(sqrt, sqrt, "sqrt", unit_pow(interp, (int)pairs.table[VAL_DATA(operand)].tail.bits, 1, 2))
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

static void binary_matrix_op(Interpreter *interp, Val left, Val right, scalar_operator function, const char *name) {
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

void binary_op(Interpreter *interp, Val left, Val right, scalar_operator function, const char *name) {
	if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT) {
		push(interp, make_float(function(VAL_NUMBER(left), VAL_NUMBER(right))));
		return;
	}

	gc_root_push(interp, left);
	gc_root_push(interp, right);
	binary_matrix_op(interp, left, right, function, name);
	gc_root_pop(interp);
	gc_root_pop(interp);
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

void p_power(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val right = chain_sp[-1];
	Val left = chain_sp[-2];

	if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT) {
		chain_sp[-2] = make_float(pow(VAL_NUMBER(left), VAL_NUMBER(right)));
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
	}

	SYNC_REGISTERS(interp, chain_ip, chain_sp - 2);
	if (VAL_TAG(left) == T_QUANTITY) {
		if (VAL_TAG(right) != T_FLOAT) {
			fail(interp, "exponent must be a number; got %s", tag_name(VAL_TAG(right)));
			return;
		}

		int numerator, denominator;
		if (!rational_of_double(VAL_NUMBER(right), &numerator, &denominator)) {
			fail(interp, "exponent must be a simple rational");
			return;
		}

		int slot = (int)VAL_DATA(left);
		int unit = unit_pow(interp, (int)pairs.table[slot].tail.bits, numerator, denominator);
		if (interp->error_flag) return;

		binary_op(interp, pairs.table[slot].head, right, pow, "^");
		if (interp->error_flag) return;

		push_quantity(interp, pop(interp), unit);
		DISPATCH(interp);
		return;
	}

	binary_op(interp, left, right, pow, "^");
	DISPATCH(interp);
}

void p_divmod(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val right = chain_sp[-1];
	Val left = chain_sp[-2];
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
	chain_sp[-2] = make_float(dividend - quotient * divisor);
	chain_sp[-1] = make_float(quotient);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
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

static uint64_t random_next_in(uint64_t *state) {
	uint64_t result = random_rotl(state[1] * 5, 7) * 9;
	uint64_t t = state[1] << 17;
	state[2] ^= state[0];
	state[3] ^= state[1];
	state[1] ^= state[2];
	state[0] ^= state[3];
	state[2] ^= t;
	state[3] = random_rotl(state[3], 45);
	return result;
}

static uint64_t random_next(void) {
	random_ensure_seeded();
	return random_next_in(random_state);
}

static int random_below_in(uint64_t *state, int bound) {
	uint64_t range = (uint64_t)bound;
	uint64_t threshold = (0 - range) % range;
	uint64_t value;

	do {
		value = random_next_in(state);
	} while (value < threshold);

	return (int)(value % range);
}

void p_seed(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val seed_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(seed_val, T_FLOAT, "seed", "a float seed");
	int seed_value = (int)VAL_NUMBER(seed_val);
	atomic_store(&random_base_seed, (uint64_t)seed_value);
	atomic_store(&random_stream_counter, 0);
	random_seeded = 0;
	random_ensure_seeded();

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_random(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	uint64_t bits = random_next() >> 11;
	*chain_sp = make_float((double)bits * 0x1.0p-53);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

int random_below(int bound) {
	random_ensure_seeded();
	return random_below_in(random_state, bound);
}

void p_random_int(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val bound_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(bound_val, T_FLOAT, "random-int", "a float bound");
	int bound = (int)VAL_NUMBER(bound_val);
	if (bound <= 0) {
		fail(interp, "bound must be positive; got %d", bound);
		return;
	}
	chain_sp[-1] = make_float((double)random_below(bound));

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_resample_indices_ext(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val seed_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(seed_val, T_FLOAT, "resample-indices-ext", "a float seed");
	Val count_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(count_val, T_FLOAT, "resample-indices-ext", "a float count");
	int n_indices = (int)VAL_NUMBER(count_val);
	if (n_indices <= 0) {
		fail(interp, "count must be positive; got %d", n_indices);
		return;
	}

	uint64_t expansion = (uint64_t)(int64_t)VAL_NUMBER(seed_val);
	uint64_t replicate_state[4];
	for (int i = 0; i < 4; i++)
		replicate_state[i] = splitmix64(&expansion);

	NEW_ARRAY(indices_handle, indices, n_indices);
	for (int i = 0; i < n_indices; i++)
		indices->items[i] = make_float((double)random_below_in(replicate_state, n_indices));

	chain_sp[-2] = make_array(indices_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_sleep(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val seconds_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(seconds_val, T_FLOAT, "sleep", "a float");

	double seconds = VAL_NUMBER(seconds_val);
	if (seconds > 0) {
		struct timespec request;
		request.tv_sec = (time_t)seconds;
		request.tv_nsec = (long)((seconds - (double)(time_t)seconds) * 1e9);
		nanosleep(&request, NULL);
	}

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

