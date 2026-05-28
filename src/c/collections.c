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
		fail(interp, "} : no matching { on the stack");
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
		fail(interp, "] : no matching [ on the stack");
		return;
	}
	int num_elements = interp->dsp - mark_index;
	NEW_ARRAY(array_handle, array, num_elements);
	for (int i = 0; i < num_elements; i++)
		array->items[i] = interp->data_stack[mark_index + i];
	interp->dsp = mark_index - 1;
	push(interp, make_array(array_handle));
}

void p_array(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP_INT(count, "array", "count");
	if (count < 0 || count > interp->dsp) {
		fail(interp, "array: count %d out of range (stack has %d available)", count, interp->dsp);
		return;
	}

	NEW_ARRAY(array_handle, array, count);

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
	else fail(interp, "cardinality: expected a set, array, or string, got %s", tag_name(collection.tag));
}

void p_member(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(value);
	POP(set_value);
	if (set_value.tag != T_SET) {
		fail(interp, "member?: expected a set, got %s", tag_name(set_value.tag));
		return;
	}
	push(interp, make_bool(set_member(interp, (int)set_value.data, value)));
}

void p_union(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);
	if (left.tag != T_SET || right.tag != T_SET) {
		fail(interp, "union: expected two sets, got %s and %s", tag_name(left.tag), tag_name(right.tag));
		return;
	}
	push(interp, make_set(set_union(interp, (int)left.data, (int)right.data)));
}

void p_intersect(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);
	if (left.tag != T_SET || right.tag != T_SET) {
		fail(interp, "intersection: expected two sets, got %s and %s", tag_name(left.tag), tag_name(right.tag));
		return;
	}
	push(interp, make_set(set_intersect(interp, (int)left.data, (int)right.data)));
}

void p_difference(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);
	if (left.tag != T_SET || right.tag != T_SET) {
		fail(interp, "difference: expected two sets, got %s and %s", tag_name(left.tag), tag_name(right.tag));
		return;
	}
	push(interp, make_set(set_difference(interp, (int)left.data, (int)right.data)));
}

void p_array_of(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP_INT(array_len, "array-of", "length");
	POP(init_val);

	NEW_ARRAY(array_handle, array, array_len);
	for (int i = 0; i < array_len; i++) {
		array->items[i] = init_val;
	}

	push(interp, make_array(array_handle));
}

static void replace_top_with_array(Interpreter *interp, int handle) {
	interp->data_stack[interp->dsp - 1] = make_array(handle);
}

void p_take(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP_INT(n_items, "take", "length");
	if (n_items < 0) n_items = 0;

	PEEK_COLLECTION_AT(source, 0, "take");
	if (n_items > source->len) n_items = source->len;

	NEW_ARRAY(result_handle, result, n_items);

	for (int i = 0; i < n_items; i++)
		result->items[i] = source->items[i];

	replace_top_with_array(interp, result_handle);
}

void p_reverse(Interpreter *interp, cell *cfa) {
	(void)cfa;

	PEEK_COLLECTION_AT(source, 0, "reverse");

	NEW_ARRAY(result_handle, result, source->len);

	for (int i = 0; i < source->len; i++)
		result->items[source->len - i - 1] = source->items[i];

	replace_top_with_array(interp, result_handle);
}

void p_concat(Interpreter *interp, cell *cfa) {
	(void)cfa;
	int i;

	PEEK_COLLECTION_AT(b, 0, "concat");
	PEEK_COLLECTION_AT(a, 1, "concat");

	NEW_ARRAY(result_handle, result, a->len + b->len);

	for (i = 0; i < a->len; i++)
		result->items[i] = a->items[i];
	for (i = 0; i < b->len; i++)
		result->items[a->len + i] = b->items[i];

	interp->data_stack[interp->dsp - 2] = make_array(result_handle);
	interp->dsp--;
}

void p_range(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP_INT(range_to, "range", "to");
	POP_INT(range_from, "range", "from");

	int step = range_to >= range_from ? 1 : -1;
	int n_values = (range_to - range_from) * step + 1;
	NEW_ARRAY(result_handle, result, n_values);

	for (int i = 0; i < n_values; i++)
		result->items[i] = make_float(range_from + i * step);

	push(interp, make_array(result_handle));
}
