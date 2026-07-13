#include "water.h"


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
		int target_handle = object_new_matrix(interp, rows, cols); \
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

#define REQUIRE_CHAIN_TAG(value, tag, op, expected_phrase) \
	do { \
		if (VAL_TAG(value) != (tag)) { \
			fail(interp, "expected " expected_phrase "; got %s", tag_name(VAL_TAG(value))); \
			return; \
		} \
	} while (0)

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

	SYNC_REGISTERS(interp, chain_ip, chain_sp - 2);
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

static Val *matrix_element_read(Interpreter *interp, cell *resume_ip, Val *slot_sp, Val source_val, int index) {
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

void p_at_e_l1l0(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip + 2, chain_sp, 1);
	int enclosing = saved_local_base(interp->return_stack[interp->local_base - 1]);
	Val source_val = interp->return_stack[enclosing + (int)chain_ip[0]];
	int index = (int)interp->return_stack[interp->local_base + (int)chain_ip[1]].number;
	Val *pushed_sp = matrix_element_read(interp, chain_ip + 2, chain_sp, source_val, index);
	if (!pushed_sp)
		return;
	DISPATCH_REGISTERS(interp, chain_ip + 2, pushed_sp);
}

static int matrix_element_write(Interpreter *interp, cell *resume_ip, Val *fail_sp, Val target_val, int index, Val element_val) {
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
		Val element_val = chain_sp[-1]; \
		Val index_val = chain_sp[-2]; \
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
	int index = (int)locals[(int)chain_ip[1]].number;
	Val element_val = locals[(int)chain_ip[2]];
	if (!matrix_element_write(interp, chain_ip + 3, chain_sp, target_val, index, element_val))
		return;
	DISPATCH_REGISTERS(interp, chain_ip + 3, chain_sp);
}

#define STORE_IJ_OP(c_name, n_consumed) \
	void c_name(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 4); \
		Val element_val = chain_sp[-1]; \
		Val j_val = chain_sp[-2]; \
		Val i_val = chain_sp[-3]; \
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

int dgemm_kernel(Interpreter *interp, int transpose_a, int transpose_b,
		double alpha,
		int a_handle, int b_handle,
		double beta, int c_handle) {
	int i, j, k, m, n, p;

	Object *A = OBJECT_AT(a_handle);
	Object *B = OBJECT_AT(b_handle);
	Object *C = OBJECT_AT(c_handle);

	int op_a_rows = transpose_a ? A->matrix.columns : A->matrix.rows;
	int op_a_cols = transpose_a ? A->matrix.rows : A->matrix.columns;
	int op_b_rows = transpose_b ? B->matrix.columns : B->matrix.rows;
	int op_b_cols = transpose_b ? B->matrix.rows : B->matrix.columns;

	if (op_a_cols != op_b_rows) {
		fail(interp, "inner dimensions must match (op(A) is %dx%d, op(B) is %dx%d)",
				op_a_rows, op_a_cols, op_b_rows, op_b_cols);
		return -1;
	}

	if (C->matrix.rows != op_a_rows || C->matrix.columns != op_b_cols) {
		fail(interp, "C must be %dx%d to match the product, but is %dx%d",
				op_a_rows, op_b_cols, C->matrix.rows, C->matrix.columns);
		return -1;
	}

	m = op_a_rows;
	n = op_b_cols;
	k = op_a_cols;

	int matmult_handle = object_new_matrix(interp, m, n);
	if (interp->error_flag) return -1;
	Object *matmult = OBJECT_AT(matmult_handle);

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
	} else if (!transpose_a && transpose_b) {
#pragma clang fp reassociate(on)
		const double * restrict a_elements = A->matrix.elements;
		const double * restrict b_elements = B->matrix.elements;
		for (i = 0; i < m; i++) {
			for (j = 0; j < n; j++) {
				const double * restrict a_row = &a_elements[i * k];
				const double * restrict b_row = &b_elements[j * k];
				double sum = 0.0;
				for (p = 0; p < k; p++)
					sum += a_row[p] * b_row[p];
				MAT(matmult, i, j) = alpha * sum + beta * MAT(C, i, j);
			}
		}
	} else if (transpose_a && !transpose_b) {
		double * restrict out_elements = matmult->matrix.elements;
		const double * restrict a_elements = A->matrix.elements;
		const double * restrict b_elements = B->matrix.elements;
		const double * restrict c_elements = C->matrix.elements;

		if (n < 8) {
			for (i = 0; i < m; i++) {
				for (j = 0; j < n; j++) {
					double sum = 0.0;
					for (p = 0; p < k; p++)
						sum += a_elements[p * m + i] * b_elements[p * n + j];
					out_elements[i * n + j] = alpha * sum + beta * c_elements[i * n + j];
				}
			}
		} else {
			for (i = 0; i < m; i++)
				for (j = 0; j < n; j++)
					out_elements[i * n + j] = beta * c_elements[i * n + j];

			for (i = 0; i < m; i++) {
				for (p = 0; p < k; p++) {
					double a_val = alpha * a_elements[p * m + i];
					const double *b_row = &b_elements[p * n];
					double *out_row = &out_elements[i * n];
					for (j = 0; j < n; j++)
						out_row[j] += a_val * b_row[j];
				}
			}
		}
	} else {
#pragma clang fp reassociate(on)
		const double * restrict a_elements = A->matrix.elements;
		const double * restrict b_elements = B->matrix.elements;
		double *a_column = malloc(sizeof(double) * (size_t)k);
		if (!a_column) {
			fail(interp, "out of memory for a %d-element column buffer", k);
			return -1;
		}

		for (i = 0; i < m; i++) {
			for (p = 0; p < k; p++)
				a_column[p] = a_elements[p * m + i];
			for (j = 0; j < n; j++) {
				const double * restrict b_row = &b_elements[j * k];
				double sum = 0.0;
				for (p = 0; p < k; p++)
					sum += a_column[p] * b_row[p];
				MAT(matmult, i, j) = alpha * sum + beta * MAT(C, i, j);
			}
		}

		free(a_column);
	}

	return matmult_handle;
}

void p_dgemm_helper(Interpreter *interp, int transpose_a, int transpose_b) {
	POP(c_val);
	POP(beta_val);
	POP(b_val);
	POP(a_val);
	POP(alpha_val);

	if (VAL_TAG(alpha_val) != T_FLOAT || VAL_TAG(beta_val) != T_FLOAT) {
		fail(interp, "alpha and beta must be floats; got %s and %s",
				tag_name(VAL_TAG(alpha_val)), tag_name(VAL_TAG(beta_val)));
		return;
	}
	if (VAL_TAG(a_val) != T_MATRIX || VAL_TAG(b_val) != T_MATRIX || VAL_TAG(c_val) != T_MATRIX) {
		fail(interp, "A, B, C must be matrices; got %s, %s, %s",
				tag_name(VAL_TAG(a_val)), tag_name(VAL_TAG(b_val)), tag_name(VAL_TAG(c_val)));
		return;
	}

	int matmult_handle = dgemm_kernel(interp, transpose_a, transpose_b,
			VAL_NUMBER(alpha_val),
			(int)VAL_DATA(a_val), (int)VAL_DATA(b_val),
			VAL_NUMBER(beta_val),
			(int)VAL_DATA(c_val));
	if (interp->error_flag) return;
	push(interp, make_matrix(matmult_handle));
}

#define DGEMM_WORD(c_name, transpose_a, transpose_b) \
	void c_name(DISPATCH_ARGS) { \
		p_dgemm_helper(interp, transpose_a, transpose_b); \
		\
		DISPATCH(interp); \
	}

DGEMM_WORD(p_dgemm_nn, 0, 0)
DGEMM_WORD(p_dgemm_tn, 1, 0)
DGEMM_WORD(p_dgemm_nt, 0, 1)
DGEMM_WORD(p_dgemm_tt, 1, 1)

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

typedef struct {
	double value;
	int index;
} ArgsortPair;

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

#define RADIX_SORT_CUTOFF 8192
#define RADIX_DIGITS 65536

typedef uint64_t __attribute__((may_alias)) sort_key;

#define DOUBLE_KEY(element) (*(sort_key *)&(element))
#define PAIR_KEY(element) (*(sort_key *)&(element).value)

#define RADIX_FORWARD(k) ((k) ^ (-((k) >> 63) | 0x8000000000000000ULL))
#define RADIX_INVERSE(k) ((k) ^ ((((k) >> 63) - 1) | 0x8000000000000000ULL))

#define RADIX_SORT(suffix, element_type, key) \
	static void radix_sort_##suffix(element_type *elements, size_t n_elements, \
			element_type *scratch, size_t *digit_counts) { \
		element_type *from = elements; \
		element_type *to = scratch; \
		int transformed = 0; \
		for (int pass = 0; pass < 4; pass++) { \
			int shift = pass * 16; \
			\
			memset(digit_counts, 0, RADIX_DIGITS * sizeof(size_t)); \
			if (transformed) \
				for (size_t i = 0; i < n_elements; i++) \
					digit_counts[(key(from[i]) >> shift) & 0xFFFF]++; \
			else \
				for (size_t i = 0; i < n_elements; i++) \
					digit_counts[(RADIX_FORWARD(key(from[i])) >> shift) & 0xFFFF]++; \
			\
			sort_key first_key = key(from[0]); \
			if (!transformed) \
				first_key = RADIX_FORWARD(first_key); \
			if (digit_counts[(first_key >> shift) & 0xFFFF] == n_elements) \
				continue; \
			\
			size_t running = 0; \
			for (int digit = 0; digit < RADIX_DIGITS; digit++) { \
				size_t digit_count = digit_counts[digit]; \
				digit_counts[digit] = running; \
				running += digit_count; \
			} \
			\
			int final_pass = pass == 3; \
			for (size_t i = 0; i < n_elements; i++) { \
				element_type element = from[i]; \
				sort_key sortable_key = key(element); \
				if (!transformed) \
					sortable_key = RADIX_FORWARD(sortable_key); \
				key(element) = final_pass ? RADIX_INVERSE(sortable_key) : sortable_key; \
				to[digit_counts[(sortable_key >> shift) & 0xFFFF]++] = element; \
			} \
			transformed = !final_pass; \
			\
			element_type *filled = to; \
			to = from; \
			from = filled; \
		} \
		\
		if (from != elements) { \
			if (transformed) \
				for (size_t i = 0; i < n_elements; i++) { \
					element_type element = from[i]; \
					key(element) = RADIX_INVERSE(key(element)); \
					elements[i] = element; \
				} \
			else \
				memcpy(elements, from, n_elements * sizeof(element_type)); \
		} else if (transformed) \
			for (size_t i = 0; i < n_elements; i++) \
				key(elements[i]) = RADIX_INVERSE(key(elements[i])); \
	}

RADIX_SORT(doubles, double, DOUBLE_KEY)
RADIX_SORT(pairs, ArgsortPair, PAIR_KEY)

#define SORT_DISPATCH(suffix, element_type) \
	static void sort_##suffix(element_type *elements, size_t n_elements) { \
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

SORT_DISPATCH(doubles, double)
SORT_DISPATCH(pairs, ArgsortPair)

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
	ArgsortPair *pairs = malloc(n_elements * sizeof(ArgsortPair));
	if (!pairs) {
		fail(interp, "out of memory");
		return -1;
	}

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
	SYNC_REGISTERS(interp, chain_ip, chain_sp - 1);

	if (VAL_TAG(mask) != T_MATRIX) {
		fail(interp, "expected a matrix mask; got %s", tag_name(VAL_TAG(mask)));
		return;
	}

	int indices_handle = matrix_nonzero_indices(interp, OBJECT_AT(VAL_DATA(mask)));
	if (interp->error_flag) return;

	chain_sp[-1] = make_matrix(indices_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

#define ADD(a, b) ((a) + (b))

#define MATRIX_REDUCE_OVERALL_OP(name, init_value, combine) \
	double name(Object *source) { \
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
		int target_handle = object_new_matrix(interp, rows, 1); \
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
		int target_handle = object_new_matrix(interp, 1, cols); \
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

#pragma float_control(precise, off, push)
MATRIX_REDUCE_OVERALL_OP(matrix_sum_overall, 0.0, ADD)
MATRIX_REDUCE_OVERALL_OP(matrix_max_overall, -INFINITY, MAX)
MATRIX_REDUCE_OVERALL_OP(matrix_min_overall, INFINITY, MIN)

MATRIX_REDUCE_ROWS_OP(matrix_sum_rows, 0.0, ADD)
MATRIX_REDUCE_ROWS_OP(matrix_max_rows, -INFINITY, MAX)
MATRIX_REDUCE_ROWS_OP(matrix_min_rows, INFINITY, MIN)

MATRIX_REDUCE_COLUMNS_OP(matrix_sum_columns, 0.0, ADD)
MATRIX_REDUCE_COLUMNS_OP(matrix_max_columns, -INFINITY, MAX)
MATRIX_REDUCE_COLUMNS_OP(matrix_min_columns, INFINITY, MIN)

#define MATRIX_ARG_OP(name, cmp) \
	static int name(Object *source) { \
		size_t num_elements = (size_t)source->matrix.rows * (size_t)source->matrix.columns; \
		const double * restrict elements = source->matrix.elements; \
		size_t best = 0; \
		for (size_t i = 1; i < num_elements; i++) \
			if (elements[i] cmp elements[best]) \
				best = i; \
		return (int)best; \
	}

MATRIX_ARG_OP(matrix_argmax_index, >)
MATRIX_ARG_OP(matrix_argmin_index, <)

double matrix_variance_overall(Object *source) {
	size_t n = (size_t)(source->matrix.rows * source->matrix.columns);
	const double * restrict elements = source->matrix.elements;
	double sum = 0.0;
	double sum_of_squares = 0.0;

	for (size_t i = 0; i < n; i++) {
		double value = elements[i];
		sum += value;
		sum_of_squares += value * value;
	}

	return (sum_of_squares - sum * sum/(double)n) / (double)(n - 1);
}

static double matrix_frobenius_overall(Object *source) {
	size_t n = (size_t)source->matrix.rows * (size_t)source->matrix.columns;
	const double * restrict elements = source->matrix.elements;
	double sum_of_squares = 0.0;
	for (size_t i = 0; i < n; i++)
		sum_of_squares += elements[i] * elements[i];
	return sqrt(sum_of_squares);
}
#pragma float_control(pop)

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
	if (VAL_TAG(diag_val) != T_FLOAT) {
		fail(interp, "expected a float fill value; got %s", tag_name(VAL_TAG(diag_val)));
		return;
	}

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
	Val source_val = chain_sp[-3];
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
	if (VAL_TAG(top) != T_FLOAT) {
		fail(interp, "expected a float dimension on top; got %s", tag_name(VAL_TAG(top)));
		return;
	}

	int num_rows, num_cols;
	Val arr_val;
	if (VAL_TAG(below) == T_FLOAT) {
		if (interp->dsp < 3) {
			fail(interp, "stack underflow (expected array below two dimensions)");
			return;
		}
		arr_val = interp->data_stack[interp->dsp - 3];
		if (VAL_TAG(arr_val) != T_ARRAY) {
			fail(interp, "expected an array; got %s", tag_name(VAL_TAG(arr_val)));
			return;
		}
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
		if (VAL_TAG(input_array->items[i]) != T_FLOAT) {
			fail(interp, "element %d is %s, expected a float", i, tag_name(VAL_TAG(input_array->items[i])));
			return;
		}
		matrix->matrix.elements[i] = VAL_NUMBER(input_array->items[i]);
	}

	push(interp, make_matrix(matrix_handle));

	DISPATCH(interp);
}

void p_dim(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	Val matrix_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(matrix_val, T_MATRIX, "dim", "a matrix");
	Object *matrix = OBJECT_AT(VAL_DATA(matrix_val));

	chain_sp[-1] = make_float(matrix->matrix.rows);
	chain_sp[0] = make_float(matrix->matrix.columns);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

void p_transpose(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val source_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(source_val, T_MATRIX, "transpose", "a matrix");
	Object *source = OBJECT_AT(VAL_DATA(source_val));

	NEW_MATRIX(target_handle, target, source->matrix.columns, source->matrix.rows);
	for (int i = 0; i < source->matrix.rows; i++)
		for (int j = 0; j < source->matrix.columns; j++)
			MAT(target, j, i) = MAT(source, i, j);

	chain_sp[-1] = make_matrix(target_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
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


#define REDUCE_OVERALL_HANDLER(primitive_name, word_name, reduce_fn) \
	void primitive_name(DISPATCH_ARGS) { \
		POP_MATRIX(source, word_name); \
		push(interp, make_float(reduce_fn(source))); \
	}

#define REDUCE_AXIS_HANDLER(primitive_name, word_name, reduce_fn) \
	void primitive_name(DISPATCH_ARGS) { \
		POP_MATRIX(source, word_name); \
		int target_handle = reduce_fn(interp, source); \
		if (!interp->error_flag) push(interp, make_matrix(target_handle)); \
	}

REDUCE_OVERALL_HANDLER(p_sum, "sum", matrix_sum_overall)
REDUCE_OVERALL_HANDLER(p_max, "max", matrix_max_overall)
REDUCE_OVERALL_HANDLER(p_min, "min", matrix_min_overall)
REDUCE_OVERALL_HANDLER(p_argmax, "argmax", matrix_argmax_index)
REDUCE_OVERALL_HANDLER(p_argmin, "argmin", matrix_argmin_index)
REDUCE_OVERALL_HANDLER(p_norm, "norm", matrix_frobenius_overall)
REDUCE_OVERALL_HANDLER(p_frobenius_norm, "frobenius-norm", matrix_frobenius_overall)
REDUCE_AXIS_HANDLER(p_row_sums, "row-sums", matrix_sum_rows)
REDUCE_AXIS_HANDLER(p_row_maxes, "row-maxes", matrix_max_rows)
REDUCE_AXIS_HANDLER(p_row_mins, "row-mins", matrix_min_rows)
REDUCE_AXIS_HANDLER(p_column_sums, "column-sums", matrix_sum_columns)
REDUCE_AXIS_HANDLER(p_column_maxes, "column-maxes", matrix_max_columns)
REDUCE_AXIS_HANDLER(p_column_mins, "column-mins", matrix_min_columns)

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
	Val matrix_val = chain_sp[-2];
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
		chain_sp[-2] = make_matrix(vector_selected_handle);

		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
	}

	if (VAL_TAG(indices_val) != T_ARRAY) {
		fail(interp, "expected an index array or vector; got %s", tag_name(VAL_TAG(indices_val)));
		return;
	}
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

void p_variance(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val source_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(source_val, T_MATRIX, "var", "a matrix");
	Object *source = OBJECT_AT(VAL_DATA(source_val));
	size_t n = (size_t)(source->matrix.rows * source->matrix.columns);
	if (n < 2) {
		fail(interp, "needs at least 2 elements; got %zu", n);
		return;
	}

	chain_sp[-1] = make_float(matrix_variance_overall(source));

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

	Val source_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(source_val, T_MATRIX, "quantile", "a matrix");
	Object *source = OBJECT_AT(VAL_DATA(source_val));
	size_t n = (size_t)(source->matrix.rows * source->matrix.columns);
	if (n == 0) {
		fail(interp, "empty matrix");
		return;
	}

	double *sorted = malloc(n * sizeof(double));
	if (!sorted) {
		fail(interp, "out of memory");
		return;
	}
	memcpy(sorted, source->matrix.elements, n * sizeof(double));
	qsort(sorted, n, sizeof(double), double_cmp);

	double rank = probability * (double)(n - 1);
	size_t lower = (size_t)rank;
	double fraction = rank - (double)lower;
	double value = sorted[lower];
	if (lower + 1 < n)
		value += fraction * (sorted[lower + 1] - sorted[lower]);

	free(sorted);
	chain_sp[-2] = make_float(value);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}
