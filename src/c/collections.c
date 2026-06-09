#include "logicforth.h"

void set_add(Interpreter *interp, int set_handle, Val value) {
	Object *set = interp->objects[set_handle];

	LOWER_BOUND(set->len, mid, val_cmp(interp, set->items[mid], value) < 0, low);
	if (low < set->len && val_cmp(interp, set->items[low], value) == 0) {
		return;
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

	LOWER_BOUND(set->len, mid, val_cmp(interp, set->items[mid], value) < 0, low);
	return low < set->len && val_cmp(interp, set->items[low], value) == 0;
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

void p_setopen(Interpreter *interp) {
	push(interp, make_mark());

	DISPATCH(interp);
}

void p_setclose(Interpreter *interp) {
	int mark_index = interp->dsp;
	while (mark_index > 0 && VAL_TAG(interp->data_stack[mark_index - 1]) != T_MARK) mark_index--;
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

	DISPATCH(interp);
}

void p_frameopen(Interpreter *interp) {
	push(interp, make_mark());

	DISPATCH(interp);
}

void p_frameclose(Interpreter *interp) {
	int mark_index = interp->dsp;
	while (mark_index > 0 && VAL_TAG(interp->data_stack[mark_index - 1]) != T_MARK) mark_index--;
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
		if (VAL_TAG(interp->data_stack[i]) != T_SYMBOL) {
			fail(interp, "} : frame keys must be symbols; got %s", tag_name(VAL_TAG(interp->data_stack[i])));
			return;
		}
		frame_put(frame, VAL_DATA(interp->data_stack[i]), interp->data_stack[i + 1]);
	}

	interp->dsp = mark_index - 1;
	push(interp, make_frame(frame_handle));

	DISPATCH(interp);
}

void p_array_open(Interpreter *interp) {
	push(interp, make_mark());

	DISPATCH(interp);
}

void p_array_close(Interpreter *interp) {
	int mark_index = interp->dsp;
	while (mark_index > 0 && VAL_TAG(interp->data_stack[mark_index - 1]) != T_MARK) mark_index--;
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

	DISPATCH(interp);
}

void p_array(Interpreter *interp) {
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

	DISPATCH(interp);
}

void p_size(Interpreter *interp) {
	POP(collection);
	if (VAL_TAG(collection) == T_SET ||
			VAL_TAG(collection) == T_ARRAY ||
			VAL_TAG(collection) == T_STRING ||
			VAL_TAG(collection) == T_FRAME)
		push(interp, make_float((double)interp->objects[VAL_DATA(collection)]->len));
	else fail(interp, "size: expected a set, array, string, or frame; got %s", tag_name(VAL_TAG(collection)));

	DISPATCH(interp);
}

void p_member(Interpreter *interp) {
	POP(value);
	POP(set_value);
	if (VAL_TAG(set_value) != T_SET) {
		fail(interp, "member?: expected a set; got %s", tag_name(VAL_TAG(set_value)));
		return;
	}
	push(interp, make_bool(set_member(interp, (int)VAL_DATA(set_value), value)));

	DISPATCH(interp);
}

void p_union(Interpreter *interp) {
	POP(right);
	POP(left);
	if (VAL_TAG(left) != T_SET || VAL_TAG(right) != T_SET) {
		fail(interp, "union: expected two sets; got %s and %s", tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));
		return;
	}
	push(interp, make_set(set_union(interp, (int)VAL_DATA(left), (int)VAL_DATA(right))));

	DISPATCH(interp);
}

void p_intersect(Interpreter *interp) {
	POP(right);
	POP(left);
	if (VAL_TAG(left) != T_SET || VAL_TAG(right) != T_SET) {
		fail(interp, "intersection: expected two sets; got %s and %s", tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));
		return;
	}
	push(interp, make_set(set_intersect(interp, (int)VAL_DATA(left), (int)VAL_DATA(right))));

	DISPATCH(interp);
}

void p_difference(Interpreter *interp) {
	POP(right);
	POP(left);
	if (VAL_TAG(left) != T_SET || VAL_TAG(right) != T_SET) {
		fail(interp, "difference: expected two sets; got %s and %s", tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));
		return;
	}
	push(interp, make_set(set_difference(interp, (int)VAL_DATA(left), (int)VAL_DATA(right))));

	DISPATCH(interp);
}

void p_array_of(Interpreter *interp) {
	POP_INT(array_len, "array-of", "length");
	POP(init_val);

	NEW_ARRAY(array_handle, array, array_len);
	for (int i = 0; i < array_len; i++) {
		array->items[i] = init_val;
	}

	push(interp, make_array(array_handle));

	DISPATCH(interp);
}

static void replace_top_with_array(Interpreter *interp, int handle) {
	interp->data_stack[interp->dsp - 1] = make_array(handle);
}

void p_take(Interpreter *interp) {
	POP_INT(n_items, "take", "length");
	if (n_items < 0) n_items = 0;

	PEEK_SEQUENCE_AT(source_val, 0, "take");
	Object *source = interp->objects[VAL_DATA(source_val)];
	if (n_items > source->len) n_items = source->len;

	NEW_ARRAY(result_handle, result, n_items);

	for (int i = 0; i < n_items; i++)
		result->items[i] = source->items[i];

	replace_top_with_array(interp, result_handle);

	DISPATCH(interp);
}

void p_reverse(Interpreter *interp) {
	PEEK_SEQUENCE_AT(source_val, 0, "reverse");
	Object *source = interp->objects[VAL_DATA(source_val)];

	NEW_ARRAY(result_handle, result, source->len);

	for (int i = 0; i < source->len; i++)
		result->items[source->len - i - 1] = source->items[i];

	replace_top_with_array(interp, result_handle);

	DISPATCH(interp);
}

void p_reverse_slice(Interpreter *interp) {
	POP_INT(n, "reverse-slice!", "count");
	POP_INT(offset, "reverse-slice!", "offset");
	PEEK_TYPE_AT(target_val, 0, "reverse-slice!", T_ARRAY);
	Object *target = interp->objects[VAL_DATA(target_val)];

	if (n < 0) {
		fail(interp, "reverse-slice!: count must be non-negative; got %d", n);
		return;
	}
	if (offset < 0 || offset + n > target->len) {
		fail(interp, "reverse-slice!: slice [%d, %d) out of bounds for length %d",
		     offset, offset + n, target->len);
		return;
	}

	int low = offset;
	int high = offset + n - 1;
	while (low < high) {
		Val saved = target->items[low];
		target->items[low] = target->items[high];
		target->items[high] = saved;
		low++;
		high--;
	}

	DISPATCH(interp);
}

void p_concat(Interpreter *interp) {
	int i;

	PEEK_SEQUENCE_AT(b_val, 0, "concat");
	PEEK_SEQUENCE_AT(a_val, 1, "concat");
	Object *b = interp->objects[VAL_DATA(b_val)];
	Object *a = interp->objects[VAL_DATA(a_val)];

	NEW_ARRAY(result_handle, result, a->len + b->len);

	for (i = 0; i < a->len; i++)
		result->items[i] = a->items[i];
	for (i = 0; i < b->len; i++)
		result->items[a->len + i] = b->items[i];

	interp->data_stack[interp->dsp - 2] = make_array(result_handle);
	interp->dsp--;

	DISPATCH(interp);
}

void p_range(Interpreter *interp) {
	POP_INT(range_to, "range", "to");
	POP_INT(range_from, "range", "from");

	int step = range_to >= range_from ? 1 : -1;
	int n_values = (range_to - range_from) * step + 1;
	NEW_ARRAY(result_handle, result, n_values);

	for (int i = 0; i < n_values; i++)
		result->items[i] = make_float(range_from + i * step);

	push(interp, make_array(result_handle));

	DISPATCH(interp);
}

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


void p_to_frame(Interpreter *interp) {
	PEEK_SEQUENCE_AT(source_val, 0, ">frame");
	if (VAL_TAG(source_val) != T_ARRAY) {
		fail(interp, ">frame: expected an array; got %s", tag_name(VAL_TAG(source_val)));
		return;
	}
	Object *source = interp->objects[VAL_DATA(source_val)];
	if (source->len % 2 != 0) {
		fail(interp, ">frame: array needs an even number of kv pairs");
		return;
	}

	NEW_FRAME(frame_handle, frame);
	for (int i = 0; i < source->len; i += 2) {
		if (VAL_TAG(source->items[i]) != T_SYMBOL) {
			fail(interp, ">frame: keys must be symbols; got %s", tag_name(VAL_TAG(source->items[i])));
			return;
		}
		frame_put(frame, VAL_DATA(source->items[i]), source->items[i + 1]);
	}

	interp->dsp--;
	push(interp, make_frame(frame_handle));

	DISPATCH(interp);
}

static Object *frame_path(Interpreter *interp, Val path_val, const char *op) {
	if (VAL_TAG(path_val) != T_ARRAY) {
		fail(interp, "%s: expected a path (array of symbols); got %s", op, tag_name(VAL_TAG(path_val)));
		return NULL;
	}

	Object *path = interp->objects[VAL_DATA(path_val)];
	for (int i = 0; i < path->len; i++) {
		if (VAL_TAG(path->items[i]) != T_SYMBOL) {
			fail(interp, "%s: path elements must be symbols; got %s", op, tag_name(VAL_TAG(path->items[i])));
			return NULL;
		}
	}
	return path;
}

#define PEEK_FRAME_PATH(frame, path, op) \
	PEEK_TYPE_AT(frame, 1, op, T_FRAME); \
	Object *path = frame_path(interp, interp->data_stack[interp->dsp - 1], op); \
	if (!path) return

#define PEEK_FRAME_PATH_VALUE(frame, path, value, op) \
	PEEK_TYPE_AT(frame, 2, op, T_FRAME); \
	Object *path = frame_path(interp, interp->data_stack[interp->dsp - 2], op); \
	if (!path) return; \
	Val value = interp->data_stack[interp->dsp - 1]

#define REQUIRE_NONEMPTY_PATH(path, op) \
	if ((path)->len == 0) { \
		fail(interp, "%s: empty path", (op)); \
		return; \
	}

#define DISPATCH_SYMBOL_OR_PATH(key_or_path, op, symbol_body, path_body) \
	do { \
		if (VAL_TAG(key_or_path) == T_SYMBOL) { \
			symbol_body; \
		} else if (VAL_TAG(key_or_path) == T_ARRAY) { \
			Object *path = frame_path(interp, (key_or_path), (op)); \
			if (!path) return; \
			path_body; \
		} else { \
			fail(interp, "%s: expected a symbol or path (array of symbols); got %s", \
					(op), tag_name(VAL_TAG(key_or_path))); \
			return; \
		} \
	} while (0)

void p_frame_get(Interpreter *interp) {
	PEEK_TYPE_AT(frame_val, 1, "@", T_FRAME);
	PEEK_AT(key_or_path, 0, "@");
	Object *frame = interp->objects[VAL_DATA(frame_val)];

	DISPATCH_SYMBOL_OR_PATH(key_or_path, "@", {
			FRAME_LOOKUP(frame, VAL_DATA(key_or_path), at, present);
			if (!present) {
			fail(interp, "@: no key :%s", &interp->vocab->symbol_pool[VAL_DATA(key_or_path)]);
			return;
			}
			Val result = frame->frame.values[at];
			interp->dsp -= 2;
			push(interp, result);
			}, {
			Val result = frame_walk(interp, frame_val, path, path->len, WALK_ERROR, NULL, "@");
			if (interp->error_flag) return;
			interp->dsp -= 2;
			push(interp, result);
			});

	DISPATCH(interp);
}

void p_frame_set(Interpreter *interp) {
	PEEK_TYPE_AT(frame_val, 2, "!", T_FRAME);
	PEEK_AT(key_or_path, 1, "!");
	PEEK_AT(value, 0, "!");
	Object *frame = interp->objects[VAL_DATA(frame_val)];

	DISPATCH_SYMBOL_OR_PATH(key_or_path, "!", {
			frame_put(frame, VAL_DATA(key_or_path), value);
			interp->dsp -= 2;
			}, {
			REQUIRE_NONEMPTY_PATH(path, "!");
			Val parent = frame_walk(interp, frame_val, path, path->len - 1, WALK_VIVIFY, NULL, "!");
			if (interp->error_flag) return;
			frame_put(interp->objects[VAL_DATA(parent)], VAL_DATA(path->items[path->len - 1]), value);
			interp->dsp -= 2;
			});

	DISPATCH(interp);
}

void p_frame_delete_at(Interpreter *interp) {
	PEEK_TYPE_AT(frame_val, 1, "delete-at", T_FRAME);
	PEEK_AT(key_or_path, 0, "delete-at");
	Object *frame = interp->objects[VAL_DATA(frame_val)];

	DISPATCH_SYMBOL_OR_PATH(key_or_path, "delete-at", {
			if (!frame_delete(frame, VAL_DATA(key_or_path))) {
			fail(interp, "delete-at: no key :%s", &interp->vocab->symbol_pool[VAL_DATA(key_or_path)]);
			return;
			}
			interp->dsp--;
			}, {
			REQUIRE_NONEMPTY_PATH(path, "delete-at");
			Val parent = frame_walk(interp, frame_val, path, path->len - 1, WALK_ERROR, NULL, "delete-at");
			if (interp->error_flag) return;
			cell leaf = VAL_DATA(path->items[path->len - 1]);
			if (VAL_TAG(parent) != T_FRAME || !frame_delete(interp->objects[VAL_DATA(parent)], leaf)) {
			fail(interp, "delete-at: no key :%s", &interp->vocab->symbol_pool[leaf]);
			return;
			}
			interp->dsp--;
			});

	DISPATCH(interp);
}

void p_frame_keys(Interpreter *interp) {
	PEEK_TYPE_AT(frame_val, 0, "keys", T_FRAME);
	Object *frame = interp->objects[VAL_DATA(frame_val)];
	NEW_ARRAY(result_handle, result, frame->len);

	for (int i = 0; i < frame->len; i++)
		result->items[i] = make_symbol((int)frame->frame.keys[i]);

	replace_top_with_array(interp, result_handle);

	DISPATCH(interp);
}

void p_frame_values(Interpreter *interp) {
	PEEK_TYPE_AT(frame_val, 0, "values", T_FRAME);
	Object *frame = interp->objects[VAL_DATA(frame_val)];
	NEW_ARRAY(result_handle, result, frame->len);

	for (int i = 0; i < frame->len; i++)
		result->items[i] = frame->frame.values[i];

	replace_top_with_array(interp, result_handle);

	DISPATCH(interp);
}

void p_merge(Interpreter *interp) {
	PEEK_TYPE_AT(right, 0, "merge", T_FRAME);
	PEEK_TYPE_AT(left, 1, "merge", T_FRAME);
	Object *right_frame = interp->objects[VAL_DATA(right)];
	Object *left_frame = interp->objects[VAL_DATA(left)];

	NEW_FRAME(result_handle, result);
	for (int i = 0; i < left_frame->len; i++)
		frame_put(result, left_frame->frame.keys[i], left_frame->frame.values[i]);
	for (int i = 0; i < right_frame->len; i++)
		frame_put(result, right_frame->frame.keys[i], right_frame->frame.values[i]);

	interp->dsp -= 2;
	push(interp, make_frame(result_handle));

	DISPATCH(interp);
}

void p_frame(Interpreter *interp) {
	PEEK_TYPE_AT(values_val, 0, "frame", T_ARRAY);
	PEEK_SEQUENCE_AT(keys_val, 1, "frame");
	Object *values = interp->objects[VAL_DATA(values_val)];
	Object *keys = interp->objects[VAL_DATA(keys_val)];
	if (keys->len != values->len) {
		fail(interp, "frame: keys and values must be the same length (%d vs %d)", keys->len, values->len);
		return;
	}
	for (int i = 0; i < keys->len; i++) {
		if (VAL_TAG(keys->items[i]) != T_SYMBOL) {
			fail(interp, "frame: keys must be symbols; got %s", tag_name(VAL_TAG(keys->items[i])));
			return;
		}
	}

	NEW_FRAME(frame_handle, frame);
	for (int i = 0; i < keys->len; i++)
		frame_put(frame, VAL_DATA(keys->items[i]), values->items[i]);

	interp->dsp -= 2;
	push(interp, make_frame(frame_handle));

	DISPATCH(interp);
}

void p_has(Interpreter *interp) {
	PEEK_AT(value, 1, "has?");
	if (VAL_TAG(value) == T_STRING) {
		POP_STRING(pattern, "has?");
		POP_STRING(subject, "has?");

		push(interp, make_bool(string_matches(interp, subject, pattern)));

		DISPATCH(interp);
	}

	PEEK_TYPE_AT(frame_val, 1, "has?", T_FRAME);
	PEEK_AT(key_or_path, 0, "has?");
	Object *frame = interp->objects[VAL_DATA(frame_val)];

	DISPATCH_SYMBOL_OR_PATH(key_or_path, "has?", {
			FRAME_LOOKUP(frame, VAL_DATA(key_or_path), at, present);
			(void)at;
			interp->dsp -= 2;
			push(interp, make_bool(present));
			}, {
			REQUIRE_NONEMPTY_PATH(path, "has?");
			int found;
			frame_walk(interp, frame_val, path, path->len, WALK_PROBE, &found, "has?");
			interp->dsp -= 2;
			push(interp, make_bool(found));
			});

	DISPATCH(interp);
}

void p_update_at(Interpreter *interp) {
	PEEK_TYPE_AT(frame_val, 2, "update-at", T_FRAME);
	PEEK_AT(key_or_path, 1, "update-at");
	PEEK_AT(xt, 0, "update-at");
	if (VAL_TAG(xt) != T_XT) {
		fail(interp, "update-at: xt required on stack; got %s", tag_name(VAL_TAG(xt)));
		return;
	}
	Object *frame = interp->objects[VAL_DATA(frame_val)];

	DISPATCH_SYMBOL_OR_PATH(key_or_path, "update-at", {
			FRAME_LOOKUP(frame, VAL_DATA(key_or_path), at, present);
			if (!present) {
			fail(interp, "update-at: no key :%s", &interp->vocab->symbol_pool[VAL_DATA(key_or_path)]);
			return;
			}
			push(interp, frame->frame.values[at]);
			execute_cfa(interp, (int)VAL_DATA(xt));
			frame->frame.values[at] = pop(interp);
			interp->dsp -= 2;
			}, {
			REQUIRE_NONEMPTY_PATH(path, "update-at");
			Val parent = frame_walk(interp, frame_val, path, path->len - 1, WALK_ERROR, NULL, "update-at");
			if (interp->error_flag) return;
			if (VAL_TAG(parent) != T_FRAME) {
			fail(interp, "update-at: parent is not a frame; got %s", tag_name(VAL_TAG(parent)));
			return;
			}
			Object *parent_obj = interp->objects[VAL_DATA(parent)];
			cell leaf = VAL_DATA(path->items[path->len - 1]);
			FRAME_LOOKUP(parent_obj, leaf, at, present);
			if (!present) {
				fail(interp, "update-at: no key :%s", &interp->vocab->symbol_pool[leaf]);
				return;
			}
			push(interp, parent_obj->frame.values[at]);
			execute_cfa(interp, (int)VAL_DATA(xt));
			parent_obj->frame.values[at] = pop(interp);
			interp->dsp -= 2;
			});

	DISPATCH(interp);
}

void p_destruct(Interpreter *interp) {
	PEEK_COLLECTION_AT(source_val, 0, "destruct");
	Object *source = interp->objects[VAL_DATA(source_val)];
	interp->dsp--;

	for (int i = 0; i < source->len; i++) {
		if (VAL_TAG(source_val) == T_FRAME) {
			push(interp, make_symbol((int)source->frame.keys[i]));
			push(interp, source->frame.values[i]);
		} else {
			push(interp, source->items[i]);
		}
	}

	DISPATCH(interp);
}

void p_destruct_to(Interpreter *interp) {
	POP(target_val);
	POP(source_val);

	if (VAL_TAG(source_val) != T_ARRAY) {
                fail(interp, "destruct-to: source must be an array; got %s", tag_name(VAL_TAG(source_val)));
                return;
	}
	if (VAL_TAG(target_val) != T_ARRAY) {
		fail(interp, "destruct-to: target must be an array; got %s", tag_name(VAL_TAG(target_val)));
		return;
	}

	Object *source = interp->objects[VAL_DATA(source_val)];
	Object *target = interp->objects[VAL_DATA(target_val)];
	if (source->len != target->len) {
		fail(interp, "destruct-to: length mismatch (source %d, target %d)", source->len, target->len);
		return;
	}

	for (int i = 0; i < source->len; i++) {
		int var_cfa;

		Val item = target->items[i];
		if (VAL_TAG(item) == T_XT) {
			var_cfa = (int)VAL_DATA(item);
		} else if (VAL_TAG(item) == T_SYMBOL) {
			const char *name = &interp->vocab->symbol_pool[VAL_DATA(item)];
			var_cfa = find(interp, name);
			if (!var_cfa)
				var_cfa = create_variable(interp, name);

			target->items[i] = make_xt(var_cfa);
		} else {
			fail(interp, "destruct-to: target item at index %d must be symbol or xt; got %s",
					i, tag_name(VAL_TAG(item)));
			return;
		}

		if ((cfa_handler)interp->vocab->dict[var_cfa] != dovar) {
			fail(interp, "destruct-to: target at index %d is not a variable", i);
			return;
		}

		interp->vocab->dict[var_cfa + 1] = (cell)source->items[i].bits;
	}

	DISPATCH(interp);
}

void p_slice_store(Interpreter *interp) {
	POP_INT(slen, "slice!", "length");
	POP_INT(sstep, "slice!", "step");
	POP_INT(sstart, "slice!", "source-start");
	POP(src_val);
	if (VAL_TAG(src_val) != T_ARRAY && VAL_TAG(src_val) != T_SET) {
		fail(interp, "slice!: source must be an array or set; got %s", tag_name(VAL_TAG(src_val)));
		return;
	}
	POP_INT(tstart, "slice!", "target-start");
	PEEK_TYPE_AT(target_val, 0, "slice!", T_ARRAY);

	Object *target = interp->objects[VAL_DATA(target_val)];
	Object *src    = interp->objects[VAL_DATA(src_val)];

	if (slen < 0) {
		fail(interp, "slice!: length must be non-negative; got %d", slen);
		return;
	}
	if (slen == 0)
		return;

	if (tstart < 0 || tstart + slen > target->len) {
		fail(interp, "slice!: target [%d, %d) out of bounds for length %d",
		     tstart, tstart + slen, target->len);
		return;
	}

	int s_first = sstart;
	int s_last  = sstart + (slen - 1) * sstep;
	int s_min = s_first < s_last ? s_first : s_last;
	int s_max = s_first > s_last ? s_first : s_last;
	if (s_min < 0 || s_max >= src->len) {
		fail(interp, "slice!: source indices [%d..%d] out of bounds for length %d",
		     s_min, s_max, src->len);
		return;
	}

	if (target == src && sstep == -1 && sstart == tstart + slen - 1) {
		for (int i = 0; i < slen / 2; i++) {
			Val t = target->items[tstart + i];
			target->items[tstart + i] = target->items[tstart + slen - 1 - i];
			target->items[tstart + slen - 1 - i] = t;
		}
		return;
	}

	if (target == src) {
		Val small_buf[64];
		Val *tmp = (slen <= 64) ? small_buf : malloc(slen * sizeof(Val));
		if (!tmp) {
			fail(interp, "slice!: out of memory");
			return;
		}
		for (int i = 0; i < slen; i++)
			tmp[i] = src->items[sstart + i * sstep];
		for (int i = 0; i < slen; i++)
			target->items[tstart + i] = tmp[i];
		if (tmp != small_buf) free(tmp);
	} else {
		for (int i = 0; i < slen; i++)
			target->items[tstart + i] = src->items[sstart + i * sstep];
	}

	DISPATCH(interp);
}

void p_to_slice(Interpreter *interp) {
	POP_INT(n, "to-slice!", "count");
	POP_INT(offset, "to-slice!", "offset");
	PEEK_TYPE_AT(target_val, 0, "to-slice!", T_ARRAY);
	Object *target = interp->objects[VAL_DATA(target_val)];

	if (n < 0) {
		fail(interp, "to-slice!: count must be non-negative; got %d", n);
		return;
	}
	if (offset < 0 || offset + n > target->len) {
		fail(interp, "to-slice!: slice [%d, %d) out of bounds for length %d",
		     offset, offset + n, target->len);
		return;
	}
	if (interp->dsp < 1 + n) {
		fail(interp, "to-slice!: stack too shallow (need %d values plus target)", n);
		return;
	}
	int start = interp->dsp - 1 - n;
	for (int i = 0; i < n; i++) {
		target->items[offset + i] = interp->data_stack[start + i];
	}
	interp->data_stack[start] = target_val;
	interp->dsp -= n;

	DISPATCH(interp);
}
		
typedef struct {
	const char *cursor;
	const char *end;
	int depth;
	char *scratch;
	int scratch_capacity;
} JSONParser;

#define JSON_MAX_DEPTH 1024

static void json_parse_value(Interpreter *interp, JSONParser *parser, Val *destination);

static void json_skip_whitespace(JSONParser *parser) {
	while (parser->cursor < parser->end) {
		char lookahead = *parser->cursor;
		if (lookahead != ' ' && lookahead != '\t' && lookahead != '\n' && lookahead != '\r') 
			break;
		parser->cursor++;
	}
}

static int json_hex4(const char *hex) {
	int value = 0;
	for (int i = 0; i < 4; i++) {
		char digit = hex[i];
		int nibble;
		if (digit >= '0' && digit <= '9') nibble = digit - '0';
		else if (digit >= 'a' && digit <= 'f') nibble = digit - 'a' + 10;
		else if (digit >= 'A' && digit <= 'F') nibble = digit - 'A' + 10;
		else return -1;
		value = (value << 4) | nibble;
	}
	return value;
}

static const char *json_string_end(const char *content, const char *end) {
	const char *closing = content;
	while (closing < end && *closing != '"') {
		if (*closing == '\\' && closing + 1 < end) closing++;
		closing++;
	}
	return closing;
}

static int json_decode_string(Interpreter *interp, const char *content, const char *closing, char *out) {
	int length = 0;
	const char *raw = content;
	while (raw < closing) {
		if (*raw != '\\') {
			out[length++] = *raw++;
			continue;
		}
		raw++;
		switch (*raw) {
			case '"': out[length++] = '"'; raw++; break;
			case '\\': out[length++] = '\\'; raw++; break;
			case '/': out[length++] = '/'; raw++; break;
			case 'b': out[length++] = '\b'; raw++; break;
			case 'f': out[length++] = '\f'; raw++; break;
			case 'n': out[length++] = '\n'; raw++; break;
			case 'r': out[length++] = '\r'; raw++; break;
			case 't': out[length++] = '\t'; raw++; break;
			case 'u': {
				raw++;
				if (closing - raw < 4) {
					fail(interp, "json>frame: truncated \\u escape");
					return -1;
				}
				int codepoint = json_hex4(raw);
				if (codepoint < 0) {
					fail(interp, "json>frame: invalid \\u escape");
					return -1;
				}
				raw += 4;
				if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
					if (closing - raw < 6 || raw[0] != '\\' || raw[1] != 'u') {
						fail(interp, "json>frame: unpaired surrogate");
						return -1;
					}
					int low = json_hex4(raw + 2);
					if (low < 0xDC00 || low > 0xDFFF) {
						fail(interp, "json>frame: invalid surrogate pair");
						return -1;
					}
					codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
					raw += 6;
				}
				if (codepoint <= 0x7F) {
					out[length++] = (char)codepoint;
				} else if (codepoint <= 0x7FF) {
					out[length++] = (char)(0xC0 | (codepoint >> 6));
					out[length++] = (char)(0x80 | (codepoint & 0x3F));
				} else if (codepoint <= 0xFFFF) {
					out[length++] = (char)(0xE0 | (codepoint >> 12));
					out[length++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
					out[length++] = (char)(0x80 | (codepoint & 0x3F));
				} else {
					out[length++] = (char)(0xF0 | (codepoint >> 18));
					out[length++] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
					out[length++] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
					out[length++] = (char)(0x80 | (codepoint & 0x3F));
				}
				break;
			}
			default:
				fail(interp, "json>frame: unsupported string escape");
				return -1;
		}
	}
	return length;
}

static void json_parse_string(Interpreter *interp, JSONParser *parser, Val *destination) {
	const char *content = parser->cursor + 1;
	const char *closing = json_string_end(content, parser->end);
	if (closing >= parser->end) {
		fail(interp, "json>frame: unterminated string");
		return;
	}

	int handle = object_new_string_uninit(interp, (int)(closing - content));
	if (interp->error_flag) return;

	Object *string = interp->objects[handle];
	int length = json_decode_string(interp, content, closing, string->bytes);
	if (length < 0) return;
	string->len = length;
	string->bytes[length] = 0;
	
	*destination = make_string(handle);
	
	parser->cursor = closing + 1;
}

static void json_parse_array(Interpreter *interp, JSONParser *parser, Val *destination) {
	if (++parser->depth > JSON_MAX_DEPTH) {
		fail(interp, "json>frame: nesting too deep");
		return;
	}

	int handle = object_new_array(interp, 0);
	if (interp->error_flag) return;
	*destination = make_array(handle);
	parser->cursor++;

	json_skip_whitespace(parser);
	if (parser->cursor < parser->end && *parser->cursor == ']') {
		parser->cursor++;
		parser->depth--;
		return;
	}

	for (;;) {
		Object *array = interp->objects[handle];
		if (array->len == array->capacity) {
			int capacity = array->capacity < 4 ? 4 : array->capacity * 2;
			array->items = realloc(array->items, sizeof(Val) * (size_t)capacity);
			array->capacity = capacity;
		}

		array->items[array->len] = make_tagged(T_NONE, 0);
		array->len++;
		
		json_parse_value(interp, parser, &interp->objects[handle]->items[array->len - 1]);
		if (interp->error_flag) return;

		json_skip_whitespace(parser);
		if (parser->cursor >= parser->end) {
			fail(interp, "json>frame: unterminated array");
			return;
		}
		if (*parser->cursor == ',') {
			parser->cursor++;
			continue;
		}
		if (*parser->cursor == ']') {
			parser->cursor++;
			break;
		}
		fail(interp, "json>frame: expected ',' or ']' in array");
		return;
	}

	parser->depth--;
}

static void json_parse_object(Interpreter *interp, JSONParser *parser, Val *destination) {
	if (++parser->depth > JSON_MAX_DEPTH) {
		fail(interp, "json>frame: nesting too deep");
		return;
	}

	int handle = object_new_frame(interp);
	if (interp->error_flag) return;
	*destination = make_frame(handle);
	parser->cursor++;

	json_skip_whitespace(parser);
	if (parser->cursor < parser->end && *parser->cursor == '}') {
		parser->cursor++;
		parser->depth--;
		return;
	}

	for (;;) {
		json_skip_whitespace(parser);
		if (parser->cursor >= parser->end || *parser->cursor != '"') {
			fail(interp, "json>frame: expected string key in object");
			return;
		}

		const char *key_content = parser->cursor + 1;
		const char *key_closing = json_string_end(key_content, parser->end);
		if (key_closing >= parser->end) {
			fail(interp, "json>frame: unterminated string");
			return;
		}

		int key_span = (int)(key_closing - key_content);
		if (key_span + 1 > parser->scratch_capacity) {
			int capacity = parser->scratch_capacity < 64 ? 64 : parser->scratch_capacity;
			while (capacity < key_span + 1) capacity *= 2;
			parser->scratch = realloc(parser->scratch, (size_t)capacity);
			parser->scratch_capacity = capacity;
		}

		int key_length = json_decode_string(interp, key_content, key_closing, parser->scratch);
		if (key_length < 0) return;
		parser->scratch[key_length] = 0;
		cell key_symbol = intern_symbol(interp, parser->scratch);
		if (interp->error_flag) return;
		
		parser->cursor = key_closing + 1;

		json_skip_whitespace(parser);
		if (parser->cursor >= parser->end || *parser->cursor != ':') {
			fail(interp, "json>frame: expected ':' after key");
			return;
		}
		parser->cursor++;

		Object *frame = interp->objects[handle];
		if (frame->len == frame->capacity) {
			int capacity = frame->capacity * 2;
			frame->frame.keys = realloc(frame->frame.keys, sizeof(cell) * (size_t)capacity);
			frame->frame.values = realloc(frame->frame.values, sizeof(Val) * (size_t)capacity);
			frame->capacity = capacity;
		}

		frame->frame.keys[frame->len] = key_symbol;
		frame->frame.values[frame->len] = make_tagged(T_NONE, 0);
		frame->len++;
		
		json_parse_value(interp, parser, &interp->objects[handle]->frame.values[frame->len - 1]);
		if (interp->error_flag) return;

		json_skip_whitespace(parser);
		if (parser->cursor >= parser->end) {
			fail(interp, "json>frame: unterminated object");
			return;
		}
		if (*parser->cursor == ',') {
			parser->cursor++;
			continue;
		}
		if (*parser->cursor == '}') {
			parser->cursor++;
			break;
		}
		fail(interp, "json>frame: expected ',' or '}' in object");
		return;
	}

	Object *frame = interp->objects[handle];
	cell *keys = frame->frame.keys;
	Val *values = frame->frame.values;
	
	for (int i = 1; i < frame->len; i++) {
		cell key_symbol = keys[i];
		Val value = values[i];
		int j = i - 1;
		
		while (j >= 0 && keys[j] > key_symbol) {
			keys[j + 1] = keys[j];
			values[j + 1] = values[j];
			j--;
		}

		keys[j + 1] = key_symbol;
		values[j + 1] = value;
	}

	int unique = 0;
	for (int i = 0; i < frame->len; i++) {
		if (unique > 0 && keys[unique - 1] == keys[i]) {
			values[unique - 1] = values[i];
		} else {
			keys[unique] = keys[i];
			values[unique] = values[i];
			unique++;
		}
	}
	frame->len = unique;

	parser->depth--;
}

static void json_parse_value(Interpreter *interp, JSONParser *parser, Val *destination) {
	json_skip_whitespace(parser);
	if (parser->cursor >= parser->end) {
		fail(interp, "json>frame: unexpected end of input");
		return;
	}

	switch (*parser->cursor) {
		case 'n':
			if (parser->end - parser->cursor >= 4 && memcmp(parser->cursor, "null", 4) == 0) {
				parser->cursor += 4;
				*destination = make_tagged(T_NONE, 0);
				return;
			}
			fail(interp, "json>frame: invalid literal");
			return;
		case 't':
			if (parser->end - parser->cursor >= 4 && memcmp(parser->cursor, "true", 4) == 0) {
				parser->cursor += 4;
				*destination = make_symbol(interp->vocab->true_symbol);
				return;
			}
			fail(interp, "json>frame: invalid literal");
			return;
		case 'f':
			if (parser->end - parser->cursor >= 5 && memcmp(parser->cursor, "false", 5) == 0) {
				parser->cursor += 5;
				*destination = make_symbol(interp->vocab->false_symbol);
				return;
			}
			fail(interp, "json>frame: invalid literal");
			return;
		case '-':
		case '0': case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9': {
			char *number_end;
			double number = strtod(parser->cursor, &number_end);
			if (number_end == parser->cursor) {
				fail(interp, "json>frame: invalid number");
				return;
			}
			parser->cursor = number_end;
			*destination = make_float(number);
			return;
		}
		case '"':
			json_parse_string(interp, parser, destination);
			return;
		case '[':
			json_parse_array(interp, parser, destination);
			return;
		case '{':
			json_parse_object(interp, parser, destination);
			return;
		default:
			fail(interp, "json>frame: unexpected character");
			return;
	}
}

void p_json_to_frame(Interpreter *interp) {
	PEEK_STRING_AT(json, 0, "json>frame");

	JSONParser parser;
	parser.cursor = json->bytes;
	parser.end = json->bytes + json->len;
	parser.depth = 0;
	parser.scratch = NULL;
	parser.scratch_capacity = 0;

	gc_root_push(interp, make_tagged(T_NONE, 0));
	if (interp->error_flag) return;

	json_parse_value(interp, &parser, &interp->gc_roots[interp->n_gc_roots - 1]);
	Val parsed = interp->gc_roots[interp->n_gc_roots - 1];
	gc_root_pop(interp);
	free(parser.scratch);
	if (interp->error_flag) return;

	json_skip_whitespace(&parser);
	if (parser.cursor != parser.end) {
		fail(interp, "json>frame: trailing content after value");
		return;
	}

	interp->data_stack[interp->dsp - 1] = parsed;

	DISPATCH(interp);
}

typedef struct {
	char *buffer;
	int length;
	int capacity;
} JSONWriter;

static void json_write_bytes(JSONWriter *writer, const char *bytes, int n) {
	if (writer->length + n > writer->capacity) {
		int capacity = writer->capacity < 64 ? 64 : writer->capacity;
		while (capacity < writer->length + n) capacity *= 2;
		writer->buffer = realloc(writer->buffer, (size_t)capacity);
		writer->capacity = capacity;
	}

	memcpy(writer->buffer + writer->length, bytes, (size_t)n);
	writer->length += n;
}

static void json_write_byte(JSONWriter *writer, char byte) {
	json_write_bytes(writer, &byte, 1);
}

static void json_write_number(JSONWriter *writer, double number) {
	char text[32];
	int n;
	
	if (number == (double)(int64_t)number && number > -1e15 && number < 1e15) {
		n = snprintf(text, sizeof text, "%lld", (long long)number);
	} else {
		for (int precision = 1; precision <= 17; precision++) {
			n = snprintf(text, sizeof text, "%.*g", precision, number);
			if (strtod(text, NULL) == number)
				break;
		}
	}
	
	json_write_bytes(writer, text, n);
}

static void json_write_string(JSONWriter *writer, const char *bytes, int len) {
	json_write_byte(writer, '"');
	
	for (int i = 0; i < len; i++) {
		unsigned char byte = (unsigned char)bytes[i];
		switch (byte) {
			case '"': json_write_bytes(writer, "\\\"", 2); break;
			case '\\': json_write_bytes(writer, "\\\\", 2); break;
			case '\b': json_write_bytes(writer, "\\b", 2); break;
			case '\f': json_write_bytes(writer, "\\f", 2); break;
			case '\n': json_write_bytes(writer, "\\n", 2); break;
			case '\r': json_write_bytes(writer, "\\r", 2); break;
			case '\t': json_write_bytes(writer, "\\t", 2); break;
			default:
				if (byte < 0x20) {
					char escape[8];
					int n = snprintf(escape, sizeof escape, "\\u%04x", byte);
					json_write_bytes(writer, escape, n);
				} else {
					json_write_byte(writer, (char)byte);
				}
		}
	}
	
	json_write_byte(writer, '"');
}

static void json_write_value(Interpreter *interp, JSONWriter *writer, Val value, int depth) {
	if (interp->error_flag) return;
	if (depth > JSON_MAX_DEPTH) {
		fail(interp, "frame>json: nesting too deep");
		return;
	}

	switch (VAL_TAG(value)) {
		case T_NONE:
			json_write_bytes(writer, "null", 4);
			return;
		case T_FLOAT:
			json_write_number(writer, VAL_NUMBER(value));
			return;
		case T_SYMBOL:
			if ((int)VAL_DATA(value) == interp->vocab->true_symbol)
				json_write_bytes(writer, "true", 4);
			else if ((int)VAL_DATA(value) == interp->vocab->false_symbol)
				json_write_bytes(writer, "false", 5);
			else
				fail(interp, "frame>json: cannot serialize a non-boolean symbol");
			return;
		case T_STRING: {
			Object *string = interp->objects[VAL_DATA(value)];
			json_write_string(writer, string->bytes, string->len);
			return;
		}
		case T_ARRAY: {
			Object *array = interp->objects[VAL_DATA(value)];
			json_write_byte(writer, '[');
			for (int i = 0; i < array->len; i++) {
				if (i > 0) json_write_bytes(writer, ", ", 2);
				json_write_value(interp, writer, array->items[i], depth + 1);
			}
			json_write_byte(writer, ']');
			return;
		}
		case T_FRAME: {
			Object *frame = interp->objects[VAL_DATA(value)];
			json_write_byte(writer, '{');
			for (int i = 0; i < frame->len; i++) {
				if (i > 0) json_write_bytes(writer, ", ", 2);
				const char *key = &interp->vocab->symbol_pool[frame->frame.keys[i]];
				json_write_string(writer, key, (int)strlen(key));
				json_write_bytes(writer, ": ", 2);
				json_write_value(interp, writer, frame->frame.values[i], depth + 1);
			}
			json_write_byte(writer, '}');
			return;
		}
		default:
			fail(interp, "frame>json: cannot serialize %s", tag_name(VAL_TAG(value)));
			return;
	}
}

void p_frame_to_json(Interpreter *interp) {
	PEEK_AT(value, 0, "frame>json");

	JSONWriter writer;
	writer.buffer = NULL;
	writer.length = 0;
	writer.capacity = 0;

	json_write_value(interp, &writer, value, 0);
	if (interp->error_flag) {
		free(writer.buffer);
		return;
	}

	int handle = object_new_string(interp, writer.buffer ? writer.buffer : "", writer.length);
	free(writer.buffer);
	if (interp->error_flag) return;

	interp->data_stack[interp->dsp - 1] = make_string(handle);

	DISPATCH(interp);
}
			
