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

	Interpreter *worker = worker_pool[pool_index];
	worker->dsp = 0;
	worker->rsp = 0;
	worker->side_dsp = 0;
	worker->local_base = 0;
	worker->run_floor = 0;
	worker->bind_trail_top = 0;
	worker->lvar_top = 0;
	worker->n_gc_roots = 0;
	worker->unwinding = 0;
	worker->unwind_target = 0;
	worker->next_mark_id = 1;
	worker->error_flag = 0;
	return worker;
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

static int references_region_depth(Val value, RegionSnapshot *snapshot, int depth) {
	if (depth > MAX_NESTING_DEPTH)
		return 1;   /* too deep / possible cycle: conservatively keep the region */
	switch (VAL_TAG(value)) {
		case T_STRING:
		case T_MATRIX:
			return VAL_DATA(value) >= snapshot->n_objects;
		case T_SET:
		case T_ARRAY: {
			int handle = (int)VAL_DATA(value);
			if (handle >= snapshot->n_objects)
				return 1;
			Object *obj = OBJECT_AT(handle);
			for (int i = 0; i < obj->len; i++)
				if (references_region_depth(obj->items[i], snapshot, depth + 1))
					return 1;
			return 0;
		}
		case T_FRAME: {
			int handle = (int)VAL_DATA(value);
			if (handle >= snapshot->n_objects)
				return 1;
			Object *obj = OBJECT_AT(handle);
			for (int i = 0; i < obj->len; i++)
				if (references_region_depth(obj->frame.values[i], snapshot, depth + 1))
					return 1;
			return 0;
		}
		case T_CONT: {
			int handle = (int)VAL_DATA(value);
			if (handle >= snapshot->n_objects)
				return 1;
			Object *obj = OBJECT_AT(handle);
			for (int i = 0; i < obj->continuation.return_len; i++)
				if (references_region_depth(obj->continuation.return_slice[i], snapshot, depth + 1))
					return 1;
			return 0;
		}
		case T_PAIR: {
			int handle = (int)VAL_DATA(value);
			if (handle >= snapshot->n_pairs)
				return 1;
			Pair *pair = &pairs.table[handle];
			return references_region_depth(pair->head, snapshot, depth + 1)
					|| references_region_depth(pair->tail, snapshot, depth + 1);
		}
		case T_LOGIC_VAR:
			return 1;
		default:
			return 0;
	}
}

static int references_region(Val value, RegionSnapshot *snapshot) {
	return references_region_depth(value, snapshot, 0);
}

static int parallel_apply(Object *domain, int worker_count,
		int items_per_claim, void (*kernel)(int, int, void *), void *context,
		RegionSnapshot *snapshot) {
	CLAMP(worker_count, 1, MAX_WORKER_THREADS);
	int object_headroom = arena.n_objects + domain->len + worker_count * SLOTS_PER_CLAIM;
	object_headroom = MIN(object_headroom, arena.max_objects);
	if (object_headroom > arena.objects_cap)
		GROW_OBJECT_TABLE(object_headroom);

	int pair_headroom = pairs.n_pairs + domain->len + worker_count * SLOTS_PER_CLAIM;
	if (pair_headroom > pairs.pairs_cap)
		GROW_PAIR_TABLE(pair_headroom);

	snapshot->used = arena.used;
	snapshot->n_objects = arena.n_objects;
	snapshot->n_pairs = pairs.n_pairs;

	worker_claim = 0;
	worker_interp = NULL;
	parallel_error = 0;
	in_parallel = 1;
	reset_thread_alloc();
	parallel_for(domain->len, worker_count, items_per_claim, kernel, context);
	in_parallel = 0;

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
	RegionSnapshot region;
	if (parallel_apply(domain, worker_count, items_per_claim, pfilter_kernel, &filter, &region)) {
		free(keep);
		abort_parallel_region(region.used, region.n_objects, region.n_pairs);
		fail(interp, "pfilter: a worker predicate failed (faulted or allocated past the parallel headroom)");
		return;
	}

	abort_parallel_region(region.used, region.n_objects, region.n_pairs);

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
	RegionSnapshot region;
	int failed = parallel_apply(domain, worker_count, items_per_claim, pmap_kernel, &mapping, &region);

	gc_root_pop(interp);

	if (failed) {
		abort_parallel_region(region.used, region.n_objects, region.n_pairs);
		fail(interp, "pmap: a worker quotation failed (faulted or allocated past the parallel headroom)");
		return;
	}

	int rewindable = 1;
	for (int i = 0; i < domain->len; i++)
		if (references_region(image->items[i], &region)) {
			rewindable = 0;
			break;
		}
	if (rewindable)
		abort_parallel_region(region.used, region.n_objects, region.n_pairs);

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
	RegionSnapshot region;

	if (parallel_apply(domain, worker_count, items_per_claim, pmap_reduce_kernel, &reduction, &region)) {
		gc_root_pop(interp);
		gc_root_pop(interp);
		abort_parallel_region(region.used, region.n_objects, region.n_pairs);
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

	if (!references_region(result, &region))
		abort_parallel_region(region.used, region.n_objects, region.n_pairs);

	interp->dsp = domain_index;
	push(interp, result);

	DISPATCH(interp);
}

