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

typedef struct {
	double x;
	double y;
} KendallPoint;

static int kendall_point_cmp(const void *left, const void *right) {
	const KendallPoint *a = left;
	const KendallPoint *b = right;

	if (a->x < b->x) return -1;
	if (a->x > b->x) return 1;
	if (a->y < b->y) return -1;
	if (a->y > b->y) return 1;

	return 0;
}

#define KENDALL_X_KEY(element) (*(sort_key *)&(element).x)
#define KENDALL_Y_KEY(element) (*(sort_key *)&(element).y)

RADIX_SORT(kendall_by_y, KendallPoint, KENDALL_Y_KEY)
RADIX_SORT(kendall_by_x, KendallPoint, KENDALL_X_KEY)

static void kendall_points_sort(KendallPoint *points, int n_points) {
	if (n_points > RADIX_SORT_CUTOFF) {
		KendallPoint *pass_elements = malloc((size_t)n_points * sizeof(KendallPoint) + RADIX_DIGITS * sizeof(size_t));
		if (pass_elements) {
			size_t *digit_counts = (size_t *)(pass_elements + n_points);
			radix_sort_kendall_by_y(points, (size_t)n_points, pass_elements, digit_counts);
			radix_sort_kendall_by_x(points, (size_t)n_points, pass_elements, digit_counts);
			free(pass_elements);
			return;
		}
	}

	qsort(points, (size_t)n_points, sizeof(KendallPoint), kendall_point_cmp);
}

static void kendall_tie_pairs(const KendallPoint *points, int n_points, int64_t *x_tie_pairs, int64_t *joint_tie_pairs) {
	int64_t x_pairs = 0;
	int64_t joint_pairs = 0;
	int x_run_start = 0;
	int joint_run_start = 0;

	for (int i = 1; i <= n_points; i++) {
		if (i == n_points || points[i].x != points[x_run_start].x) {
			int64_t run_length = i - x_run_start;
			x_pairs += run_length * (run_length - 1) / 2;
			x_run_start = i;
		}
		if (i == n_points || points[i].x != points[joint_run_start].x || points[i].y != points[joint_run_start].y) {
			int64_t run_length = i - joint_run_start;
			joint_pairs += run_length * (run_length - 1) / 2;
			joint_run_start = i;
		}
	}

	*x_tie_pairs = x_pairs;
	*joint_tie_pairs = joint_pairs;
}

static int64_t sorted_run_tie_pairs(const double *values, int n_values) {
	int64_t n_tied_pairs = 0;
	int run_start = 0;

	for (int i = 1; i <= n_values; i++) {
		if (i == n_values || values[i] != values[run_start]) {
			int64_t run_length = i - run_start;
			n_tied_pairs += run_length * (run_length - 1) / 2;
			run_start = i;
		}
	}

	return n_tied_pairs;
}

static int64_t merge_exchange_count(double *values, double *merged_values, int n_values) {
	int64_t n_exchanges = 0;
	double *from = values;
	double *to = merged_values;

	for (int width = 1; width < n_values; width *= 2) {
		for (int low = 0; low < n_values; low += 2 * width) {
			int middle = low + width;
			int high = low + 2 * width;
			if (middle > n_values) middle = n_values;
			if (high > n_values) high = n_values;

			int i = low;
			int j = middle;
			int k = low;
			while (i < middle && j < high) {
				if (from[j] < from[i]) {
					n_exchanges += middle - i;
					to[k++] = from[j++];
				} else {
					to[k++] = from[i++];
				}
			}
			while (i < middle) to[k++] = from[i++];
			while (j < high) to[k++] = from[j++];
		}

		double *merged = to;
		to = from;
		from = merged;
	}

	if (from != values)
		memcpy(values, from, (size_t)n_values * sizeof(double));

	return n_exchanges;
}

void p_correlation_kendall(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val ys_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(ys_val, T_MATRIX, "correlation-kendall", "a matrix");
	Val xs_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(xs_val, T_MATRIX, "correlation-kendall", "a matrix");

	Object *xs = OBJECT_AT(VAL_DATA(xs_val));
	Object *ys = OBJECT_AT(VAL_DATA(ys_val));
	int n_x_elements = xs->matrix.rows * xs->matrix.columns;
	int n_y_elements = ys->matrix.rows * ys->matrix.columns;
	
	if (n_x_elements != n_y_elements) {
		fail(interp, "expected equal-length vectors; got %d and %d elements", n_x_elements, n_y_elements);
		return;
	}
	if (n_x_elements < 2) {
		fail(interp, "needs at least 2 elements; got %d", n_x_elements);
		return;
	}

	int n_points = n_x_elements;
	KendallPoint *points = malloc((size_t)n_points * sizeof(KendallPoint));
	if (!points) {
		fail(interp, "out of memory");
		return;
	}
	double *ys_in_x_order = malloc((size_t)n_points * 2 * sizeof(double));
	if (!ys_in_x_order) {
		free(points);
		fail(interp, "out of memory");
		return;
	}
	double *merged_values = ys_in_x_order + n_points;

	const double *x_elements = xs->matrix.elements;
	const double *y_elements = ys->matrix.elements;
	for (int i = 0; i < n_points; i++) {
		points[i].x = x_elements[i];
		points[i].y = y_elements[i];
	}
	kendall_points_sort(points, n_points);

	int64_t x_tie_pairs;
	int64_t joint_tie_pairs;
	
	kendall_tie_pairs(points, n_points, &x_tie_pairs, &joint_tie_pairs);
	for (int i = 0; i < n_points; i++)
		ys_in_x_order[i] = points[i].y;
	
	int64_t n_exchanges = merge_exchange_count(ys_in_x_order, merged_values, n_points);
	int64_t y_tie_pairs = sorted_run_tie_pairs(ys_in_x_order, n_points);
	int64_t n_pairs = (int64_t)n_points * (n_points - 1) / 2;

	double concordant_minus_discordant = (double)(n_pairs - x_tie_pairs - y_tie_pairs + joint_tie_pairs - 2 * n_exchanges);
	double tau = concordant_minus_discordant / sqrt((double)(n_pairs - x_tie_pairs) * (double)(n_pairs - y_tie_pairs));

	free(points);
	free(ys_in_x_order);
	chain_sp[-2] = make_float(tau);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

