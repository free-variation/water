#include "logicforth.h"

void p_map(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP_XT(xt, "map");
	PEEK_COLLECTION_AT(source_val, 0, "map");
	Object *source = interp->objects[source_val.data];
	int source_index = interp->dsp - 1;

	NEW_ARRAY(result_handle, result, source->len);
	memset(result->items, 0, sizeof(Val) * (size_t)MAX(source->len, 1));
	gc_root_push(interp, make_array(result_handle));

	for (int i = 0; i < source->len && !interp->error_flag; i++) {
		int dsp_before = interp->dsp;
		push(interp, source->items[i]);
		execute_cfa(interp, xt);
		if (interp->error_flag) break;
		if (interp->dsp != dsp_before + 1) {
			fail(interp, "map: quotation must leave exactly one value per element, but changed the stack by %d", interp->dsp - dsp_before);
			break;
		}
		result->items[i] = pop(interp);
	}

	gc_root_pop(interp);

	if (!interp->error_flag) {
		interp->dsp = source_index;
		push(interp, make_array(result_handle));
	}
}

void p_mapn(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP_INT(arity, "mapn", "arity");
	if (arity < 1) {
		fail(interp, "mapn: arity must be >= 1, got %d", arity);
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
		if (interp->data_stack[first_source + i].tag != T_ARRAY) {
			fail(interp, "mapn: source %d is %s, expected an array", i, tag_name(interp->data_stack[first_source + i].tag));
			return;
		}
		Object *source = interp->objects[interp->data_stack[first_source + i].data];
		if (row_count < 0) row_count = source->len;
		else if (source->len != row_count) {
			fail(interp, "mapn: source arrays differ in length (%d vs %d)", source->len, row_count);
			return;
		}
	}

	NEW_ARRAY(result_handle, result, row_count);
	memset(result->items, 0, sizeof(Val) * (size_t)MAX(row_count, 1));

	gc_root_push(interp, make_array(result_handle));

	for (int row = 0; row < row_count && !interp->error_flag; row++) {
		int dsp_before = interp->dsp;
		for (int source_index = 0; source_index < arity; source_index++) {
			Object *source = interp->objects[interp->data_stack[first_source + source_index].data];
			push(interp, source->items[row]);
		}
		execute_cfa(interp, xt);
		if (interp->error_flag) break;
		if (interp->dsp != dsp_before + 1) {
			fail(interp, "mapn: quotation must leave exactly one value per row, but changed the stack by %d", interp->dsp - dsp_before);
			break;
		}
		result->items[row] = pop(interp);
	}

	gc_root_pop(interp);

	if (!interp->error_flag) {
		interp->dsp = first_source;
		push(interp, make_array(result_handle));
	}
}

void p_filter(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP_XT(xt, "filter");
	PEEK_COLLECTION_AT(source_val, 0, "filter");
	Object *source = interp->objects[source_val.data];
	int source_index = interp->dsp - 1;

	int *keep = malloc((size_t)MAX(source->len, 1) * sizeof(int));
	int n_kept = 0;
	for (int i = 0; i < source->len && !interp->error_flag; i++) {
		int dsp_before = interp->dsp;
		push(interp, source->items[i]);
		execute_cfa(interp, xt);
		if (interp->error_flag) break;
		if (interp->dsp != dsp_before + 1) {
			fail(interp, "filter: predicate must leave exactly one value per element, but changed the stack by %d", interp->dsp - dsp_before);
			break;
		}
		keep[i] = truthy(pop(interp));
		n_kept += keep[i];
	}

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
}

void p_reduce(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP_XT(combiner, "reduce");
	POP(init_val);
	PEEK_COLLECTION_AT(source_val, 0, "reduce");
	Object *source = interp->objects[source_val.data];

	Val result_val = init_val;
	for (int i = 0; i < source->len; i++) {
		push(interp, result_val);
		push(interp, source->items[i]);

		execute_cfa(interp, combiner);

		result_val = pop(interp);
		if (interp->error_flag) return;
	}

	pop(interp);
	push(interp, result_val);
}



