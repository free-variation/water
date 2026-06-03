#include "logicforth.h"

double scalar_add(double a, double b) { return a + b; }

double scalar_subtract(double a, double b) { return a - b; }

double scalar_multiply(double a, double b) { return a * b; }

double scalar_divide(double a, double b) { return a / b; }

int matrix_scalar_op(Interpreter *interp, Val left_val, Val right_val, scalar_operator op) {
	Object *left = interp->objects[VAL_DATA(left_val)];
	Object *right = interp->objects[VAL_DATA(right_val)];

	if (left->matrix.rows != right->matrix.rows || left->matrix.columns != right->matrix.columns) {
		fail(interp, "element-wise op: matrix shapes differ (%dx%d vs %dx%d)",
				left->matrix.rows, left->matrix.columns,
				right->matrix.rows, right->matrix.columns);
		return -1;
	}

	int rows = left->matrix.rows;
	int columns = right->matrix.columns;
	int target_handle = object_new_matrix(interp, rows, columns);
	if (interp->error_flag) return -1;

	Object *target = interp->objects[target_handle];
	for (int i = 0; i < rows * columns; i++) {
		target->matrix.elements[i] = op(left->matrix.elements[i], right->matrix.elements[i]);
	}

	return target_handle;
}

void p_at_i(Interpreter *interp) {

	POP_INT(index, "@i", "index");

	POP(source_val);
	if (VAL_TAG(source_val) == T_ARRAY) {
		Object *array = interp->objects[VAL_DATA(source_val)];
		if (index < 0 || index >= array->len) {
			fail(interp, "@i: array index %d out of bounds (length %d)", index, array->len);
			return;
		}

		push(interp, array->items[index]);
	} else if (VAL_TAG(source_val) == T_MATRIX) {
		Object *source = interp->objects[VAL_DATA(source_val)];
		if (index < 0 || index >= source->matrix.rows) {
			fail(interp, "@i: row index %d out of bounds (%d rows)", index, source->matrix.rows);
			return;
		}

		int num_columns = source->matrix.columns;
		NEW_MATRIX(row_handle, row, 1, num_columns);
		for (int j = 0; j < num_columns; j++)
			MAT(row, 0, j) = MAT(source, index, j);

		push(interp, make_matrix(row_handle));
	} else {
		fail(interp, "@i: expected an array or matrix; got %s", tag_name(VAL_TAG(source_val)));
	}
	DISPATCH(interp);
}

void p_store_i(Interpreter *interp) {

	PEEK_TYPE_AT(array_val, 2, "!i", T_ARRAY);
	PEEK_AT(index_val, 1, "!i");
	if (VAL_TAG(index_val) != T_FLOAT) {
		fail(interp, "!i: expected a float index; got %s", tag_name(VAL_TAG(index_val)));
		return;
	}
	int index = (int)VAL_NUMBER(index_val);
	PEEK_AT(value, 0, "!i");

	Object *array = interp->objects[VAL_DATA(array_val)];
	if (index < 0 || index >= array->len) {
		fail(interp, "!i: array index %d out of bounds (length %d)", index, array->len);
		return;
	}
	array->items[index] = value;
	interp->dsp -= 2;
	DISPATCH(interp);
}

void p_at_j(Interpreter *interp) {

	POP_INT(index, "@j", "index");
	POP_MATRIX(source, "@j");

	if (index < 0 || index >= source->matrix.columns) {
		fail(interp, "@j: column index %d out of bounds (%d columns)", index, source->matrix.columns);
		return;
	}

	int num_rows = source->matrix.rows;
	NEW_MATRIX(col_handle, col, num_rows, 1);
	for (int i = 0; i < num_rows; i++)
		MAT(col, i, 0) = MAT(source, i, index);

	push(interp, make_matrix(col_handle));
	DISPATCH(interp);
}

void p_at_ij(Interpreter *interp) {

	POP_INT(j, "@i,j", "column index");
	POP_INT(i, "@i,j", "row index");
	POP_MATRIX(source, "@i,j");

	if (i < 0 || i >= source->matrix.rows) {
		fail(interp, "@i,j: row index %d out of bounds (%d rows)", i, source->matrix.rows);
		return;
	}
	if (j < 0 || j >= source->matrix.columns) {
		fail(interp, "@i,j: column index %d out of bounds (%d columns)", j, source->matrix.columns);
		return;
	}

	push(interp, make_float(MAT(source, i, j)));
	DISPATCH(interp);
}

int dgemm_kernel(Interpreter *interp, int transpose_a, int transpose_b,
		double alpha,
		int a_handle, int b_handle,
		double beta, int c_handle) {
	int i, j, k, m, n, p;

	Object *A = interp->objects[a_handle];
	Object *B = interp->objects[b_handle];
	Object *C = interp->objects[c_handle];

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
	Object *matmult = interp->objects[matmult_handle];

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
	} else {
		for (i = 0; i < m; i++) {
			for (j = 0; j < n; j++) {
				double sum = 0.0;
				for (p = 0; p < k; p++) {
					double a_val = transpose_a ? MAT(A, p, i) : MAT(A, i, p);
					double b_val = transpose_b ? MAT(B, j, p) : MAT(B, p, j);
					sum += a_val * b_val;
				}
				MAT(matmult, i, j) = alpha * sum + beta * MAT(C, i, j);
			}
		}
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

void p_dgemm_nn(Interpreter *interp) {
	p_dgemm_helper(interp, 0, 0);
	DISPATCH(interp);
}

void p_dgemm_tn(Interpreter *interp) {
	p_dgemm_helper(interp, 1, 0);
	DISPATCH(interp);
}

void p_dgemm_nt(Interpreter *interp) {
	p_dgemm_helper(interp, 0, 1);
	DISPATCH(interp);
}

void p_dgemm_tt(Interpreter *interp) {
	p_dgemm_helper(interp, 1, 1);
	DISPATCH(interp);
}

double reduce_add(double accumulator, double element) { return accumulator + element; }

double reduce_max(double accumulator, double element) { return accumulator > element ? accumulator : element; }

double reduce_min(double accumulator, double element) { return accumulator < element ? accumulator : element; }

double matrix_reduce_overall(Object *source, reducer fn, double identity) {
	int num_elements = source->matrix.rows * source->matrix.columns;
	double accumulator = identity;
	for (int i = 0; i < num_elements; i++)
		accumulator = fn(accumulator, source->matrix.elements[i]);
	return accumulator;
}

int matrix_reduce_rows(Interpreter *interp, Object *source, reducer fn, double identity) {
	int rows = source->matrix.rows;
	int cols = source->matrix.columns;
	int target_handle = object_new_matrix(interp, rows, 1);
	if (interp->error_flag) return -1;
	Object *target = interp->objects[target_handle];
	for (int i = 0; i < rows; i++) {
		double accumulator = identity;
		for (int j = 0; j < cols; j++)
			accumulator = fn(accumulator, MAT(source, i, j));
		MAT(target, i, 0) = accumulator;
	}
	return target_handle;
}

int matrix_reduce_columns(Interpreter *interp, Object *source, reducer fn, double identity) {
	int rows = source->matrix.rows;
	int cols = source->matrix.columns;
	int target_handle = object_new_matrix(interp, 1, cols);
	if (interp->error_flag) return -1;
	Object *target = interp->objects[target_handle];
	for (int j = 0; j < cols; j++) MAT(target, 0, j) = identity;
	for (int i = 0; i < rows; i++)
		for (int j = 0; j < cols; j++)
			MAT(target, 0, j) = fn(MAT(target, 0, j), MAT(source, i, j));
	return target_handle;
}

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

void p_0_matrix(Interpreter *interp) {
	int matrix_handle = create_matrix(interp);
	if (interp->error_flag) return;
	push(interp, make_matrix(matrix_handle));
	DISPATCH(interp);
}

void p_diagonal_matrix(Interpreter *interp) {

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

	Object *diag_matrix = interp->objects[diag_matrix_handle];
	double diag_element = VAL_NUMBER(diag_val);
	for (int i = 0; i < diag_matrix->matrix.rows; i++) {
		MAT(diag_matrix, i, i) = diag_element;
	}

	push(interp, make_matrix(diag_matrix_handle));
	DISPATCH(interp);
}

void p_diagonal(Interpreter *interp) {

	POP_MATRIX(source, "diagonal");

	int diag_len = MIN(source->matrix.rows, source->matrix.columns);
	NEW_MATRIX(diag_handle, diagonal, 1, diag_len);

	for (int i = 0; i < diag_len; i++)
		diagonal->matrix.elements[i] = MAT(source, i, i);

	push(interp, make_matrix(diag_handle));
	DISPATCH(interp);
}

void p_reshape(Interpreter *interp) {

	POP_INT(new_cols, "reshape", "column count");
	POP_INT(new_rows, "reshape", "row count");
	POP_MATRIX(source, "reshape");

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

void p_matrix(Interpreter *interp) {

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
		Object *arr = interp->objects[VAL_DATA(below)];
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
	Object *matrix = interp->objects[matrix_handle];
	Object *input_array = interp->objects[VAL_DATA(array_val)];
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

void p_dim(Interpreter *interp) {

	POP_MATRIX(m, "dim");
	push(interp, make_float(m->matrix.rows));
	push(interp, make_float(m->matrix.columns));
	DISPATCH(interp);
}

void p_transpose(Interpreter *interp) {

	POP_MATRIX(source, "transpose");
	NEW_MATRIX(target_handle, target, source->matrix.columns, source->matrix.rows);
	for (int i = 0; i < source->matrix.rows; i++)
		for (int j = 0; j < source->matrix.columns; j++)
			MAT(target, j, i) = MAT(source, i, j);

	push(interp, make_matrix(target_handle));
	DISPATCH(interp);
}


#define REDUCE_OVERALL(primitive_name, word_name, fn, identity) \
	void primitive_name(Interpreter *interp) { \
		POP_MATRIX(source, word_name); \
		push(interp, make_float(matrix_reduce_overall(source, fn, identity))); \
	}

#define REDUCE_ROWS(primitive_name, word_name, fn, identity) \
	void primitive_name(Interpreter *interp) { \
		POP_MATRIX(source, word_name); \
		int target_handle = matrix_reduce_rows(interp, source, fn, identity); \
		if (!interp->error_flag) push(interp, make_matrix(target_handle)); \
	}

#define REDUCE_COLUMNS(primitive_name, word_name, fn, identity) \
	void primitive_name(Interpreter *interp) { \
		POP_MATRIX(source, word_name); \
		int target_handle = matrix_reduce_columns(interp, source, fn, identity); \
		if (!interp->error_flag) push(interp, make_matrix(target_handle)); \
	}

REDUCE_OVERALL(p_sum, "sum", reduce_add, 0.0)
REDUCE_OVERALL(p_max, "max", reduce_max, -INFINITY)
REDUCE_OVERALL(p_min, "min", reduce_min, INFINITY)
REDUCE_ROWS(p_row_sums, "row-sums", reduce_add, 0.0)
REDUCE_ROWS(p_row_maxes, "row-maxes", reduce_max, -INFINITY)
REDUCE_ROWS(p_row_mins, "row-mins", reduce_min, INFINITY)
REDUCE_COLUMNS(p_column_sums, "column-sums", reduce_add, 0.0)
REDUCE_COLUMNS(p_column_maxes, "column-maxes", reduce_max, -INFINITY)
REDUCE_COLUMNS(p_column_mins, "column-mins", reduce_min, INFINITY)
