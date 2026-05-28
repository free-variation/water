#include "logicforth.h"

void p_map(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(xt);
	if (interp->dsp < 1) {
		fail(interp, "map: expected a set/array and a quotation, but the stack is too shallow");
		return;
	}
	int source_index = interp->dsp - 1;
	Val source_value = interp->data_stack[source_index];
	if (xt.tag != T_XT || (source_value.tag != T_SET && source_value.tag != T_ARRAY)) {
		fail(interp, "map: expected a set/array and a quotation, got %s and %s",
				tag_name(source_value.tag), tag_name(xt.tag));
		return;
	}

	Object *source = interp->objects[source_value.data];

	NEW_ARRAY(result_handle, result, source->len);
	memset(result->items, 0, sizeof(Val) * (size_t)MAX(source->len, 1));
	gc_root_push(interp, make_array(result_handle));

	for (int i = 0; i < source->len && !interp->error_flag; i++) {
		int dsp_before = interp->dsp;
		push(interp, source->items[i]);
		execute_cfa(interp, (int)xt.data);
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
	POP(xt);
	if (xt.tag != T_XT) {
		fail(interp, "mapn: expected a quotation, got %s", tag_name(xt.tag));
		return;
	}
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
		execute_cfa(interp, (int)xt.data);
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

	POP(xt);
	if (interp->dsp < 1) {
		fail(interp, "filter: expected a set/array and a quotation, but the stack is too shallow");
		return;
	}
	int source_index = interp->dsp - 1;
	Val source_value = interp->data_stack[source_index];
	if (xt.tag != T_XT || (source_value.tag != T_SET && source_value.tag != T_ARRAY)) {
		fail(interp, "filter: expected a set/array and a quotation, got %s and %s",
				tag_name(source_value.tag), tag_name(xt.tag));
		return;
	}

	Object *source = interp->objects[source_value.data];

	int *keep = malloc((size_t)MAX(source->len, 1) * sizeof(int));
	int n_kept = 0;
	for (int i = 0; i < source->len && !interp->error_flag; i++) {
		int dsp_before = interp->dsp;
		push(interp, source->items[i]);
		execute_cfa(interp, (int)xt.data);
		if (interp->error_flag) break;
		if (interp->dsp != dsp_before + 1) {
			fail(interp, "filter: predicate must leave exactly one value per element, but changed the stack by %d", interp->dsp - dsp_before);
			break;
		}
		keep[i] = truthy(pop(interp));
		n_kept += keep[i];
	}

	if (interp->error_flag) { free(keep); return; }

	int result_handle = object_new_array(interp, n_kept);
	if (interp->error_flag) { free(keep); return; }
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




