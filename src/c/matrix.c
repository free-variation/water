#include "telic.h"


#define MATRIX_ELEMENTWISE_OP(name, opname, op) \
	int name(Interpreter *interp, Val left_val, Val right_val) { \
		Object *left = OBJECT_AT(VAL_DATA(left_val)); \
		Object *right = OBJECT_AT(VAL_DATA(right_val)); \
		int left_rows = left->matrix.rows, left_cols = left->matrix.columns; \
		int right_rows = right->matrix.rows, right_cols = right->matrix.columns; \
		int rows = left_rows > right_rows ? left_rows : right_rows; \
		int cols = left_cols > right_cols ? left_cols : right_cols; \
		if ((left_rows != rows && left_rows != 1) || (right_rows != rows && right_rows != 1) || \
			(left_cols != cols && left_cols != 1) || (right_cols != cols && right_cols != 1)) { \
			fail(interp, "shapes not broadcast-compatible (%dx%d vs %dx%d)", \
					left_rows, left_cols, right_rows, right_cols); \
			return -1; \
		} \
		int target_handle = object_new_matrix_raw(interp, rows, cols); \
		if (interp->error_flag) return -1; \
		Object *target = OBJECT_AT(target_handle); \
		if (left_rows == right_rows && left_cols == right_cols) { \
			size_t n = (size_t)rows * (size_t)cols; \
			const double * restrict l = left->matrix.elements; \
			const double * restrict r = right->matrix.elements; \
			double * restrict t = target->matrix.elements; \
			for (size_t i = 0; i < n; i++) \
				t[i] = l[i] op r[i]; \
		} else { \
			for (int i = 0; i < rows; i++) \
				for (int j = 0; j < cols; j++) \
					MAT(target, i, j) = \
						MAT(left, left_rows == 1 ? 0 : i, left_cols == 1 ? 0 : j) op \
						MAT(right, right_rows == 1 ? 0 : i, right_cols == 1 ? 0 : j); \
		} \
		return target_handle; \
	}

MATRIX_ELEMENTWISE_OP(matrix_add, "+", +)
MATRIX_ELEMENTWISE_OP(matrix_sub, "-", -)
MATRIX_ELEMENTWISE_OP(matrix_mul, "*", *)
MATRIX_ELEMENTWISE_OP(matrix_div, "/", /)

#define MATRIX_COMPARISON_KERNEL(name, op) \
	int name(Interpreter *interp, Val left_val, Val right_val, const char *word) { \
		if (VAL_TAG(left_val) == T_MATRIX && VAL_TAG(right_val) == T_MATRIX) { \
			Object *left = OBJECT_AT(VAL_DATA(left_val)); \
			Object *right = OBJECT_AT(VAL_DATA(right_val)); \
			int rows = left->matrix.rows, cols = left->matrix.columns; \
			if (rows != right->matrix.rows || cols != right->matrix.columns) { \
				fail(interp, "%s: matrix shapes differ (%dx%d vs %dx%d)", word, \
						rows, cols, right->matrix.rows, right->matrix.columns); \
				return -1; \
			} \
			int target_handle = object_new_matrix_raw(interp, rows, cols); \
			if (interp->error_flag) return -1; \
			size_t n = (size_t)rows * (size_t)cols; \
			const double * restrict l = left->matrix.elements; \
			const double * restrict r = right->matrix.elements; \
			double * restrict t = OBJECT_AT(target_handle)->matrix.elements; \
			for (size_t i = 0; i < n; i++) \
				t[i] = l[i] op r[i]; \
			return target_handle; \
		} \
		if (VAL_TAG(left_val) == T_MATRIX && VAL_TAG(right_val) == T_FLOAT) { \
			Object *source = OBJECT_AT(VAL_DATA(left_val)); \
			double scalar = VAL_NUMBER(right_val); \
			int target_handle = object_new_matrix_raw(interp, source->matrix.rows, source->matrix.columns); \
			if (interp->error_flag) return -1; \
			size_t n = (size_t)source->matrix.rows * (size_t)source->matrix.columns; \
			const double * restrict s = source->matrix.elements; \
			double * restrict t = OBJECT_AT(target_handle)->matrix.elements; \
			for (size_t i = 0; i < n; i++) \
				t[i] = s[i] op scalar; \
			return target_handle; \
		} \
		if (VAL_TAG(left_val) == T_FLOAT && VAL_TAG(right_val) == T_MATRIX) { \
			Object *source = OBJECT_AT(VAL_DATA(right_val)); \
			double scalar = VAL_NUMBER(left_val); \
			int target_handle = object_new_matrix_raw(interp, source->matrix.rows, source->matrix.columns); \
			if (interp->error_flag) return -1; \
			size_t n = (size_t)source->matrix.rows * (size_t)source->matrix.columns; \
			const double * restrict s = source->matrix.elements; \
			double * restrict t = OBJECT_AT(target_handle)->matrix.elements; \
			for (size_t i = 0; i < n; i++) \
				t[i] = scalar op s[i]; \
			return target_handle; \
		} \
		fail(interp, "%s: expected floats or matrices; got %s and %s", word, \
				tag_name(VAL_TAG(left_val)), tag_name(VAL_TAG(right_val))); \
		return -1; \
	}

MATRIX_COMPARISON_KERNEL(matrix_compare_lt, <)
MATRIX_COMPARISON_KERNEL(matrix_compare_gt, >)
MATRIX_COMPARISON_KERNEL(matrix_compare_lte, <=)
MATRIX_COMPARISON_KERNEL(matrix_compare_gte, >=)
MATRIX_COMPARISON_KERNEL(matrix_compare_eq, ==)
MATRIX_COMPARISON_KERNEL(matrix_compare_neq, !=)

#define REQUIRE_CHAIN_INDEX(index, limit, op, axis_phrase, limit_phrase) \
	do { \
		if ((index) < 0 || (index) >= (limit)) { \
			SYNC_REGISTERS(interp, chain_ip, chain_sp); \
			fail(interp, "" axis_phrase " %d out of bounds (%d " limit_phrase ")", (index), (limit)); \
			return; \
		} \
	} while (0)

void p_at_j(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val index_val = chain_sp[-1];
	Val source_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(index_val, T_FLOAT, "@j", "a float index");
	REQUIRE_CHAIN_TAG(source_val, T_MATRIX, "@j", "a matrix");
	Object *source = OBJECT_AT(VAL_DATA(source_val));
	int index = (int)VAL_NUMBER(index_val);

	REQUIRE_CHAIN_INDEX(index, source->matrix.columns, "@j", "column index", "columns");

	SYNC_REGISTERS(interp, chain_ip, chain_sp);
	int num_rows = source->matrix.rows;
	int col_handle = object_new_matrix(interp, num_rows, 1);
	if (interp->error_flag)
		return;
	Object *col = OBJECT_AT(col_handle);
	for (int i = 0; i < num_rows; i++)
		MAT(col, i, 0) = MAT(source, i, index);

	chain_sp[-2] = make_matrix(col_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_at_ij(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3);
	Val j_val = chain_sp[-1];
	Val i_val = chain_sp[-2];
	Val source_val = chain_sp[-3];
	REQUIRE_CHAIN_TAG(j_val, T_FLOAT, "@i,j", "a float column index");
	REQUIRE_CHAIN_TAG(i_val, T_FLOAT, "@i,j", "a float row index");
	REQUIRE_CHAIN_TAG(source_val, T_MATRIX, "@i,j", "a matrix");
	Object *source = OBJECT_AT(VAL_DATA(source_val));
	int i = (int)VAL_NUMBER(i_val);
	int j = (int)VAL_NUMBER(j_val);

	REQUIRE_CHAIN_INDEX(i, source->matrix.rows, "@i,j", "row index", "rows");
	REQUIRE_CHAIN_INDEX(j, source->matrix.columns, "@i,j", "column index", "columns");

	chain_sp[-3] = make_float(MAT(source, i, j));

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}

void p_at_e(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val index_val = chain_sp[-1];
	Val source_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(index_val, T_FLOAT, "@e", "a float index");
	REQUIRE_CHAIN_TAG(source_val, T_MATRIX, "@e", "a matrix");
	Object *source = OBJECT_AT(VAL_DATA(source_val));
	int index = (int)VAL_NUMBER(index_val);

	int n_elements = source->matrix.rows * source->matrix.columns;
	REQUIRE_CHAIN_INDEX(index, n_elements, "@e", "element index", "elements");

	chain_sp[-2] = make_float(source->matrix.elements[index]);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

static inline __attribute__((always_inline)) Val *matrix_element_read(Interpreter *interp, cell *resume_ip, Val *slot_sp, Val source_val, int index) {
	if (VAL_TAG(source_val) != T_MATRIX) {
		SYNC_REGISTERS(interp, resume_ip, slot_sp);
		fail(interp, "expected a matrix; got %s", tag_name(VAL_TAG(source_val)));
		return NULL;
	}
	Object *source = OBJECT_AT(VAL_DATA(source_val));

	int n_elements = source->matrix.rows * source->matrix.columns;
	if (index < 0 || index >= n_elements) {
		SYNC_REGISTERS(interp, resume_ip, slot_sp);
		fail(interp, "element index %d out of bounds (%d elements)", index, n_elements);
		return NULL;
	}

	*slot_sp = make_float(source->matrix.elements[index]);
	return slot_sp + 1;
}

void p_at_e_lit(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip + 1, chain_sp, 1);
	Val *pushed_sp = matrix_element_read(interp, chain_ip + 1, chain_sp - 1, chain_sp[-1], (int)chain_ip[0]);
	if (!pushed_sp)
		return;
	DISPATCH_REGISTERS(interp, chain_ip + 1, pushed_sp);
}

void p_at_e_local0(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip + 1, chain_sp, 1);
	int index = (int)interp->return_stack[interp->local_base + (int)chain_ip[0]].number;
	Val *pushed_sp = matrix_element_read(interp, chain_ip + 1, chain_sp - 1, chain_sp[-1], index);
	if (!pushed_sp)
		return;
	DISPATCH_REGISTERS(interp, chain_ip + 1, pushed_sp);
}

void p_at_e_ll0(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip + 2, chain_sp, 1);
	Val *locals = interp->return_stack + interp->local_base;
	Val source_val = locals[(int)chain_ip[0]];
	int index = (int)locals[(int)chain_ip[1]].number;
	Val *pushed_sp = matrix_element_read(interp, chain_ip + 2, chain_sp, source_val, index);
	if (!pushed_sp)
		return;
	DISPATCH_REGISTERS(interp, chain_ip + 2, pushed_sp);
}

void p_at_e_lit_local0(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip + 2, chain_sp, 1);
	Val source_val = interp->return_stack[interp->local_base + (int)chain_ip[0]];
	Val *pushed_sp = matrix_element_read(interp, chain_ip + 2, chain_sp, source_val, (int)chain_ip[1]);
	if (!pushed_sp)
		return;
	DISPATCH_REGISTERS(interp, chain_ip + 2, pushed_sp);
}

void p_at_e_swap_local0(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip + 1, chain_sp, 1);
	Val index_val = chain_sp[-1];
	if (VAL_TAG(index_val) != T_FLOAT) {
		SYNC_REGISTERS(interp, chain_ip + 1, chain_sp);
		fail(interp, "expected a float index; got %s", tag_name(VAL_TAG(index_val)));
		return;
	}

	Val source_val = interp->return_stack[interp->local_base + (int)chain_ip[0]];
	Val *pushed_sp = matrix_element_read(interp, chain_ip + 1, chain_sp - 1, source_val, (int)VAL_NUMBER(index_val));
	if (!pushed_sp)
		return;
	DISPATCH_REGISTERS(interp, chain_ip + 1, pushed_sp);
}

void p_at_e_depth_top(DISPATCH_ARGS) {
	int matrix_depth = (int)chain_ip[0];
	REQUIRE_STACK_DEPTH(interp, chain_ip + 1, chain_sp, matrix_depth + 1);
	Val index_val = chain_sp[-1];
	if (VAL_TAG(index_val) != T_FLOAT) {
		SYNC_REGISTERS(interp, chain_ip + 1, chain_sp);
		fail(interp, "expected a float index; got %s", tag_name(VAL_TAG(index_val)));
		return;
	}

	Val source_val = chain_sp[-1 - matrix_depth];
	Val *pushed_sp = matrix_element_read(interp, chain_ip + 1, chain_sp - 1, source_val, (int)VAL_NUMBER(index_val));
	if (!pushed_sp)
		return;
	DISPATCH_REGISTERS(interp, chain_ip + 1, pushed_sp);
}

void p_at_e_depth(DISPATCH_ARGS) {
	int matrix_depth = (int)chain_ip[0];
	int index_depth = (int)chain_ip[1];
	int deepest = matrix_depth > index_depth ? matrix_depth : index_depth;
	REQUIRE_STACK_DEPTH(interp, chain_ip + 2, chain_sp, deepest + 1);
	REQUIRE_STACK_ROOM(interp, chain_ip + 2, chain_sp, 1);

	Val index_val = chain_sp[-1 - index_depth];
	if (VAL_TAG(index_val) != T_FLOAT) {
		SYNC_REGISTERS(interp, chain_ip + 2, chain_sp);
		fail(interp, "expected a float index; got %s", tag_name(VAL_TAG(index_val)));
		return;
	}

	Val source_val = chain_sp[-1 - matrix_depth];
	Val *pushed_sp = matrix_element_read(interp, chain_ip + 2, chain_sp, source_val, (int)VAL_NUMBER(index_val));
	if (!pushed_sp)
		return;
	DISPATCH_REGISTERS(interp, chain_ip + 2, pushed_sp);
}

void p_gather_e_local0(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip + 1, chain_sp, 2);
	int position = (int)interp->return_stack[interp->local_base + (int)chain_ip[0]].number;
	Val index_vector_val = chain_sp[-1];
	Val value_val = chain_sp[-2];
	if (VAL_TAG(index_vector_val) != T_MATRIX) {
		SYNC_REGISTERS(interp, chain_ip + 1, chain_sp);
		fail(interp, "expected a matrix; got %s", tag_name(VAL_TAG(index_vector_val)));
		return;
	}
	Object *index_vector = OBJECT_AT(VAL_DATA(index_vector_val));

	int n_elements = index_vector->matrix.rows * index_vector->matrix.columns;
	if (position < 0 || position >= n_elements) {
		SYNC_REGISTERS(interp, chain_ip + 1, chain_sp);
		fail(interp, "element index %d out of bounds (%d elements)", position, n_elements);
		return;
	}
	int gathered_index = (int)index_vector->matrix.elements[position];

	Val *pushed_sp = matrix_element_read(interp, chain_ip + 1, chain_sp - 2, value_val, gathered_index);
	if (!pushed_sp)
		return;
	DISPATCH_REGISTERS(interp, chain_ip + 1, pushed_sp);
}

static inline __attribute__((always_inline)) int matrix_element_write(Interpreter *interp, cell *resume_ip, Val *fail_sp, Val target_val, int index, Val element_val) {
	if (VAL_TAG(element_val) != T_FLOAT && VAL_TAG(element_val) != T_NONE) {
		SYNC_REGISTERS(interp, resume_ip, fail_sp);
		fail(interp, "expected a float or null value; got %s", tag_name(VAL_TAG(element_val)));
		return 0;
	}
	if (VAL_TAG(target_val) != T_MATRIX) {
		SYNC_REGISTERS(interp, resume_ip, fail_sp);
		fail(interp, "expected a matrix; got %s", tag_name(VAL_TAG(target_val)));
		return 0;
	}
	Object *target = OBJECT_AT(VAL_DATA(target_val));

	int n_elements = target->matrix.rows * target->matrix.columns;
	if (index < 0 || index >= n_elements) {
		SYNC_REGISTERS(interp, resume_ip, fail_sp);
		fail(interp, "element index %d out of bounds (%d elements)", index, n_elements);
		return 0;
	}

	target->matrix.elements[index] = VAL_NUMBER(element_val);
	return 1;
}

#define STORE_E_OP(c_name, n_consumed) \
	void c_name(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3); \
		Val element_val = chain_sp[-2]; \
		Val index_val = chain_sp[-1]; \
		Val target_val = chain_sp[-3]; \
		REQUIRE_CHAIN_TAG(index_val, T_FLOAT, "!e", "a float index"); \
		if (!matrix_element_write(interp, chain_ip, chain_sp, target_val, (int)VAL_NUMBER(index_val), element_val)) \
			return; \
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - (n_consumed)); \
	}

STORE_E_OP(p_store_e, 2)
STORE_E_OP(p_store_e_drop, 3)

void p_store_e_lll0(DISPATCH_ARGS) {
	Val *locals = interp->return_stack + interp->local_base;
	Val target_val = locals[(int)chain_ip[0]];
	Val element_val = locals[(int)chain_ip[1]];
	int index = (int)locals[(int)chain_ip[2]].number;
	if (!matrix_element_write(interp, chain_ip + 3, chain_sp, target_val, index, element_val))
		return;
	DISPATCH_REGISTERS(interp, chain_ip + 3, chain_sp);
}

#define STORE_IJ_OP(c_name, n_consumed) \
	void c_name(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 4); \
		Val element_val = chain_sp[-3]; \
		Val j_val = chain_sp[-1]; \
		Val i_val = chain_sp[-2]; \
		Val target_val = chain_sp[-4]; \
		if (VAL_TAG(element_val) != T_FLOAT && VAL_TAG(element_val) != T_NONE) { \
			SYNC_REGISTERS(interp, chain_ip, chain_sp); \
			fail(interp, "expected a float or null value; got %s", tag_name(VAL_TAG(element_val))); \
			return; \
		} \
		REQUIRE_CHAIN_TAG(j_val, T_FLOAT, "!i,j", "a float column index"); \
		REQUIRE_CHAIN_TAG(i_val, T_FLOAT, "!i,j", "a float row index"); \
		REQUIRE_CHAIN_TAG(target_val, T_MATRIX, "!i,j", "a matrix"); \
		Object *target = OBJECT_AT(VAL_DATA(target_val)); \
		int i = (int)VAL_NUMBER(i_val); \
		int j = (int)VAL_NUMBER(j_val); \
		\
		REQUIRE_CHAIN_INDEX(i, target->matrix.rows, "!i,j", "row index", "rows"); \
		REQUIRE_CHAIN_INDEX(j, target->matrix.columns, "!i,j", "column index", "columns"); \
		\
		MAT(target, i, j) = VAL_NUMBER(element_val); \
		\
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - (n_consumed)); \
	}

STORE_IJ_OP(p_store_ij, 3)
STORE_IJ_OP(p_store_ij_drop, 4)

void p_matmul(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val right_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(right_val, T_MATRIX, "matmul", "a matrix");
	Val left_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(left_val, T_MATRIX, "matmul", "a matrix");

	Object *left = OBJECT_AT(VAL_DATA(left_val));
	Object *right = OBJECT_AT(VAL_DATA(right_val));
	int n_rows = left->matrix.rows;
	int n_inner = left->matrix.columns;
	int n_columns = right->matrix.columns;

	if (right->matrix.rows != n_inner) {
		fail(interp, "inner dimensions must match (A is %dx%d, B is %dx%d)",
				n_rows, n_inner, right->matrix.rows, n_columns);
		return;
	}

	int handle = object_new_matrix(interp, n_rows, n_columns);
	if (interp->error_flag)
		return;

	const double * restrict left_elements = left->matrix.elements;
	const double * restrict right_elements = right->matrix.elements;
	double * restrict product = OBJECT_AT(handle)->matrix.elements;

	for (int i = 0; i < n_rows; i++) {
		double * restrict product_row = &product[i * n_columns];
		for (int p = 0; p < n_inner; p++) {
			double left_value = left_elements[i * n_inner + p];
			const double * restrict right_row = &right_elements[p * n_columns];
			for (int j = 0; j < n_columns; j++)
				product_row[j] += left_value * right_row[j];
		}
	}

	chain_sp[-2] = make_matrix(handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

static size_t sort_partition_nans(double *elements, size_t n_elements) {
	int any_nan = 0;
	for (size_t i = 0; i < n_elements; i++)
		any_nan |= elements[i] != elements[i];
	if (!any_nan)
		return n_elements;

	size_t sortable = 0;
	for (size_t i = 0; i < n_elements; i++)
		if (!isnan(elements[i]))
			elements[sortable++] = elements[i];

	for (size_t i = sortable; i < n_elements; i++)
		elements[i] = NAN;

	return sortable;
}

static inline int argsort_pair_before(ArgsortPair left, ArgsortPair right) {
	if (left.value != right.value)
		return left.value < right.value;

	return left.index < right.index;
}

#define SWAP_ELEMENTS(element_type, left, right) \
	do { \
		element_type swap_tmp = (left); \
		(left) = (right); \
		(right) = swap_tmp; \
	} while (0)

#define SORT_KERNELS(suffix, element_type, before) \
	static void insertion_sort_##suffix(element_type *elements, size_t n_elements) { \
		for (size_t i = 1; i < n_elements; i++) { \
			element_type inserted = elements[i]; \
			size_t j = i; \
			while (j > 0 && before(inserted, elements[j - 1])) { \
				elements[j] = elements[j - 1]; \
				j--; \
			} \
			elements[j] = inserted; \
		} \
	} \
	\
	static element_type median_of_three_##suffix(element_type *elements, size_t n_elements) { \
		size_t mid = n_elements / 2; \
		size_t last = n_elements - 1; \
		\
		if (before(elements[mid], elements[0])) \
			SWAP_ELEMENTS(element_type, elements[mid], elements[0]); \
		if (before(elements[last], elements[0])) \
			SWAP_ELEMENTS(element_type, elements[last], elements[0]); \
		if (before(elements[last], elements[mid])) \
			SWAP_ELEMENTS(element_type, elements[last], elements[mid]); \
		\
		return elements[mid]; \
	} \
	\
	static void quicksort_##suffix(element_type *elements, size_t n_elements) { \
		while (n_elements > 24) { \
			element_type pivot = median_of_three_##suffix(elements, n_elements); \
			size_t i = 0; \
			size_t j = n_elements - 1; \
			\
			for (;;) { \
				while (before(elements[i], pivot)) \
					i++; \
				while (before(pivot, elements[j])) \
					j--; \
				if (i >= j) \
					break; \
				\
				SWAP_ELEMENTS(element_type, elements[i], elements[j]); \
				\
				i++; \
				j--; \
			} \
			\
			size_t left_count = j + 1; \
			if (left_count <= n_elements - left_count) { \
				quicksort_##suffix(elements, left_count); \
				elements += left_count; \
				n_elements -= left_count; \
			} else { \
				quicksort_##suffix(elements + left_count, n_elements - left_count); \
				n_elements = left_count; \
			} \
		} \
		insertion_sort_##suffix(elements, n_elements); \
	}

#define DOUBLE_BEFORE(left, right) ((left) < (right))
SORT_KERNELS(doubles, double, DOUBLE_BEFORE)
SORT_KERNELS(pairs, ArgsortPair, argsort_pair_before)

#define DOUBLE_KEY(element) (*(sort_key *)&(element))
#define PAIR_KEY(element) (*(sort_key *)&(element).value)

RADIX_SORT(doubles, double, DOUBLE_KEY)
RADIX_SORT(pairs, ArgsortPair, PAIR_KEY)

#define SORT_DISPATCH(linkage, suffix, element_type) \
	linkage void sort_##suffix(element_type *elements, size_t n_elements) { \
		if (n_elements > RADIX_SORT_CUTOFF) { \
			element_type *scratch = malloc(n_elements * sizeof(element_type) \
					+ RADIX_DIGITS * sizeof(size_t)); \
			if (scratch) { \
				radix_sort_##suffix(elements, n_elements, scratch, \
						(size_t *)(scratch + n_elements)); \
				free(scratch); \
				return; \
			} \
		} \
		\
		quicksort_##suffix(elements, n_elements); \
	}

SORT_DISPATCH(, doubles, double)
SORT_DISPATCH(, pairs, ArgsortPair)

static int vector_length(Interpreter *interp, Object *vector, const char *noun_phrase) {
	int n_rows = vector->matrix.rows;
	int n_columns = vector->matrix.columns;
	if (n_rows != 1 && n_columns != 1) {
		fail(interp, "expected %s (nx1 or 1xn); got %dx%d", noun_phrase, n_rows, n_columns);
		return -1;
	}

	return n_rows * n_columns;
}

int vector_sorted_copy(Interpreter *interp, Object *source) {
	int length = vector_length(interp, source, "a vector");
	if (length < 0)
		return -1;

	int sorted_vector_handle = object_new_matrix(interp, source->matrix.rows, source->matrix.columns);
	if (interp->error_flag)
		return -1;

	Object *sorted_vector = OBJECT_AT(sorted_vector_handle);
	size_t n_elements = (size_t)length;
	memcpy(sorted_vector->matrix.elements, source->matrix.elements, sizeof(double) * n_elements);
	size_t sortable = sort_partition_nans(sorted_vector->matrix.elements, n_elements);
	sort_doubles(sorted_vector->matrix.elements, sortable);

	return sorted_vector_handle;
}

int vector_argsort_copy(Interpreter *interp, Object *source) {
	int length = vector_length(interp, source, "a vector");
	if (length < 0)
		return -1;

	int permutation_handle = object_new_matrix(interp, source->matrix.rows, source->matrix.columns);
	if (interp->error_flag)
		return -1;

	size_t n_elements = (size_t)length;
	ArgsortPair *pairs;
	MALLOC_OR_FAIL_RETURNING(interp, pairs, n_elements * sizeof(ArgsortPair), -1);

	const double *elements = source->matrix.elements;
	size_t sortable = 0;
	for (size_t i = 0; i < n_elements; i++) {
		if (isnan(elements[i]))
			continue;
		pairs[sortable].value = elements[i];
		pairs[sortable].index = (int)i;
		sortable++;
	}

	size_t nan_tail = sortable;
	for (size_t i = 0; i < n_elements; i++) {
		if (!isnan(elements[i]))
			continue;
		pairs[nan_tail].value = NAN;
		pairs[nan_tail].index = (int)i;
		nan_tail++;
	}
		
	sort_pairs(pairs, sortable);

	
	Object *permutation = OBJECT_AT(permutation_handle);
	for (size_t i = 0; i < n_elements; i++)
		permutation->matrix.elements[i] = (double)pairs[i].index;

	free(pairs);
	return permutation_handle;
}

int matrix_nonzero_indices(Interpreter *interp, Object *source) {
	int n_rows = source->matrix.rows;
	int n_columns = source->matrix.columns;
	size_t n_elements = (size_t)n_rows * (size_t)n_columns;
	const double *elements = source->matrix.elements;

	int n_nonzero = 0;
	for (size_t i = 0; i < n_elements; i++)
		n_nonzero += elements[i] != 0.0;

	int indices_handle = object_new_matrix(interp,
			n_rows == 1 ? 1 : n_nonzero,
			n_rows == 1 ? n_nonzero : 1);
	if (interp->error_flag)
		return -1;

	Object *indices = OBJECT_AT(indices_handle);
	int write_index = 0;
	for (size_t i = 0; i < n_elements; i++)
		if (elements[i] != 0.0)
			indices->matrix.elements[write_index++] = (double)i;

	return indices_handle;
}

void p_where(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val mask = chain_sp[-1];
	SYNC_REGISTERS(interp, chain_ip, chain_sp);

	REQUIRE_CHAIN_TAG(mask, T_MATRIX, "where", "a matrix mask");

	int indices_handle = matrix_nonzero_indices(interp, OBJECT_AT(VAL_DATA(mask)));
	if (interp->error_flag) return;

	chain_sp[-1] = make_matrix(indices_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

#define ADD(a, b) ((a) + (b))

static inline int double_is_nan_bits(double element) {
	sort_key bits;
	memcpy(&bits, &element, sizeof bits);
	return (bits & 0x7FFFFFFFFFFFFFFFULL) > 0x7FF0000000000000ULL;
}

#define NAN_SUBSTITUTED(element, substitute) \
	((element) != (element) ? (substitute) : (element))

#define MATRIX_REDUCE_OVERALL_OP(name, init_value, combine, nan_substitute) \
	double name(Object *source) { \
		size_t num_elements = (size_t)source->matrix.rows * (size_t)source->matrix.columns; \
		const double * restrict elements = source->matrix.elements; \
		double accumulator_0 = init_value; \
		double accumulator_1 = init_value; \
		double accumulator_2 = init_value; \
		double accumulator_3 = init_value; \
		size_t i = 0; \
		for (; i + 3 < num_elements; i += 4) { \
			accumulator_0 = combine(accumulator_0, NAN_SUBSTITUTED(elements[i], nan_substitute)); \
			accumulator_1 = combine(accumulator_1, NAN_SUBSTITUTED(elements[i + 1], nan_substitute)); \
			accumulator_2 = combine(accumulator_2, NAN_SUBSTITUTED(elements[i + 2], nan_substitute)); \
			accumulator_3 = combine(accumulator_3, NAN_SUBSTITUTED(elements[i + 3], nan_substitute)); \
		} \
		double accumulator = combine(combine(accumulator_0, accumulator_1), \
							combine(accumulator_2, accumulator_3)); \
		for (; i < num_elements; i++) \
			accumulator = combine(accumulator, NAN_SUBSTITUTED(elements[i], nan_substitute)); \
		return accumulator; \
	}

#if defined(__clang__)
#define MATRIX_REDUCE_FAST_FP _Pragma("clang fp reassociate(on) contract(fast)")
#define MATRIX_REDUCE_FAST_ATTR
#elif defined(__GNUC__)
#define MATRIX_REDUCE_FAST_FP
#define MATRIX_REDUCE_FAST_ATTR __attribute__((optimize("-fassociative-math", "-fno-signed-zeros", "-fno-trapping-math", "-ffp-contract=fast")))
#else
#define MATRIX_REDUCE_FAST_FP
#define MATRIX_REDUCE_FAST_ATTR
#endif

#define MATRIX_REDUCE_OVERALL_DENSE(name, init_value, combine) \
	static MATRIX_REDUCE_FAST_ATTR double name(Object *source) { \
		MATRIX_REDUCE_FAST_FP \
		size_t num_elements = (size_t)source->matrix.rows * (size_t)source->matrix.columns; \
		const double * restrict elements = source->matrix.elements; \
		double accumulator_0 = init_value; \
		double accumulator_1 = init_value; \
		double accumulator_2 = init_value; \
		double accumulator_3 = init_value; \
		size_t i = 0; \
		for (; i + 3 < num_elements; i += 4) { \
			accumulator_0 = combine(accumulator_0, elements[i]); \
			accumulator_1 = combine(accumulator_1, elements[i + 1]); \
			accumulator_2 = combine(accumulator_2, elements[i + 2]); \
			accumulator_3 = combine(accumulator_3, elements[i + 3]); \
		} \
		double accumulator = combine(combine(accumulator_0, accumulator_1), \
							combine(accumulator_2, accumulator_3)); \
		for (; i < num_elements; i++) \
			accumulator = combine(accumulator, elements[i]); \
		return accumulator; \
	}

#define MATRIX_REDUCE_ROWS_OP(name, init_value, combine) \
	int name(Interpreter *interp, Object *source) { \
		int rows = source->matrix.rows; \
		int cols = source->matrix.columns; \
		int target_handle = object_new_matrix_raw(interp, rows, 1); \
		if (interp->error_flag) return -1; \
		Object *target = OBJECT_AT(target_handle); \
		for (int i = 0; i < rows; i++) { \
			const double * restrict row = &MAT(source, i, 0); \
			double accumulator = init_value; \
			for (int j = 0; j < cols; j++) \
				accumulator = combine(accumulator, row[j]); \
			MAT(target, i, 0) = accumulator; \
		} \
		return target_handle; \
	}

#define MATRIX_REDUCE_COLUMNS_OP(name, init_value, combine) \
	int name(Interpreter *interp, Object *source) { \
		int rows = source->matrix.rows; \
		int cols = source->matrix.columns; \
		int target_handle = object_new_matrix_raw(interp, 1, cols); \
		if (interp->error_flag) return -1; \
		Object *target = OBJECT_AT(target_handle); \
		double * restrict target_elements = target->matrix.elements; \
		for (int j = 0; j < cols; j++) target_elements[j] = init_value; \
		for (int i = 0; i < rows; i++) { \
			const double * restrict row = &MAT(source, i, 0); \
			for (int j = 0; j < cols; j++) \
				target_elements[j] = combine(target_elements[j], row[j]); \
		} \
		return target_handle; \
	}

MATRIX_REDUCE_OVERALL_OP(matrix_max_overall, -INFINITY, MAX, -INFINITY)
MATRIX_REDUCE_OVERALL_OP(matrix_min_overall, INFINITY, MIN, INFINITY)

MATRIX_REDUCE_OVERALL_DENSE(matrix_sum_dense, 0.0, ADD)

#if defined(__clang__)
#pragma float_control(precise, off, push)
#elif defined(__GNUC__)
#pragma GCC push_options
#pragma GCC optimize ("-fassociative-math", "-fno-signed-zeros", "-fno-trapping-math")
#endif
MATRIX_REDUCE_ROWS_OP(matrix_sum_rows, 0.0, ADD)
MATRIX_REDUCE_ROWS_OP(matrix_max_rows, -INFINITY, MAX)
MATRIX_REDUCE_ROWS_OP(matrix_min_rows, INFINITY, MIN)

MATRIX_REDUCE_COLUMNS_OP(matrix_sum_columns, 0.0, ADD)
MATRIX_REDUCE_COLUMNS_OP(matrix_max_columns, -INFINITY, MAX)
MATRIX_REDUCE_COLUMNS_OP(matrix_min_columns, INFINITY, MIN)

static double matrix_nonmissing_count(Object *source) {
	size_t num_elements = (size_t)source->matrix.rows * (size_t)source->matrix.columns;
	const double * restrict elements = source->matrix.elements;
	size_t n_nonmissing = 0;
	for (size_t i = 0; i < num_elements; i++)
		n_nonmissing += !double_is_nan_bits(elements[i]);
	return (double)n_nonmissing;
}

#if defined(__clang__)
#pragma float_control(pop)
#elif defined(__GNUC__)
#pragma GCC pop_options
#endif

static double matrix_frobenius_overall(Object *source) {
	size_t n = (size_t)source->matrix.rows * (size_t)source->matrix.columns;
	const double * restrict elements = source->matrix.elements;
	double sum_of_squares = 0.0;
	for (size_t i = 0; i < n; i++) {
		double element = NAN_SUBSTITUTED(elements[i], 0.0);
		sum_of_squares += element * element;
	}
	return sqrt(sum_of_squares);
}

double matrix_sum_overall(Object *source) {
	double statistic = matrix_sum_dense(source);
	if (!isnan(statistic))
		return statistic;

	size_t num_elements = (size_t)source->matrix.rows * (size_t)source->matrix.columns;
	const double * restrict elements = source->matrix.elements;
	double accumulator = 0.0;
	for (size_t i = 0; i < num_elements; i++) {
		double element = elements[i];
		if (element != element)
			continue;
		accumulator += element;
	}
	return accumulator;
}

#define MATRIX_ARG_OP(name, cmp) \
	static double name(Object *source) { \
		size_t num_elements = (size_t)source->matrix.rows * (size_t)source->matrix.columns; \
		const double * restrict elements = source->matrix.elements; \
		size_t best = num_elements; \
		for (size_t i = 0; i < num_elements; i++) { \
			if (double_is_nan_bits(elements[i])) \
				continue; \
			if (best == num_elements || elements[i] cmp elements[best]) \
				best = i; \
		} \
		return best == num_elements ? NAN : (double)best; \
	}

MATRIX_ARG_OP(matrix_argmax_index, >)
MATRIX_ARG_OP(matrix_argmin_index, <)

int create_matrix(Interpreter *interp) {
	Val right = pop(interp);
	if (interp->error_flag) return -1;
	Val left = pop(interp);
	if (interp->error_flag) return -1;
	if (VAL_TAG(left) != T_FLOAT || VAL_TAG(right) != T_FLOAT) {
		fail(interp, "matrix dimensions: expected two floats (rows cols); got %s and %s",
				tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));
		return -1;
	}

	int num_rows = (int)(VAL_NUMBER(left));
	int num_columns = (int)(VAL_NUMBER(right));
	if (num_rows < 0 || num_columns < 0) {
		fail(interp, "matrix dimensions: must be non-negative; got %dx%d", num_rows, num_columns);
		return -1;
	}
	if (num_columns != 0 && num_rows > INT_MAX / num_columns) {
		fail(interp, "matrix dimensions: %dx%d too large (element count overflows)", num_rows, num_columns);
		return -1;
	}

	return object_new_matrix(interp, num_rows, num_columns);
}

void p_0_matrix(DISPATCH_ARGS) {
	int matrix_handle = create_matrix(interp);
	if (interp->error_flag) return;
	push(interp, make_matrix(matrix_handle));

	DISPATCH(interp);
}

void p_diagonal_matrix(DISPATCH_ARGS) {
	if (interp->dsp > 0) {
		push(interp, interp->data_stack[interp->dsp - 1]);
	}
	int diag_matrix_handle = create_matrix(interp);
	if (interp->error_flag) return;

	POP(diag_val);
	REQUIRE_CHAIN_TAG(diag_val, T_FLOAT, "diagonal-matrix", "a float fill value");

	Object *diag_matrix = OBJECT_AT(diag_matrix_handle);
	double diag_element = VAL_NUMBER(diag_val);
	for (int i = 0; i < diag_matrix->matrix.rows; i++) {
		MAT(diag_matrix, i, i) = diag_element;
	}

	push(interp, make_matrix(diag_matrix_handle));

	DISPATCH(interp);
}

void p_diagonal(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val source_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(source_val, T_MATRIX, "diagonal", "a matrix");
	Object *source = OBJECT_AT(VAL_DATA(source_val));

	int diag_len = MIN(source->matrix.rows, source->matrix.columns);
	NEW_MATRIX(diag_handle, diagonal, 1, diag_len);

	for (int i = 0; i < diag_len; i++)
		diagonal->matrix.elements[i] = MAT(source, i, i);

	chain_sp[-1] = make_matrix(diag_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_reshape(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3);
	Val cols_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(cols_val, T_FLOAT, "reshape", "a float column count");
	int new_cols = (int)VAL_NUMBER(cols_val);
	Val rows_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(rows_val, T_FLOAT, "reshape", "a float row count");
	int new_rows = (int)VAL_NUMBER(rows_val);
	int unit;
	Val source_val = quantity_unwrap(chain_sp[-3], &unit);
	REQUIRE_CHAIN_TAG(source_val, T_MATRIX, "reshape", "a matrix");
	Object *source = OBJECT_AT(VAL_DATA(source_val));

	if (new_rows < 0 || new_cols < 0) {
		fail(interp, "dimensions must be non-negative; got %dx%d", new_rows, new_cols);
		return;
	}
	if (new_cols != 0 && new_rows > INT_MAX / new_cols) {
		fail(interp, "%dx%d too large (element count overflows)", new_rows, new_cols);
		return;
	}
	int total = source->matrix.rows * source->matrix.columns;
	if (new_rows * new_cols != total) {
		fail(interp, "cannot reshape %d elements (%dx%d) into %dx%d (%d)",
				total, source->matrix.rows, source->matrix.columns,
				new_rows, new_cols, new_rows * new_cols);
		return;
	}

	NEW_MATRIX(target_handle, target, new_rows, new_cols);
	memcpy(target->matrix.elements, source->matrix.elements,
			(size_t)total * sizeof(double));

	if (unit) {
		SYNC_REGISTERS(interp, chain_ip, chain_sp - 3);
		push_quantity(interp, make_matrix(target_handle), unit);
		DISPATCH(interp);
	}

	chain_sp[-3] = make_matrix(target_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}

void p_matrix(DISPATCH_ARGS) {
	if (interp->dsp < 2) {
		fail(interp, "stack underflow (expected array and at least one dimension)");
		return;
	}
	Val top = interp->data_stack[interp->dsp - 1];
	Val below = interp->data_stack[interp->dsp - 2];
	REQUIRE_CHAIN_TAG(top, T_FLOAT, "matrix", "a float dimension on top");

	int num_rows, num_cols;
	Val arr_val;
	if (VAL_TAG(below) == T_FLOAT) {
		if (interp->dsp < 3) {
			fail(interp, "stack underflow (expected array below two dimensions)");
			return;
		}
		arr_val = interp->data_stack[interp->dsp - 3];
		REQUIRE_CHAIN_TAG(arr_val, T_ARRAY, "matrix", "an array");
		num_rows = (int)VAL_NUMBER(below);
		num_cols = (int)VAL_NUMBER(top);
		interp->dsp -= 3;
	} else if (VAL_TAG(below) == T_ARRAY) {
		num_rows = (int)VAL_NUMBER(top);
		Object *arr = OBJECT_AT(VAL_DATA(below));
		if (num_rows <= 0 || arr->len % num_rows != 0) {
			fail(interp, "%d elements does not divide evenly into %d rows", arr->len, num_rows);
			return;
		}
		num_cols = arr->len / num_rows;
		arr_val = below;
		interp->dsp -= 2;
	} else {
		fail(interp, "expected an array below the dimension(s); got %s", tag_name(VAL_TAG(below)));
		return;
	}

	push(interp, arr_val);
	push(interp, make_float(num_rows));
	push(interp, make_float(num_cols));
	int matrix_handle = create_matrix(interp);
	if (interp->error_flag) return;

	POP(array_val);
	Object *matrix = OBJECT_AT(matrix_handle);
	Object *input_array = OBJECT_AT(VAL_DATA(array_val));
	int num_elements = matrix->matrix.rows * matrix->matrix.columns;
	if (input_array->len != num_elements) {
		fail(interp, "array has %d elements but %dx%d needs %d",
				input_array->len, matrix->matrix.rows, matrix->matrix.columns, num_elements);
		return;
	}

	for (int i = 0; i < num_elements; i++) {
		Val element = input_array->items[i];
		if (VAL_TAG(element) == T_NONE) {
			matrix->matrix.elements[i] = NAN;
			continue;
		}
		if (VAL_TAG(element) != T_FLOAT) {
			fail(interp, "element %d is %s, expected a float", i, tag_name(VAL_TAG(element)));
			return;
		}
		matrix->matrix.elements[i] = VAL_NUMBER(element);
	}

	push(interp, make_matrix(matrix_handle));

	DISPATCH(interp);
}

void p_matrix_to_array(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	int unit;
	Val source_val = quantity_unwrap(chain_sp[-1], &unit);
	REQUIRE_CHAIN_TAG(source_val, T_MATRIX, "matrix>array", "a matrix");
	Object *source = OBJECT_AT(VAL_DATA(source_val));

	int n_elements = source->matrix.rows * source->matrix.columns;
	NEW_ARRAY(elements_handle, elements, n_elements);
	const double *matrix_elements = source->matrix.elements;

	if (unit) {
		memset(elements->items, 0, sizeof(Val) * (size_t)MAX(n_elements, 1));
		gc_root_push(interp, make_array(elements_handle));
		for (int i = 0; i < n_elements; i++) {
			double element = matrix_elements[i];
			if (element != element) {
				elements->items[i] = make_tagged(T_NONE, 0);
				continue;
			}

			elements->items[i] = quantity_of(interp, make_float(element), unit);
			if (interp->error_flag) {
				gc_root_pop(interp);
				return;
			}
		}
		gc_root_pop(interp);
	} else {
		for (int i = 0; i < n_elements; i++)
			elements->items[i] = make_float(matrix_elements[i]);
	}

	chain_sp[-1] = make_array(elements_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_dim(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	int unit;
	Val matrix_val = quantity_unwrap(chain_sp[-1], &unit);
	(void)unit;
	REQUIRE_CHAIN_TAG(matrix_val, T_MATRIX, "dim", "a matrix");
	Object *matrix = OBJECT_AT(VAL_DATA(matrix_val));

	chain_sp[-1] = make_float(matrix->matrix.rows);
	chain_sp[0] = make_float(matrix->matrix.columns);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

void p_transpose(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	int unit;
	Val source_val = quantity_unwrap(chain_sp[-1], &unit);

	if (VAL_TAG(source_val) == T_ARRAY) {
		Object *rows = OBJECT_AT(VAL_DATA(source_val));
		int n_rows = rows->len;
		int n_columns = 0;

		for (int i = 0; i < n_rows; i++) {
			if (VAL_TAG(rows->items[i]) != T_ARRAY) {
				fail(interp, "transpose: row %d is %s, expected an array",
						i, tag_name(VAL_TAG(rows->items[i])));
				return;
			}
			int row_len = OBJECT_AT(VAL_DATA(rows->items[i]))->len;
			if (i == 0)
				n_columns = row_len;
			else if (row_len != n_columns) {
				fail(interp, "transpose: row %d has %d element(s), expected %d",
						i, row_len, n_columns);
				return;
			}
		}

		NEW_ARRAY(transposed_handle, transposed, n_columns);
		memset(transposed->items, 0, sizeof(Val) * (size_t)MAX(n_columns, 1));
		gc_root_push(interp, make_array(transposed_handle));

		for (int j = 0; j < n_columns; j++) {
			int column_handle = object_new_array(interp, n_rows);
			if (interp->error_flag) {
				gc_root_pop(interp);
				return;
			}
			Object *column = OBJECT_AT(column_handle);
			Object *source_rows = OBJECT_AT(VAL_DATA(source_val));
			for (int i = 0; i < n_rows; i++)
				column->items[i] = OBJECT_AT(VAL_DATA(source_rows->items[i]))->items[j];
			OBJECT_AT(transposed_handle)->items[j] = make_array(column_handle);
		}

		gc_root_pop(interp);
		chain_sp[-1] = make_array(transposed_handle);

		DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
	}

	REQUIRE_CHAIN_TAG(source_val, T_MATRIX, "transpose", "a matrix");
	Object *source = OBJECT_AT(VAL_DATA(source_val));

	NEW_MATRIX(target_handle, target, source->matrix.columns, source->matrix.rows);
	for (int i = 0; i < source->matrix.rows; i++)
		for (int j = 0; j < source->matrix.columns; j++)
			MAT(target, j, i) = MAT(source, i, j);

	if (unit) {
		SYNC_REGISTERS(interp, chain_ip, chain_sp - 1);
		push_quantity(interp, make_matrix(target_handle), unit);
		DISPATCH(interp);
	}

	chain_sp[-1] = make_matrix(target_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

static inline int mesh_replaces(double mask_element) {
	return !double_is_nan_bits(mask_element) && mask_element != 0.0;
}

void p_mesh(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3);
	Val mask_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(mask_val, T_MATRIX, "mesh", "a mask matrix");
	Object *mask = OBJECT_AT(VAL_DATA(mask_val));
	const double *mask_elements = mask->matrix.elements;
	int n_mask = mask->matrix.rows * mask->matrix.columns;

	Val subject_val = chain_sp[-3];
	Val replacement_val = chain_sp[-1];

	if (VAL_TAG(subject_val) == T_ARRAY) {
		Object *subject = OBJECT_AT(VAL_DATA(subject_val));
		int n_elements = subject->len;

		if (n_mask != n_elements) {
			fail(interp, "mask has %d elements; subject has %d", n_mask, n_elements);
			return;
		}
		if (VAL_TAG(replacement_val) == T_MATRIX) {
			fail(interp, "expected an array or a single replacement value for an array subject; got a matrix");
			return;
		}

		Val *replacement_items = NULL;
		if (VAL_TAG(replacement_val) == T_ARRAY) {
			Object *replacement = OBJECT_AT(VAL_DATA(replacement_val));
			if (replacement->len != n_elements) {
				fail(interp, "replacement has %d elements; subject has %d", replacement->len, n_elements);
				return;
			}
			replacement_items = replacement->items;
		}

		NEW_ARRAY(picked_handle, picked, n_elements);
		Val *subject_items = subject->items;
		for (int i = 0; i < n_elements; i++) {
			if (mesh_replaces(mask_elements[i]))
				picked->items[i] = replacement_items ? replacement_items[i] : replacement_val;
			else
				picked->items[i] = subject_items[i];
		}

		chain_sp[-3] = make_array(picked_handle);
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
	}

	int subject_unit;
	Val subject_matrix_val = quantity_unwrap(subject_val, &subject_unit);
	REQUIRE_CHAIN_TAG(subject_matrix_val, T_MATRIX, "mesh", "a matrix or array subject");
	Object *subject = OBJECT_AT(VAL_DATA(subject_matrix_val));

	if (mask->matrix.rows != subject->matrix.rows || mask->matrix.columns != subject->matrix.columns) {
		fail(interp, "mask is %dx%d; subject is %dx%d",
				mask->matrix.rows, mask->matrix.columns,
				subject->matrix.rows, subject->matrix.columns);
		return;
	}

	int replacement_unit;
	Val replacement_matrix_val = quantity_unwrap(replacement_val, &replacement_unit);
	double conversion = 1.0;
	if (VAL_TAG(replacement_matrix_val) != T_NONE) {
		if ((subject_unit == 0) != (replacement_unit == 0)) {
			fail(interp, "cannot mesh a quantity and a plain number");
			return;
		}
		if (subject_unit != replacement_unit) {
			if (!unit_conversion(replacement_unit, subject_unit, &conversion)) {
				fail(interp, "unit mismatch");
				return;
			}
		}
	}

	const double *replacement_elements = NULL;
	double replacement_scalar = 0.0;
	if (VAL_TAG(replacement_matrix_val) == T_MATRIX) {
		Object *replacement = OBJECT_AT(VAL_DATA(replacement_matrix_val));
		if (replacement->matrix.rows != subject->matrix.rows
				|| replacement->matrix.columns != subject->matrix.columns) {
			fail(interp, "replacement is %dx%d; subject is %dx%d",
					replacement->matrix.rows, replacement->matrix.columns,
					subject->matrix.rows, subject->matrix.columns);
			return;
		}
		replacement_elements = replacement->matrix.elements;
	} else if (VAL_TAG(replacement_matrix_val) == T_FLOAT) {
		replacement_scalar = VAL_NUMBER(replacement_matrix_val) * conversion;
	} else if (VAL_TAG(replacement_matrix_val) == T_NONE) {
		replacement_scalar = NAN;
	} else {
		fail(interp, "expected a float, null, matrix, or quantity replacement; got %s",
				tag_name(VAL_TAG(replacement_matrix_val)));
		return;
	}

	NEW_MATRIX(target_handle, target, subject->matrix.rows, subject->matrix.columns);
	const double *subject_elements = subject->matrix.elements;
	double *target_elements = target->matrix.elements;
	for (int i = 0; i < n_mask; i++) {
		if (mesh_replaces(mask_elements[i]))
			target_elements[i] = replacement_elements ? replacement_elements[i] * conversion : replacement_scalar;
		else
			target_elements[i] = subject_elements[i];
	}

	if (subject_unit) {
		SYNC_REGISTERS(interp, chain_ip, chain_sp - 3);
		push_quantity(interp, make_matrix(target_handle), subject_unit);
		DISPATCH(interp);
	}

	chain_sp[-3] = make_matrix(target_handle);
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}

void p_submatrix(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 5);
	Val col_end_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(col_end_val, T_FLOAT, "submatrix", "a float col-end");
	int col_end = (int)VAL_NUMBER(col_end_val);
	Val col_start_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(col_start_val, T_FLOAT, "submatrix", "a float col-start");
	int col_start = (int)VAL_NUMBER(col_start_val);
	Val row_end_val = chain_sp[-3];
	REQUIRE_CHAIN_TAG(row_end_val, T_FLOAT, "submatrix", "a float row-end");
	int row_end = (int)VAL_NUMBER(row_end_val);
	Val row_start_val = chain_sp[-4];
	REQUIRE_CHAIN_TAG(row_start_val, T_FLOAT, "submatrix", "a float row-start");
	int row_start = (int)VAL_NUMBER(row_start_val);
	Val source_val = chain_sp[-5];
	REQUIRE_CHAIN_TAG(source_val, T_MATRIX, "submatrix", "a matrix");
	Object *source = OBJECT_AT(VAL_DATA(source_val));

	if (row_start < 0 || row_end > source->matrix.rows || row_start > row_end
			|| col_start < 0 || col_end > source->matrix.columns || col_start > col_end) {
		fail(interp, "[%d,%d)x[%d,%d) out of bounds for %dx%d",
				row_start, row_end, col_start, col_end,
				source->matrix.rows, source->matrix.columns);
		return;
	}

	int slice_rows = row_end - row_start;
	int slice_cols = col_end - col_start;
	NEW_MATRIX(slice_handle, slice, slice_rows, slice_cols);

	for (int row = 0; row < slice_rows; row++)
		for (int col = 0; col < slice_cols; col++)
			MAT(slice, row, col) = MAT(source, row_start + row, col_start + col);

	chain_sp[-5] = make_matrix(slice_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 4);
}


#define POP_MATRIX_OR_QUANTITY(name, word_name, unit) \
	POP(name##_val); \
	int unit; \
	name##_val = quantity_unwrap(name##_val, &unit); \
	if (VAL_TAG(name##_val) != T_MATRIX) { \
		fail(interp, "expected %s; got %s", tag_name(T_MATRIX), tag_name(VAL_TAG(name##_val))); \
		return; \
	} \
	Object *name = OBJECT_AT(VAL_DATA(name##_val))

#define MISSING_CHECK_NONE ((void)0)

#define MISSING_CHECK_INFINITE \
	do { \
		if (isinf(statistic) && matrix_nonmissing_count(source) == 0.0) { \
			fail(interp, "all elements are NaN (missing)"); \
			return; \
		} \
	} while (0)

#define MISSING_CHECK_NAN \
	do { \
		if (isnan(statistic)) { \
			fail(interp, "all elements are NaN (missing)"); \
			return; \
		} \
	} while (0)

#define REDUCE_OVERALL_HANDLER(primitive_name, word_name, reduce_fn, result_unit, missing_check) \
	void primitive_name(DISPATCH_ARGS) { \
		POP_MATRIX_OR_QUANTITY(source, word_name, unit); \
		(void)unit; \
		double statistic = reduce_fn(source); \
		missing_check; \
		push_quantity(interp, make_float(statistic), (result_unit)); \
	}

#define REDUCE_AXIS_HANDLER(primitive_name, word_name, reduce_fn) \
	void primitive_name(DISPATCH_ARGS) { \
		POP_MATRIX_OR_QUANTITY(source, word_name, unit); \
		int target_handle = reduce_fn(interp, source); \
		if (!interp->error_flag) push_quantity(interp, make_matrix(target_handle), unit); \
	}

REDUCE_OVERALL_HANDLER(p_sum, "sum", matrix_sum_overall, unit, MISSING_CHECK_NONE)
REDUCE_OVERALL_HANDLER(p_max, "max", matrix_max_overall, unit, MISSING_CHECK_INFINITE)
REDUCE_OVERALL_HANDLER(p_min, "min", matrix_min_overall, unit, MISSING_CHECK_INFINITE)
REDUCE_OVERALL_HANDLER(p_argmax, "argmax", matrix_argmax_index, 0, MISSING_CHECK_NAN)
REDUCE_OVERALL_HANDLER(p_argmin, "argmin", matrix_argmin_index, 0, MISSING_CHECK_NAN)
REDUCE_OVERALL_HANDLER(p_frobenius_norm, "frobenius-norm", matrix_frobenius_overall, unit, MISSING_CHECK_NONE)
REDUCE_OVERALL_HANDLER(p_nonmissing_count, "nonmissing-count", matrix_nonmissing_count, 0, MISSING_CHECK_NONE)
REDUCE_AXIS_HANDLER(p_row_sums, "row-sums", matrix_sum_rows)
REDUCE_AXIS_HANDLER(p_row_maxes, "row-maxes", matrix_max_rows)
REDUCE_AXIS_HANDLER(p_row_mins, "row-mins", matrix_min_rows)
REDUCE_AXIS_HANDLER(p_column_sums, "column-sums", matrix_sum_columns)
REDUCE_AXIS_HANDLER(p_column_maxes, "column-maxes", matrix_max_columns)
REDUCE_AXIS_HANDLER(p_column_mins, "column-mins", matrix_min_columns)

double matrix_variance_overall(Object *source, size_t *n_nonmissing_out) {
	size_t n = (size_t)(source->matrix.rows * source->matrix.columns);
	const double * restrict elements = source->matrix.elements;
	double reference = 0.0;
	int have_reference = 0;
	double sum_shifted = 0.0;
	double sum_shifted_squares = 0.0;
	size_t n_nonmissing = 0;

	for (size_t i = 0; i < n; i++) {
		double value = elements[i];
		if (value != value)
			continue;
		if (!have_reference) {
			reference = value;
			have_reference = 1;
		}
		double shifted = value - reference;
		n_nonmissing++;
		sum_shifted += shifted;
		sum_shifted_squares += shifted * shifted;
	}

	*n_nonmissing_out = n_nonmissing;
	if (n_nonmissing < 2)
		return NAN;
	return (sum_shifted_squares - sum_shifted * sum_shifted / (double)n_nonmissing) / (double)(n_nonmissing - 1);
}

void p_variance(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	int unit;
	Val source_val = quantity_unwrap(chain_sp[-1], &unit);
	REQUIRE_CHAIN_TAG(source_val, T_MATRIX, "var", "a matrix");
	Object *source = OBJECT_AT(VAL_DATA(source_val));

	size_t n_nonmissing;
	double variance = matrix_variance_overall(source, &n_nonmissing);
	if (n_nonmissing < 2) {
		fail(interp, "needs at least 2 non-NaN elements; got %zu", n_nonmissing);
		return;
	}

	if (unit) {
		int squared_unit = unit_pow(interp, unit, 2, 1);
		if (interp->error_flag)
			return;

		SYNC_REGISTERS(interp, chain_ip, chain_sp - 1);
		push_quantity(interp, make_float(variance), squared_unit);
		DISPATCH(interp);
	}

	chain_sp[-1] = make_float(variance);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_quantile(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val probability_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(probability_val, T_FLOAT, "quantile", "a float probability");
	double probability = VAL_NUMBER(probability_val);
	if (probability < 0.0 || probability > 1.0) {
		fail(interp, "probability must be in [0,1]; got %g", probability);
		return;
	}

	int unit;
	Val source_val = quantity_unwrap(chain_sp[-2], &unit);
	REQUIRE_CHAIN_TAG(source_val, T_MATRIX, "quantile", "a matrix");
	Object *source = OBJECT_AT(VAL_DATA(source_val));
	size_t n = (size_t)(source->matrix.rows * source->matrix.columns);
	if (n == 0) {
		fail(interp, "empty matrix");
		return;
	}

	double *sorted;
	MALLOC_OR_FAIL(interp, sorted, n * sizeof(double));
	size_t n_nonmissing = 0;
	for (size_t i = 0; i < n; i++) {
		double element = source->matrix.elements[i];
		if (element != element)
			continue;
		sorted[n_nonmissing++] = element;
	}
	if (n_nonmissing == 0) {
		free(sorted);
		fail(interp, "all elements are NaN (missing)");
		return;
	}
	sort_doubles(sorted, n_nonmissing);

	double rank = probability * (double)(n_nonmissing - 1);
	size_t lower = (size_t)rank;
	double fraction = rank - (double)lower;
	double value = sorted[lower];

	if (lower + 1 < n_nonmissing)
		value += fraction * (sorted[lower + 1] - sorted[lower]);

	free(sorted);

	if (unit) {
		SYNC_REGISTERS(interp, chain_ip, chain_sp - 2);
		push_quantity(interp, make_float(value), unit);
		DISPATCH(interp);
	}

	chain_sp[-2] = make_float(value);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_cumulative_sum(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	int unit;
	Val source_val = quantity_unwrap(chain_sp[-1], &unit);
	REQUIRE_CHAIN_TAG(source_val, T_MATRIX, "cumulative-sum", "a matrix");
	Object *source = OBJECT_AT(VAL_DATA(source_val));

	int n_rows = source->matrix.rows;
	int n_columns = source->matrix.columns;
	int n_elements = n_rows * n_columns;

	NEW_MATRIX(running_sums_handle, running_sums, n_rows, n_columns);

	const double *source_elements = source->matrix.elements;
	double *running_sums_elements = running_sums->matrix.elements;
	double running_total = 0.0;
	for (int i = 0; i < n_elements; i++) {
		running_total += source_elements[i];
		running_sums_elements[i] = running_total;
	}

	if (unit) {
		SYNC_REGISTERS(interp, chain_ip, chain_sp - 1);
		push_quantity(interp, make_matrix(running_sums_handle), unit);
		DISPATCH(interp);
	}

	chain_sp[-1] = make_matrix(running_sums_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_matrix_range(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3);
	Val step_val = chain_sp[-1];
	Val end_val = chain_sp[-2];
	Val start_val = chain_sp[-3];

	if (VAL_TAG(start_val) != T_FLOAT || VAL_TAG(end_val) != T_FLOAT || VAL_TAG(step_val) != T_FLOAT) {
		fail(interp, "expected three floats (start end step); got %s, %s, %s",
				tag_name(VAL_TAG(start_val)), tag_name(VAL_TAG(end_val)), tag_name(VAL_TAG(step_val)));
		return;
	}

	double start = VAL_NUMBER(start_val);
	double end = VAL_NUMBER(end_val);
	double step = VAL_NUMBER(step_val);

	if (!isfinite(start) || !isfinite(end) || !isfinite(step)) {
		fail(interp, "start, end, and step must be finite");
		return;
	}

	if (step == 0.0) {
		fail(interp, "step cannot be zero");
		return;
	}

	if ((step > 0.0 && end < start) || (step < 0.0 && end > start)) {
		fail(interp, "step sign does not match start/end direction");
		return;
	}

	double raw_steps = (end - start) / step;
	if (raw_steps > (double)INT_MAX - 1.0) {
		fail(interp, "too many elements");
		return;
	}
	int n_steps = (int)raw_steps + 1;
	NEW_MATRIX(handle, matrix, 1, n_steps);
	double *elements = matrix->matrix.elements;

	for (int i = 0; i < n_steps; i++)
		elements[i] = start + i * step;

	chain_sp[-3] = make_matrix(handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}

void p_select_rows(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val indices_val = chain_sp[-1];
	int unit;
	Val matrix_val = quantity_unwrap(chain_sp[-2], &unit);

	if (VAL_TAG(matrix_val) == T_ARRAY) {
		Object *source_array = OBJECT_AT(VAL_DATA(matrix_val));
		int n_source = source_array->len;
		const double *vector_elements = NULL;
		Object *index_array = NULL;
		int n_indices;

		if (VAL_TAG(indices_val) == T_MATRIX) {
			Object *index_vector = OBJECT_AT(VAL_DATA(indices_val));
			n_indices = vector_length(interp, index_vector, "an index vector");
			if (n_indices < 0)
				return;
			vector_elements = index_vector->matrix.elements;
		} else {
			REQUIRE_CHAIN_TAG(indices_val, T_ARRAY, "select-rows", "an index array or vector");
			index_array = OBJECT_AT(VAL_DATA(indices_val));
			n_indices = index_array->len;
		}

		for (int i = 0; i < n_indices; i++) {
			Val index_val = vector_elements ? make_float(vector_elements[i]) : index_array->items[i];
			if (VAL_TAG(index_val) != T_FLOAT) {
				fail(interp, "index %d is %s, expected a float", i, tag_name(VAL_TAG(index_val)));
				return;
			}
			int element = (int)VAL_NUMBER(index_val);
			if (element < 0 || element >= n_source) {
				fail(interp, "array index %d out of bounds (length %d)", element, n_source);
				return;
			}
		}

		NEW_ARRAY(gathered_handle, gathered, n_indices);
		source_array = OBJECT_AT(VAL_DATA(matrix_val));
		if (vector_elements)
			vector_elements = OBJECT_AT(VAL_DATA(indices_val))->matrix.elements;
		else
			index_array = OBJECT_AT(VAL_DATA(indices_val));

		for (int i = 0; i < n_indices; i++) {
			int element = vector_elements
				? (int)vector_elements[i] : (int)VAL_NUMBER(index_array->items[i]);
			gathered->items[i] = source_array->items[element];
		}

		chain_sp[-2] = make_array(gathered_handle);

		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
	}

	REQUIRE_CHAIN_TAG(matrix_val, T_MATRIX, "select-rows", "a matrix");
	Object *source = OBJECT_AT(VAL_DATA(matrix_val));

	int n_source_columns = source->matrix.columns;
	int n_source_rows = source->matrix.rows;

	if (VAL_TAG(indices_val) == T_MATRIX) {
		Object *index_vector = OBJECT_AT(VAL_DATA(indices_val));
		int n_indices = vector_length(interp, index_vector, "an index vector");
		if (n_indices < 0)
			return;

		const double *elements = index_vector->matrix.elements;
		for (int i = 0; i < n_indices; i++) {
			int row = (int)elements[i];
			if (row < 0 || row >= n_source_rows) {
				fail(interp, "row %d out of bounds (%d rows)", row, n_source_rows);
				return;
			}
		}

		NEW_MATRIX(vector_selected_handle, vector_selected, n_indices, n_source_columns);

		for (int i = 0; i < n_indices; i++)
			memcpy(&MAT(vector_selected, i, 0), &MAT(source, (int)elements[i], 0), sizeof(double) * (size_t)n_source_columns);
		if (unit) {
			SYNC_REGISTERS(interp, chain_ip, chain_sp - 2);
			push_quantity(interp, make_matrix(vector_selected_handle), unit);
			DISPATCH(interp);
		}
		chain_sp[-2] = make_matrix(vector_selected_handle);

		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
	}

	REQUIRE_CHAIN_TAG(indices_val, T_ARRAY, "select-rows", "an index array or vector");
	Object *indices = OBJECT_AT(VAL_DATA(indices_val));

	for (int i = 0; i < indices->len; i++) {
		if (VAL_TAG(indices->items[i]) != T_FLOAT) {
			fail(interp, "index %d is %s, expected a float", i, tag_name(VAL_TAG(indices->items[i])));
			return;
		}
		int row = (int)VAL_NUMBER(indices->items[i]);
		if (row < 0 || row >= n_source_rows) {
			fail(interp, "row %d out of bounds (%d rows)", row, n_source_rows);
			return;
		}
	}

	NEW_MATRIX(selected_handle, selected, indices->len, n_source_columns);

	for (int i = 0; i < indices->len; i++) {
		int row = (int)VAL_NUMBER(indices->items[i]);
		memcpy(&MAT(selected, i, 0), &MAT(source, row, 0), sizeof(double) * (size_t)n_source_columns);
	}
	
	if (unit) {
		SYNC_REGISTERS(interp, chain_ip, chain_sp - 2);
		push_quantity(interp, make_matrix(selected_handle), unit);
		DISPATCH(interp);
	}

	chain_sp[-2] = make_matrix(selected_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_augment(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val b_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(b_val, T_MATRIX, "augment", "a matrix");
	Val a_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(a_val, T_MATRIX, "augment", "a matrix");
	Object *a = OBJECT_AT(VAL_DATA(a_val));
	Object *b = OBJECT_AT(VAL_DATA(b_val));

	if (a->matrix.rows != b->matrix.rows) {
		fail(interp, "row counts differ (%d vs %d)", a->matrix.rows, b->matrix.rows);
		return;
	}

	int rows = a->matrix.rows;
	int a_columns = a->matrix.columns;
	int b_columns = b->matrix.columns;
	NEW_MATRIX(augmented_handle, augmented, rows, a_columns + b_columns);

	for (int i = 0; i < rows; i++) {
		memcpy(&MAT(augmented, i, 0), &MAT(a, i, 0), sizeof(double) * (size_t)a_columns);
		memcpy(&MAT(augmented, i, a_columns), &MAT(b, i, 0), sizeof(double) * (size_t)b_columns);
	}

	chain_sp[-2] = make_matrix(augmented_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_vstack(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val b_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(b_val, T_MATRIX, "vstack", "a matrix");
	Val a_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(a_val, T_MATRIX, "vstack", "a matrix");
	Object *a = OBJECT_AT(VAL_DATA(a_val));
	Object *b = OBJECT_AT(VAL_DATA(b_val));

	if (a->matrix.columns != b->matrix.columns) {
		fail(interp, "column counts differ (%d vs %d)", a->matrix.columns, b->matrix.columns);
		return;
	}

	int columns = a->matrix.columns;
	int a_rows = a->matrix.rows;
	int b_rows = b->matrix.rows;
	NEW_MATRIX(stacked_handle, stacked, a_rows + b_rows, columns);

	size_t a_cells = (size_t)a_rows * (size_t)columns;
	memcpy(stacked->matrix.elements, a->matrix.elements, sizeof(double) * a_cells);
	memcpy(stacked->matrix.elements + a_cells, b->matrix.elements,
			sizeof(double) * (size_t)b_rows * (size_t)columns);

	chain_sp[-2] = make_matrix(stacked_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

