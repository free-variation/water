#include "logicforth.h"

static Interpreter *worker_pool[MAX_WORKER_THREADS];
static _Atomic int worker_claim;
static _Thread_local Interpreter *worker_interp;
static _Thread_local int worker_slot;
static _Atomic int parallel_error;

void p_map(Interpreter *interp) {
	POP_XT(xt, "map");
	PEEK_SEQUENCE_AT(source_val, 0, "map");
	Object *source = OBJECT_AT(VAL_DATA(source_val));
	int source_index = interp->dsp - 1;

	NEW_ARRAY(result_handle, result, source->len);
	memset(result->items, 0, sizeof(Val) * (size_t)MAX(source->len, 1));
	gc_root_push(interp, make_array(result_handle));

	CallContext ctx;
	call_open(interp, xt, &ctx);
	for (int i = 0; i < source->len && !interp->error_flag; i++) {
		int dsp_before = interp->dsp;
		push(interp, source->items[i]);
		call_step(interp, &ctx, xt);
		if (interp->error_flag) break;
		if (interp->dsp != dsp_before + 1) {
			fail(interp, "map: quotation must leave exactly one value per element, but changed the stack by %d", interp->dsp - dsp_before);
			break;
		}
		result->items[i] = pop(interp);
	}
	call_close(interp, &ctx);

	gc_root_pop(interp);

	if (!interp->error_flag) {
		interp->dsp = source_index;
		push(interp, make_array(result_handle));
	}

	DISPATCH(interp);
}

void p_mapn(Interpreter *interp) {
	POP_INT(arity, "mapn", "arity");
	if (arity < 1) {
		fail(interp, "mapn: arity must be >= 1; got %d", arity);
		return;
	}
	POP_XT(xt, "mapn");
	if (arity > interp->dsp) {
		fail(interp, "mapn: arity %d exceeds %d values on the stack", arity, interp->dsp);
		return;
	}

	int first_source = interp->dsp - arity;
	int row_count = -1;
	for (int i = 0; i < arity; i++) {
		if (VAL_TAG(interp->data_stack[first_source + i]) != T_ARRAY) {
			fail(interp, "mapn: source %d is %s, expected an array", i, tag_name(VAL_TAG(interp->data_stack[first_source + i])));
			return;
		}
		Object *source = OBJECT_AT(VAL_DATA(interp->data_stack[first_source + i]));
		if (row_count < 0) row_count = source->len;
		else if (source->len != row_count) {
			fail(interp, "mapn: source arrays differ in length (%d vs %d)", source->len, row_count);
			return;
		}
	}

	NEW_ARRAY(result_handle, result, row_count);
	memset(result->items, 0, sizeof(Val) * (size_t)MAX(row_count, 1));

	gc_root_push(interp, make_array(result_handle));

	CallContext ctx;
	call_open(interp, xt, &ctx);
	for (int row = 0; row < row_count && !interp->error_flag; row++) {
		int dsp_before = interp->dsp;
		for (int source_index = 0; source_index < arity; source_index++) {
			Object *source = OBJECT_AT(VAL_DATA(interp->data_stack[first_source + source_index]));
			push(interp, source->items[row]);
		}
		call_step(interp, &ctx, xt);
		if (interp->error_flag) break;
		if (interp->dsp != dsp_before + 1) {
			fail(interp, "mapn: quotation must leave exactly one value per row, but changed the stack by %d", interp->dsp - dsp_before);
			break;
		}
		result->items[row] = pop(interp);
	}
	call_close(interp, &ctx);

	gc_root_pop(interp);

	if (!interp->error_flag) {
		interp->dsp = first_source;
		push(interp, make_array(result_handle));
	}

	DISPATCH(interp);
}

void p_filter(Interpreter *interp) {
	POP_XT(xt, "filter");
	PEEK_SEQUENCE_AT(source_val, 0, "filter");
	Object *source = OBJECT_AT(VAL_DATA(source_val));
	int source_index = interp->dsp - 1;

	int *keep = malloc((size_t)MAX(source->len, 1) * sizeof(int));
	int n_kept = 0;
	CallContext ctx;
	call_open(interp, xt, &ctx);
	for (int i = 0; i < source->len && !interp->error_flag; i++) {
		int dsp_before = interp->dsp;
		push(interp, source->items[i]);
		call_step(interp, &ctx, xt);
		if (interp->error_flag) break;
		if (interp->dsp != dsp_before + 1) {
			fail(interp, "filter: predicate must leave exactly one value per element, but changed the stack by %d", interp->dsp - dsp_before);
			break;
		}
		keep[i] = truthy(pop(interp));
		n_kept += keep[i];
	}
	call_close(interp, &ctx);

	if (interp->error_flag) {
		free(keep);
		return;
	}
	int result_handle = object_new_array(interp, n_kept);
	if (interp->error_flag) {
		free(keep);
		return;
	}
	Object *result = OBJECT_AT(result_handle);

	int result_idx = 0;
	for (int i = 0; i < source->len; i++) {
		if (keep[i])
			result->items[result_idx++] = source->items[i];
	}

	free(keep);
	interp->dsp = source_index;
	push(interp, make_array(result_handle));

	DISPATCH(interp);
}

void p_reduce(Interpreter *interp) {
	POP_XT(combiner, "reduce");
	POP(init_val);
	PEEK_SEQUENCE_AT(source_val, 0, "reduce");
	Object *source = OBJECT_AT(VAL_DATA(source_val));

	Val result_val = init_val;
	CallContext ctx;
	call_open(interp, combiner, &ctx);
	for (int i = 0; i < source->len && !interp->error_flag; i++) {
		push(interp, result_val);
		push(interp, source->items[i]);

		call_step(interp, &ctx, combiner);
		if (interp->error_flag) break;

		result_val = pop(interp);
	}
	call_close(interp, &ctx);
	if (interp->error_flag) return;

	pop(interp);
	push(interp, result_val);

	DISPATCH(interp);
}

#define COUNTED_LOOP(name, word_name, per_iter) \
	void name(Interpreter *interp) { \
		POP_INT(n, word_name, "count"); \
		POP_XT(xt, word_name); \
		if (n < 0) { \
			fail(interp, word_name ": count must be non-negative; got %d", n); \
			return; \
		} \
		CallContext ctx; \
		call_open(interp, xt, &ctx); \
		if (ctx.fast) { \
			for (int i = 0; i < n && !interp->error_flag; i++) { \
				per_iter; \
				call_invoke(interp); \
			} \
			call_close(interp, &ctx); \
		} else { \
			for (int i = 0; i < n && !interp->error_flag; i++) { \
				per_iter; \
				execute_cfa(interp, xt); \
			} \
		} \
		DISPATCH(interp); \
	}

COUNTED_LOOP(p_times,   "times",   (void)i)
COUNTED_LOOP(p_i_times, "i-times", push(interp, make_float((double)i)))

static Interpreter *claim_worker(void) {
	int pool_index = atomic_fetch_add(&worker_claim, 1);
	worker_slot = pool_index;

	if (!worker_pool[pool_index])
		worker_pool[pool_index] = worker_init(pool_index + 1);

	worker_pool[pool_index]->dsp = 0;
	worker_pool[pool_index]->error_flag = 0;
	return worker_pool[pool_index];
}

typedef struct {
	int function;
	Object *domain;
	Object *image;
} PmapContext;


static int cpu_count(void) {
	long n_cores = sysconf(_SC_NPROCESSORS_ONLN);
	return n_cores > 0 ? (int)n_cores : 1;
}

void p_num_cores(Interpreter *interp) {
	push(interp, make_float(cpu_count()));
	DISPATCH(interp);
}

static void pmap_kernel(int start_index, int end_index, void *context) {
	PmapContext *mapping = context;

	if (!worker_interp) 
		worker_interp = claim_worker();
	
	for (int i = start_index; i < end_index; i++) {
		push(worker_interp, mapping->domain->items[i]);
		execute_cfa(worker_interp, mapping->function);
		if (worker_interp->error_flag) {
			parallel_error = 1;
			return;
		}
		mapping->image->items[i] = pop(worker_interp);
	}
}

static int parallel_apply(Object *domain, int worker_count,
		int items_per_claim, void (*kernel)(int, int, void *), void *context) {
	int object_headroom = arena.n_objects + domain->len + worker_count * SLOTS_PER_CLAIM;
	object_headroom = MIN(object_headroom, arena.max_objects);
	if (object_headroom > arena.objects_cap)
		GROW_OBJECT_TABLE(object_headroom);

	int pair_headroom = pairs.n_pairs + domain->len + worker_count * SLOTS_PER_CLAIM;
	if (pair_headroom > pairs.pairs_cap)
		GROW_PAIR_TABLE(pair_headroom);

	size_t saved_used = arena.used;
	int saved_n_objects = arena.n_objects;
	int saved_n_pairs = pairs.n_pairs;

	worker_claim = 0;
	worker_interp = NULL;
	parallel_error = 0;
	in_parallel = 1;
	parallel_for(domain->len, worker_count, items_per_claim, kernel, context);
	in_parallel = 0;

	if (parallel_error)
		abort_parallel_region(saved_used, saved_n_objects, saved_n_pairs);

	return parallel_error;
}

typedef struct {
	int predicate;
	Object *domain;
	char *keep;
} PfilterContext;

static void pfilter_kernel(int start_index, int end_index, void *context) {
	PfilterContext *filter = context;

	if (!worker_interp)
		worker_interp = claim_worker();

	for (int i = start_index; i < end_index; i++) {
		push(worker_interp, filter->domain->items[i]);
		execute_cfa(worker_interp, filter->predicate);
		if (worker_interp->error_flag) {
			parallel_error = 1;
			return;
		}
		filter->keep[i] = truthy(pop(worker_interp));
	}
}

void p_pfilter(Interpreter *interp) {
	POP_XT(predicate, "pfilter-ext");
	POP_INT(items_per_claim, "pfilter-ext", "items per claim");
	POP_INT(worker_count, "pfilter", "worker count");
	PEEK_SEQUENCE_AT(domain_val, 0, "pfilter-ext");

	Object *domain = OBJECT_AT(VAL_DATA(domain_val));
	int domain_index = interp->dsp - 1;

	char *keep = calloc((size_t)MAX(domain->len, 1), 1);

	PfilterContext filter = { .predicate = predicate, .domain = domain, .keep = keep };
	if (parallel_apply(domain, worker_count, items_per_claim, pfilter_kernel, &filter)) {
		free(keep);
		fail(interp, "pfilter: a worker predicate failed (faulted or allocated past the parallel headroom)");
		return;
	}

	int n_kept = 0;
	for (int i = 0; i < domain->len; i++)
		n_kept += keep[i];

	NEW_ARRAY(image_handle, image, n_kept);
	int write_index = 0;
	for (int i = 0; i < domain->len; i++)
		if (keep[i])
			image->items[write_index++] = domain->items[i];

	free(keep);
	interp->dsp = domain_index;
	push(interp, make_array(image_handle));

	DISPATCH(interp);
}

void p_pmap(Interpreter *interp) {
	POP_XT(function, "pmap-ext");
	POP_INT(items_per_claim, "pmap-ext", "items per claim");
	POP_INT(worker_count, "pmap", "worker count");
	PEEK_SEQUENCE_AT(domain_val, 0, "pmap-ext");

	Object *domain = OBJECT_AT(VAL_DATA(domain_val));
	int domain_index = interp->dsp - 1;

	NEW_ARRAY(image_handle, image, domain->len);
	memset(image->items, 0, sizeof(Val) * (size_t)MAX(domain->len, 1));
	gc_root_push(interp, make_array(image_handle));

	PmapContext mapping = { .function = function, .domain = domain, .image = image };
	int failed = parallel_apply(domain, worker_count, items_per_claim, pmap_kernel, &mapping);

	gc_root_pop(interp);

	if (failed) {
		fail(interp, "pmap: a worker quotation failed (faulted or allocated past the parallel headroom)");
		return;
	}

	interp->dsp = domain_index;
	push(interp, make_array(image_handle));

	DISPATCH(interp);
}

typedef struct {
	int map_function;
	int combine_function;
	Object *domain;
	Object *partials;
} PmapReduceContext;

static void pmap_reduce_kernel(int start_index, int end_index, void *context) {
	PmapReduceContext *reduction = context;

	if (!worker_interp)
		worker_interp = claim_worker();

	Val accumulator = reduction->partials->items[worker_slot];
	for (int i = start_index; i < end_index; i++) {
		push(worker_interp, accumulator);
		push(worker_interp, reduction->domain->items[i]);

		execute_cfa(worker_interp, reduction->map_function);
		execute_cfa(worker_interp, reduction->combine_function);
		if (worker_interp->error_flag) {
			parallel_error = 1;
			return;
		}

		accumulator = pop(worker_interp);
	}

	reduction->partials->items[worker_slot] = accumulator;
}

void p_pmap_reduce(Interpreter *interp) {
	POP_XT(combine_function, "pmap-reduce-ext");
	POP_XT(map_function, "pmap-reduce-ext");
	POP(identity);
	POP_INT(items_per_claim, "pmap-reduce-ext", "items per claim");
	POP_INT(worker_count, "pmap-reduce-ext", "worker count");
	PEEK_SEQUENCE_AT(domain_val, 0, "pmap-reduce-ext");

	Object *domain = OBJECT_AT(VAL_DATA(domain_val));
	int domain_index = interp->dsp - 1;

	CLAMP(worker_count, 1, MAX_WORKER_THREADS);

	gc_root_push(interp, identity);

	NEW_ARRAY(partials_handle, partials, worker_count);
	for (int i = 0; i < worker_count; i++) 
		partials->items[i] = identity;
	gc_root_push(interp, make_array(partials_handle));

	PmapReduceContext reduction = {
		.map_function = map_function,
		.combine_function = combine_function,
		.domain = domain,
		.partials = partials,
	};

	if (parallel_apply(domain, worker_count, items_per_claim, pmap_reduce_kernel, &reduction)) {
		gc_root_pop(interp);
		gc_root_pop(interp);
		fail(interp, "pmap-reduce: a worker quotation failed (faulted or allocated past the parallel headroom)");
		return;
	}

	push(interp, identity);
	for (int i = 0; i < worker_count; i++) {
		push(interp, partials->items[i]);
		execute_cfa(interp, combine_function);
		if (interp->error_flag) {
			gc_root_pop(interp);
			gc_root_pop(interp);
			return;
		}
	}

	Val result = pop(interp);

	gc_root_pop(interp);
	gc_root_pop(interp);
	interp->dsp = domain_index;
	push(interp, result);

	DISPATCH(interp);
}

