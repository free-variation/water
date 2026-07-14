#include "water.h"

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
