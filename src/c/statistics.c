#include "water.h"

typedef struct {
	double value;
	double response;
} CARTFeatureEntry;

typedef struct {
	CARTFeatureEntry *entries;
	int *rows;
} CARTSortedColumn;

typedef enum { FEATURE_NUMERIC, FEATURE_CATEGORICAL } CARTFeatureKind;

typedef struct {
	CARTFeatureKind kind;
	CARTSortedColumn numeric;
	int *levels;
	int n_levels;
	Val *level_values;
} CARTFeature;

typedef struct {
	CARTFeature *features;
	int n_features;
	double *response;
	cell *feature_names;
} CARTSample;

typedef struct {
	int feature;
	double threshold;
	int category_offset;
	int left_child;
	int right_child;
	double prediction;
	int n_rows;
	int row_start;
	int missing_left;
	int split_missing_count;
} CARTNode;

typedef struct {
	CARTNode *nodes;
	int n_nodes;
	int n_allocated;
	char *category_flags;
	int n_category_flags;
	int category_flags_cap;
} CART;

typedef struct {
	int *row_index;
	char *branch;
	CARTFeatureEntry *staged_entries;
	int *staged_rows;
} CARTPartition;

double matrix_variance_overall(Object *source, size_t *n_nonmissing_out) {
	size_t n = (size_t)(source->matrix.rows * source->matrix.columns);
	const double * restrict elements = source->matrix.elements;
	double sum = 0.0;
	double sum_of_squares = 0.0;
	size_t n_nonmissing = 0;

	for (size_t i = 0; i < n; i++) {
		double value = elements[i];
		if (value != value)
			continue;
		n_nonmissing++;
		sum += value;
		sum_of_squares += value * value;
	}

	*n_nonmissing_out = n_nonmissing;
	if (n_nonmissing < 2)
		return NAN;
	return (sum_of_squares - sum * sum/(double)n_nonmissing) / (double)(n_nonmissing - 1);
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

static int finite_sorted_copy(Interpreter *interp, const Object *sample, double **elements_out) {
	int n_cells = sample->matrix.rows * sample->matrix.columns;

	double *elements;
	MALLOC_OR_FAIL_RETURNING(interp, elements, sizeof(double) * (size_t)MAX(n_cells, 1), -1);

	int n_finite = 0;
	for (int i = 0; i < n_cells; i++) {
		double value = sample->matrix.elements[i];
		if (value != value)
			continue;
		elements[n_finite++] = value;
	}
	sort_doubles(elements, (size_t)n_finite);

	*elements_out = elements;
	return n_finite;
}

void p_ks_distance(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	int right_unit;
	Val right_val = quantity_unwrap(chain_sp[-1], &right_unit);
	REQUIRE_CHAIN_TAG(right_val, T_MATRIX, "ks-distance", "a matrix");
	int left_unit;
	Val left_val = quantity_unwrap(chain_sp[-2], &left_unit);
	REQUIRE_CHAIN_TAG(left_val, T_MATRIX, "ks-distance", "a matrix");
	(void)right_unit;
	(void)left_unit;

	Object *left = OBJECT_AT(VAL_DATA(left_val));
	Object *right = OBJECT_AT(VAL_DATA(right_val));

	double *left_elements;
	int n_left = finite_sorted_copy(interp, left, &left_elements);
	if (n_left < 0)
		return;

	double *right_elements;
	int n_right = finite_sorted_copy(interp, right, &right_elements);
	if (n_right < 0) {
		free(left_elements);
		return;
	}

	if (n_left == 0 || n_right == 0) {
		free(left_elements);
		free(right_elements);
		fail(interp, "a sample has no finite values");
		return;
	}

	int i = 0;
	int j = 0;
	double distance = 0.0;
	while (i < n_left && j < n_right) {
		double pooled = MIN(left_elements[i], right_elements[j]);
		while (i < n_left && left_elements[i] == pooled)
			i++;
		while (j < n_right && right_elements[j] == pooled)
			j++;

		double gap = fabs((double)i / n_left - (double)j / n_right);
		if (gap > distance)
			distance = gap;
	}

	free(left_elements);
	free(right_elements);
	chain_sp[-2] = make_float(distance);

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
	int ys_unit;
	Val ys_val = quantity_unwrap(chain_sp[-1], &ys_unit);
	REQUIRE_CHAIN_TAG(ys_val, T_MATRIX, "correlation-kendall", "a matrix");
	int xs_unit;
	Val xs_val = quantity_unwrap(chain_sp[-2], &xs_unit);
	REQUIRE_CHAIN_TAG(xs_val, T_MATRIX, "correlation-kendall", "a matrix");
	(void)ys_unit;
	(void)xs_unit;

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
	KendallPoint *points;
	MALLOC_OR_FAIL(interp, points, (size_t)n_points * sizeof(KendallPoint));
	double *ys_in_x_order;
	MALLOC_OR_FAIL_CLEANUP(interp, ys_in_x_order, (size_t)n_points * 2 * sizeof(double), free(points));
	double *merged_values = ys_in_x_order + n_points;

	const double *x_elements = xs->matrix.elements;
	const double *y_elements = ys->matrix.elements;
	n_points = 0;
	for (int i = 0; i < n_x_elements; i++) {
		double x = x_elements[i];
		double y = y_elements[i];
		if (x != x || y != y)
			continue;
		points[n_points].x = x;
		points[n_points].y = y;
		n_points++;
	}
	if (n_points < 2) {
		free(points);
		free(ys_in_x_order);
		fail(interp, "needs at least 2 complete pairs; got %d", n_points);
		return;
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

typedef struct {
	int feature;
	double threshold;
	int n_left;
	double left_sum;
	int missing_count;
	double missing_sum;
	int missing_left;
	double score;
} CARTSplit;

static CARTSplit scan_feature(const CARTFeatureEntry *entries, int node_start, int node_end,
		double node_sum, int min_samples) {
	int finite_end = node_end;
	double missing_sum = 0.0;
	while (finite_end > node_start && entries[finite_end - 1].value != entries[finite_end - 1].value)
		missing_sum += entries[--finite_end].response;
	int missing_count = node_end - finite_end;
	int n_finite = finite_end - node_start;
	double finite_sum = node_sum - missing_sum;

	CARTSplit best = { .feature = -1, .threshold = 0.0, .n_left = 0, .left_sum = 0.0,
			.missing_count = missing_count, .missing_sum = missing_sum, .missing_left = 1,
			.score = -INFINITY };
	double left_sum = 0.0;

	for (int i = node_start; i + 1 < finite_end; i++) {
		left_sum += entries[i].response;
		int n_left = i - node_start + 1;

		if (entries[i].value == entries[i + 1].value)
			continue;

		int n_right = n_finite - n_left;
		double right_sum = finite_sum - left_sum;
		double threshold = (entries[i].value + entries[i + 1].value) / 2.0;

		if (n_left + missing_count >= min_samples && n_right >= min_samples) {
			double left_total = left_sum + missing_sum;
			double score = left_total * left_total / (n_left + missing_count)
					+ right_sum * right_sum / n_right;
			if (score > best.score) {
				best.score = score;
				best.threshold = threshold;
				best.n_left = n_left;
				best.left_sum = left_sum;
				best.missing_left = 1;
			}
		}

		if (n_left >= min_samples && n_right + missing_count >= min_samples) {
			double right_total = right_sum + missing_sum;
			double score = left_sum * left_sum / n_left
					+ right_total * right_total / (n_right + missing_count);
			if (score > best.score) {
				best.score = score;
				best.threshold = threshold;
				best.n_left = n_left;
				best.left_sum = left_sum;
				best.missing_left = 0;
			}
		}
	}

	return best;
}

typedef struct {
	double *level_sum;
	int *level_count;
	int *level_order;
	char *goes_left;
} CARTCategoryStats;

static int level_mean_cmp(void *stats_thunk, const void *left, const void *right) {
	const CARTCategoryStats *stats = stats_thunk;
	int left_level = *(const int *)left;
	int right_level = *(const int *)right;

	double left_key = stats->level_sum[left_level] * stats->level_count[right_level];
	double right_key = stats->level_sum[right_level] * stats->level_count[left_level];

	return (left_key > right_key) - (left_key < right_key);
}

static int order_categories(const int *levels, int n_levels,
		const int *row_index, int node_start, int node_end,
		const double *response, CARTCategoryStats *category_stats,
		double *node_sum_out) {
	double *level_sum = category_stats->level_sum;
	int *level_count = category_stats->level_count;
	int *level_order = category_stats->level_order;

	for (int level = 0; level < n_levels; level++) {
		level_sum[level] = 0.0;
		level_count[level] = 0;
	}

	double node_sum = 0.0;

	for (int i = node_start; i < node_end; i++) {
		int row = row_index[i];
		int level = levels[row];

		level_sum[level] += response[row];
		level_count[level]++;

		node_sum += response[row];
	}

	int n_present = 0;
	for (int level = 0; level < n_levels; level++)
		if (level_count[level] > 0)
			level_order[n_present++] = level;

	platform_qsort_r(level_order, (size_t)n_present, sizeof(int), category_stats, level_mean_cmp);

	*node_sum_out = node_sum;
	return n_present;
}

static CARTSplit scan_categorical(const int *levels, int n_levels,
		const int *row_index, int node_start, int node_end,
		const double *response, int min_samples,
		CARTCategoryStats *category_stats) {
	double node_sum;
	int n_present = order_categories(levels, n_levels, row_index, node_start, node_end,
			response, category_stats, &node_sum);

	int n_rows = node_end - node_start;
	double *level_sum = category_stats->level_sum;
	int *level_count = category_stats->level_count;
	int *level_order = category_stats->level_order;

	CARTSplit best = { .feature = -1, .threshold = 0.0, .n_left = 0, .left_sum = 0.0, .score = -INFINITY };
	double left_sum = 0.0;
	int left_count = 0;

	for (int j = 0; j + 1 < n_present; j++) {
		int level = level_order[j];
		left_sum += level_sum[level];
		left_count += level_count[level];

		int right_count = n_rows - left_count;
		if (left_count < min_samples || right_count < min_samples)
			continue;

		double right_sum = node_sum - left_sum;
		double score = left_sum * left_sum / left_count
				+ right_sum * right_sum / right_count;

		if (score > best.score) {
			best.score = score;
			best.n_left = left_count;
			best.left_sum = left_sum;
		}
	}

	return best;
}

static CARTSplit choose_split(const CARTSample *sample, const CARTPartition *partition,
		CARTCategoryStats *category_stats, int node_start, int node_end,
		double node_sum, int min_samples) {
	int n_rows = node_end - node_start;

	CARTSplit best = { .feature = -1, .threshold = 0.0, .n_left = 0, .left_sum = 0.0,
			.score = node_sum * node_sum / n_rows };

	for (int f = 0; f < sample->n_features; f++) {
		const CARTFeature *feature = &sample->features[f];

		CARTSplit candidate = feature->kind == FEATURE_NUMERIC
			? scan_feature(feature->numeric.entries, node_start, node_end, node_sum, min_samples)
			: scan_categorical(feature->levels, feature->n_levels, partition->row_index,
					node_start, node_end, sample->response, min_samples, category_stats);

		if (candidate.score > best.score) {
			best = candidate;
			best.feature = f;
		}
	}

	return best;
}

enum { BRANCH_LEFT = 0, BRANCH_RIGHT = 1 };

static void mark_branches(const int *rows, int node_start, int node_end,
		int n_left, int missing_count, int missing_left, char *branch) {
	int finite_end = node_end - missing_count;
	int right_start = node_start + n_left;
	for (int i = node_start; i < right_start; i++)
		branch[rows[i]] = BRANCH_LEFT;
	for (int i = right_start; i < finite_end; i++)
		branch[rows[i]] = BRANCH_RIGHT;
	char missing_branch = missing_left ? BRANCH_LEFT : BRANCH_RIGHT;
	for (int i = finite_end; i < node_end; i++)
		branch[rows[i]] = missing_branch;
}

static void mark_branches_categorical(const int *levels, int n_levels,
		const int *row_index, int node_start, int node_end,
		const double *response, int n_left,
		CARTCategoryStats *category_stats, char *branch) {
	double node_sum;
	int n_present = order_categories(levels, n_levels, row_index, node_start, node_end,
			response, category_stats, &node_sum);

	char *goes_left = category_stats->goes_left;
	for (int level = 0; level < n_levels; level++)
		goes_left[level] = 0;

	int left_count = 0;
	for (int j = 0; j < n_present && left_count < n_left; j++) {
		int level = category_stats->level_order[j];
		goes_left[level] = 1;
		left_count += category_stats->level_count[level];
	}

	for (int i = node_start; i < node_end; i++) {
		int row = row_index[i];
		branch[row] = goes_left[levels[row]] ? BRANCH_LEFT : BRANCH_RIGHT;
	}
}

static void partition_column(CARTSortedColumn *column, int node_start, int node_end, int n_left,
		const char *branch, CARTFeatureEntry *staged_entries, int *staged_rows) {
	int right_start = node_start + n_left;
	int left_index = node_start;
	int right_index = right_start;

	for (int i = node_start; i < node_end; i++) {
		int row = column->rows[i];
		if (branch[row] == BRANCH_LEFT) {
			staged_entries[left_index] = column->entries[i];
			staged_rows[left_index] = row;
			left_index++;
		} else {
			staged_entries[right_index] = column->entries[i];
			staged_rows[right_index] = row;
			right_index++;
		}
	}

	memcpy(&column->entries[node_start], &staged_entries[node_start], sizeof(CARTFeatureEntry) * (size_t)(node_end - node_start));
	memcpy(&column->rows[node_start], &staged_rows[node_start], sizeof(int) * (size_t)(node_end - node_start));
}

static void partition_row_index(int *row_index, int node_start, int node_end, const char *branch) {
	int left_index = node_start;
	int right_index = node_end - 1;

	while (left_index <= right_index) {
		while (left_index <= right_index && branch[row_index[left_index]] == BRANCH_LEFT)
			left_index++;
		while (left_index <= right_index && branch[row_index[right_index]] == BRANCH_RIGHT)
			right_index--;

		if (left_index < right_index) {
			int swap_row = row_index[left_index];
			row_index[left_index] = row_index[right_index];
			row_index[right_index] = swap_row;
			left_index++;
			right_index--;
		}
	}
}

static void partition_node(CARTSample *sample, CARTPartition *partition,
		CARTCategoryStats *category_stats, const CARTSplit *split,
		int node_start, int node_end) {
	const CARTFeature *feature = &sample->features[split->feature];

	if (feature->kind == FEATURE_NUMERIC)
		mark_branches(feature->numeric.rows, node_start, node_end, split->n_left,
				split->missing_count, split->missing_left, partition->branch);
	else
		mark_branches_categorical(feature->levels, feature->n_levels,
				partition->row_index, node_start, node_end, sample->response,
				split->n_left, category_stats, partition->branch);

	for (int f = 0; f < sample->n_features; f++) {
		if (sample->features[f].kind != FEATURE_NUMERIC || f == split->feature)
			continue;
		partition_column(&sample->features[f].numeric, node_start, node_end,
				split->n_left, partition->branch, partition->staged_entries, partition->staged_rows);
	}

	partition_row_index(partition->row_index, node_start, node_end, partition->branch);
}

static int append_node(CART *tree) {
	GROW_IF_FULL_SYS(tree->n_nodes, tree->n_allocated, tree->nodes);
	int node_index = tree->n_nodes++;
	return node_index;
}

static void record_categorical_split(CART *tree, int node_index,
		const CARTCategoryStats *category_stats, int n_levels) {
	int offset = tree->n_category_flags;

	while (tree->n_category_flags + n_levels > tree->category_flags_cap) {
		tree->category_flags_cap = tree->category_flags_cap ? tree->category_flags_cap * 2 : 8;
		tree->category_flags = realloc(tree->category_flags, (size_t)tree->category_flags_cap);
	}

	const char *goes_left = category_stats->goes_left;
	for (int level = 0; level < n_levels; level++)
		tree->category_flags[offset + level] = goes_left[level];

	tree->n_category_flags += n_levels;
	tree->nodes[node_index].category_offset = offset;
}

static int append_leaf(CART *tree, int node_start, int n_rows, double node_sum) {
	int node_index = append_node(tree);
	CARTNode *node = &tree->nodes[node_index];
	node->feature = -1;
	node->threshold = 0.0;
	node->category_offset = -1;
	node->left_child = -1;
	node->right_child = -1;
	node->prediction = node_sum / n_rows;
	node->n_rows = n_rows;
	node->row_start = node_start;
	node->missing_left = 0;
	node->split_missing_count = 0;
	return node_index;
}

static int split_left_count(const CARTSplit *split, double *left_sum_out) {
	int missing_to_left = split->missing_left ? split->missing_count : 0;
	*left_sum_out = split->left_sum + (split->missing_left ? split->missing_sum : 0.0);
	return split->n_left + missing_to_left;
}

static int grow_node(CARTSample *sample, CARTPartition *partition,
		CARTCategoryStats *category_stats, CART *tree,
		int max_depth, int min_samples, int node_start, int node_end, double node_sum, int tree_depth) {
	int n_rows = node_end - node_start;

	int node_index = append_leaf(tree, node_start, n_rows, node_sum);

	if (tree_depth >= max_depth)
		return node_index;

	CARTSplit split = choose_split(sample, partition, category_stats,
			node_start, node_end, node_sum, min_samples);
	if (split.feature < 0)
		return node_index;

	partition_node(sample, partition, category_stats, &split, node_start, node_end);

	if (sample->features[split.feature].kind == FEATURE_CATEGORICAL)
		record_categorical_split(tree, node_index, category_stats,
				sample->features[split.feature].n_levels);

	double left_sum;
	int n_left = split_left_count(&split, &left_sum);
	int right_start = node_start + n_left;
	double right_sum = node_sum - left_sum;
	int left = grow_node(sample, partition, category_stats, tree,
			max_depth, min_samples, node_start, right_start, left_sum, tree_depth + 1);
	int right = grow_node(sample, partition, category_stats, tree,
			max_depth, min_samples, right_start, node_end, right_sum, tree_depth + 1);

	CARTNode *node = &tree->nodes[node_index];
	node->feature = split.feature;
	node->threshold = split.threshold;
	node->left_child = left;
	node->right_child = right;
	node->missing_left = split.missing_left;
	node->split_missing_count = split.missing_count;

	return node_index;
}

#define PARALLEL_FIT_MIN_ROWS 20000

typedef struct {
	int node_start;
	int node_end;
	double node_sum;
	int tree_depth;
	int parent_index;
	int child_side;
	CART fragment;
	CARTCategoryStats category_stats;
} CARTSubtreeTask;

typedef struct {
	CARTSubtreeTask *tasks;
	int n_tasks;
	int n_allocated;
} CARTSubtreeList;

static int grow_top(CARTSample *sample, CARTPartition *partition, CARTCategoryStats *category_stats,
		CART *tree, CARTSubtreeList *subtrees, int max_depth, int min_samples, int frontier_depth,
		int node_start, int node_end, double node_sum, int tree_depth, int parent_index, int child_side) {
	if (tree_depth == frontier_depth) {
		GROW_IF_FULL_SYS(subtrees->n_tasks, subtrees->n_allocated, subtrees->tasks);
		CARTSubtreeTask *task = &subtrees->tasks[subtrees->n_tasks++];
		task->node_start = node_start;
		task->node_end = node_end;
		task->node_sum = node_sum;
		task->tree_depth = tree_depth;
		task->parent_index = parent_index;
		task->child_side = child_side;
		task->fragment = (CART){0};
		task->category_stats = (CARTCategoryStats){0};
		return -1;
	}

	int n_rows = node_end - node_start;

	int node_index = append_leaf(tree, node_start, n_rows, node_sum);

	if (tree_depth >= max_depth)
		return node_index;

	CARTSplit split = choose_split(sample, partition, category_stats, node_start, node_end, node_sum, min_samples);
	if (split.feature < 0)
		return node_index;

	partition_node(sample, partition, category_stats, &split, node_start, node_end);

	if (sample->features[split.feature].kind == FEATURE_CATEGORICAL)
		record_categorical_split(tree, node_index, category_stats, sample->features[split.feature].n_levels);

	double left_sum;
	int n_left = split_left_count(&split, &left_sum);
	int right_start = node_start + n_left;
	double right_sum = node_sum - left_sum;
	int left = grow_top(sample, partition, category_stats, tree, subtrees, max_depth, min_samples,
			frontier_depth, node_start, right_start, left_sum, tree_depth + 1, node_index, 0);
	int right = grow_top(sample, partition, category_stats, tree, subtrees, max_depth, min_samples,
			frontier_depth, right_start, node_end, right_sum, tree_depth + 1, node_index, 1);

	CARTNode *node = &tree->nodes[node_index];
	node->feature = split.feature;
	node->threshold = split.threshold;
	node->left_child = left;
	node->right_child = right;
	node->missing_left = split.missing_left;
	node->split_missing_count = split.missing_count;

	return node_index;
}

static void graft_fragment(CART *tree, const CART *fragment, int parent_index, int child_side) {
	int node_base = tree->n_nodes;
	int flags_base = tree->n_category_flags;

	while (tree->n_nodes + fragment->n_nodes > tree->n_allocated) {
		tree->n_allocated = tree->n_allocated ? tree->n_allocated * 2 : 8;
		tree->nodes = realloc(tree->nodes, sizeof(CARTNode) * (size_t)tree->n_allocated);
	}
	while (tree->n_category_flags + fragment->n_category_flags > tree->category_flags_cap) {
		tree->category_flags_cap = tree->category_flags_cap ? tree->category_flags_cap * 2 : 8;
		tree->category_flags = realloc(tree->category_flags, (size_t)tree->category_flags_cap);
	}

	if (fragment->n_category_flags > 0)
		memcpy(&tree->category_flags[flags_base], fragment->category_flags, (size_t)fragment->n_category_flags);
	tree->n_category_flags += fragment->n_category_flags;

	for (int i = 0; i < fragment->n_nodes; i++) {
		CARTNode node = fragment->nodes[i];
		if (node.left_child >= 0) {
			node.left_child += node_base;
			node.right_child += node_base;
		}
		if (node.category_offset >= 0)
			node.category_offset += flags_base;
		tree->nodes[node_base + i] = node;
	}
	tree->n_nodes += fragment->n_nodes;

	if (child_side == 0)
		tree->nodes[parent_index].left_child = node_base;
	else
		tree->nodes[parent_index].right_child = node_base;
}

typedef struct {
	CARTSample *sample;
	CARTPartition *partition;
	CARTSubtreeTask *tasks;
	int max_depth;
	int min_samples;
} CARTGrowContext;

static void grow_subtree_kernel(int start_index, int end_index, void *context_pointer) {
	CARTGrowContext *context = context_pointer;

	for (int t = start_index; t < end_index; t++) {
		CARTSubtreeTask *task = &context->tasks[t];
		grow_node(context->sample, context->partition, &task->category_stats, &task->fragment,
				context->max_depth, context->min_samples,
				task->node_start, task->node_end, task->node_sum, task->tree_depth);
	}
}

static void grow_tree_parallel(Interpreter *interp, CARTSample *sample, CARTPartition *partition,
		CARTCategoryStats *top_category_stats, CART *tree, int max_depth, int min_samples,
		int frontier_depth, int n_rows, double root_sum, int worker_count, int max_levels) {
	CARTSubtreeList subtrees = {0};
	grow_top(sample, partition, top_category_stats, tree, &subtrees,
			max_depth, min_samples, frontier_depth, 0, n_rows, root_sum, 0, -1, 0);

	if (subtrees.n_tasks == 0) {
		free(subtrees.tasks);
		return;
	}

	for (int t = 0; max_levels > 0 && t < subtrees.n_tasks; t++) {
		CARTCategoryStats *stats = &subtrees.tasks[t].category_stats;
		stats->level_sum = malloc((size_t)max_levels * sizeof(double));
		stats->level_count = malloc((size_t)max_levels * sizeof(int));
		stats->level_order = malloc((size_t)max_levels * sizeof(int));
		stats->goes_left = malloc((size_t)max_levels * sizeof(char));
		if (!stats->level_sum || !stats->level_count || !stats->level_order || !stats->goes_left) {
			for (int done = 0; done <= t; done++) {
				free(subtrees.tasks[done].category_stats.level_sum);
				free(subtrees.tasks[done].category_stats.level_count);
				free(subtrees.tasks[done].category_stats.level_order);
				free(subtrees.tasks[done].category_stats.goes_left);
			}
			free(subtrees.tasks);
			fail(interp, "out of memory");
			return;
		}
	}

	CARTGrowContext context = {
		.sample = sample,
		.partition = partition,
		.tasks = subtrees.tasks,
		.max_depth = max_depth,
		.min_samples = min_samples,
	};
	parallel_for(subtrees.n_tasks, worker_count, 1, grow_subtree_kernel, &context);

	for (int t = 0; t < subtrees.n_tasks; t++) {
		CARTSubtreeTask *task = &subtrees.tasks[t];
		graft_fragment(tree, &task->fragment, task->parent_index, task->child_side);
		free(task->fragment.nodes);
		free(task->fragment.category_flags);
		free(task->category_stats.level_sum);
		free(task->category_stats.level_count);
		free(task->category_stats.level_order);
		free(task->category_stats.goes_left);
	}
	free(subtrees.tasks);
}

static void presort_numeric_column(const double *column_values, const double *response,
		int n_rows, CARTSortedColumn *sorted_column, ArgsortPair *value_rows) {
	int n_finite = 0;
	int missing_index = n_rows;
	for (int row = 0; row < n_rows; row++) {
		double value = column_values[row];
		if (value == value) {
			value_rows[n_finite].value = value;
			value_rows[n_finite].index = row;
			n_finite++;
		} else {
			missing_index--;
			value_rows[missing_index].value = value;
			value_rows[missing_index].index = row;
		}
	}

	sort_pairs(value_rows, (size_t)n_finite);

	CARTFeatureEntry *entries = sorted_column->entries;
	int *rows = sorted_column->rows;

	for (int i = 0; i < n_rows; i++) {
		int row = value_rows[i].index;
		entries[i].value = value_rows[i].value;
		entries[i].response = response[row];
		rows[i] = row;
	}
}

static void free_cart_sample(CARTSample *sample, int n_initialized_features) {
	for (int f = 0; f < n_initialized_features; f++) {
		CARTFeature *feature = &sample->features[f];

		if (feature->kind == FEATURE_NUMERIC) {
			free(feature->numeric.entries);
			free(feature->numeric.rows);
		} else {
			free(feature->levels);
			free(feature->level_values);
		}
	}

	free(sample->features);
}

static int category_value_cmp(void *interpreter, const void *left, const void *right) {
	Interpreter *interp = interpreter;
	return val_cmp(interp, *(const Val *)left, *(const Val *)right);
}


static int encode_categorical_column(Interpreter *interp, Object *column, int n_rows,
		int **levels_out, int *n_levels_out, Val **level_values_out)
{
	Val *sorted_values;
	MALLOC_OR_FAIL_RETURNING(interp, sorted_values, (size_t)n_rows * sizeof(Val), -1);

	memcpy(sorted_values, column->items, (size_t)n_rows * sizeof(Val));
	platform_qsort_r(sorted_values, (size_t)n_rows, sizeof(Val), interp, category_value_cmp);

	int n_levels = 1;
	for (int sorted_index = 1; sorted_index < n_rows; sorted_index++)
			if (val_cmp(interp, sorted_values[sorted_index], sorted_values[sorted_index - 1]) != 0)
				n_levels++;

	Val *level_values;
	MALLOC_OR_FAIL_RETURNING_CLEANUP(interp, level_values, (size_t)n_levels * sizeof(Val), free(sorted_values), -1);

	int level = 0;
	level_values[0] = sorted_values[0];
	for (int sorted_index = 1; sorted_index < n_rows; sorted_index++)
		if (val_cmp(interp, sorted_values[sorted_index], sorted_values[sorted_index - 1]) != 0)
			level_values[++level] = sorted_values[sorted_index];

	int *levels;
	MALLOC_OR_FAIL_RETURNING_CLEANUP(interp, levels, (size_t)n_rows * sizeof(int), { free(sorted_values); free(level_values); }, -1);

	for (int row = 0; row < n_rows; row++) {
		Val value = column->items[row];
		LOWER_BOUND(n_levels, probe, val_cmp(interp, level_values[probe], value) < 0, code);
		levels[row] = code;
	}

	free(sorted_values);

	*levels_out = levels;
	*n_levels_out = n_levels;
	*level_values_out = level_values;

	return 0;
}

static int build_cart_sample(Interpreter *interp, Object *features, double *response,
		int n_rows, CARTSample *sample) {
	int n_features = features->len;

	CARTFeature *feature_columns;
	CALLOC_OR_FAIL_RETURNING(interp, feature_columns, (size_t)n_features, sizeof(CARTFeature), -1);
	sample->features = feature_columns;

	ArgsortPair *value_rows;
	MALLOC_OR_FAIL_RETURNING_CLEANUP(interp, value_rows, (size_t)n_rows * sizeof(ArgsortPair),
			free_cart_sample(sample, 0), -1);

	for (int f = 0; f < n_features; f++) {
		Val column_val = features->frame.values[f];
		int unit;
		Val numeric_val = quantity_unwrap(column_val, &unit);
		CARTFeature *feature = &feature_columns[f];

		if (VAL_TAG(numeric_val) == T_MATRIX) {
			Object *column = OBJECT_AT(VAL_DATA(numeric_val));
			feature->kind = FEATURE_NUMERIC;
			MALLOC_OR_FAIL_RETURNING_CLEANUP(interp, feature->numeric.entries, (size_t)n_rows * sizeof(CARTFeatureEntry),
					{ free(value_rows); free_cart_sample(sample, f); }, -1);
			MALLOC_OR_FAIL_RETURNING_CLEANUP(interp, feature->numeric.rows, (size_t)n_rows * sizeof(int),
					{ free(value_rows); free_cart_sample(sample, f + 1); }, -1);
			presort_numeric_column(column->matrix.elements, response, n_rows, &feature->numeric, value_rows);
		} else {
			Object *column = OBJECT_AT(VAL_DATA(column_val));
			feature->kind = FEATURE_CATEGORICAL;
			if (encode_categorical_column(interp, column, n_rows,
					&feature->levels, &feature->n_levels, &feature->level_values) < 0) {
				free(value_rows);
				free_cart_sample(sample, f);
				return -1;
			}
		}
	}

	free(value_rows);

	sample->n_features = n_features;
	sample->response = response;
	sample->feature_names = features->frame.keys;

	return 0;
}

typedef struct {
	int n_levels;
	const Val *level_values;
} FeatureLevels;

static void cart_fill_frame(Interpreter *interp, const CART *tree,
		const cell *feature_names, const double *response, const FeatureLevels *feature_levels,
		const int *row_index, int node_index, int frame_handle, int store_leaf_responses) {
	const CARTNode *node = &tree->nodes[node_index];
	Object *frame = OBJECT_AT(frame_handle);

	frame_put(frame, intern_symbol(interp, "prediction"), make_float(node->prediction));
	frame_put(frame, intern_symbol(interp, "n_rows"), make_float((double)node->n_rows));

	if (node->left_child < 0) {
		if (store_leaf_responses) {
			int responses_handle = object_new_matrix(interp, node->n_rows, 1);
			if (interp->error_flag)
				return;
			double *responses = OBJECT_AT(responses_handle)->matrix.elements;
			for (int i = 0; i < node->n_rows; i++)
				responses[i] = response[row_index[node->row_start + i]];
			frame_put(OBJECT_AT(frame_handle), intern_symbol(interp, "responses"), make_matrix(responses_handle));
		}
		return;
	}

	frame_put(frame, intern_symbol(interp, "feature"), make_symbol((int)feature_names[node->feature]));

	if (feature_levels[node->feature].level_values == NULL) {
		frame_put(frame, intern_symbol(interp, "threshold"), make_float(node->threshold));
		if (node->split_missing_count > 0)
			frame_put(frame, intern_symbol(interp, "default"),
					make_symbol((int)intern_symbol(interp, node->missing_left ? "left" : "right")));
	} else {
		int categories_handle = object_new_set(interp);
		if (interp->error_flag)
			return;
		const Val *level_values = feature_levels[node->feature].level_values;
		for (int level = 0; level < feature_levels[node->feature].n_levels; level++)
			if (tree->category_flags[node->category_offset + level])
				set_add(interp, categories_handle, level_values[level]);
		frame_put(OBJECT_AT(frame_handle), intern_symbol(interp, "categories"), make_set(categories_handle));
	}

	int left_handle = object_new_frame(interp);
	if (interp->error_flag)
		return;
	frame_put(OBJECT_AT(frame_handle), intern_symbol(interp, "left"), make_frame(left_handle));

	int right_handle = object_new_frame(interp);
	if (interp->error_flag)
		return;
	frame_put(OBJECT_AT(frame_handle), intern_symbol(interp, "right"), make_frame(right_handle));

	cart_fill_frame(interp, tree, feature_names, response, feature_levels,
			row_index, node->left_child, left_handle, store_leaf_responses);
	if (interp->error_flag)
		return;
	cart_fill_frame(interp, tree, feature_names, response, feature_levels,
			row_index, node->right_child, right_handle, store_leaf_responses);
}

static void free_fit_allocations(CARTPartition *partition, CARTCategoryStats *category_stats,
		CART *tree, CARTSample *sample, int n_features) {
	free(partition->row_index);
	free(partition->branch);
	free(partition->staged_entries);
	free(partition->staged_rows);

	free(category_stats->level_sum);
	free(category_stats->level_count);
	free(category_stats->level_order);
	free(category_stats->goes_left);

	free(tree->nodes);
	free(tree->category_flags);

	free_cart_sample(sample, n_features);
}

static double frame_number_or(Interpreter *interp, Object *frame, const char *name, double fallback) {
	cell key = intern_symbol(interp, name);
	FRAME_LOOKUP(frame, key, at, present);
	if (!present)
		return fallback;

	Val value = frame->frame.values[at];
	return VAL_TAG(value) == T_FLOAT ? VAL_NUMBER(value) : fallback;
}

static int validate_tree_inputs(Interpreter *interp, Object *features, Object *y, int *n_rows_out) {
	int y_rows = y->matrix.rows;
	int y_columns = y->matrix.columns;
	if (y_rows != 1 && y_columns != 1) {
		fail(interp, "expected a response vector (nx1 or 1xn); got %dx%d", y_rows, y_columns);
		return -1;
	}

	int n_rows = y_rows * y_columns;
	int n_features = features->len;
	if (n_rows < 1 || n_features < 1) {
		fail(interp, "need at least one row and one feature; got %d rows, %d features", n_rows, n_features);
		return -1;
	}

	for (int f = 0; f < n_features; f++) {
		int column_unit;
		Val column_val = quantity_unwrap(features->frame.values[f], &column_unit);
		Tag column_tag = VAL_TAG(column_val);
		(void)column_unit;

		int column_length;
		if (column_tag == T_MATRIX) {
			Object *column = OBJECT_AT(VAL_DATA(column_val));
			column_length = column->matrix.rows * column->matrix.columns;
		} else if (column_tag == T_ARRAY) {
			column_length = OBJECT_AT(VAL_DATA(column_val))->len;
		} else {
			fail(interp, "feature column must be a numeric vector or an array; got %s", tag_name(column_tag));
			return -1;
		}
		if (column_length != n_rows) {
			fail(interp, "feature column has %d rows; response has %d", column_length, n_rows);
			return -1;
		}
	}

	*n_rows_out = n_rows;
	return 0;
}

static int fit_tree_build(Interpreter *interp, Object *features, Object *y, Object *params, int parallel) {
	int n_rows;
	if (validate_tree_inputs(interp, features, y, &n_rows) < 0)
		return -1;
	int n_features = features->len;

	int max_depth = (int)frame_number_or(interp, params, "max-depth", (double)INT_MAX);
	int min_samples = MAX((int)frame_number_or(interp, params, "min-samples", 1.0), 1);
	int store_leaf_responses = frame_number_or(interp, params, "store-leaf-responses", 0.0) != 0.0;

	CARTSample sample = {0};
	CARTPartition partition = {0};
	CARTCategoryStats category_stats = {0};
	CART tree = {0};

	double *response = y->matrix.elements;
	if (build_cart_sample(interp, features, response, n_rows, &sample) < 0)
		return -1;

	int max_levels = 0;
	for (int f = 0; f < n_features; f++)
		if (sample.features[f].kind == FEATURE_CATEGORICAL && sample.features[f].n_levels > max_levels)
			max_levels = sample.features[f].n_levels;

	if (max_levels > 0) {
		MALLOC_OR_FAIL_RETURNING_CLEANUP(interp, category_stats.level_sum, (size_t)max_levels * sizeof(double),
				free_fit_allocations(&partition, &category_stats, &tree, &sample, n_features), -1);
		MALLOC_OR_FAIL_RETURNING_CLEANUP(interp, category_stats.level_count, (size_t)max_levels * sizeof(int),
				free_fit_allocations(&partition, &category_stats, &tree, &sample, n_features), -1);
		MALLOC_OR_FAIL_RETURNING_CLEANUP(interp, category_stats.level_order, (size_t)max_levels * sizeof(int),
				free_fit_allocations(&partition, &category_stats, &tree, &sample, n_features), -1);
		MALLOC_OR_FAIL_RETURNING_CLEANUP(interp, category_stats.goes_left, (size_t)max_levels * sizeof(char),
				free_fit_allocations(&partition, &category_stats, &tree, &sample, n_features), -1);
	}

	MALLOC_OR_FAIL_RETURNING_CLEANUP(interp, partition.row_index, (size_t)n_rows * sizeof(int),
			free_fit_allocations(&partition, &category_stats, &tree, &sample, n_features), -1);
	for (int i = 0; i < n_rows; i++)
		partition.row_index[i] = i;

	MALLOC_OR_FAIL_RETURNING_CLEANUP(interp, partition.branch, (size_t)n_rows * sizeof(char),
			free_fit_allocations(&partition, &category_stats, &tree, &sample, n_features), -1);
	MALLOC_OR_FAIL_RETURNING_CLEANUP(interp, partition.staged_entries, (size_t)n_rows * sizeof(CARTFeatureEntry),
			free_fit_allocations(&partition, &category_stats, &tree, &sample, n_features), -1);
	MALLOC_OR_FAIL_RETURNING_CLEANUP(interp, partition.staged_rows, (size_t)n_rows * sizeof(int),
			free_fit_allocations(&partition, &category_stats, &tree, &sample, n_features), -1);

	double root_sum = 0.0;
	for (int i = 0; i < n_rows; i++)
		root_sum += response[i];

	if (parallel) {
		int worker_count = cpu_count();
		int frontier_depth = 1;
		while ((1 << frontier_depth) < 4 * worker_count && frontier_depth < 12)
			frontier_depth++;
		grow_tree_parallel(interp, &sample, &partition, &category_stats, &tree,
				max_depth, min_samples, frontier_depth, n_rows, root_sum, worker_count, max_levels);
		if (interp->error_flag) {
			free_fit_allocations(&partition, &category_stats, &tree, &sample, n_features);
			return -1;
		}
	} else {
		grow_node(&sample, &partition, &category_stats, &tree, max_depth, min_samples, 0, n_rows, root_sum, 0);
	}

	int root_handle = object_new_frame(interp);
	if (interp->error_flag) {
		free_fit_allocations(&partition, &category_stats, &tree, &sample, n_features);
		return -1;
	}
	gc_root_push(interp, make_frame(root_handle));

	FeatureLevels *feature_levels;
	MALLOC_OR_FAIL_RETURNING_CLEANUP(interp, feature_levels, (size_t)n_features * sizeof(FeatureLevels),
			{ gc_root_pop(interp); free_fit_allocations(&partition, &category_stats, &tree, &sample, n_features); }, -1);
	for (int f = 0; f < n_features; f++) {
		feature_levels[f].n_levels = sample.features[f].n_levels;
		feature_levels[f].level_values = sample.features[f].level_values;
	}

	cart_fill_frame(interp, &tree, sample.feature_names, sample.response, feature_levels,
			partition.row_index, 0, root_handle, store_leaf_responses);

	free(feature_levels);
	free_fit_allocations(&partition, &category_stats, &tree, &sample, n_features);
	gc_root_pop(interp);
	if (interp->error_flag)
		return -1;

	return root_handle;
}

static int fit_tree_serial(Interpreter *interp, Object *features, Object *y, Object *params) {
	return fit_tree_build(interp, features, y, params, 0);
}

static int fit_tree_parallel(Interpreter *interp, Object *features, Object *y, Object *params) {
	return fit_tree_build(interp, features, y, params, 1);
}

static int tree_word_operands(Interpreter *interp, Val *chain_sp,
		int (*build)(Interpreter *, Object *, Object *, Object *)) {
	Val params_val = chain_sp[-1];
	if (VAL_TAG(params_val) != T_FRAME) {
		fail(interp, "expected a parameters frame; got %s", tag_name(VAL_TAG(params_val)));
		return -1;
	}

	int y_unit;
	Val y_matrix_val = quantity_unwrap(chain_sp[-2], &y_unit);
	if (VAL_TAG(y_matrix_val) != T_MATRIX) {
		fail(interp, "expected a numeric response vector; got %s", tag_name(VAL_TAG(y_matrix_val)));
		return -1;
	}
	(void)y_unit;

	Val features_val = chain_sp[-3];
	if (VAL_TAG(features_val) != T_FRAME) {
		fail(interp, "expected a features frame; got %s", tag_name(VAL_TAG(features_val)));
		return -1;
	}

	return build(interp, OBJECT_AT(VAL_DATA(features_val)),
			OBJECT_AT(VAL_DATA(y_matrix_val)), OBJECT_AT(VAL_DATA(params_val)));
}

void p_fit_tree(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3);

	int root_handle = tree_word_operands(interp, chain_sp, fit_tree_serial);
	if (interp->error_flag)
		return;

	chain_sp[-3] = make_frame(root_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}

void p_pfit_tree(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3);

	int root_handle = tree_word_operands(interp, chain_sp, fit_tree_parallel);
	if (interp->error_flag)
		return;

	chain_sp[-3] = make_frame(root_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}
