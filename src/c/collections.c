#include "logicforth.h"

void set_add(Interpreter *interp, int set_handle, Val value) {
	Object *set = interp->objects[set_handle];

	int low = 0, high = set->len;
	while (low < high) {
		int mid = (low + high) / 2;
		int cmp = val_cmp(interp, set->items[mid], value);

		if (cmp == 0) return;
		if (cmp < 0) low = mid + 1;
		else high = mid;
	}

	if (set->len >= set->cap) {
		set->cap *= 2;
		set->items = realloc(set->items, sizeof(Val) * (size_t)set->cap);
	}

	memmove(&set->items[low + 1], &set->items[low],
			sizeof(Val) * (size_t)(set->len - low));
	set->items[low] = value;
	set->len++;
}

int set_member(Interpreter *interp, int set_handle, Val value) {
	Object *set = interp->objects[set_handle];

	int low = 0, high = set->len;
	while (low < high) {
		int mid = (low + high) / 2;
		int cmp = val_cmp(interp, set->items[mid], value);
		if (cmp == 0) return 1;
		if (cmp < 0) low = mid + 1;
		else high = mid;
	}
	return 0;
}

int set_union(Interpreter *interp, int handle_a, int handle_b) {
	int union_handle = object_new_set(interp);
	Object *set_a = interp->objects[handle_a];
	Object *set_b = interp->objects[handle_b];

	for (int i = 0; i < set_a->len; i++) set_add(interp, union_handle, set_a->items[i]);
	for (int i = 0; i < set_b->len; i++) set_add(interp, union_handle, set_b->items[i]);

	return union_handle;
}

int set_intersect(Interpreter *interp, int handle_a, int handle_b) {
	int intersection_handle = object_new_set(interp);
	Object *set_a = interp->objects[handle_a];

	for (int i = 0; i < set_a->len; i++)
		if (set_member(interp, handle_b, set_a->items[i]))
			set_add(interp, intersection_handle, set_a->items[i]);

	return intersection_handle;
}

int set_difference(Interpreter *interp, int handle_a, int handle_b) {
	int difference_handle = object_new_set(interp);
	Object *set_a = interp->objects[handle_a];

	for (int i = 0; i < set_a->len; i++)
		if (!set_member(interp, handle_b, set_a->items[i]))
			set_add(interp, difference_handle, set_a->items[i]);

	return difference_handle;
}

void p_setopen(Interpreter *interp, cell *cfa) {
	(void)cfa;

	push(interp, make_mark());
}

void p_setclose(Interpreter *interp, cell *cfa) {
	(void)cfa;

	int mark_index = interp->dsp;
	while (mark_index > 0 && interp->data_stack[mark_index - 1].tag != T_MARK) mark_index--;
	if (mark_index == 0) {
		type_error(interp, "}");
		return;
	}

	int set_handle = object_new_set(interp);
	for (int i = mark_index; i < interp->dsp; i++) {
		set_add(interp, set_handle, interp->data_stack[i]);
	}
	interp->dsp = mark_index - 1;
	push(interp, make_set(set_handle));
}

void p_array_open(Interpreter *interp, cell *cfa) {
	(void)cfa;

	push(interp, make_mark());
}

void p_array_close(Interpreter *interp, cell *cfa) {
	(void)cfa;

	int mark_index = interp->dsp;
	while (mark_index > 0 && interp->data_stack[mark_index - 1].tag != T_MARK) mark_index--;
	if (mark_index == 0) {
		type_error(interp, "]");
		return;
	}
	int num_elements = interp->dsp - mark_index;
	int array_handle = object_new_array(interp, num_elements);
	for (int i = 0; i < num_elements; i++)
		interp->objects[array_handle]->items[i] = interp->data_stack[mark_index + i];
	interp->dsp = mark_index - 1;
	push(interp, make_array(array_handle));
}

void p_array(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(count_value);
	if (count_value.tag != T_FLOAT) {
		type_error(interp, "array");
		return;
	}

	int count = (int)unpack_float(count_value);
	if (count < 0 || count > interp->dsp) {
		type_error(interp, "array");
		return;
	}

	int array_handle = object_new_array(interp, count);
	if (interp->error_flag) return;
	Object *array = interp->objects[array_handle];

	int first_item = interp->dsp - count;
	for (int i = 0; i < count; i++)
		array->items[i] = interp->data_stack[first_item + i];
	interp->dsp = first_item;

	push(interp, make_array(array_handle));
}

void p_cardinality(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(collection);
	if (collection.tag == T_SET || collection.tag == T_ARRAY || collection.tag == T_STRING)
		push(interp, make_float((double)interp->objects[collection.data]->len));
	else type_error(interp, "cardinality");
}

void p_member(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(value);
	POP(set_value);
	if (set_value.tag != T_SET) {
		type_error(interp, "member?");
		return;
	}
	push(interp, make_bool(set_member(interp, (int)set_value.data, value)));
}

void p_union(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);
	if (left.tag != T_SET || right.tag != T_SET) {
		type_error(interp, "union");
		return;
	}
	push(interp, make_set(set_union(interp, (int)left.data, (int)right.data)));
}

void p_intersect(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);
	if (left.tag != T_SET || right.tag != T_SET) {
		type_error(interp, "intersection");
		return;
	}
	push(interp, make_set(set_intersect(interp, (int)left.data, (int)right.data)));
}

void p_difference(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);
	if (left.tag != T_SET || right.tag != T_SET) {
		type_error(interp, "difference");
		return;
	}
	push(interp, make_set(set_difference(interp, (int)left.data, (int)right.data)));
}

void p_map(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(xt);
	POP(source_value);
	if (xt.tag != T_XT || (source_value.tag != T_SET && source_value.tag != T_ARRAY)) {
		type_error(interp, "map");
		return;
	}
	Object *source = interp->objects[source_value.data];
	int source_length = source->len;
	Val *snapshot = malloc(sizeof(Val) * (size_t)MAX(source_length, 1));
	memcpy(snapshot, source->items, sizeof(Val) * (size_t)source_length);
	gc_root_push(interp, source_value);
	int result_handle = object_new_array(interp, source_length);
	if (interp->error_flag) { gc_root_pop(interp); free(snapshot); return; }
	Object *result = interp->objects[result_handle];
	memset(result->items, 0, sizeof(Val) * (size_t)MAX(source_length, 1));
	gc_root_push(interp, make_array(result_handle));
	for (int i = 0; i < source_length && !interp->error_flag; i++) {
		push(interp, snapshot[i]);
		execute_cfa(interp, (int)xt.data);
		if (interp->error_flag) break;
		result->items[i] = pop(interp);
	}
	gc_root_pop(interp);
	gc_root_pop(interp);
	free(snapshot);
	push(interp, make_array(result_handle));
}

void p_mapn(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(arity_value);
	if (arity_value.tag != T_FLOAT) {
		type_error(interp, "mapn");
		return;
	}
	int arity = (int)unpack_float(arity_value);
	if (arity < 1) {
		type_error(interp, "mapn");
		return;
	}
	POP(xt);
	if (xt.tag != T_XT) {
		type_error(interp, "mapn");
		return;
	}
	if (arity > interp->dsp) {
		type_error(interp, "mapn");
		return;
	}

	int first_source = interp->dsp - arity;
	int row_count = -1;
	for (int i = 0; i < arity; i++) {
		if (interp->data_stack[first_source + i].tag != T_ARRAY) {
			type_error(interp, "mapn");
			return;
		}
		Object *source = interp->objects[interp->data_stack[first_source + i].data];
		if (row_count < 0) row_count = source->len;
		else if (source->len != row_count) {
			type_error(interp, "mapn");
			return;
		}
	}

	int result_handle = object_new_array(interp, row_count);
	if (interp->error_flag) return;
	Object *result = interp->objects[result_handle];
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
		if (interp->dsp != dsp_before + 1) { type_error(interp, "mapn"); break; }
		result->items[row] = pop(interp);
	}
	gc_root_pop(interp);

	if (!interp->error_flag) {
		interp->dsp = first_source;
		push(interp, make_array(result_handle));
	}
}

void p_array_of(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(size_val);
	if (size_val.tag != T_FLOAT) {
		type_error(interp, "array length");
		return;
	}

	POP(init_val);

	int array_len = (int)(unpack_float(size_val));
	int array_handle = object_new_array(interp, array_len);
	if (interp->error_flag) return;

	Object *array = interp->objects[array_handle];
	for (int i = 0; i < array_len; i++) {
		array->items[i] = init_val;
	}

	push(interp, make_array(array_handle));
}
