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
			fail(interp, opname ": shapes not broadcast-compatible (%dx%d vs %dx%d)", \
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

void p_at_j(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val index_val = chain_sp[-1];
	Val source_val = chain_sp[-2];
	if (VAL_TAG(index_val) != T_FLOAT) {
		SYNC_REGISTERS(interp, chain_ip, chain_sp);
		fail(interp, "@j: expected a float index; got %s", tag_name(VAL_TAG(index_val)));
		return;
	}
	if (VAL_TAG(source_val) != T_MATRIX) {
		SYNC_REGISTERS(interp, chain_ip, chain_sp);
		fail(interp, "@j: expected a matrix; got %s", tag_name(VAL_TAG(source_val)));
		return;
	}
	Object *source = OBJECT_AT(VAL_DATA(source_val));
	int index = (int)VAL_NUMBER(index_val);

	if (index < 0 || index >= source->matrix.columns) {
		SYNC_REGISTERS(interp, chain_ip, chain_sp);
		fail(interp, "@j: column index %d out of bounds (%d columns)", index, source->matrix.columns);
		return;
	}

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
	if (VAL_TAG(j_val) != T_FLOAT) {
		SYNC_REGISTERS(interp, chain_ip, chain_sp);
		fail(interp, "@i,j: expected a float column index; got %s", tag_name(VAL_TAG(j_val)));
		return;
	}
	if (VAL_TAG(i_val) != T_FLOAT) {
		SYNC_REGISTERS(interp, chain_ip, chain_sp);
		fail(interp, "@i,j: expected a float row index; got %s", tag_name(VAL_TAG(i_val)));
		return;
	}
	if (VAL_TAG(source_val) != T_MATRIX) {
		SYNC_REGISTERS(interp, chain_ip, chain_sp);
		fail(interp, "@i,j: expected a matrix; got %s", tag_name(VAL_TAG(source_val)));
		return;
	}
	Object *source = OBJECT_AT(VAL_DATA(source_val));
	int i = (int)VAL_NUMBER(i_val);
	int j = (int)VAL_NUMBER(j_val);

	if (i < 0 || i >= source->matrix.rows) {
		SYNC_REGISTERS(interp, chain_ip, chain_sp);
		fail(interp, "@i,j: row index %d out of bounds (%d rows)", i, source->matrix.rows);
		return;
	}
	if (j < 0 || j >= source->matrix.columns) {
		SYNC_REGISTERS(interp, chain_ip, chain_sp);
		fail(interp, "@i,j: column index %d out of bounds (%d columns)", j, source->matrix.columns);
		return;
	}

	chain_sp[-3] = make_float(MAT(source, i, j));

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}

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
		fail(interp, "dgemm: inner dimensions must match (op(A) is %dx%d, op(B) is %dx%d)",
				op_a_rows, op_a_cols, op_b_rows, op_b_cols);
		return -1;
	}

	if (C->matrix.rows != op_a_rows || C->matrix.columns != op_b_cols) {
		fail(interp, "dgemm: C must be %dx%d to match the product, but is %dx%d",
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
			fail(interp, "dgemm: out of memory for a %d-element column buffer", k);
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
		fail(interp, "dgemm: alpha and beta must be floats; got %s and %s",
				tag_name(VAL_TAG(alpha_val)), tag_name(VAL_TAG(beta_val)));
		return;
	}
	if (VAL_TAG(a_val) != T_MATRIX || VAL_TAG(b_val) != T_MATRIX || VAL_TAG(c_val) != T_MATRIX) {
		fail(interp, "dgemm: A, B, C must be matrices; got %s, %s, %s",
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

void p_dgemm_nn(DISPATCH_ARGS) {
	p_dgemm_helper(interp, 0, 0);

	DISPATCH(interp);
}

void p_dgemm_tn(DISPATCH_ARGS) {
	p_dgemm_helper(interp, 1, 0);

	DISPATCH(interp);
}

void p_dgemm_nt(DISPATCH_ARGS) {
	p_dgemm_helper(interp, 0, 1);

	DISPATCH(interp);
}

void p_dgemm_tt(DISPATCH_ARGS) {
	p_dgemm_helper(interp, 1, 1);

	DISPATCH(interp);
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

static void insertion_sort_doubles(double *elements, size_t n_elements) {
	for (size_t i = 1; i < n_elements; i++) {
		double value = elements[i];
		size_t j = i;
		while (j > 0 && elements[j - 1] > value) {
			elements[j] = elements[j - 1];
			j--;
		}
		elements[j] = value;
	}
}

static double median_of_three_doubles(double *elements, size_t n_elements) {
	size_t mid = n_elements / 2;
	size_t last = n_elements - 1;

	if (elements[mid] < elements[0])
		SWAP_DOUBLES(elements[mid], elements[0]);
	if (elements[last] < elements[0])
		SWAP_DOUBLES(elements[last], elements[0]);
	if (elements[last] < elements[mid])
		SWAP_DOUBLES(elements[last], elements[mid]);

	return elements[mid];
}

static void quicksort_doubles(double *elements, size_t n_elements) {
	while (n_elements > 24) {
		double pivot = median_of_three_doubles(elements, n_elements);
		size_t i = 0;
		size_t j = n_elements - 1;

		for (;;) {
			while (elements[i] < pivot)
				i++;
			while (elements[j] > pivot)
				j--;
			if (i >= j)
				break;

			SWAP_DOUBLES(elements[i], elements[j]);

			i++;
			j--;
		}

		size_t left_count = j + 1;
		if (left_count <= n_elements - left_count) {
			quicksort_doubles(elements, left_count);
			elements += left_count;
			n_elements -= left_count;
		} else {
			quicksort_doubles(elements + left_count, n_elements - left_count);
			n_elements = left_count;
		}
	}
	insertion_sort_doubles(elements, n_elements);
}

#define RADIX_SORT_CUTOFF 65536
#define RADIX_DIGITS 65536

typedef uint64_t __attribute__((may_alias)) sort_key;

static void radix_sort_doubles(double *elements, size_t n_elements,
		sort_key *scratch, size_t *digit_counts) {
	sort_key *keys = (sort_key *)elements;

	for (size_t i = 0; i < n_elements; i++)
		keys[i] ^= -(keys[i] >> 63) | 0x8000000000000000ULL;

	sort_key *from = keys;
	sort_key *to = scratch;
	for (int pass = 0; pass < 4; pass++) {
		int shift = pass * 16;

		memset(digit_counts, 0, RADIX_DIGITS * sizeof(size_t));
		for (size_t i = 0; i < n_elements; i++)
			digit_counts[(from[i] >> shift) & 0xFFFF]++;
		if (digit_counts[(from[0] >> shift) & 0xFFFF] == n_elements)
			continue;

		size_t running = 0;
		for (int digit = 0; digit < RADIX_DIGITS; digit++) {
			size_t digit_count = digit_counts[digit];
			digit_counts[digit] = running;
			running += digit_count;
		}

		for (size_t i = 0; i < n_elements; i++)
			to[digit_counts[(from[i] >> shift) & 0xFFFF]++] = from[i];

		sort_key *filled = to;
		to = from;
		from = filled;
	}
	if (from != keys)
		memcpy(keys, from, n_elements * sizeof(sort_key));

	for (size_t i = 0; i < n_elements; i++)
		keys[i] ^= ((keys[i] >> 63) - 1) | 0x8000000000000000ULL;
}

static void sort_doubles(double *elements, size_t n_elements) {
	size_t sortable = sort_partition_nans(elements, n_elements);

	if (sortable > RADIX_SORT_CUTOFF) {
		sort_key *scratch = malloc(sortable * sizeof(sort_key)
				+ RADIX_DIGITS * sizeof(size_t));
		if (scratch) {
			radix_sort_doubles(elements, sortable, scratch,
					(size_t *)(scratch + sortable));
			free(scratch);
			return;
		}
	}

	quicksort_doubles(elements, sortable);
}

int vector_sorted_copy(Interpreter *interp, Object *source) {
	int n_rows = source->matrix.rows;
	int n_columns = source->matrix.columns;
	if (n_rows != 1 && n_columns != 1) {
		fail(interp, "sort: expected a vector (nx1 or 1xn); got %dx%d",
				n_rows, n_columns);
		return -1;
	}

	int sorted_vector_handle = object_new_matrix(interp, n_rows, n_columns);
	if (interp->error_flag)
		return -1;
	
	Object *sorted_vector = OBJECT_AT(sorted_vector_handle);
	size_t n_elements = (size_t)n_rows * (size_t)n_columns;
	memcpy(sorted_vector->matrix.elements, source->matrix.elements, sizeof(double) * n_elements);
	sort_doubles(sorted_vector->matrix.elements, n_elements);

	return sorted_vector_handle;
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
		fail(interp, "diagonal-matrix: expected a float fill value; got %s", tag_name(VAL_TAG(diag_val)));
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
	POP_MATRIX(source, "diagonal");

	int diag_len = MIN(source->matrix.rows, source->matrix.columns);
	NEW_MATRIX(diag_handle, diagonal, 1, diag_len);

	for (int i = 0; i < diag_len; i++)
		diagonal->matrix.elements[i] = MAT(source, i, i);

	push(interp, make_matrix(diag_handle));

	DISPATCH(interp);
}

void p_reshape(DISPATCH_ARGS) {
	POP_INT(new_cols, "reshape", "column count");
	POP_INT(new_rows, "reshape", "row count");
	POP_MATRIX(source, "reshape");

	if (new_rows < 0 || new_cols < 0) {
		fail(interp, "reshape: dimensions must be non-negative; got %dx%d", new_rows, new_cols);
		return;
	}
	if (new_cols != 0 && new_rows > INT_MAX / new_cols) {
		fail(interp, "reshape: %dx%d too large (element count overflows)", new_rows, new_cols);
		return;
	}
	int total = source->matrix.rows * source->matrix.columns;
	if (new_rows * new_cols != total) {
		fail(interp, "reshape: cannot reshape %d elements (%dx%d) into %dx%d (%d)",
				total, source->matrix.rows, source->matrix.columns,
				new_rows, new_cols, new_rows * new_cols);
		return;
	}

	NEW_MATRIX(target_handle, target, new_rows, new_cols);
	memcpy(target->matrix.elements, source->matrix.elements,
			(size_t)total * sizeof(double));

	push(interp, make_matrix(target_handle));

	DISPATCH(interp);
}

void p_matrix(DISPATCH_ARGS) {
	if (interp->dsp < 2) {
		fail(interp, "matrix: stack too shallow (expected array and at least one dimension)");
		return;
	}
	Val top = interp->data_stack[interp->dsp - 1];
	Val below = interp->data_stack[interp->dsp - 2];
	if (VAL_TAG(top) != T_FLOAT) {
		fail(interp, "matrix: expected a float dimension on top; got %s", tag_name(VAL_TAG(top)));
		return;
	}

	int num_rows, num_cols;
	Val arr_val;
	if (VAL_TAG(below) == T_FLOAT) {
		if (interp->dsp < 3) {
			fail(interp, "matrix: stack too shallow (expected array below two dimensions)");
			return;
		}
		arr_val = interp->data_stack[interp->dsp - 3];
		if (VAL_TAG(arr_val) != T_ARRAY) {
			fail(interp, "matrix: expected an array; got %s", tag_name(VAL_TAG(arr_val)));
			return;
		}
		num_rows = (int)VAL_NUMBER(below);
		num_cols = (int)VAL_NUMBER(top);
		interp->dsp -= 3;
	} else if (VAL_TAG(below) == T_ARRAY) {
		num_rows = (int)VAL_NUMBER(top);
		Object *arr = OBJECT_AT(VAL_DATA(below));
		if (num_rows <= 0 || arr->len % num_rows != 0) {
			fail(interp, "matrix: %d elements does not divide evenly into %d rows", arr->len, num_rows);
			return;
		}
		num_cols = arr->len / num_rows;
		arr_val = below;
		interp->dsp -= 2;
	} else {
		fail(interp, "matrix: expected an array below the dimension(s); got %s", tag_name(VAL_TAG(below)));
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
		fail(interp, "matrix: array has %d elements but %dx%d needs %d",
				input_array->len, matrix->matrix.rows, matrix->matrix.columns, num_elements);
		return;
	}

	for (int i = 0; i < num_elements; i++) {
		if (VAL_TAG(input_array->items[i]) != T_FLOAT) {
			fail(interp, "matrix: element %d is %s, expected a float", i, tag_name(VAL_TAG(input_array->items[i])));
			return;
		}
		matrix->matrix.elements[i] = VAL_NUMBER(input_array->items[i]);
	}

	push(interp, make_matrix(matrix_handle));

	DISPATCH(interp);
}

void p_dim(DISPATCH_ARGS) {
	POP_MATRIX(m, "dim");
	push(interp, make_float(m->matrix.rows));
	push(interp, make_float(m->matrix.columns));

	DISPATCH(interp);
}

void p_transpose(DISPATCH_ARGS) {
	POP_MATRIX(source, "transpose");
	NEW_MATRIX(target_handle, target, source->matrix.columns, source->matrix.rows);
	for (int i = 0; i < source->matrix.rows; i++)
		for (int j = 0; j < source->matrix.columns; j++)
			MAT(target, j, i) = MAT(source, i, j);

	push(interp, make_matrix(target_handle));

	DISPATCH(interp);
}

void p_submatrix(DISPATCH_ARGS) {
	POP_INT(col_end, "submatrix", "col-end");
	POP_INT(col_start, "submatrix", "col-start");
	POP_INT(row_end, "submatrix", "row-end");
	POP_INT(row_start, "submatrix", "row-start");
	POP_MATRIX(source, "submatrix");

	if (row_start < 0 || row_end > source->matrix.rows || row_start > row_end
			|| col_start < 0 || col_end > source->matrix.columns || col_start > col_end) {
		fail(interp, "submatrix: [%d,%d)x[%d,%d) out of bounds for %dx%d",
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

	push(interp, make_matrix(slice_handle));

	DISPATCH(interp);
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
	POP(step_val);
	POP(end_val);
	POP(start_val);

	if (VAL_TAG(start_val) != T_FLOAT || VAL_TAG(end_val) != T_FLOAT || VAL_TAG(step_val) != T_FLOAT) {
		fail(interp, "matrix-range: expected three floats (start end step); got %s, %s, %s",
				tag_name(VAL_TAG(start_val)), tag_name(VAL_TAG(end_val)), tag_name(VAL_TAG(step_val)));
		return;
	}

	double start = VAL_NUMBER(start_val);
	double end = VAL_NUMBER(end_val);
	double step = VAL_NUMBER(step_val);

	if (step == 0.0) {
		fail(interp, "matrix-range: step cannot be zero");
		return;
	}

	if ((step > 0.0 && end < start) || (step < 0.0 && end > start)) {
		fail(interp, "matrix-range: step sign does not match start/end direction");
		return;
	}

	double raw_steps = (end - start) / step;
	if (raw_steps > (double)INT_MAX - 1.0) {
		fail(interp, "matrix-range: too many elements");
		return;
	}
	int n_steps = (int)raw_steps + 1;
	NEW_MATRIX(handle, matrix, 1, n_steps);
	double *elements = matrix->matrix.elements;

	for (int i = 0; i < n_steps; i++)
		elements[i] = start + i * step;

	push(interp, make_matrix(handle));

	DISPATCH(interp);
}

void p_select_rows(DISPATCH_ARGS) {
	PEEK_TYPE_AT(indices_val, 0, "select-rows", T_ARRAY);
	PEEK_TYPE_AT(matrix_val, 1, "select-rows", T_MATRIX);
	Object *indices = OBJECT_AT(VAL_DATA(indices_val));
	Object *source = OBJECT_AT(VAL_DATA(matrix_val));

	int columns = source->matrix.columns;
	int source_rows = source->matrix.rows;

	for (int i = 0; i < indices->len; i++) {
		if (VAL_TAG(indices->items[i]) != T_FLOAT) {
			fail(interp, "select-rows: index %d is %s, expected a float", i, tag_name(VAL_TAG(indices->items[i])));
			return;
		}
		int row = (int)VAL_NUMBER(indices->items[i]);
		if (row < 0 || row >= source_rows) {
			fail(interp, "select-rows: row %d out of bounds (%d rows)", row, source_rows);
			return;
		}
	}

	NEW_MATRIX(selected_handle, selected, indices->len, columns);

	for (int i = 0; i < indices->len; i++) {
		int row = (int)VAL_NUMBER(indices->items[i]);
		memcpy(&MAT(selected, i, 0), &MAT(source, row, 0), sizeof(double) * (size_t)columns);
	}

	interp->data_stack[interp->dsp - 2] = make_matrix(selected_handle);
	interp->dsp--;

	DISPATCH(interp);
}

void p_augment(DISPATCH_ARGS) {
	PEEK_TYPE_AT(b_val, 0, "augment", T_MATRIX);
	PEEK_TYPE_AT(a_val, 1, "augment", T_MATRIX);
	Object *a = OBJECT_AT(VAL_DATA(a_val));
	Object *b = OBJECT_AT(VAL_DATA(b_val));

	if (a->matrix.rows != b->matrix.rows) {
		fail(interp, "augment: row counts differ (%d vs %d)", a->matrix.rows, b->matrix.rows);
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

	interp->data_stack[interp->dsp - 2] = make_matrix(augmented_handle);
	interp->dsp--;

	DISPATCH(interp);
}

void p_vstack(DISPATCH_ARGS) {
	PEEK_TYPE_AT(b_val, 0, "vstack", T_MATRIX);
	PEEK_TYPE_AT(a_val, 1, "vstack", T_MATRIX);
	Object *a = OBJECT_AT(VAL_DATA(a_val));
	Object *b = OBJECT_AT(VAL_DATA(b_val));

	if (a->matrix.columns != b->matrix.columns) {
		fail(interp, "vstack: column counts differ (%d vs %d)", a->matrix.columns, b->matrix.columns);
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

	interp->data_stack[interp->dsp - 2] = make_matrix(stacked_handle);
	interp->dsp--;

	DISPATCH(interp);
}

void p_variance(DISPATCH_ARGS) {
	POP_MATRIX(source, "var");
	size_t n = (size_t)(source->matrix.rows * source->matrix.columns);
	if (n < 2) {
		fail(interp, "var: needs at least 2 elements; got %zu", n);
		return;
	}

	push(interp, make_float(matrix_variance_overall(source)));

	DISPATCH(interp);
}

void p_quantile(DISPATCH_ARGS) {
	POP_FLOAT(probability, "quantile", "probability");
	if (probability < 0.0 || probability > 1.0) {
		fail(interp, "quantile: probability must be in [0,1]; got %g", probability);
		return;
	}

	POP_MATRIX(source, "quantile");
	size_t n = (size_t)(source->matrix.rows * source->matrix.columns);
	if (n == 0) {
		fail(interp, "quantile: empty matrix");
		return;
	}

	double *sorted = malloc(n * sizeof(double));
	if (!sorted) {
		fail(interp, "quantile: out of memory");
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
	push(interp, make_float(value));

	DISPATCH(interp);
}
