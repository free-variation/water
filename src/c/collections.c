#include "water.h"


void set_add(Interpreter *interp, int set_handle, Val value) {
	Object *set = OBJECT_AT(set_handle);


	LOWER_BOUND(set->len, mid, val_cmp(interp, set->items[mid], value) < 0, low);
	if (low < set->len && val_cmp(interp, set->items[low], value) == 0) {
		return;
	}

	GROW_IF_FULL(set->len, set->capacity, set->items);

	memmove(&set->items[low + 1], &set->items[low],
			sizeof(Val) * (size_t)(set->len - low));
	set->items[low] = value;
	set->len++;
}

int set_member(Interpreter *interp, int set_handle, Val value) {
	Object *set = OBJECT_AT(set_handle);

	LOWER_BOUND(set->len, mid, val_cmp(interp, set->items[mid], value) < 0, low);
	return low < set->len && val_cmp(interp, set->items[low], value) == 0;
}

void set_remove(Interpreter *interp, int set_handle, Val value) {
	Object *set = OBJECT_AT(set_handle);

	LOWER_BOUND(set->len, mid, val_cmp(interp, set->items[mid], value) < 0, low);
	if (low < set->len && val_cmp(interp, set->items[low], value) == 0) {
		memmove(&set->items[low], &set->items[low + 1],
				sizeof(Val) * (size_t)(set->len - low - 1));
		set->len--;
	}
}

int set_union(Interpreter *interp, int handle_a, int handle_b) {
	int union_handle = object_new_set(interp);
	if (interp->error_flag) return -1;

	Object *union_set = OBJECT_AT(union_handle);
	Object *set_a = OBJECT_AT(handle_a);
	Object *set_b = OBJECT_AT(handle_b);

	int i = 0, j = 0;
	while (i < set_a->len || j < set_b->len) {
		Val merged_value;
		if (j >= set_b->len)
			merged_value = set_a->items[i++];
		else if (i >= set_a->len)
			merged_value = set_b->items[j++];
		else {
			int cmp = val_cmp(interp, set_a->items[i], set_b->items[j]);
			if (cmp < 0)
				merged_value = set_a->items[i++];
			else if (cmp > 0)
				merged_value = set_b->items[j++];
			else {
				merged_value = set_a->items[i++];
				j++;
			}
		}

		GROW_IF_FULL(union_set->len, union_set->capacity, union_set->items);
		union_set->items[union_set->len++] = merged_value;
	}

	return union_handle;
}

int set_intersect(Interpreter *interp, int handle_a, int handle_b) {
	int intersection_handle = object_new_set(interp);
	if (interp->error_flag) return -1;

	Object *intersection_set = OBJECT_AT(intersection_handle);
	Object *set_a = OBJECT_AT(handle_a);
	Object *set_b = OBJECT_AT(handle_b);

	int i = 0, j = 0;
	while (i < set_a->len && j < set_b->len) {
		int cmp = val_cmp(interp, set_a->items[i], set_b->items[j]);
		if (cmp < 0)
			i++;
		else if (cmp > 0)
			j++;
		else {
			GROW_IF_FULL(intersection_set->len, intersection_set->capacity, intersection_set->items);
			intersection_set->items[intersection_set->len++] = set_a->items[i];
			i++;
			j++;
		}
	}

	return intersection_handle;
}

int set_difference(Interpreter *interp, int handle_a, int handle_b) {
	int difference_handle = object_new_set(interp);
	if (interp->error_flag) return -1;

	Object *difference_set = OBJECT_AT(difference_handle);
	Object *set_a = OBJECT_AT(handle_a);
	Object *set_b = OBJECT_AT(handle_b);

	int i = 0, j = 0;
	while (i < set_a->len) {
		int cmp = (j >= set_b->len) ? -1 : val_cmp(interp, set_a->items[i], set_b->items[j]);
		if (cmp < 0) {
			GROW_IF_FULL(difference_set->len, difference_set->capacity, difference_set->items);
			difference_set->items[difference_set->len++] = set_a->items[i++];
		} else if (cmp > 0) {
			j++;
		} else {
			i++;
			j++;
		}
	}

	return difference_handle;
}

#define FIND_MARK(mark_index, errmsg) \
	int mark_index = interp->dsp; \
	while (mark_index > 0 && VAL_TAG(interp->data_stack[mark_index - 1]) != T_MARK) \
		mark_index--; \
	if (mark_index == 0) { \
		fail(interp, "%s", (errmsg)); \
		return; \
	}

void p_setopen(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	*chain_sp = make_tagged(T_MARK, '<');

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

void p_setclose(DISPATCH_ARGS) {
	FIND_MARK(mark_index, "> : no matching < on the stack");

	int set_handle = build_set_from_values(interp,
			&interp->data_stack[mark_index], interp->dsp - mark_index);
	if (interp->error_flag) return;
	interp->data_stack[mark_index - 1] = make_set(set_handle);

	DISPATCH_REGISTERS(interp, chain_ip, interp->data_stack + mark_index);
}

void p_frameopen(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	*chain_sp = make_tagged(T_MARK, '{');

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}


static void frame_set_pair(Interpreter *interp, int frame_handle, Val key, Val value);

void p_frameclose(DISPATCH_ARGS) {
	FIND_MARK(mark_index, "} : no matching { on the stack");

	int count = interp->dsp - mark_index;
	if (count % 2 != 0) {
		fail(interp, "frame needs key/value pairs");
		return;
	}

	NEW_FRAME(frame_handle, frame);
	(void)frame;
	Val built_frame = make_frame(frame_handle);
	gc_root_push(interp, built_frame);
	for (int i = mark_index; i < interp->dsp; i += 2) {
		frame_set_pair(interp, frame_handle, interp->data_stack[i], interp->data_stack[i + 1]);
		if (interp->error_flag) {
			gc_root_pop(interp);
			return;
		}
	}
	gc_root_pop(interp);

	interp->data_stack[mark_index - 1] = built_frame;

	DISPATCH_REGISTERS(interp, chain_ip, interp->data_stack + mark_index);
}

void p_array_open(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	*chain_sp = make_tagged(T_MARK, '[');

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

void p_list_open(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	*chain_sp = make_tagged(T_MARK, '(');

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

void p_array_close(DISPATCH_ARGS) {
	FIND_MARK(mark_index, "] : no matching [ on the stack");
	int num_elements = interp->dsp - mark_index;
	NEW_ARRAY(array_handle, array, num_elements);
	for (int i = 0; i < num_elements; i++)
		array->items[i] = interp->data_stack[mark_index + i];
	interp->data_stack[mark_index - 1] = make_array(array_handle);

	DISPATCH_REGISTERS(interp, chain_ip, interp->data_stack + mark_index);
}

void p_list_close(DISPATCH_ARGS) {
	FIND_MARK(mark_index, ")] : no matching [( on the stack");

	int num_elements = interp->dsp - mark_index;
	if (num_elements == 0) {
		interp->data_stack[mark_index - 1] = make_tagged(T_NONE, 0);
	} else {
		interp->data_stack[mark_index - 1] = interp->data_stack[mark_index + num_elements - 1];
		for (int i = num_elements - 2; i >= 0; i--) {
			int slot = object_new_pair(interp);
			if (interp->error_flag)
				return;
			pairs.table[slot].head = interp->data_stack[mark_index + i];
			pairs.table[slot].tail = interp->data_stack[mark_index - 1];
			interp->data_stack[mark_index - 1] = make_pair(slot);
		}
	}
	DISPATCH_REGISTERS(interp, chain_ip, interp->data_stack + mark_index);
}

void p_array(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val count_val = chain_sp[-1];
	if (VAL_TAG(count_val) != T_FLOAT) {
		fail(interp, "expected a float count; got %s", tag_name(VAL_TAG(count_val)));
		return;
	}
	int count = (int)VAL_NUMBER(count_val);
	Val *gathered_base = chain_sp - 1 - count;
	if (count < 0 || gathered_base < interp->data_stack) {
		fail(interp, "count %d out of range (stack has %d available)", count, (int)(chain_sp - 1 - interp->data_stack));
		return;
	}

	NEW_ARRAY(array_handle, array, count);

	for (int i = 0; i < count; i++)
		array->items[i] = gathered_base[i];
	gathered_base[0] = make_array(array_handle);

	DISPATCH_REGISTERS(interp, chain_ip, gathered_base + 1);
}

void p_cons(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	int slot = object_new_pair(interp);
	if (interp->error_flag) return;

	pairs.table[slot].head = chain_sp[-2];
	pairs.table[slot].tail = chain_sp[-1];
	chain_sp[-2] = make_pair(slot);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_head_tail(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	Val pair_val = chain_sp[-1];
	if (VAL_TAG(pair_val) != T_PAIR) {
		fail(interp, "expected a pair; got %s", tag_name(VAL_TAG(pair_val)));
		return;
	}
	Pair *pair = &pairs.table[VAL_DATA(pair_val)];
	chain_sp[-1] = pair->head;
	chain_sp[0] = pair->tail;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

void p_array_to_cons(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val array_val = chain_sp[-1];
	if (VAL_TAG(array_val) != T_ARRAY) {
		fail(interp, "expected an array; got %s", tag_name(VAL_TAG(array_val)));
		return;
	}
	Object *array = OBJECT_AT(VAL_DATA(array_val));
	int n = array->len;

	Val result;
	if (n == 0)
		result = make_tagged(T_NONE, 0);
	else {

		gc_root_push(interp, array->items[n - 1]);
		for (int i = n - 2; i >= 0; i--) {
			int slot = object_new_pair(interp);
			if (interp->error_flag) {
				gc_root_pop(interp);
				return;
			}
			pairs.table[slot].head = array->items[i];
			pairs.table[slot].tail = interp->gc_roots[interp->n_gc_roots - 1];
			interp->gc_roots[interp->n_gc_roots - 1] = make_pair(slot);
		}
		result = interp->gc_roots[interp->n_gc_roots - 1];
		gc_root_pop(interp);
	}

	chain_sp[-1] = result;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_cons_to_array(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val list_val = chain_sp[-1];

	int count = 1;
	Val cur = deref(interp, list_val);
	while (VAL_TAG(cur) == T_PAIR) {
		if (count > COPY_SPINE_MAX) {
			fail(interp, "list too long or cyclic");
			return;
		}
		count++;
		cur = deref(interp, pairs.table[VAL_DATA(cur)].tail);
	}

	int handle = object_new_array(interp, count);
	if (interp->error_flag)
		return;
	Object *array = OBJECT_AT(handle);

	int i = 0;
	cur = deref(interp, list_val);
	while (VAL_TAG(cur) == T_PAIR) {
		array->items[i++] = deref(interp, pairs.table[VAL_DATA(cur)].head);
		cur = deref(interp, pairs.table[VAL_DATA(cur)].tail);
	}
	array->items[i] = deref(interp, cur);

	chain_sp[-1] = make_array(handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

static int val_cmp_qsort(void *interp, const void *left, const void *right) {
	return val_cmp((Interpreter *)interp, *(const Val *)left, *(const Val *)right);
}

int build_set_from_values(Interpreter *interp, const Val *values, int count) {
	int set_handle = object_new_set(interp);
	if (interp->error_flag)
		return -1;

	Object *set = OBJECT_AT(set_handle);
	if (count > set->capacity) {
		set->items = arena_realloc(set->items, sizeof(Val) * (size_t)count);
		set->capacity = count;
	}
	memcpy(set->items, values, sizeof(Val) * (size_t)count);

	if (count > 0) {
		platform_qsort_r(set->items, (size_t)count, sizeof(Val), interp, val_cmp_qsort);

		int unique = 1;
		for (int i = 1; i < count; i++)
			if (val_cmp(interp, set->items[unique - 1], set->items[i]) != 0)
				set->items[unique++] = set->items[i];
		set->len = unique;
	}

	return set_handle;
}

void p_set(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val count_val = chain_sp[-1];
	if (VAL_TAG(count_val) != T_FLOAT) {
		fail(interp, "expected a float count; got %s", tag_name(VAL_TAG(count_val)));
		return;
	}
	int count = (int)VAL_NUMBER(count_val);
	Val *gathered_base = chain_sp - 1 - count;
	if (count < 0 || gathered_base < interp->data_stack) {
		fail(interp, "count %d out of range (stack has %d available)", count, (int)(chain_sp - 1 - interp->data_stack));
		return;
	}

	int set_handle = build_set_from_values(interp, gathered_base, count);
	if (interp->error_flag)
		return;

	gathered_base[0] = make_set(set_handle);

	DISPATCH_REGISTERS(interp, chain_ip, gathered_base + 1);
}

void p_array_to_set(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val array_val = chain_sp[-1];
	if (VAL_TAG(array_val) != T_ARRAY) {
		fail(interp, "expected %s; got %s", tag_name(T_ARRAY), tag_name(VAL_TAG(array_val)));
		return;
	}
	Object *array = OBJECT_AT(VAL_DATA(array_val));

	int set_handle = build_set_from_values(interp, array->items, array->len);
	if (interp->error_flag)
		return;

	chain_sp[-1] = make_set(set_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

int set_elements_copy(Interpreter *interp, Object *source) {
	int n_items = source->len;
	int elements_handle = object_new_array(interp, n_items);
	if (interp->error_flag)
		return -1;

	Object *elements = OBJECT_AT(elements_handle);
	memcpy(elements->items, source->items, sizeof(Val) * (size_t)n_items);

	return elements_handle;
}

int array_sorted_copy(Interpreter *interp, Object *source) {
	int n_items = source->len;
	int sorted_array_handle = object_new_array(interp, n_items);
	if (interp->error_flag)
		return -1;

	Object *sorted_array = OBJECT_AT(sorted_array_handle);
	memcpy(sorted_array->items, source->items, sizeof(Val) * (size_t)n_items);
	if (n_items > 0)
		platform_qsort_r(sorted_array->items, (size_t)n_items, sizeof(Val), interp, val_cmp_qsort);

	return sorted_array_handle;
}


void p_byte_size(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val string_val = chain_sp[-1];
	if (VAL_TAG(string_val) != T_STRING) {
		fail(interp, "expected a string; got %s", tag_name(VAL_TAG(string_val)));
		return;
	}

	chain_sp[-1] = make_float((double)OBJECT_AT(VAL_DATA(string_val))->len);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_member(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val set_val = chain_sp[-2];
	if (VAL_TAG(set_val) != T_SET) {
		SYNC_REGISTERS(interp, chain_ip, chain_sp);
		fail(interp, "expected a set; got %s", tag_name(VAL_TAG(set_val)));
		return;
	}
	SYNC_REGISTERS(interp, chain_ip, chain_sp - 2);
	int present = set_member(interp, (int)VAL_DATA(set_val), chain_sp[-1]);
	if (interp->error_flag)
		return;
	chain_sp[-2] = make_bool(present);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

#define BINARY_SET_OP(c_name, combine) \
	void c_name(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2); \
		Val left = chain_sp[-2]; \
		Val right = chain_sp[-1]; \
		if (VAL_TAG(left) != T_SET || VAL_TAG(right) != T_SET) { \
			SYNC_REGISTERS(interp, chain_ip, chain_sp); \
			Val offender = VAL_TAG(left) != T_SET ? left : right; \
			fail(interp, "expected %s; got %s", tag_name(T_SET), tag_name(VAL_TAG(offender))); \
			return; \
		} \
		\
		SYNC_REGISTERS(interp, chain_ip, chain_sp); \
		int combined_handle = combine(interp, (int)VAL_DATA(left), (int)VAL_DATA(right)); \
		if (interp->error_flag) return; \
		\
		chain_sp[-2] = make_set(combined_handle); \
		\
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1); \
	}

BINARY_SET_OP(p_union, set_union)
BINARY_SET_OP(p_intersect, set_intersect)
BINARY_SET_OP(p_difference, set_difference)

void p_set_add(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val set_val = chain_sp[-2];
	if (VAL_TAG(set_val) != T_SET) {
		fail(interp, "expected a set; got %s", tag_name(VAL_TAG(set_val)));
		return;
	}

	set_add(interp, (int)VAL_DATA(set_val), chain_sp[-1]);
	if (interp->error_flag)
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_set_remove(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val set_val = chain_sp[-2];
	if (VAL_TAG(set_val) != T_SET) {
		fail(interp, "expected a set; got %s", tag_name(VAL_TAG(set_val)));
		return;
	}

	set_remove(interp, (int)VAL_DATA(set_val), chain_sp[-1]);
	if (interp->error_flag)
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_add_last(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val array_val = chain_sp[-2];
	if (VAL_TAG(array_val) != T_ARRAY) {
		SYNC_REGISTERS(interp, chain_ip, chain_sp);
		fail(interp, "expected an array; got %s", tag_name(VAL_TAG(array_val)));
		return;
	}
	Object *array = OBJECT_AT(VAL_DATA(array_val));
	SYNC_REGISTERS(interp, chain_ip, chain_sp - 1);
	GROW_IF_FULL(array->len, array->capacity, array->items);
	array->items[array->len++] = chain_sp[-1];

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_remove_last(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val array_val = chain_sp[-1];
	if (VAL_TAG(array_val) != T_ARRAY) {
		SYNC_REGISTERS(interp, chain_ip, chain_sp);
		fail(interp, "expected an array; got %s", tag_name(VAL_TAG(array_val)));
		return;
	}
	Object *array = OBJECT_AT(VAL_DATA(array_val));
	if (array->len == 0) {
		SYNC_REGISTERS(interp, chain_ip, chain_sp - 1);
		fail(interp, "empty array");
		return;
	}

	array->len--;
	chain_sp[-1] = array->items[array->len];

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}


#define NEW_SEGMENT(name, word, element_type) \
	void name(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1); \
		Val length_val = chain_sp[-1]; \
		if (VAL_TAG(length_val) != T_FLOAT) { \
			SYNC_REGISTERS(interp, chain_ip, chain_sp); \
			fail(interp, "expected a float length; got %s", tag_name(VAL_TAG(length_val))); \
			return; \
		} \
		int length = (int)VAL_NUMBER(length_val); \
		if (length < 0) { \
			SYNC_REGISTERS(interp, chain_ip, chain_sp); \
			fail(interp, "length must be non-negative; got %d", length); \
			return; \
		} \
		\
		SYNC_REGISTERS(interp, chain_ip, chain_sp); \
		int handle = object_new_segment(interp, length, element_type); \
		if (interp->error_flag) return; \
		chain_sp[-1] = make_segment(handle); \
		\
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp); \
	}

NEW_SEGMENT(p_int_segment,    "int-segment",    SEGMENT_INT)
NEW_SEGMENT(p_double_segment, "double-segment", SEGMENT_DOUBLE)

#define REQUIRE_CHAIN_SEQUENCE(value) \
	do { \
		if (VAL_TAG(value) != T_ARRAY && VAL_TAG(value) != T_SET) { \
			SYNC_REGISTERS(interp, chain_ip, chain_sp); \
			fail(interp, "expected array or set; got %s", tag_name(VAL_TAG(value))); \
			return; \
		} \
	} while (0)

void p_array_of(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val length_val = chain_sp[-1];
	if (VAL_TAG(length_val) != T_FLOAT) {
		fail(interp, "expected a float length; got %s", tag_name(VAL_TAG(length_val)));
		return;
	}
	int array_len = (int)VAL_NUMBER(length_val);

	NEW_ARRAY(array_handle, array, array_len);
	for (int i = 0; i < array_len; i++)
		array->items[i] = chain_sp[-2];

	chain_sp[-2] = make_array(array_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_take(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val length_val = chain_sp[-1];
	if (VAL_TAG(length_val) != T_FLOAT) {
		fail(interp, "expected a float length; got %s", tag_name(VAL_TAG(length_val)));
		return;
	}
	int n_items = (int)VAL_NUMBER(length_val);
	if (n_items < 0) n_items = 0;

	Val source_val = chain_sp[-2];
	REQUIRE_CHAIN_SEQUENCE(source_val);
	Object *source = OBJECT_AT(VAL_DATA(source_val));
	if (n_items > source->len) n_items = source->len;

	NEW_ARRAY(result_handle, result, n_items);

	for (int i = 0; i < n_items; i++)
		result->items[i] = source->items[i];

	chain_sp[-2] = make_array(result_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_reverse(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val source_val = chain_sp[-1];
	REQUIRE_CHAIN_SEQUENCE(source_val);
	Object *source = OBJECT_AT(VAL_DATA(source_val));

	NEW_ARRAY(result_handle, result, source->len);

	for (int i = 0; i < source->len; i++)
		result->items[source->len - i - 1] = source->items[i];

	chain_sp[-1] = make_array(result_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_concat(DISPATCH_ARGS) {
	int i;

	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val b_val = chain_sp[-1];
	Val a_val = chain_sp[-2];
	REQUIRE_CHAIN_SEQUENCE(b_val);
	REQUIRE_CHAIN_SEQUENCE(a_val);
	Object *b = OBJECT_AT(VAL_DATA(b_val));
	Object *a = OBJECT_AT(VAL_DATA(a_val));

	NEW_ARRAY(result_handle, result, a->len + b->len);

	for (i = 0; i < a->len; i++)
		result->items[i] = a->items[i];
	for (i = 0; i < b->len; i++)
		result->items[a->len + i] = b->items[i];

	chain_sp[-2] = make_array(result_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

static int flatten_count(Interpreter *interp, Object *source, int depth) {
	if (depth > MAX_NESTING_DEPTH) {
		fail(interp, "nesting too deep (cycle?)");
		return 0;
	}

	int count = 0;
	for (int i = 0; i < source->len; i++) {
		Val item = source->items[i];
		if (VAL_TAG(item) == T_ARRAY) {
			count += flatten_count(interp, OBJECT_AT(VAL_DATA(item)), depth + 1);
			if (interp->error_flag) return 0;
		} else {
			count++;
		}
	}
	return count;
}

static void flatten_fill(Object *source, Object *target, int *offset) {
for (int i = 0; i < source->len; i++) {
	Val item = source->items[i];
	if (VAL_TAG(item) == T_ARRAY)
		flatten_fill(OBJECT_AT(VAL_DATA(item)), target, offset);
	else
		target->items[(*offset)++] = item;
}
}

void p_flatten_array(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val arrays_val = chain_sp[-1];
	if (VAL_TAG(arrays_val) != T_ARRAY) {
		fail(interp, "expected %s; got %s", tag_name(T_ARRAY), tag_name(VAL_TAG(arrays_val)));
		return;
	}
	Object *arrays = OBJECT_AT(VAL_DATA(arrays_val));

	int has_nested = 0;
	for (int i = 0; i < arrays->len; i++)
		if (VAL_TAG(arrays->items[i]) == T_ARRAY) {
			has_nested = 1;
			break;
		}
	if (!has_nested)
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp);

	int total = flatten_count(interp, arrays, 0);
	if (interp->error_flag) return;

	NEW_ARRAY(destination_handle, destination, total);
	int offset = 0;
	flatten_fill(arrays, destination, &offset);

	chain_sp[-1] = make_array(destination_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_sample(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3);
	Val source_val = chain_sp[-3];
	REQUIRE_CHAIN_SEQUENCE(source_val);
	Val replacement_val = chain_sp[-1];
	Val count_val = chain_sp[-2];
	if (VAL_TAG(count_val) != T_FLOAT) {
		fail(interp, "expected a float count; got %s", tag_name(VAL_TAG(count_val)));
		return;
	}
	int count = (int)VAL_NUMBER(count_val);
	Object *source = OBJECT_AT(VAL_DATA(source_val));
	int with_replacement = truthy(replacement_val);

	if (count < 0) {
		fail(interp, "count must be non-negative; got %d", count);
		return;
	}

	if (source->len == 0) {
		int empty_handle = object_new_array(interp, 0);
		if (interp->error_flag) return;

		chain_sp[-3] = make_array(empty_handle);
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
	}

	if (!with_replacement && count > source->len) {
		fail(interp, "cannot draw %d without replacement from %d", count, source->len);
		return;
	}

	NEW_ARRAY(sampled_handle, sampled, count);

	if (with_replacement) {
		for (int i = 0; i < count; i++)
			sampled->items[i] = source->items[random_below(source->len)];
	} else {
		int *indices = malloc(sizeof(int) * (size_t)source->len);
		for (int i = 0; i < source->len; i++)
			indices[i] = i;

		for (int i = 0; i < count; i++) {
			int j = i + random_below(source->len - i);
			int chosen = indices[j];
			indices[j] = indices[i];
			sampled->items[i] = source->items[chosen];
		}
		free(indices);
	}

	chain_sp[-3] = make_array(sampled_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}


void p_range(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val to_val = chain_sp[-1];
	Val from_val = chain_sp[-2];
	if (VAL_TAG(to_val) != T_FLOAT) {
		fail(interp, "expected a float to; got %s", tag_name(VAL_TAG(to_val)));
		return;
	}
	if (VAL_TAG(from_val) != T_FLOAT) {
		fail(interp, "expected a float from; got %s", tag_name(VAL_TAG(from_val)));
		return;
	}
	int range_to = (int)VAL_NUMBER(to_val);
	int range_from = (int)VAL_NUMBER(from_val);

	int step = range_to >= range_from ? 1 : -1;
	int64_t span = (int64_t)range_to - (int64_t)range_from;
	int64_t count = (span < 0 ? -span : span) + 1;
	if (count > INT_MAX) {
		fail(interp, "too many elements (%lld)", (long long)count);
		return;
	}
	int n_values = (int)count;

	NEW_ARRAY(result_handle, result, n_values);

	for (int i = 0; i < n_values; i++)
		result->items[i] = make_float(range_from + i * step);

	chain_sp[-2] = make_array(result_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void frame_reserve(Object *frame, int needed) {
	if (frame->capacity >= needed)
		return;
	while (frame->capacity < needed)
		frame->capacity *= 2;
	frame->frame.keys = arena_realloc(frame->frame.keys, sizeof(cell) * (size_t)frame->capacity);
	frame->frame.values = arena_realloc(frame->frame.values, sizeof(Val) * (size_t)frame->capacity);
}

void frame_put(Object *frame, cell key, Val value) {
	FRAME_LOOKUP(frame, key, at, present);
	if (present) {
		frame->frame.values[at] = value;
		return;
	}

	frame_reserve(frame, frame->len + 1);

	memmove(&frame->frame.keys[at + 1], &frame->frame.keys[at], sizeof(cell) * (size_t)(frame->len - at));
	memmove(&frame->frame.values[at + 1], &frame->frame.values[at], sizeof(Val) * (size_t)(frame->len - at));
	frame->frame.keys[at] = key;
	frame->frame.values[at] = value;
	frame->len++;
}

typedef struct {
	cell key;
	Val value;
	int order;
} FrameEntry;

static int frame_entry_cmp(const void *a, const void *b) {
	const FrameEntry *left = a;
	const FrameEntry *right = b;
	if (left->key != right->key)
		return left->key < right->key ? -1 : 1;
	return (left->order > right->order) - (left->order < right->order);
}

void frame_sort_dedup(Object *frame) {
	if (frame->len < 2)
		return;

	int ascending = 1;
	for (int i = 1; i < frame->len; i++) {
		if (frame->frame.keys[i - 1] >= frame->frame.keys[i]) {
			ascending = 0;
			break;
		}
	}
	if (ascending)
		return;

	if (frame->len <= 64) {
		for (int i = 1; i < frame->len; i++) {
			cell key = frame->frame.keys[i];
			Val value = frame->frame.values[i];
			int j = i - 1;
			while (j >= 0 && frame->frame.keys[j] > key) {
				frame->frame.keys[j + 1] = frame->frame.keys[j];
				frame->frame.values[j + 1] = frame->frame.values[j];
				j--;
			}
			frame->frame.keys[j + 1] = key;
			frame->frame.values[j + 1] = value;
		}
		int unique = 0;
		for (int i = 0; i < frame->len; i++) {
			if (unique > 0 && frame->frame.keys[unique - 1] == frame->frame.keys[i])
				frame->frame.values[unique - 1] = frame->frame.values[i];
			else {
				frame->frame.keys[unique] = frame->frame.keys[i];
				frame->frame.values[unique] = frame->frame.values[i];
				unique++;
			}
		}
		frame->len = unique;
		return;
	}

	FrameEntry *entries = malloc(sizeof(FrameEntry) * (size_t)frame->len);
	for (int i = 0; i < frame->len; i++) {
		entries[i].key = frame->frame.keys[i];
		entries[i].value = frame->frame.values[i];
		entries[i].order = i;
	}

	qsort(entries, (size_t)frame->len, sizeof(FrameEntry), frame_entry_cmp);

	int unique = 0;
	for (int i = 0; i < frame->len; i++) {
		if (unique > 0 && frame->frame.keys[unique - 1] == entries[i].key)
			frame->frame.values[unique - 1] = entries[i].value;
		else {
			frame->frame.keys[unique] = entries[i].key;
			frame->frame.values[unique] = entries[i].value;
			unique++;
		}
	}
	frame->len = unique;

	free(entries);
}

int frame_delete(Object *frame, cell key) {
	FRAME_LOOKUP(frame, key, at, present);
	if (!present) return 0;
	memmove(&frame->frame.keys[at], &frame->frame.keys[at + 1], sizeof(cell) * (size_t)(frame->len - at - 1));
	memmove(&frame->frame.values[at], &frame->frame.values[at + 1], sizeof(Val) * (size_t)(frame->len - at - 1));
	frame->len--;
	return 1;
}


void p_array_to_frame(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val source_val = chain_sp[-1];
	if (VAL_TAG(source_val) != T_ARRAY) {
		fail(interp, "expected an array; got %s", tag_name(VAL_TAG(source_val)));
		return;
	}
	Object *source = OBJECT_AT(VAL_DATA(source_val));
	if (source->len % 2 != 0) {
		fail(interp, "array needs an even number of kv pairs");
		return;
	}

	NEW_FRAME(frame_handle, frame);
	(void)frame;
	Val built_frame = make_frame(frame_handle);
	gc_root_push(interp, built_frame);

	for (int i = 0; i < source->len; i += 2) {
		frame_set_pair(interp, frame_handle, source->items[i], source->items[i + 1]);
		if (interp->error_flag) {
			gc_root_pop(interp);
			return;
		}
	}
	gc_root_pop(interp);

	chain_sp[-1] = built_frame;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

static Object *frame_path_resolve(Interpreter *interp, Val path_val,
		int allow_search, int *is_search) {
	if (is_search)
		*is_search = 0;
	if (VAL_TAG(path_val) != T_ARRAY) {
		fail(interp, "expected a path (array of symbols); got %s", tag_name(VAL_TAG(path_val)));
		return NULL;
	}

	Object *path = OBJECT_AT(VAL_DATA(path_val));
	for (int i = 0; i < path->len; i++) {
		Val step = path->items[i];
		int search_step = VAL_TAG(step) == T_ARRAY;
		if (!search_step) {
			if (VAL_TAG(step) != T_SYMBOL) {
				fail(interp, "path elements must be symbols; got %s", tag_name(VAL_TAG(step)));
				return NULL;
			}
			cell key = VAL_DATA(step);
			search_step = key == (cell)vocab.wildcard_symbol
					|| key == (cell)vocab.descendant_symbol;
		}
		if (search_step) {
			if (!allow_search) {
				fail(interp, "path has a wildcard, descendant, or predicate; use select-keys/select-values");
				return NULL;
			}
			if (is_search)
				*is_search = 1;
			return path;
		}
	}
	return path;
}

static Object *frame_path(Interpreter *interp, Val path_val) {
	if (VAL_TAG(path_val) != T_ARRAY) {
		fail(interp, "expected a path (array of symbols); got %s", tag_name(VAL_TAG(path_val)));
		return NULL;
	}
	Object *path = OBJECT_AT(VAL_DATA(path_val));
	for (int i = 0; i < path->len; i++) {
		if (VAL_TAG(path->items[i]) != T_SYMBOL) {
			fail(interp, "path elements must be symbols; got %s", tag_name(VAL_TAG(path->items[i])));
			return NULL;
		}
	}
	return path;
}

static int leaf_is_axis(Interpreter *interp, cell leaf) {
	if (leaf == (cell)vocab.wildcard_symbol || leaf == (cell)vocab.descendant_symbol) {
		fail(interp, "path has a wildcard or descendant; use select-keys/select-values");
		return 1;
	}
	return 0;
}

static void frame_set_pair(Interpreter *interp, int frame_handle, Val key, Val value) {
	if (VAL_TAG(key) == T_SYMBOL) {
		frame_put(OBJECT_AT(frame_handle), VAL_DATA(key), value);
		return;
	}

	if (VAL_TAG(key) == T_ARRAY) {
		Object *path = frame_path(interp, key);
		if (!path)
			return;

		if (path->len == 0) {
			fail(interp, "empty path key");
			return;
		}

		Val parent = frame_walk(interp, make_frame(frame_handle), path, path->len - 1, WALK_VIVIFY, NULL);
		if (interp->error_flag) return;

		frame_put(OBJECT_AT(VAL_DATA(parent)), VAL_DATA(path->items[path->len - 1]), value);
		return;
	}

	fail(interp, "keys must be symbols or paths; got %s", tag_name(VAL_TAG(key)));
}

#define PEEK_FRAME_PATH(frame, path, op) \
	PEEK_TYPE_AT(frame, 1, op, T_FRAME); \
	Object *path = frame_path(interp, interp->data_stack[interp->dsp - 1]); \
	if (!path) return

#define PEEK_FRAME_PATH_VALUE(frame, path, value, op) \
	PEEK_TYPE_AT(frame, 2, op, T_FRAME); \
	Object *path = frame_path(interp, interp->data_stack[interp->dsp - 2]); \
	if (!path) return; \
	Val value = interp->data_stack[interp->dsp - 1]

#define REQUIRE_NONEMPTY_PATH(path, op) \
	if ((path)->len == 0) { \
		fail(interp, "empty path"); \
		return; \
	}

#define DISPATCH_SYMBOL_OR_PATH(key_or_path, op, symbol_body, path_body) \
	do { \
		if (VAL_TAG(key_or_path) == T_SYMBOL) { \
			symbol_body; \
		} else if (VAL_TAG(key_or_path) == T_ARRAY) { \
			Object *path = frame_path(interp, (key_or_path)); \
			if (!path) return; \
			path_body; \
		} else { \
			fail(interp, "%s: expected a symbol or path (array of symbols); got %s", \
					(op), tag_name(VAL_TAG(key_or_path))); \
			return; \
		} \
	} while (0)

void p_frame_get_symbol(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val frame_val = chain_sp[-2];
	Val key = chain_sp[-1];
	if (VAL_TAG(frame_val) != T_FRAME || VAL_TAG(key) != T_SYMBOL) {
		RETARGET_OP(p_frame_get);
		MUSTTAIL return p_frame_get(interp, chain_ip, chain_sp);
	}

	Object *frame = OBJECT_AT(VAL_DATA(frame_val));
	FRAME_LOOKUP(frame, VAL_DATA(key), at, present);
	if (!present) {
		fail(interp, "no key :%s", &vocab.symbol_pool[VAL_DATA(key)]);
		return;
	}

	chain_sp[-2] = frame->frame.values[at];
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_frame_set_symbol(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3);
	Val frame_val = chain_sp[-3];
	Val key = chain_sp[-2];
	Val value = chain_sp[-1];
	if (VAL_TAG(frame_val) != T_FRAME || VAL_TAG(key) != T_SYMBOL) {
		RETARGET_OP(p_frame_set);
		MUSTTAIL return p_frame_set(interp, chain_ip, chain_sp);
	}

	frame_put(OBJECT_AT(VAL_DATA(frame_val)), VAL_DATA(key), value);
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}

void p_frame_get(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val frame_val = chain_sp[-2];
	if (VAL_TAG(frame_val) != T_FRAME) {
		fail(interp, "expected %s; got %s", tag_name(T_FRAME), tag_name(VAL_TAG(frame_val)));
		return;
	}
	Val key_or_path = chain_sp[-1];
	Object *frame = OBJECT_AT(VAL_DATA(frame_val));

	DISPATCH_SYMBOL_OR_PATH(key_or_path, "@", {
			RETARGET_OP(p_frame_get_symbol);
			FRAME_LOOKUP(frame, VAL_DATA(key_or_path), at, present);
			if (!present) {
			fail(interp, "no key :%s", &vocab.symbol_pool[VAL_DATA(key_or_path)]);
			return;
			}
			chain_sp[-2] = frame->frame.values[at];
			}, {
			Val result = frame_walk(interp, frame_val, path, path->len, WALK_ERROR, NULL);
			if (interp->error_flag) return;
			chain_sp[-2] = result;
			});

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_frame_get_or(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3);
	Val frame_val = chain_sp[-3];
	if (VAL_TAG(frame_val) != T_FRAME) {
		fail(interp, "expected %s; got %s", tag_name(T_FRAME), tag_name(VAL_TAG(frame_val)));
		return;
	}
	Val key_or_path = chain_sp[-2];
	Val fallback = chain_sp[-1];
	Object *frame = OBJECT_AT(VAL_DATA(frame_val));

	DISPATCH_SYMBOL_OR_PATH(key_or_path, "@or", {
			FRAME_LOOKUP(frame, VAL_DATA(key_or_path), at, present);
			chain_sp[-3] = present ? frame->frame.values[at] : fallback;
			}, {
			int walk_found = 0;
			Val walked = frame_walk(interp, frame_val, path, path->len, WALK_PROBE, &walk_found);
			chain_sp[-3] = walk_found ? walked : fallback;
			});

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}

void p_frame_set(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3);
	Val frame_val = chain_sp[-3];
	if (VAL_TAG(frame_val) != T_FRAME) {
		fail(interp, "expected %s; got %s", tag_name(T_FRAME), tag_name(VAL_TAG(frame_val)));
		return;
	}
	Val key_or_path = chain_sp[-2];
	Val value = chain_sp[-1];
	Object *frame = OBJECT_AT(VAL_DATA(frame_val));

	DISPATCH_SYMBOL_OR_PATH(key_or_path, "!", {
			RETARGET_OP(p_frame_set_symbol);
			frame_put(frame, VAL_DATA(key_or_path), value);
			}, {
			REQUIRE_NONEMPTY_PATH(path, "!");
			if (leaf_is_axis(interp, VAL_DATA(path->items[path->len - 1]))) return;
			Val parent = frame_walk(interp, frame_val, path, path->len - 1, WALK_VIVIFY, NULL);
			if (interp->error_flag) return;
			frame_put(OBJECT_AT(VAL_DATA(parent)), VAL_DATA(path->items[path->len - 1]), value);
			});

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}

void p_frame_delete_at(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val frame_val = chain_sp[-2];
	if (VAL_TAG(frame_val) != T_FRAME) {
		fail(interp, "expected %s; got %s", tag_name(T_FRAME), tag_name(VAL_TAG(frame_val)));
		return;
	}
	Val key_or_path = chain_sp[-1];
	Object *frame = OBJECT_AT(VAL_DATA(frame_val));

	DISPATCH_SYMBOL_OR_PATH(key_or_path, "delete-at", {
			if (!frame_delete(frame, VAL_DATA(key_or_path))) {
			fail(interp, "no key :%s", &vocab.symbol_pool[VAL_DATA(key_or_path)]);
			return;
			}
			}, {
			REQUIRE_NONEMPTY_PATH(path, "delete-at");
			if (leaf_is_axis(interp, VAL_DATA(path->items[path->len - 1]))) return;
			Val parent = frame_walk(interp, frame_val, path, path->len - 1, WALK_ERROR, NULL);
			if (interp->error_flag) return;
			cell leaf = VAL_DATA(path->items[path->len - 1]);
			if (VAL_TAG(parent) != T_FRAME || !frame_delete(OBJECT_AT(VAL_DATA(parent)), leaf)) {
			fail(interp, "no key :%s", &vocab.symbol_pool[leaf]);
			return;
			}
			});

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_frame_keys(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val frame_val = chain_sp[-1];
	if (VAL_TAG(frame_val) != T_FRAME) {
		fail(interp, "expected %s; got %s", tag_name(T_FRAME), tag_name(VAL_TAG(frame_val)));
		return;
	}
	Object *frame = OBJECT_AT(VAL_DATA(frame_val));

	NEW_ARRAY(result_handle, result, frame->len);

	for (int i = 0; i < frame->len; i++)
		result->items[i] = make_symbol((int)frame->frame.keys[i]);

	chain_sp[-1] = make_array(result_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_frame_values(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val frame_val = chain_sp[-1];
	if (VAL_TAG(frame_val) != T_FRAME) {
		fail(interp, "expected %s; got %s", tag_name(T_FRAME), tag_name(VAL_TAG(frame_val)));
		return;
	}
	Object *frame = OBJECT_AT(VAL_DATA(frame_val));

	NEW_ARRAY(result_handle, result, frame->len);

	for (int i = 0; i < frame->len; i++)
		result->items[i] = frame->frame.values[i];

	chain_sp[-1] = make_array(result_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_frame_to_array(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val frame_val = chain_sp[-1];
	if (VAL_TAG(frame_val) != T_FRAME) {
		fail(interp, "expected %s; got %s", tag_name(T_FRAME), tag_name(VAL_TAG(frame_val)));
		return;
	}
	Object *frame = OBJECT_AT(VAL_DATA(frame_val));

	NEW_ARRAY(result_handle, result, frame->len * 2);

	for (int i = 0; i < frame->len; i++) {
		result->items[2 * i] = make_symbol((int)frame->frame.keys[i]);
		result->items[2 * i + 1] = frame->frame.values[i];
	}

	chain_sp[-1] = make_array(result_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

static int predicate_holds(Interpreter *interp, Val tree_node, Object *pred) {
	if (pred->len < 2) {
		fail(interp, "malformed predicate (expected [key] or [key op value])");
		return 0;
	}
	int op = (int)VAL_NUMBER(pred->items[0]);
	Val key = pred->items[1];
	Val subject = make_tagged(T_NONE, 0);
	int present = 0;

	if (VAL_TAG(key) == T_ARRAY) {
		Object *subpath = OBJECT_AT(VAL_DATA(key));
		subject = frame_walk(interp, tree_node, subpath, subpath->len, WALK_PROBE, &present);
	} else if (VAL_DATA(key) == (cell)vocab.self_symbol) {
		subject = tree_node;
		present = 1;
	} else if (VAL_TAG(tree_node) == T_FRAME) {
		Object *frame = OBJECT_AT(VAL_DATA(tree_node));
		FRAME_LOOKUP(frame, VAL_DATA(key), at, found);
		if (found) {
			subject = frame->frame.values[at];
			present = 1;
		}
	}

	if (op == PRED_EXISTS)
		return present;
	if (!present)
		return 0;

	if (op == PRED_EQ || op == PRED_LT || op == PRED_GT) {
		if (pred->len < 3) {
			fail(interp, "malformed predicate (comparison needs a value)");
			return 0;
		}
	}
	if (op == PRED_EQ)
		return val_cmp(interp, subject, pred->items[2]) == 0;
	if (op == PRED_LT)
		return val_cmp(interp, subject, pred->items[2]) < 0;
	if (op == PRED_GT)
		return val_cmp(interp, subject, pred->items[2]) > 0;

	return 0;
}

typedef enum { SELECT_VALUES, SELECT_KEYS, SELECT_EXISTS } SelectMode;

static void select_descendants(Interpreter *interp, Val tree_node, Object *path, int depth,
		cell *trail, int trail_len, int matches, SelectMode mode, int *found);

static void select_walk(Interpreter *interp, Val tree_node, Object *path, int depth,
		cell *trail, int trail_len, int matches, SelectMode mode, int *found) {
	if (mode == SELECT_EXISTS && *found)
		return;
	if (depth == path->len) {
		if (mode == SELECT_EXISTS) {
			*found = 1;
			return;
		}
		Val captured;
		if (mode == SELECT_KEYS) {
			int path_handle = object_new_array(interp, trail_len);
			if (interp->error_flag)
				return;
			Object *captured_path = OBJECT_AT(path_handle);
			for (int i = 0; i < trail_len; i++)
				captured_path->items[i] = make_symbol((int)trail[i]);
			captured = make_array(path_handle);
		} else {
			captured = tree_node;
		}
		Object *matches_array = OBJECT_AT(matches);
		GROW_IF_FULL(matches_array->len, matches_array->capacity, matches_array->items);
		matches_array->items[matches_array->len++] = captured;
		return;
	}

	Val step = path->items[depth];
	if (VAL_TAG(step) == T_ARRAY) {
		if (predicate_holds(interp, tree_node, OBJECT_AT(VAL_DATA(step))))
			select_walk(interp, tree_node, path, depth + 1, trail, trail_len, matches, mode, found);
		return;
	}

	if (VAL_TAG(tree_node) != T_FRAME)
		return;
	if (trail_len >= SELECT_MAX_DEPTH) {
		fail(interp, "structure too deeply nested (cycle?)");
		return;
	}

	Object *frame = OBJECT_AT(VAL_DATA(tree_node));
	cell key = VAL_DATA(step);
	if (key == (cell)vocab.wildcard_symbol) {
		for (int i = 0; i < frame->len; i++) {
			trail[trail_len] = frame->frame.keys[i];
			select_walk(interp, frame->frame.values[i], path, depth + 1, trail, trail_len + 1, matches, mode, found);
			if (mode == SELECT_EXISTS && *found)
				return;
		}
	} else if (key == (cell)vocab.descendant_symbol) {
		select_descendants(interp, tree_node, path, depth + 1, trail, trail_len, matches, mode, found);
	} else {
		FRAME_LOOKUP(frame, key, at, present);
		if (present) {
			trail[trail_len] = key;
			select_walk(interp, frame->frame.values[at], path, depth + 1, trail, trail_len + 1, matches, mode, found);
		}
	}
}

#define SELECT_OP(c_name, mode) \
	void c_name(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2); \
		Val path_val = chain_sp[-1]; \
		if (VAL_TAG(path_val) != T_ARRAY) { \
			fail(interp, "expected %s; got %s", tag_name(T_ARRAY), tag_name(VAL_TAG(path_val))); \
			return; \
		} \
		Val frame_val = chain_sp[-2]; \
		if (VAL_TAG(frame_val) != T_FRAME) { \
			fail(interp, "expected %s; got %s", tag_name(T_FRAME), tag_name(VAL_TAG(frame_val))); \
			return; \
		} \
		Object *path = OBJECT_AT(VAL_DATA(path_val)); \
		\
		int matches = object_new_array(interp, 8); \
		if (interp->error_flag) \
			return; \
		OBJECT_AT(matches)->len = 0; \
		gc_root_push(interp, make_array(matches)); \
		\
		cell trail[SELECT_MAX_DEPTH]; \
		select_walk(interp, frame_val, path, 0, trail, 0, matches, mode, NULL); \
		gc_root_pop(interp); \
		if (interp->error_flag) \
			return; \
		\
		chain_sp[-2] = make_array(matches); \
		\
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1); \
	}

SELECT_OP(p_select_values, SELECT_VALUES)
SELECT_OP(p_select_keys, SELECT_KEYS)

static void select_descendants(Interpreter *interp, Val tree_node, Object *path, int depth,
		cell *trail, int trail_len, int matches, SelectMode mode, int *found) {
	select_walk(interp, tree_node, path, depth, trail, trail_len, matches, mode, found);
	if (mode == SELECT_EXISTS && *found)
		return;
	if (VAL_TAG(tree_node) != T_FRAME)
		return;
	if (trail_len >= SELECT_MAX_DEPTH) {
		fail(interp, "structure too deeply nested (cycle?)");
		return;
	}

	Object *frame = OBJECT_AT(VAL_DATA(tree_node));
	for (int i = 0; i < frame->len; i++) {
		trail[trail_len] = frame->frame.keys[i];
		select_descendants(interp, frame->frame.values[i], path, depth, trail, trail_len + 1, matches, mode, found);
		if (mode == SELECT_EXISTS && *found)
			return;
	}
}


void p_merge(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val right = chain_sp[-1];
	Val left = chain_sp[-2];
	if (VAL_TAG(right) != T_FRAME || VAL_TAG(left) != T_FRAME) {
		Val offender = VAL_TAG(right) != T_FRAME ? right : left;
		fail(interp, "expected %s; got %s", tag_name(T_FRAME), tag_name(VAL_TAG(offender)));
		return;
	}
	Object *right_frame = OBJECT_AT(VAL_DATA(right));
	Object *left_frame = OBJECT_AT(VAL_DATA(left));

	NEW_FRAME(result_handle, result);
	frame_reserve(result, left_frame->len + right_frame->len);

	int i = 0;
	int j = 0;
	int n = 0;
	while (i < left_frame->len && j < right_frame->len) {
		cell left_key = left_frame->frame.keys[i];
		cell right_key = right_frame->frame.keys[j];
		if (left_key < right_key) {
			result->frame.keys[n] = left_key;
			result->frame.values[n] = left_frame->frame.values[i];
			i++;
		} else if (left_key > right_key) {
			result->frame.keys[n] = right_key;
			result->frame.values[n] = right_frame->frame.values[j];
			j++;
		} else {
			result->frame.keys[n] = right_key;
			result->frame.values[n] = right_frame->frame.values[j];
			i++;
			j++;
		}
		n++;
	}

	while (i < left_frame->len) {
		result->frame.keys[n] = left_frame->frame.keys[i];
		result->frame.values[n] = left_frame->frame.values[i];
		i++;
		n++;
	}

	while (j < right_frame->len) {
		result->frame.keys[n] = right_frame->frame.keys[j];
		result->frame.values[n] = right_frame->frame.values[j];
		j++;
		n++;
	}

	result->len = n;

	chain_sp[-2] = make_frame(result_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_frame(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val values_val = chain_sp[-1];
	if (VAL_TAG(values_val) != T_ARRAY) {
		fail(interp, "expected %s; got %s", tag_name(T_ARRAY), tag_name(VAL_TAG(values_val)));
		return;
	}
	Val keys_val = chain_sp[-2];
	REQUIRE_CHAIN_SEQUENCE(keys_val);
	Object *values = OBJECT_AT(VAL_DATA(values_val));
	Object *keys = OBJECT_AT(VAL_DATA(keys_val));
	if (keys->len != values->len) {
		fail(interp, "keys and values must be the same length (%d vs %d)", keys->len, values->len);
		return;
	}
	for (int i = 0; i < keys->len; i++) {
		if (VAL_TAG(keys->items[i]) != T_SYMBOL) {
			fail(interp, "keys must be symbols; got %s", tag_name(VAL_TAG(keys->items[i])));
			return;
		}
	}

	NEW_FRAME(frame_handle, frame);
	frame_reserve(frame, keys->len);
	for (int i = 0; i < keys->len; i++) {
		frame->frame.keys[i] = VAL_DATA(keys->items[i]);
		frame->frame.values[i] = values->items[i];
	}
	frame->len = keys->len;
	frame_sort_dedup(frame);

	chain_sp[-2] = make_frame(frame_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_has(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	if (VAL_TAG(chain_sp[-2]) == T_FRAME && VAL_TAG(chain_sp[-1]) == T_SYMBOL) {
		Object *lookup_frame = OBJECT_AT(VAL_DATA(chain_sp[-2]));
		FRAME_LOOKUP(lookup_frame, VAL_DATA(chain_sp[-1]), at, present);
		(void)at;
		chain_sp[-2] = make_bool(present);
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
	}

	if (VAL_TAG(chain_sp[-2]) == T_STRING) {
		Val pattern_val = chain_sp[-1];
		if (VAL_TAG(pattern_val) != T_STRING) {
			fail(interp, "expected %s; got %s", tag_name(T_STRING), tag_name(VAL_TAG(pattern_val)));
			return;
		}
		Object *pattern = OBJECT_AT(VAL_DATA(pattern_val));
		Object *subject = OBJECT_AT(VAL_DATA(chain_sp[-2]));

		int matched = string_matches(interp, subject, pattern);
		if (interp->error_flag)
			return;
		chain_sp[-2] = make_bool(matched);

		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
	}

	Val frame_val = chain_sp[-2];
	if (VAL_TAG(frame_val) != T_FRAME) {
		fail(interp, "expected %s; got %s", tag_name(T_FRAME), tag_name(VAL_TAG(frame_val)));
		return;
	}
	Val key_or_path = chain_sp[-1];

	if (VAL_TAG(key_or_path) != T_ARRAY) {
		fail(interp, "expected a symbol or path (array of symbols); got %s", tag_name(VAL_TAG(key_or_path)));
		return;
	}

	int is_search;
	Object *path = frame_path_resolve(interp, key_or_path, 1, &is_search);
	if (!path)
		return;
	REQUIRE_NONEMPTY_PATH(path, "has?");

	int found;
	if (is_search) {
		found = 0;
		cell trail[SELECT_MAX_DEPTH];
		select_walk(interp, frame_val, path, 0, trail, 0, -1, SELECT_EXISTS, &found);
		if (interp->error_flag)
			return;
	} else {
		frame_walk(interp, frame_val, path, path->len, WALK_PROBE, &found);
	}
	chain_sp[-2] = make_bool(found);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_update_at(DISPATCH_ARGS) {
	PEEK_TYPE_AT(frame_val, 2, "update-at", T_FRAME);
	PEEK_AT(key_or_path, 1, "update-at");
	PEEK_AT(xt, 0, "update-at");
	if (VAL_TAG(xt) != T_XT) {
		fail(interp, "xt required on stack; got %s", tag_name(VAL_TAG(xt)));
		return;
	}
	Object *frame = OBJECT_AT(VAL_DATA(frame_val));

	DISPATCH_SYMBOL_OR_PATH(key_or_path, "update-at", {
			FRAME_LOOKUP(frame, VAL_DATA(key_or_path), at, present);
			if (!present) {
			fail(interp, "no key :%s", &vocab.symbol_pool[VAL_DATA(key_or_path)]);
			return;
			}
			push(interp, frame->frame.values[at]);
			execute_xt(interp, (int)VAL_DATA(xt));
			if (interp->error_flag) return;
			frame_put(frame, VAL_DATA(key_or_path), pop(interp));
			interp->dsp -= 2;
			}, {
			REQUIRE_NONEMPTY_PATH(path, "update-at");
			if (leaf_is_axis(interp, VAL_DATA(path->items[path->len - 1]))) return;
			Val parent = frame_walk(interp, frame_val, path, path->len - 1, WALK_ERROR, NULL);
			if (interp->error_flag) return;
			if (VAL_TAG(parent) != T_FRAME) {
			fail(interp, "parent is not a frame; got %s", tag_name(VAL_TAG(parent)));
			return;
			}
			Object *parent_obj = OBJECT_AT(VAL_DATA(parent));
			cell leaf = VAL_DATA(path->items[path->len - 1]);
			FRAME_LOOKUP(parent_obj, leaf, at, present);
			if (!present) {
				fail(interp, "no key :%s", &vocab.symbol_pool[leaf]);
				return;
			}
			push(interp, parent_obj->frame.values[at]);
			execute_xt(interp, (int)VAL_DATA(xt));
			if (interp->error_flag) return;
			frame_put(parent_obj, leaf, pop(interp));
			interp->dsp -= 2;
			});

	DISPATCH(interp);
}

static Val frame_field(Val frame_val, cell col) {
	if (VAL_TAG(frame_val) != T_FRAME)
		return make_tagged(T_NONE, 0);

	Object *frame = OBJECT_AT(VAL_DATA(frame_val));
	FRAME_LOOKUP(frame, col, at, present);
	return present ? frame->frame.values[at] : make_tagged(T_NONE, 0);
}

static int row_cmp(void *interp, const void *left, const void *right) {
	return val_cmp((Interpreter *)interp, *(const Val *)left, *(const Val *)right);
}

void p_group_by(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val col_val = chain_sp[-1];
	if (VAL_TAG(col_val) != T_SYMBOL) {
		fail(interp, "expected %s; got %s", tag_name(T_SYMBOL), tag_name(VAL_TAG(col_val)));
		return;
	}
	cell col = VAL_DATA(col_val);
	Val rows_val = chain_sp[-2];
	if (VAL_TAG(rows_val) != T_ARRAY) {
		fail(interp, "expected %s; got %s", tag_name(T_ARRAY), tag_name(VAL_TAG(rows_val)));
		return;
	}
	int row_count = OBJECT_AT(VAL_DATA(rows_val))->len;

	int frame_handle = object_new_frame(interp);
	if (interp->error_flag)
		return;
	gc_root_push(interp, make_frame(frame_handle));
	if (interp->error_flag)
		return;


	for (int i = 0; i < row_count; i++) {
		Val row = OBJECT_AT(VAL_DATA(rows_val))->items[i];
		cell key = VAL_DATA(frame_field(row, col));

		Object *frame = OBJECT_AT(frame_handle);
		FRAME_LOOKUP(frame, key, at, present);
		int bag;
		if (present) {
			bag = (int)VAL_DATA(frame->frame.values[at]);
		} else {
			bag = object_new_array(interp, 0);
			if (interp->error_flag) {
				gc_root_pop(interp);
				return;
			}
			frame_put(OBJECT_AT(frame_handle), key, make_array(bag));
		}

		Object *bag_obj = OBJECT_AT(bag);
		GROW_IF_FULL(bag_obj->len, bag_obj->capacity, bag_obj->items);
		bag_obj->items[bag_obj->len++] = row;
	}


	Object *result = OBJECT_AT(frame_handle);
	for (int i = 0; i < result->len; i++) {
		int bag = (int)VAL_DATA(result->frame.values[i]);
		Object *bag_obj = OBJECT_AT(bag);
		platform_qsort_r(bag_obj->items, (size_t)bag_obj->len, sizeof(Val), interp, row_cmp);
		int unique = 0;
		for (int source = 0; source < bag_obj->len; source++)
			if (unique == 0 || val_cmp(interp, bag_obj->items[unique - 1], bag_obj->items[source]) != 0)
				bag_obj->items[unique++] = bag_obj->items[source];
		bag_obj->len = unique;
		bag_obj->kind = OBJECT_SET;
		result->frame.values[i] = make_set(bag);
	}

	Val grouped = interp->gc_roots[interp->n_gc_roots - 1];
	gc_root_pop(interp);
	chain_sp[-2] = grouped;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_destruct(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val source_val = chain_sp[-1];
	Tag tag = VAL_TAG(source_val);
	if (tag != T_ARRAY && tag != T_SET && tag != T_FRAME) {
		fail(interp, "expected array, set, or frame; got %s", tag_name(VAL_TAG(source_val)));
		return;
	}
	Object *source = OBJECT_AT(VAL_DATA(source_val));

	int n_pushed = tag == T_FRAME ? source->len * 2 : source->len;
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp - 1, n_pushed);

	Val *slot = chain_sp - 1;
	for (int i = 0; i < source->len; i++) {
		if (tag == T_FRAME) {
			*slot++ = make_symbol((int)source->frame.keys[i]);
			*slot++ = source->frame.values[i];
		} else
			*slot++ = source->items[i];
	}

	DISPATCH_REGISTERS(interp, chain_ip, slot);
}

void p_destruct_to(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val target_val = chain_sp[-1];
	Val source_val = chain_sp[-2];

	if (VAL_TAG(source_val) != T_ARRAY) {
			fail(interp, "source must be an array; got %s", tag_name(VAL_TAG(source_val)));
			return;
	}
	if (VAL_TAG(target_val) != T_ARRAY) {
		fail(interp, "target must be an array; got %s", tag_name(VAL_TAG(target_val)));
		return;
	}

	Object *source = OBJECT_AT(VAL_DATA(source_val));
	Object *target = OBJECT_AT(VAL_DATA(target_val));
	if (source->len != target->len) {
		fail(interp, "length mismatch (source %d, target %d)", source->len, target->len);
		return;
	}

	for (int i = 0; i < source->len; i++) {
		int var_cfa;

		Val item = target->items[i];
		if (VAL_TAG(item) == T_XT) {
			var_cfa = (int)VAL_DATA(item);
		} else if (VAL_TAG(item) == T_SYMBOL) {
			const char *name = &vocab.symbol_pool[VAL_DATA(item)];
			var_cfa = find(name);
			if (!var_cfa)
				var_cfa = create_variable(interp, name);

			target->items[i] = make_xt(var_cfa);
		} else {
			fail(interp, "target item at index %d must be symbol or xt; got %s",
					i, tag_name(VAL_TAG(item)));
			return;
		}

		if ((cfa_handler)vocab.dict[var_cfa] != dovar) {
			fail(interp, "target at index %d is not a variable", i);
			return;
		}

		vocab.dict[var_cfa + 1] = (cell)source->items[i].bits;
	}

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}

void p_slice_store(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 6);
	Val slen_val = chain_sp[-1];
	if (VAL_TAG(slen_val) != T_FLOAT) {
		fail(interp, "expected a float length; got %s", tag_name(VAL_TAG(slen_val)));
		return;
	}
	int slen = (int)VAL_NUMBER(slen_val);
	Val sstep_val = chain_sp[-2];
	if (VAL_TAG(sstep_val) != T_FLOAT) {
		fail(interp, "expected a float step; got %s", tag_name(VAL_TAG(sstep_val)));
		return;
	}
	int sstep = (int)VAL_NUMBER(sstep_val);
	Val sstart_val = chain_sp[-3];
	if (VAL_TAG(sstart_val) != T_FLOAT) {
		fail(interp, "expected a float source-start; got %s", tag_name(VAL_TAG(sstart_val)));
		return;
	}
	int sstart = (int)VAL_NUMBER(sstart_val);
	Val src_val = chain_sp[-4];
	if (VAL_TAG(src_val) != T_ARRAY && VAL_TAG(src_val) != T_SET) {
		fail(interp, "source must be an array or set; got %s", tag_name(VAL_TAG(src_val)));
		return;
	}
	Val tstart_val = chain_sp[-5];
	if (VAL_TAG(tstart_val) != T_FLOAT) {
		fail(interp, "expected a float target-start; got %s", tag_name(VAL_TAG(tstart_val)));
		return;
	}
	int tstart = (int)VAL_NUMBER(tstart_val);
	Val target_val = chain_sp[-6];
	if (VAL_TAG(target_val) != T_ARRAY) {
		fail(interp, "expected %s; got %s", tag_name(T_ARRAY), tag_name(VAL_TAG(target_val)));
		return;
	}

	Object *target = OBJECT_AT(VAL_DATA(target_val));
	Object *src    = OBJECT_AT(VAL_DATA(src_val));

	if (slen < 0) {
		fail(interp, "length must be non-negative; got %d", slen);
		return;
	}
	if (slen == 0)
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 5);

	if (tstart < 0 || tstart + slen > target->len) {
		fail(interp, "target [%d, %d) out of bounds for length %d",
			tstart, tstart + slen, target->len);
		return;
	}

	int s_first = sstart;
	int s_last  = sstart + (slen - 1) * sstep;
	int s_min = MIN(s_first, s_last);
	int s_max = MAX(s_first, s_last);
	if (s_min < 0 || s_max >= src->len) {
		fail(interp, "source indices [%d..%d] out of bounds for length %d",
			s_min, s_max, src->len);
		return;
	}

	if (target == src && sstep == -1 && sstart == tstart + slen - 1) {
		for (int i = 0; i < slen / 2; i++) {
			Val t = target->items[tstart + i];
			target->items[tstart + i] = target->items[tstart + slen - 1 - i];
			target->items[tstart + slen - 1 - i] = t;
		}
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 5);
	}

	if (target == src) {
		Val inline_staging[64];
		Val *staged = (slen <= 64) ? inline_staging : malloc(slen * sizeof(Val));
		if (!staged) {
			fail(interp, "out of memory");
			return;
		}
		for (int i = 0; i < slen; i++)
			staged[i] = src->items[sstart + i * sstep];
		for (int i = 0; i < slen; i++)
			target->items[tstart + i] = staged[i];
		if (staged != inline_staging) free(staged);
	} else {
		for (int i = 0; i < slen; i++)
			target->items[tstart + i] = src->items[sstart + i * sstep];
	}

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 5);
}

void p_to_slice(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3);
	Val n_val = chain_sp[-1];
	if (VAL_TAG(n_val) != T_FLOAT) {
		fail(interp, "expected a float count; got %s", tag_name(VAL_TAG(n_val)));
		return;
	}
	int n = (int)VAL_NUMBER(n_val);
	Val offset_val = chain_sp[-2];
	if (VAL_TAG(offset_val) != T_FLOAT) {
		fail(interp, "expected a float offset; got %s", tag_name(VAL_TAG(offset_val)));
		return;
	}
	int offset = (int)VAL_NUMBER(offset_val);
	Val target_val = chain_sp[-3];
	if (VAL_TAG(target_val) != T_ARRAY) {
		fail(interp, "expected %s; got %s", tag_name(T_ARRAY), tag_name(VAL_TAG(target_val)));
		return;
	}
	Object *target = OBJECT_AT(VAL_DATA(target_val));

	if (n < 0) {
		fail(interp, "count must be non-negative; got %d", n);
		return;
	}
	if (offset < 0 || offset + n > target->len) {
		fail(interp, "slice [%d, %d) out of bounds for length %d",
			offset, offset + n, target->len);
		return;
	}
	Val *slid_values = chain_sp - 3 - n;
	if (slid_values < interp->data_stack) {
		fail(interp, "stack underflow (need %d values plus target)", n);
		return;
	}
	for (int i = 0; i < n; i++)
		target->items[offset + i] = slid_values[i];
	slid_values[0] = target_val;

	DISPATCH_REGISTERS(interp, chain_ip, slid_values + 1);
}

typedef struct {
	const char *cursor;
	const char *end;
	int depth;
	char *scratch;
	int scratch_capacity;
} JSONParser;


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
					fail(interp, "truncated \\u escape");
					return -1;
				}
				int codepoint = json_hex4(raw);
				if (codepoint < 0) {
					fail(interp, "invalid \\u escape");
					return -1;
				}
				raw += 4;
				if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
					if (closing - raw < 6 || raw[0] != '\\' || raw[1] != 'u') {
						fail(interp, "unpaired surrogate");
						return -1;
					}
					int low = json_hex4(raw + 2);
					if (low < 0xDC00 || low > 0xDFFF) {
						fail(interp, "invalid surrogate pair");
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
				fail(interp, "unsupported string escape");
				return -1;
		}
	}
	return length;
}

static void json_parse_string(Interpreter *interp, JSONParser *parser, Val *destination) {
	const char *content = parser->cursor + 1;
	const char *closing = json_string_end(content, parser->end);
	if (closing >= parser->end) {
		fail(interp, "unterminated string");
		return;
	}

	int handle = object_new_string_uninit(interp, (int)(closing - content));
	if (interp->error_flag) return;

	Object *string = OBJECT_AT(handle);
	int length = json_decode_string(interp, content, closing, string->bytes);
	if (length < 0) return;
	string->len = length;
	string->bytes[length] = 0;

	*destination = make_string(handle);

	parser->cursor = closing + 1;
}

static void json_parse_array(Interpreter *interp, JSONParser *parser, Val *destination) {
	if (++parser->depth > JSON_MAX_DEPTH) {
		fail(interp, "nesting too deep");
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
		Object *array = OBJECT_AT(handle);
		GROW_IF_FULL(array->len, array->capacity, array->items);

		array->items[array->len] = make_tagged(T_NONE, 0);
		array->len++;

		json_parse_value(interp, parser, &OBJECT_AT(handle)->items[array->len - 1]);
		if (interp->error_flag) return;

		json_skip_whitespace(parser);
		if (parser->cursor >= parser->end) {
			fail(interp, "unterminated array");
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
		fail(interp, "expected ',' or ']' in array");
		return;
	}

	parser->depth--;
}

static void json_parse_object(Interpreter *interp, JSONParser *parser, Val *destination) {
	if (++parser->depth > JSON_MAX_DEPTH) {
		fail(interp, "nesting too deep");
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
			fail(interp, "expected string key in object");
			return;
		}

		const char *key_content = parser->cursor + 1;
		const char *key_closing = json_string_end(key_content, parser->end);
		if (key_closing >= parser->end) {
			fail(interp, "unterminated string");
			return;
		}

		int key_span = (int)(key_closing - key_content);
		if (key_span + 1 > parser->scratch_capacity) {
			int capacity = parser->scratch_capacity < 64 ? 64 : parser->scratch_capacity;
			while (capacity < key_span + 1) capacity *= 2;
			char *grown_scratch = realloc(parser->scratch, (size_t)capacity);
			if (!grown_scratch) {
				fail(interp, "out of memory");
				return;
			}
			parser->scratch = grown_scratch;
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
			fail(interp, "expected ':' after key");
			return;
		}
		parser->cursor++;

		Object *frame = OBJECT_AT(handle);
		frame_reserve(frame, frame->len + 1);

		frame->frame.keys[frame->len] = key_symbol;
		frame->frame.values[frame->len] = make_tagged(T_NONE, 0);
		frame->len++;

		json_parse_value(interp, parser, &OBJECT_AT(handle)->frame.values[frame->len - 1]);
		if (interp->error_flag) return;

		json_skip_whitespace(parser);
		if (parser->cursor >= parser->end) {
			fail(interp, "unterminated object");
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
		fail(interp, "expected ',' or '}' in object");
		return;
	}

	frame_sort_dedup(OBJECT_AT(handle));

	parser->depth--;
}

static void json_parse_value(Interpreter *interp, JSONParser *parser, Val *destination) {
	json_skip_whitespace(parser);
	if (parser->cursor >= parser->end) {
		fail(interp, "unexpected end of input");
		return;
	}

	switch (*parser->cursor) {
		case 'n':
			if (parser->end - parser->cursor >= 4 && memcmp(parser->cursor, "null", 4) == 0) {
				parser->cursor += 4;
				*destination = make_tagged(T_NONE, 0);
				return;
			}
			fail(interp, "invalid literal");
			return;
		case 't':
			if (parser->end - parser->cursor >= 4 && memcmp(parser->cursor, "true", 4) == 0) {
				parser->cursor += 4;
				*destination = make_symbol(vocab.true_symbol);
				return;
			}
			fail(interp, "invalid literal");
			return;
		case 'f':
			if (parser->end - parser->cursor >= 5 && memcmp(parser->cursor, "false", 5) == 0) {
				parser->cursor += 5;
				*destination = make_symbol(vocab.false_symbol);
				return;
			}
			fail(interp, "invalid literal");
			return;
		case '-':
		case '0': case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9': {
			char *number_end;
			double number = strtod(parser->cursor, &number_end);
			if (number_end == parser->cursor) {
				fail(interp, "invalid number");
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
			fail(interp, "unexpected character");
			return;
	}
}

void p_json_to_frame(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val json_val = chain_sp[-1];
	if (VAL_TAG(json_val) != T_STRING) {
		fail(interp, "expected %s; got %s", tag_name(T_STRING), tag_name(VAL_TAG(json_val)));
		return;
	}
	Object *json = OBJECT_AT(VAL_DATA(json_val));

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
		fail(interp, "trailing content after value");
		return;
	}

	chain_sp[-1] = parsed;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

typedef struct {
	char *buffer;
	int length;
	int capacity;
	int failed;
} JSONWriter;

static void json_write_bytes(JSONWriter *writer, const char *bytes, int n) {
	if (writer->failed)
		return;

	if (writer->length + n > writer->capacity) {
		int capacity = writer->capacity < 64 ? 64 : writer->capacity;
		while (capacity < writer->length + n) capacity *= 2;
		char *grown_buffer = realloc(writer->buffer, (size_t)capacity);
		if (!grown_buffer) {
			writer->failed = 1;
			return;
		}
		writer->buffer = grown_buffer;
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
		fail(interp, "nesting too deep");
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
			if ((int)VAL_DATA(value) == vocab.true_symbol)
				json_write_bytes(writer, "true", 4);
			else if ((int)VAL_DATA(value) == vocab.false_symbol)
				json_write_bytes(writer, "false", 5);
			else
				fail(interp, "cannot serialize a non-boolean symbol");
			return;
		case T_STRING: {
			Object *string = OBJECT_AT(VAL_DATA(value));
			json_write_string(writer, string->bytes, string->len);
			return;
		}
		case T_ARRAY: {
			Object *array = OBJECT_AT(VAL_DATA(value));
			json_write_byte(writer, '[');
			for (int i = 0; i < array->len; i++) {
				if (i > 0) json_write_bytes(writer, ", ", 2);
				json_write_value(interp, writer, array->items[i], depth + 1);
			}
			json_write_byte(writer, ']');
			return;
		}
		case T_FRAME: {
			Object *frame = OBJECT_AT(VAL_DATA(value));
			json_write_byte(writer, '{');
			for (int i = 0; i < frame->len; i++) {
				if (i > 0) json_write_bytes(writer, ", ", 2);
				const char *key = &vocab.symbol_pool[frame->frame.keys[i]];
				json_write_string(writer, key, (int)strlen(key));
				json_write_bytes(writer, ": ", 2);
				json_write_value(interp, writer, frame->frame.values[i], depth + 1);
			}
			json_write_byte(writer, '}');
			return;
		}
		default:
			fail(interp, "cannot serialize %s", tag_name(VAL_TAG(value)));
			return;
	}
}

void p_frame_to_json(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val value = chain_sp[-1];

	JSONWriter writer;
	writer.buffer = NULL;
	writer.length = 0;
	writer.capacity = 0;
	writer.failed = 0;

	json_write_value(interp, &writer, value, 0);
	if (interp->error_flag) {
		free(writer.buffer);
		return;
	}
	if (writer.failed) {
		free(writer.buffer);
		fail(interp, "out of memory");
		return;
	}

	int handle = object_new_string(interp, writer.buffer ? writer.buffer : "", writer.length);
	free(writer.buffer);
	if (interp->error_flag) return;

	chain_sp[-1] = make_string(handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

