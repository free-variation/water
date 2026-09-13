#include "telic.h"

static Interpreter *worker_pool[MAX_WORKER_THREADS];
static _Atomic int worker_claim;
static _Thread_local Interpreter *worker_interp;
static _Thread_local int worker_slot;
static _Atomic int parallel_error;

static int call_one_result(Interpreter *interp, CallContext *context, int xt, int dsp_before,
		const char *op, const char *callable, const char *unit, Val *result_out) {
	call_step(interp, context, xt);
	if (interp->error_flag)
		return 0;
	if (interp->dsp != dsp_before + 1) {
		fail(interp, "%s: %s must leave exactly one value per %s, but changed the stack by %d",
				op, callable, unit, interp->dsp - dsp_before);
		return 0;
	}

	*result_out = pop(interp);
	return 1;
}

void p_map(DISPATCH_ARGS) {
	POP_CALLABLE(xt, "map");
	PEEK_SEQUENCE_AT(source_val, 0, "map");
	Object *source = OBJECT_AT(VAL_DATA(source_val));
	int source_index = interp->dsp - 1;

	NEW_ARRAY(result_handle, result, source->len);
	memset(result->items, 0, sizeof(Val) * (size_t)MAX(source->len, 1));
	gc_root_push(interp, make_array(result_handle));

	CallContext context;
	call_open_callable(interp, xt_val, &context);
	for (int i = 0; i < source->len && !interp->error_flag; i++) {
		int dsp_before = interp->dsp;
		push(interp, source->items[i]);
		if (!call_one_result(interp, &context, xt, dsp_before, "map", "quotation", "element", &result->items[i]))
			break;
	}
	call_close(interp, &context);

	gc_root_pop(interp);

	if (!interp->error_flag) {
		interp->dsp = source_index;
		push(interp, make_array(result_handle));
	}

	DISPATCH(interp);
}

void p_each(DISPATCH_ARGS) {
	POP_CALLABLE(xt, "each");
	PEEK_SEQUENCE_AT(source_val, 0, "each");
	Object *source = OBJECT_AT(VAL_DATA(source_val));
	int source_index = interp->dsp - 1;

	CallContext context;
	call_open_callable(interp, xt_val, &context);
	for (int i = 0; i < source->len && !interp->error_flag; i++) {
		int dsp_before = interp->dsp;
		push(interp, source->items[i]);

		call_step(interp, &context, xt);
		if (interp->error_flag)
			break;

		if (interp->dsp != dsp_before) {
			fail(interp, "each: quotation must leave nothing per element, but changed the stack by %d",
					interp->dsp - dsp_before);
			break;
		}
	}
	call_close(interp, &context);

	if (interp->error_flag)
		return;

	interp->dsp = source_index;

	DISPATCH(interp);
}

void p_nmap(DISPATCH_ARGS) {
	POP_INT(arity, "nmap", "arity");
	if (arity < 1) {
		fail(interp, "arity must be >= 1; got %d", arity);
		return;
	}
	POP_CALLABLE(xt, "nmap");
	if (arity > interp->dsp) {
		fail(interp, "arity %d exceeds %d values on the stack", arity, interp->dsp);
		return;
	}

	int first_source = interp->dsp - arity;
	int row_count = -1;
	for (int i = 0; i < arity; i++) {
		if (VAL_TAG(interp->data_stack[first_source + i]) != T_ARRAY) {
			fail(interp, "source %d is %s, expected an array", i, tag_name(VAL_TAG(interp->data_stack[first_source + i])));
			return;
		}
		Object *source = OBJECT_AT(VAL_DATA(interp->data_stack[first_source + i]));
		if (row_count < 0) row_count = source->len;
		else if (source->len != row_count) {
			fail(interp, "source arrays differ in length (%d vs %d)", source->len, row_count);
			return;
		}
	}

	NEW_ARRAY(result_handle, result, row_count);
	memset(result->items, 0, sizeof(Val) * (size_t)MAX(row_count, 1));

	gc_root_push(interp, make_array(result_handle));

	CallContext context;
	call_open_callable(interp, xt_val, &context);
	for (int row = 0; row < row_count && !interp->error_flag; row++) {
		int dsp_before = interp->dsp;
		for (int source_index = 0; source_index < arity; source_index++) {
			Object *source = OBJECT_AT(VAL_DATA(interp->data_stack[first_source + source_index]));
			push(interp, source->items[row]);
		}
		if (!call_one_result(interp, &context, xt, dsp_before, "nmap", "quotation", "row", &result->items[row]))
			break;
	}
	call_close(interp, &context);

	gc_root_pop(interp);

	if (!interp->error_flag) {
		interp->dsp = first_source;
		push(interp, make_array(result_handle));
	}

	DISPATCH(interp);
}

void p_filter(DISPATCH_ARGS) {
	POP_CALLABLE(xt, "filter");
	PEEK_SEQUENCE_AT(source_val, 0, "filter");
	Object *source = OBJECT_AT(VAL_DATA(source_val));
	int source_index = interp->dsp - 1;

	int *keep;
	MALLOC_OR_FAIL(interp, keep, (size_t)MAX(source->len, 1) * sizeof(int));
	int n_kept = 0;
	CallContext context;
	call_open_callable(interp, xt_val, &context);
	for (int i = 0; i < source->len && !interp->error_flag; i++) {
		int dsp_before = interp->dsp;
		push(interp, source->items[i]);
		Val verdict;
		if (!call_one_result(interp, &context, xt, dsp_before, "filter", "predicate", "element", &verdict))
			break;
		keep[i] = truthy(verdict);
		n_kept += keep[i];
	}
	call_close(interp, &context);

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

void p_find_first(DISPATCH_ARGS) {
	POP_CALLABLE(pred, "find-first");
	PEEK_SEQUENCE_AT(source_val, 0, "find-first");
	Object *source = OBJECT_AT(VAL_DATA(source_val));
	int source_index = interp->dsp - 1;

	Val found = make_tagged(T_NONE, 0);
	CallContext context;
	call_open_callable(interp, pred_val, &context);
	for (int i = 0; i < source->len && !interp->error_flag; i++) {
		int dsp_before = interp->dsp;
		push(interp, source->items[i]);
		Val verdict;
		if (!call_one_result(interp, &context, pred, dsp_before, "find-first", "predicate", "element", &verdict))
			break;
		if (truthy(verdict)) {
			found = source->items[i];
			break;
		}
	}
	call_close(interp, &context);

	if (interp->error_flag)
		return;

	interp->dsp = source_index;
	push(interp, found);

	DISPATCH(interp);
}

void p_reduce(DISPATCH_ARGS) {
	POP_CALLABLE(combiner, "reduce");
	POP(init_val);
	PEEK_SEQUENCE_AT(source_val, 0, "reduce");
	Object *source = OBJECT_AT(VAL_DATA(source_val));

	Val result_val = init_val;
	CallContext context;
	call_open_callable(interp, combiner_val, &context);
	if (interp->dsp + 2 > DATA_STACK_DEPTH) {
		call_close(interp, &context);
		fail(interp, "stack overflow");
		return;
	}
	for (int i = 0; i < source->len && !interp->error_flag; i++) {
		interp->data_stack[interp->dsp++] = result_val;
		interp->data_stack[interp->dsp++] = source->items[i];

		call_step(interp, &context, combiner);
		if (interp->error_flag) break;

		result_val = interp->data_stack[--interp->dsp];
	}
	call_close(interp, &context);
	if (interp->error_flag) return;

	pop(interp);
	push(interp, result_val);

	DISPATCH(interp);
}

#define COUNTED_LOOP(name, word_name, per_iter) \
	void name(DISPATCH_ARGS) { \
		POP_INT(n, word_name, "count"); \
		POP_CALLABLE(xt, word_name); \
		if (n < 0) { \
			fail(interp, "length must be non-negative; got %d", n); \
			return; \
		} \
		CallContext context; \
		call_open_callable(interp, xt_val, &context); \
		if (context.fast) { \
			for (int i = 0; i < n && !interp->error_flag; i++) { \
				per_iter; \
				if (context.reuses_locals) \
					interp->loop_local_refill = 1; \
				call_invoke(interp); \
			} \
			call_close(interp, &context); \
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

typedef enum {
	COMBINE_DISPATCHED,
	COMBINE_ADD,
	COMBINE_SUB,
	COMBINE_MUL,
	COMBINE_DIV
} CombineKind;

static CombineKind combine_kind(Val combiner) {
	if (VAL_TAG(combiner) != T_XT)
		return COMBINE_DISPATCHED;

	cell handler = vocab.dict[(int)VAL_DATA(combiner)];
	if (handler == (cell)p_add_f || handler == (cell)p_add)
		return COMBINE_ADD;
	if (handler == (cell)p_sub_f || handler == (cell)p_sub)
		return COMBINE_SUB;
	if (handler == (cell)p_mul_f || handler == (cell)p_mul)
		return COMBINE_MUL;
	if (handler == (cell)p_div_f || handler == (cell)p_div)
		return COMBINE_DIV;

	return COMBINE_DISPATCHED;
}

static double combine_floats(CombineKind kind, double accumulator, double term) {
	switch (kind) {
	case COMBINE_ADD: return accumulator + term;
	case COMBINE_SUB: return accumulator - term;
	case COMBINE_MUL: return accumulator * term;
	case COMBINE_DIV: return accumulator / term;
	case COMBINE_DISPATCHED: break;
	}

	return term;
}

void p_fold_times(DISPATCH_ARGS) {
	POP_INT(n, "fold-times", "count");
	POP_CALLABLE(combiner, "fold-times");
	POP_CALLABLE(mapper, "fold-times");
	POP(init_val);
	if (n < 0) {
		fail(interp, "length must be non-negative; got %d", n);
		return;
	}

	CombineKind kind = combine_kind(combiner_val);
	if (kind != COMBINE_DISPATCHED && VAL_TAG(init_val) != T_FLOAT)
		kind = COMBINE_DISPATCHED;

	Val accumulator = init_val;
	CallContext context;
	call_open_callable(interp, mapper_val, &context);
	if (interp->dsp + 2 > DATA_STACK_DEPTH) {
		call_close(interp, &context);
		fail(interp, "stack overflow");
		return;
	}
	if (kind != COMBINE_DISPATCHED) {
		double total = VAL_NUMBER(init_val);
		if (context.fast) {
			for (int i = 0; i < n && !interp->error_flag; i++) {
				interp->data_stack[interp->dsp++] = make_float((double)i);

				if (context.reuses_locals)
					interp->loop_local_refill = 1;
				call_invoke(interp);
				if (interp->error_flag) break;

				total = combine_floats(kind, total, interp->data_stack[--interp->dsp].number);
			}
		} else {
			for (int i = 0; i < n && !interp->error_flag; i++) {
				interp->data_stack[interp->dsp++] = make_float((double)i);

				execute_cfa(interp, mapper);
				if (interp->error_flag) break;

				total = combine_floats(kind, total, interp->data_stack[--interp->dsp].number);
			}
		}
		accumulator = make_float(total);
	} else {
		for (int i = 0; i < n && !interp->error_flag; i++) {
			interp->data_stack[interp->dsp++] = make_float((double)i);

			if (context.reuses_locals)
				interp->loop_local_refill = 1;
			call_step(interp, &context, mapper);
			if (interp->error_flag) break;

			Val term = interp->data_stack[--interp->dsp];
			interp->data_stack[interp->dsp++] = accumulator;
			interp->data_stack[interp->dsp++] = term;
			execute_xt(interp, combiner);
			if (interp->error_flag) break;

			accumulator = interp->data_stack[--interp->dsp];
		}
	}
	call_close(interp, &context);
	if (interp->error_flag) return;

	push(interp, accumulator);

	DISPATCH(interp);
}

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
	Val function;
	int function_cfa;
	Object *domain;
	Object *image;
} PmapContext;


int cpu_count(void) {
	long n_cores = sysconf(_SC_NPROCESSORS_ONLN);
	return n_cores > 0 ? (int)n_cores : 1;
}

int worker_pool_count(void) {
	int n_workers = 0;
	for (int i = 0; i < MAX_WORKER_THREADS; i++)
		if (worker_pool[i])
			n_workers++;
	return n_workers;
}

void p_num_cores(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	*chain_sp = make_float(cpu_count());

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

static void pmap_kernel(int start_index, int end_index, void *context) {
	PmapContext *mapping = context;

	if (!worker_interp)
		worker_interp = claim_worker();

	for (int i = start_index; i < end_index; i++) {
		int dsp_before = worker_interp->dsp;
		push(worker_interp, mapping->domain->items[i]);
		push_curried_bindings(worker_interp, mapping->function);
		execute_cfa(worker_interp, mapping->function_cfa);
		if (worker_interp->error_flag) {
			parallel_error = 1;
			return;
		}
		if (worker_interp->dsp != dsp_before + 1) {
			fail(worker_interp, "pmap: quotation must leave exactly one value per element, but changed the stack by %d",
					worker_interp->dsp - dsp_before);
			parallel_error = 1;
			return;
		}
		mapping->image->items[i] = worker_interp->data_stack[worker_interp->dsp - 1];
	}
}

static int references_region_depth(Val value, ParallelRegion *snapshot, int depth) {
	if (depth > MAX_NESTING_DEPTH)
		return 1;
	switch (VAL_TAG(value)) {
		case T_STRING:
		case T_MATRIX:
		case T_SEGMENT:
		case T_EXACT:
			return VAL_DATA(value) >= snapshot->n_objects;
		case T_SET:
		case T_CURRIED:
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
		case T_QUANTITY:
		case T_COMPLEX: {
			int handle = (int)VAL_DATA(value);
			if (handle >= snapshot->n_pairs)
				return 1;
			return references_region_depth(pairs.table[handle].head, snapshot, depth + 1);
		}
		case T_LOGIC_VAR:
		case T_REST:
			return 1;
		default:
			return 0;
	}
}

static int references_region(Val value, ParallelRegion *snapshot) {
	return references_region_depth(value, snapshot, 0);
}

static int parallel_apply(Object *domain, int worker_count,
		int items_per_claim, void (*kernel)(int, int, void *), void *context,
		ParallelRegion *region) {
	region_begin(region, domain->len, worker_count);

	worker_claim = 0;
	worker_interp = NULL;
	parallel_error = 0;
	parallel_for(domain->len, worker_count, items_per_claim, kernel, context);
	in_parallel = 0;

	return parallel_error;
}

static const char *parallel_worker_error(void) {
	int claimed = atomic_load(&worker_claim);
	if (claimed > MAX_WORKER_THREADS)
		claimed = MAX_WORKER_THREADS;
	for (int i = 0; i < claimed; i++)
		if (worker_pool[i] && worker_pool[i]->error_flag && worker_pool[i]->error_message[0])
			return worker_pool[i]->error_message;
	return NULL;
}

typedef struct {
	Val predicate;
	int predicate_cfa;
	Object *domain;
	char *keep;
} PfilterContext;

static void pfilter_kernel(int start_index, int end_index, void *context) {
	PfilterContext *filter = context;

	if (!worker_interp)
		worker_interp = claim_worker();

	for (int i = start_index; i < end_index; i++) {
		int dsp_before = worker_interp->dsp;
		push(worker_interp, filter->domain->items[i]);
		push_curried_bindings(worker_interp, filter->predicate);
		execute_cfa(worker_interp, filter->predicate_cfa);
		if (worker_interp->error_flag) {
			parallel_error = 1;
			return;
		}
		if (worker_interp->dsp != dsp_before + 1) {
			fail(worker_interp, "pfilter: predicate must leave exactly one value per element, but changed the stack by %d",
					worker_interp->dsp - dsp_before);
			parallel_error = 1;
			return;
		}
		filter->keep[i] = truthy(pop(worker_interp));
	}
}

void p_pfilter(DISPATCH_ARGS) {
	if (in_parallel) {
		fail(interp, "cannot run inside a parallel worker; use serial map/filter/reduce");
		return;
	}
	POP_CALLABLE(predicate, "pfilter-ext");
	POP_INT(items_per_claim, "pfilter-ext", "items per claim");
	POP_INT(worker_count, "pfilter", "worker count");
	PEEK_SEQUENCE_AT(domain_val, 0, "pfilter-ext");

	Object *domain = OBJECT_AT(VAL_DATA(domain_val));
	int domain_index = interp->dsp - 1;

	char *keep;
	CALLOC_OR_FAIL(interp, keep, (size_t)MAX(domain->len, 1), 1);

	PfilterContext filter = { .predicate = predicate_val, .predicate_cfa = predicate,
		.domain = domain, .keep = keep };
	ParallelRegion region;
	if (parallel_apply(domain, worker_count, items_per_claim, pfilter_kernel, &filter, &region)) {
		free(keep);
		region_abort(&region);
		const char *worker_message = parallel_worker_error();
		fail(interp, "%s", worker_message ? worker_message
				: "a worker predicate failed (faulted or allocated past the parallel headroom)");
		return;
	}

	region_abort(&region);

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

#ifdef GC_DEBUG
static int val_refs_young(Val v, int object_base, int pair_base) {
	Tag t = VAL_TAG(v);
	if (t == T_QUANTITY || t == T_COMPLEX)
		return (int)VAL_DATA(v) >= pair_base;
	if (t == T_STRING || t == T_SET || t == T_ARRAY || t == T_CURRIED || t == T_FRAME ||
			t == T_MATRIX || t == T_SEGMENT || t == T_CONT)
		return (int)VAL_DATA(v) >= object_base;
	return 0;
}

static void debug_check_no_old_to_young(int object_base, int pair_base, int image_handle) {
	for (int h = 0; h < object_base; h++) {
		if (h == image_handle)
			continue;
		Object *obj = arena.objects[h];
		if (!obj)
			continue;
		Val *vals = NULL;
		int n = 0;
		switch (obj->kind) {
			case OBJECT_SET:
			case OBJECT_ARRAY:
				vals = obj->items;
				n = obj->len;
				break;
			case OBJECT_FRAME:
				vals = obj->frame.values;
				n = obj->len;
				break;
			case OBJECT_CONTINUATION:
				vals = obj->continuation.return_slice;
				n = obj->continuation.return_len;
				break;
			default:
				break;
		}
		for (int i = 0; i < n; i++)
			GC_ASSERT(!val_refs_young(vals[i], object_base, pair_base), "old object references a young object (worker mutated shared state)");
	}
	for (int s = 0; s < pair_base; s++) {
		GC_ASSERT(!val_refs_young(pairs.table[s].head, object_base, pair_base), "old pair references a young object");
		GC_ASSERT(!val_refs_young(pairs.table[s].tail, object_base, pair_base), "old pair references a young object");
	}
}
#endif

void p_pmap(DISPATCH_ARGS) {
	if (in_parallel) {
		fail(interp, "cannot run inside a parallel worker; use serial map/filter/reduce");
		return;
	}
	POP_CALLABLE(function, "pmap-ext");
	POP_INT(items_per_claim, "pmap-ext", "items per claim");
	POP_INT(worker_count, "pmap", "worker count");
	PEEK_SEQUENCE_AT(domain_val, 0, "pmap-ext");

	Object *domain = OBJECT_AT(VAL_DATA(domain_val));
	int domain_index = interp->dsp - 1;

	NEW_ARRAY(image_handle, image, domain->len);
	memset(image->items, 0, sizeof(Val) * (size_t)MAX(domain->len, 1));
	gc_root_push(interp, make_array(image_handle));

	PmapContext mapping = { .function = function_val, .function_cfa = function,
		.domain = domain, .image = image };
	ParallelRegion region;
	int failed = parallel_apply(domain, worker_count, items_per_claim, pmap_kernel, &mapping, &region);

	gc_root_pop(interp);

	if (failed) {
		region_abort(&region);
		const char *worker_message = parallel_worker_error();
		fail(interp, "%s", worker_message ? worker_message
				: "a worker quotation failed (faulted or allocated past the parallel headroom)");
		return;
	}

#ifdef GC_DEBUG
	if (parallel_region_collected)
		debug_check_no_old_to_young(region.n_objects, region.n_pairs, image_handle);
#endif

	int rewindable = 1;
	for (int i = 0; i < domain->len; i++)
		if (references_region(image->items[i], &region)) {
			rewindable = 0;
			break;
		}
	if (rewindable)
		region_abort(&region);
	else
		region_commit(&region);

	interp->dsp = domain_index;
	push(interp, make_array(image_handle));

	DISPATCH(interp);
}

typedef struct {
	Val map_function;
	Val combine_function;
	int map_function_cfa;
	int combine_function_cfa;
	Object *domain;
	Object *partials;
} PmapReduceContext;

static void pmap_reduce_kernel(int start_index, int end_index, void *context) {
	PmapReduceContext *reduction = context;

	if (!worker_interp)
		worker_interp = claim_worker();

	Val accumulator = reduction->partials->items[worker_slot];
	for (int i = start_index; i < end_index; i++) {
		int dsp_before = worker_interp->dsp;
		push(worker_interp, accumulator);
		push(worker_interp, reduction->domain->items[i]);

		push_curried_bindings(worker_interp, reduction->map_function);
		execute_cfa(worker_interp, reduction->map_function_cfa);
		push_curried_bindings(worker_interp, reduction->combine_function);
		execute_cfa(worker_interp, reduction->combine_function_cfa);
		if (worker_interp->error_flag) {
			parallel_error = 1;
			return;
		}
		if (worker_interp->dsp != dsp_before + 1) {
			fail(worker_interp, "pmap-reduce: map and combine must leave exactly one value per element, but changed the stack by %d",
					worker_interp->dsp - dsp_before);
			parallel_error = 1;
			return;
		}

		accumulator = pop(worker_interp);
	}

	reduction->partials->items[worker_slot] = accumulator;
}

void p_pmap_reduce(DISPATCH_ARGS) {
	if (in_parallel) {
		fail(interp, "cannot run inside a parallel worker; use serial map/filter/reduce");
		return;
	}
	POP_CALLABLE(combine_function, "pmap-reduce-ext");
	POP_CALLABLE(map_function, "pmap-reduce-ext");
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
		.map_function = map_function_val,
		.combine_function = combine_function_val,
		.map_function_cfa = map_function,
		.combine_function_cfa = combine_function,
		.domain = domain,
		.partials = partials,
	};
	ParallelRegion region;

	if (parallel_apply(domain, worker_count, items_per_claim, pmap_reduce_kernel, &reduction, &region)) {
		gc_root_pop(interp);
		gc_root_pop(interp);
		region_abort(&region);
		const char *worker_message = parallel_worker_error();
		fail(interp, "%s", worker_message ? worker_message
				: "a worker quotation failed (faulted or allocated past the parallel headroom)");
		return;
	}

	gc_root_push(interp, combine_function_val);

	push(interp, identity);
	for (int i = 0; i < worker_count; i++) {
		push(interp, partials->items[i]);
		push_curried_bindings(interp, combine_function_val);
		if (!interp->error_flag)
			execute_xt(interp, combine_function);
		if (interp->error_flag) {
			gc_root_pop(interp);
			gc_root_pop(interp);
			gc_root_pop(interp);
			return;
		}
	}

	Val result = pop(interp);

	gc_root_pop(interp);
	gc_root_pop(interp);
	gc_root_pop(interp);

	if (!references_region(result, &region))
		region_abort(&region);
	else
		region_commit(&region);

	interp->dsp = domain_index;
	push(interp, result);

	DISPATCH(interp);
}

