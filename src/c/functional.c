#include "logicforth.h"

void p_map(Interpreter *interp) {
	POP_XT(xt, "map");
	PEEK_SEQUENCE_AT(source_val, 0, "map");
	Object *source = interp->objects[VAL_DATA(source_val)];
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
		Object *source = interp->objects[VAL_DATA(interp->data_stack[first_source + i])];
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
			Object *source = interp->objects[VAL_DATA(interp->data_stack[first_source + source_index])];
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
	Object *source = interp->objects[VAL_DATA(source_val)];
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
	Object *result = interp->objects[result_handle];

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
	Object *source = interp->objects[VAL_DATA(source_val)];

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
