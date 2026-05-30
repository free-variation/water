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

	if (set->len >= set->capacity) {
		set->capacity *= 2;
		set->items = realloc(set->items, sizeof(Val) * (size_t)set->capacity);
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
		fail(interp, "> : no matching < on the stack");
		return;
	}

	int set_handle = object_new_set(interp);
	for (int i = mark_index; i < interp->dsp; i++) {
		set_add(interp, set_handle, interp->data_stack[i]);
	}
	interp->dsp = mark_index - 1;
	push(interp, make_set(set_handle));
}

void p_frameopen(Interpreter *interp, cell *cfa) {
	(void)cfa;

	push(interp, make_mark());
}

void p_frameclose(Interpreter *interp, cell *cfa) {
	(void)cfa;

	int mark_index = interp->dsp;
	while (mark_index > 0 && interp->data_stack[mark_index - 1].tag != T_MARK) mark_index--;
	if (mark_index == 0) {
		fail(interp, "} : no matching { on the stack");
		return;
	}

	int count = interp->dsp - mark_index;
	if (count % 2 != 0) {
		fail(interp, "} : frame needs key/value pairs");
		return;
	}

	NEW_FRAME(frame_handle, frame);
	for (int i = mark_index; i < interp->dsp; i += 2) {
		if (interp->data_stack[i].tag != T_SYMBOL) {
			fail(interp, "} : frame keys must be symbols, got %s", tag_name(interp->data_stack[i].tag));
			return;
		}
		frame_put(frame, interp->data_stack[i].data, interp->data_stack[i + 1]);
	}

	interp->dsp = mark_index - 1;
	push(interp, make_frame(frame_handle));
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

void p_size(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(collection);
	if (collection.tag == T_SET ||
			collection.tag == T_ARRAY ||
			collection.tag == T_STRING ||
			collection.tag == T_FRAME)
		push(interp, make_float((double)interp->objects[collection.data]->len));
	else fail(interp, "size: expected a set, array, string, or frame; got %s", tag_name(collection.tag));
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

	PEEK_COLLECTION_AT(source_val, 0, "take");
	Object *source = interp->objects[source_val.data];
	if (n_items > source->len) n_items = source->len;

	NEW_ARRAY(result_handle, result, n_items);

	for (int i = 0; i < n_items; i++)
		result->items[i] = source->items[i];

	replace_top_with_array(interp, result_handle);
}

void p_reverse(Interpreter *interp, cell *cfa) {
	(void)cfa;

	PEEK_COLLECTION_AT(source_val, 0, "reverse");
	Object *source = interp->objects[source_val.data];

	NEW_ARRAY(result_handle, result, source->len);

	for (int i = 0; i < source->len; i++)
		result->items[source->len - i - 1] = source->items[i];

	replace_top_with_array(interp, result_handle);
}

void p_concat(Interpreter *interp, cell *cfa) {
	(void)cfa;
	int i;

	PEEK_COLLECTION_AT(b_val, 0, "concat");
	PEEK_COLLECTION_AT(a_val, 1, "concat");
	Object *b = interp->objects[b_val.data];
	Object *a = interp->objects[a_val.data];

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

int frame_find(Object *frame, cell key) {
	int low = 0;
	int high = frame->len;
	int mid;
	cell mid_key;

	while (low < high) {
		mid = (low + high) / 2;
		mid_key = frame->frame.keys[mid];
		if (mid_key < key)
			low = mid + 1;
		else 
			high = mid;
	}

	return low;
}

#define FRAME_LOOKUP(obj, key, at, present) \
	int at = frame_find((obj), (key)); \
	int present = (at) < (obj)->len && (obj)->frame.keys[at] == (key)

void frame_put(Object *frame, cell key, Val value) {
	FRAME_LOOKUP(frame, key, at, present);
	if (present) {
		frame->frame.values[at] = value;
		return;
	}

	if (frame->len >= frame->capacity) {
		frame->capacity *= 2;
		frame->frame.keys = realloc(frame->frame.keys, sizeof(cell) * (size_t)frame->capacity);
		frame->frame.values = realloc(frame->frame.values, sizeof(Val) * (size_t)frame->capacity);
	}

	memmove(&frame->frame.keys[at + 1], &frame->frame.keys[at], sizeof(cell) * (size_t)(frame->len - at));
	memmove(&frame->frame.values[at + 1], &frame->frame.values[at], sizeof(Val) * (size_t)(frame->len - at));
	frame->frame.keys[at] = key;
	frame->frame.values[at] = value;
	frame->len++;
}

int frame_delete(Object *frame, cell key) {
	FRAME_LOOKUP(frame, key, at, present);
	if (!present) return 0;
	memmove(&frame->frame.keys[at], &frame->frame.keys[at + 1], sizeof(cell) * (size_t)(frame->len - at - 1));
	memmove(&frame->frame.values[at], &frame->frame.values[at + 1], sizeof(Val) * (size_t)(frame->len - at - 1));
	frame->len--;
	return 1;
}


void p_to_frame(Interpreter *interp, cell *cfa) {
	(void)cfa;

	PEEK_COLLECTION_AT(source_val, 0, ">frame");
	if (source_val.tag != T_ARRAY) {
		fail(interp, ">frame: expected an array, got %s", tag_name(source_val.tag));
		return;
	}
	Object *source = interp->objects[source_val.data];
	if (source->len % 2 != 0) {
		fail(interp, ">frame: array needs an even number of kv pairs");
		return;
	}

	NEW_FRAME(frame_handle, frame);
	for (int i = 0; i < source->len; i += 2) {
		if (source->items[i].tag != T_SYMBOL) {
			fail(interp, ">frame: keys must be symbols, got %s", tag_name(source->items[i].tag));
			return;
		}
		frame_put(frame, source->items[i].data, source->items[i + 1]);
	}

	interp->dsp--;
	push(interp, make_frame(frame_handle));
}

static Object *frame_path(Interpreter *interp, Val path_val, const char *op) {
	if (path_val.tag != T_ARRAY) {
		fail(interp, "%s : expected a path (array of symbols), got %s", op, tag_name(path_val.tag));
		return NULL;
	}
	
	Object *path = interp->objects[path_val.data];
	for (int i = 0; i < path->len; i++) {
		if (path->items[i].tag != T_SYMBOL) {
			fail(interp, "%s: path elements must be symbols, got %s", op, tag_name(path->items[i].tag));
			return NULL;
		}
	}
	return path;
}

typedef enum { WALK_ERROR, WALK_VIVIFY, WALK_PROBE } FrameWalkMode;

static Val frame_walk(Interpreter *interp, Val node, Object *path,
		int count, FrameWalkMode mode, int *found, const char *op) {
	for (int i = 0; i < count; i++) {
		if (node.tag != T_FRAME) {
			if (found) *found = 0;
			if (mode != WALK_PROBE)
				fail(interp, "%s : cannot descend into %s", op, tag_name(node.tag));
			return node;
		}

		cell key = path->items[i].data;
		Object *frame = interp->objects[node.data];
		FRAME_LOOKUP(frame, key, at, present);
		if (present && (mode != WALK_VIVIFY || frame->frame.values[at].tag == T_FRAME)) {
			node = frame->frame.values[at];
		} else if (mode == WALK_VIVIFY) {
			int child = object_new_frame(interp);
			frame_put(interp->objects[node.data], key, make_frame(child));
			node = make_frame(child);
		} else {
			if (found) *found = 0;
			if (mode != WALK_PROBE)
				fail(interp, "%s : no key :%s", op, &interp->vocab->symbol_pool[key]);
			return node;
		}
	}
	if (found) *found = 1;
	return node;
}

#define PEEK_FRAME_PATH(frame, path, op) \
	PEEK_AT(frame, 1, op, T_FRAME); \
	Object *path = frame_path(interp, interp->data_stack[interp->dsp - 1], op); \
	if (!path) return

#define PEEK_FRAME_PATH_VALUE(frame, path, value, op) \
	PEEK_AT(frame, 2, op, T_FRAME); \
	Object *path = frame_path(interp, interp->data_stack[interp->dsp - 2], op); \
	if (!path) return; \
	Val value = interp->data_stack[interp->dsp - 1]

#define REQUIRE_NONEMPTY_PATH(path, op) \
	if ((path)->len == 0) { \
		fail(interp, "%s : empty path", (op)); \
		return; \
	}

void p_frame_get(Interpreter *interp, cell *cfa) {
	(void)cfa;

	PEEK_FRAME_PATH(frame, path, "@");

	Val result = frame_walk(interp, frame, path, path->len, WALK_ERROR, NULL, "@");
	if (interp->error_flag) return;

	interp->dsp -= 2;
	push(interp, result);
}

void p_frame_set(Interpreter *interp, cell *cfa) {
	(void)cfa;

	PEEK_FRAME_PATH_VALUE(frame, path, value, "!");
	REQUIRE_NONEMPTY_PATH(path, "!");

	Val parent = frame_walk(interp, frame, path, path->len - 1, WALK_VIVIFY, NULL, "!");
	if (interp->error_flag) return;

	frame_put(interp->objects[parent.data], path->items[path->len - 1].data, value);
	interp->dsp -= 2;
}

void p_frame_delete_at(Interpreter *interp, cell *cfa) {
	(void)cfa;

	PEEK_FRAME_PATH(frame, path, "delete-at");
	REQUIRE_NONEMPTY_PATH(path, "delete-at");

	Val parent = frame_walk(interp, frame, path, path->len - 1, WALK_ERROR, NULL, "delete-at");
	if (interp->error_flag) return;

	cell leaf = path->items[path->len - 1].data;
	if (parent.tag != T_FRAME || !frame_delete(interp->objects[parent.data], leaf)) {
		fail(interp, "delete-at : no key :%s", &interp->vocab->symbol_pool[leaf]);
		return;
	}
	interp->dsp--;
}

void p_frame_keys(Interpreter *interp, cell *cfa) {
	(void)cfa;

	PEEK_AT(frame_val, 0, "keys", T_FRAME);
	Object *frame = interp->objects[frame_val.data];
	NEW_ARRAY(result_handle, result, frame->len);

	for (int i = 0; i < frame->len; i++)
		result->items[i] = make_symbol((int)frame->frame.keys[i]);

	replace_top_with_array(interp, result_handle);
}

void p_frame_values(Interpreter *interp, cell *cfa) {
	(void)cfa;

	PEEK_AT(frame_val, 0, "values", T_FRAME);
	Object *frame = interp->objects[frame_val.data];
	NEW_ARRAY(result_handle, result, frame->len);

	for (int i = 0; i < frame->len; i++)
		result->items[i] = frame->frame.values[i];

	replace_top_with_array(interp, result_handle);
}

void p_merge(Interpreter *interp, cell *cfa) {
	(void)cfa;

	PEEK_AT(right, 0, "merge", T_FRAME);
	PEEK_AT(left, 1, "merge", T_FRAME);
	Object *right_frame = interp->objects[right.data];
	Object *left_frame = interp->objects[left.data];

	NEW_FRAME(result_handle, result);
	for (int i = 0; i < left_frame->len; i++)
		frame_put(result, left_frame->frame.keys[i], left_frame->frame.values[i]);
	for (int i = 0; i < right_frame->len; i++)
		frame_put(result, right_frame->frame.keys[i], right_frame->frame.values[i]);

	interp->dsp -= 2;
	push(interp, make_frame(result_handle));
}

void p_frame(Interpreter *interp, cell *cfa) {
	(void)cfa;

	PEEK_AT(values_val, 0, "frame", T_ARRAY);
	PEEK_COLLECTION_AT(keys_val, 1, "frame");
	Object *values = interp->objects[values_val.data];
	Object *keys = interp->objects[keys_val.data];
	if (keys->len != values->len) {
		fail(interp, "frame: keys and values must be the same length (%d vs %d)", keys->len, values->len);
		return;
	}
	for (int i = 0; i < keys->len; i++) {
		if (keys->items[i].tag != T_SYMBOL) {
			fail(interp, "frame: keys must be symbols, got %s", tag_name(keys->items[i].tag));
			return;
		}
	}

	NEW_FRAME(frame_handle, frame);
	for (int i = 0; i < keys->len; i++)
		frame_put(frame, keys->items[i].data, values->items[i]);

	interp->dsp -= 2;
	push(interp, make_frame(frame_handle));
}

void p_has(Interpreter *interp, cell *cfa) {
	(void)cfa;

	PEEK_FRAME_PATH(frame, path, "has?");
	REQUIRE_NONEMPTY_PATH(path, "has?");
	int found;

	frame_walk(interp, frame, path, path->len, WALK_PROBE, &found, "has?");

	interp->dsp -= 2;
	push(interp, make_bool(found));
}

void p_update_at(Interpreter *interp, cell *cfa) {
	(void)cfa;

	PEEK_FRAME_PATH_VALUE(frame, path, xt, "update-at");
	REQUIRE_NONEMPTY_PATH(path, "update-at");
	if (xt.tag != T_XT) {
		fail(interp, "update-at: xt required on stack, got %s", tag_name(xt.tag));
		return;
	}

	Val parent = frame_walk(interp, frame, path, path->len - 1, WALK_ERROR, NULL, "update-at");
	if (interp->error_flag) return;
	if (parent.tag != T_FRAME) {
		fail(interp, "update-at: parent is not a frame, got %s", tag_name(parent.tag));
		return;
	}

	Object *parent_obj = interp->objects[parent.data];
	cell leaf = path->items[path->len - 1].data;
	FRAME_LOOKUP(parent_obj, leaf, at, present);
	if (!present) {
		fail(interp, "update-at: no key :%s", &interp->vocab->symbol_pool[leaf]);
		return;
	}

	push(interp, parent_obj->frame.values[at]);
	execute_cfa(interp, (int)xt.data);
	parent_obj->frame.values[at] = pop(interp);

	interp->dsp -= 2;
}
	

