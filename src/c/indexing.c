
#include "water.h"

static inline __attribute__((always_inline)) Val *array_index_fetch(Interpreter *interp, cell *sync_ip, Val *sp, Val source_val, int index) {
	if (VAL_TAG(source_val) == T_ARRAY) {
		Object *array = OBJECT_AT(VAL_DATA(source_val));
		if (index < 0 || index >= array->len) {
			SYNC_REGISTERS(interp, sync_ip, sp);
			fail(interp, "array index %d out of bounds (length %d)", index, array->len);
			return NULL;
		}
		*sp = array->items[index];
		return sp + 1;
	}
	if (VAL_TAG(source_val) == T_MATRIX) {
		Object *source = OBJECT_AT(VAL_DATA(source_val));
		if (index < 0 || index >= source->matrix.rows) {
			SYNC_REGISTERS(interp, sync_ip, sp);
			fail(interp, "row index %d out of bounds (%d rows)", index, source->matrix.rows);
			return NULL;
		}

		int num_columns = source->matrix.columns;
		int row_handle = object_new_matrix(interp, 1, num_columns);
		if (interp->error_flag)
			return NULL;
		Object *row = OBJECT_AT(row_handle);
		for (int j = 0; j < num_columns; j++)
			MAT(row, 0, j) = MAT(source, index, j);

		*sp = make_matrix(row_handle);
		return sp + 1;
	}
	if (VAL_TAG(source_val) == T_SEGMENT) {
		Object *segment = OBJECT_AT(VAL_DATA(source_val));
		if (index < 0 || index >= segment->segment.length) {
			SYNC_REGISTERS(interp, sync_ip, sp);
			fail(interp, "segment index %d out of bounds (length %d)", index, segment->segment.length);
			return NULL;
		}
		*sp = make_float(segment_get(segment, index));
		return sp + 1;
	}
	fail(interp, "expected an array or matrix; got %s", tag_name(VAL_TAG(source_val)));
	return NULL;
}


void p_at_i_array(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);

	Val source_val = chain_sp[-2];
	Val index_val = chain_sp[-1];
	if (VAL_TAG(source_val) != T_ARRAY || VAL_TAG(index_val) != T_FLOAT) {
		RETARGET_OP(p_at_i);
		MUSTTAIL return p_at_i(interp, chain_ip, chain_sp);
	}

	Object *array = OBJECT_AT(VAL_DATA(source_val));
	int index = (int)VAL_NUMBER(index_val);
	if (index <0 || index >= array->len) {
		fail(interp, "array index %d out of bounds (length %d)", index, array->len);
		return;
	}

	chain_sp[-2] = array->items[index];

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_at_i_segment(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);

	Val source_val = chain_sp[-2];
	Val index_val = chain_sp[-1];
	if (VAL_TAG(source_val) != T_SEGMENT || VAL_TAG(index_val) != T_FLOAT) {
		RETARGET_OP(p_at_i);
		MUSTTAIL return p_at_i(interp, chain_ip, chain_sp);
	}

	Object *segment = OBJECT_AT(VAL_DATA(source_val));
	int index = (int)VAL_NUMBER(index_val);
	if (index < 0 || index >= segment->segment.length) {
		fail(interp, "segment index %d out of bounds (length %d)", index, segment->segment.length);
		return;
	}

	chain_sp[-2] = make_float(segment_get(segment, index));

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_at_i(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val index_val = chain_sp[-1];
	if (VAL_TAG(index_val) != T_FLOAT) {
		fail(interp, "expected a float index; got %s", tag_name(VAL_TAG(index_val)));
		return;
	}


	if (VAL_TAG(chain_sp[-2]) == T_ARRAY)
		RETARGET_OP(p_at_i_array);
	else if (VAL_TAG(chain_sp[-2]) == T_SEGMENT)
		RETARGET_OP(p_at_i_segment);
	Val *pushed_sp = array_index_fetch(interp, chain_ip, chain_sp - 2, chain_sp[-2], (int)VAL_NUMBER(index_val));
	if (!pushed_sp)
		return;
	DISPATCH_REGISTERS(interp, chain_ip, pushed_sp);
}

void p_at_i_local0(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip + 1, chain_sp, 1);
	int index = (int)interp->return_stack[interp->local_base + (int)chain_ip[0]].number;
	Val *pushed_sp = array_index_fetch(interp, chain_ip + 1, chain_sp - 1, chain_sp[-1], index);
	if (!pushed_sp)
		return;
	DISPATCH_REGISTERS(interp, chain_ip + 1, pushed_sp);
}

void p_at_i_swap_local0(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip + 1, chain_sp, 1);
	Val index_val = chain_sp[-1];
	if (VAL_TAG(index_val) != T_FLOAT) {
		SYNC_REGISTERS(interp, chain_ip + 1, chain_sp);
		fail(interp, "expected a float index; got %s", tag_name(VAL_TAG(index_val)));
		return;
	}

	Val source_val = interp->return_stack[interp->local_base + (int)chain_ip[0]];
	Val *pushed_sp = array_index_fetch(interp, chain_ip + 1, chain_sp - 1, source_val, (int)VAL_NUMBER(index_val));
	if (!pushed_sp)
		return;
	DISPATCH_REGISTERS(interp, chain_ip + 1, pushed_sp);
}

void p_at_i_swap_local1(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip + 1, chain_sp, 1);
	Val index_val = chain_sp[-1];
	if (VAL_TAG(index_val) != T_FLOAT) {
		SYNC_REGISTERS(interp, chain_ip + 1, chain_sp);
		fail(interp, "expected a float index; got %s", tag_name(VAL_TAG(index_val)));
		return;
	}

	int enclosing = saved_local_base(interp->return_stack[interp->local_base - 1]);
	Val source_val = interp->return_stack[enclosing + (int)chain_ip[0]];
	Val *pushed_sp = array_index_fetch(interp, chain_ip + 1, chain_sp - 1, source_val, (int)VAL_NUMBER(index_val));
	if (!pushed_sp)
		return;
	DISPATCH_REGISTERS(interp, chain_ip + 1, pushed_sp);
}

void p_at_i_lit(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip + 1, chain_sp, 1);
	Val *pushed_sp = array_index_fetch(interp, chain_ip + 1, chain_sp - 1, chain_sp[-1], (int)chain_ip[0]);
	if (!pushed_sp)
		return;
	DISPATCH_REGISTERS(interp, chain_ip + 1, pushed_sp);
}

void p_at_i_lit_local0(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip + 2, chain_sp, 1);
	Val source_val = interp->return_stack[interp->local_base + (int)chain_ip[0]];
	Val *pushed_sp = array_index_fetch(interp, chain_ip + 2, chain_sp, source_val, (int)chain_ip[1]);
	if (!pushed_sp)
		return;
	DISPATCH_REGISTERS(interp, chain_ip + 2, pushed_sp);
}

void p_at_i_ll0(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip + 2, chain_sp, 1);
	Val *locals = interp->return_stack + interp->local_base;
	Val source_val = locals[(int)chain_ip[0]];
	int index = (int)locals[(int)chain_ip[1]].number;
	Val *pushed_sp = array_index_fetch(interp, chain_ip + 2, chain_sp, source_val, index);
	if (!pushed_sp)
		return;
	DISPATCH_REGISTERS(interp, chain_ip + 2, pushed_sp);
}

void p_at_i_l1l0(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip + 2, chain_sp, 1);
	int enclosing = saved_local_base(interp->return_stack[interp->local_base - 1]);
	Val source_val = interp->return_stack[enclosing + (int)chain_ip[0]];
	int index = (int)interp->return_stack[interp->local_base + (int)chain_ip[1]].number;
	Val *pushed_sp = array_index_fetch(interp, chain_ip + 2, chain_sp, source_val, index);
	if (!pushed_sp)
		return;
	DISPATCH_REGISTERS(interp, chain_ip + 2, pushed_sp);
}

void p_gather_local0(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip + 1, chain_sp, 2);
	int row_index = (int)interp->return_stack[interp->local_base + (int)chain_ip[0]].number;
	Val index_array = chain_sp[-1];
	Val value_array = chain_sp[-2];

	double gathered_index;
	if (VAL_TAG(index_array) == T_SEGMENT) {
		Object *segment = OBJECT_AT(VAL_DATA(index_array));
		if (row_index < 0 || row_index >= segment->segment.length) {
			SYNC_REGISTERS(interp, chain_ip + 1, chain_sp);
			fail(interp, "segment index %d out of bounds (length %d)", row_index, segment->segment.length);
			return;
		}
		gathered_index = segment_get(segment, row_index);
	} else if (VAL_TAG(index_array) == T_ARRAY) {
		Object *array = OBJECT_AT(VAL_DATA(index_array));
		if (row_index < 0 || row_index >= array->len) {
			SYNC_REGISTERS(interp, chain_ip + 1, chain_sp);
			fail(interp, "array index %d out of bounds (length %d)", row_index, array->len);
			return;
		}
		Val element = array->items[row_index];
		if (VAL_TAG(element) != T_FLOAT) {
			SYNC_REGISTERS(interp, chain_ip + 1, chain_sp);
			fail(interp, "gather index must be a number; got %s", tag_name(VAL_TAG(element)));
			return;
		}
		gathered_index = VAL_NUMBER(element);
	} else {
		SYNC_REGISTERS(interp, chain_ip + 1, chain_sp);
		fail(interp, "expected an array or segment; got %s", tag_name(VAL_TAG(index_array)));
		return;
	}

	Val *pushed_sp = array_index_fetch(interp, chain_ip + 1, chain_sp - 2, value_array, (int)gathered_index);
	if (!pushed_sp)
		return;
	DISPATCH_REGISTERS(interp, chain_ip + 1, pushed_sp);
}

static inline __attribute__((always_inline)) int array_element_store(Interpreter *interp, cell *sync_ip, Val *sp, Val target_val, Val index_val, Val value) {
	if (VAL_TAG(index_val) != T_FLOAT) {
		SYNC_REGISTERS(interp, sync_ip, sp);
		fail(interp, "expected a float index; got %s", tag_name(VAL_TAG(index_val)));
		return 0;
	}
	int index = (int)VAL_NUMBER(index_val);

	if (VAL_TAG(target_val) == T_ARRAY) {
		Object *array = OBJECT_AT(VAL_DATA(target_val));
		if (index < 0 || index >= array->len) {
			SYNC_REGISTERS(interp, sync_ip, sp);
			fail(interp, "array index %d out of bounds (length %d)", index, array->len);
			return 0;
		}
		array->items[index] = value;
		return 1;
	}
	if (VAL_TAG(target_val) == T_SEGMENT) {
		if (VAL_TAG(value) != T_FLOAT) {
			SYNC_REGISTERS(interp, sync_ip, sp);
			fail(interp, "segment stores a float; got %s", tag_name(VAL_TAG(value)));
			return 0;
		}
		Object *segment = OBJECT_AT(VAL_DATA(target_val));
		segment_set(segment, index, VAL_NUMBER(value));
		return 1;
	}
	SYNC_REGISTERS(interp, sync_ip, sp);
	fail(interp, "expected an array or segment; got %s", tag_name(VAL_TAG(target_val)));
	return 0;
}

static inline __attribute__((always_inline)) Val *array_element_store_fast(Interpreter *interp, cell *chain_ip, Val *chain_sp) {
	Val target_val = chain_sp[-3];
	Val index_val = chain_sp[-2];
	if (VAL_TAG(target_val) != T_ARRAY || VAL_TAG(index_val) != T_FLOAT)
		return NULL;
	Object *array = OBJECT_AT(VAL_DATA(target_val));
	int index = (int)VAL_NUMBER(index_val);
	if (index < 0 || index >= array->len) {
		SYNC_REGISTERS(interp, chain_ip, chain_sp);
		fail(interp, "array index %d out of bounds (length %d)", index, array->len);
		return NULL;
	}
	array->items[index] = chain_sp[-1];
	return chain_sp;
}

#define STORE_I_ARRAY_OP(c_name, generic, n_consumed) \
	void c_name(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3); \
		if (!array_element_store_fast(interp, chain_ip, chain_sp)) { \
			if (interp->error_flag) \
				return; \
			RETARGET_OP(generic); \
			MUSTTAIL return generic(interp, chain_ip, chain_sp); \
		} \
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - (n_consumed)); \
	}

STORE_I_ARRAY_OP(p_store_i_array, p_store_i, 2)
STORE_I_ARRAY_OP(p_store_i_drop_array, p_store_i_drop, 3)

#define STORE_I_OP(c_name, quickened, n_consumed) \
	void c_name(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 3); \
		if (VAL_TAG(chain_sp[-3]) == T_ARRAY) \
			RETARGET_OP(quickened); \
		if (!array_element_store(interp, chain_ip, chain_sp, chain_sp[-3], chain_sp[-2], chain_sp[-1])) \
			return; \
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - (n_consumed)); \
	}

STORE_I_OP(p_store_i, p_store_i_array, 2)
STORE_I_OP(p_store_i_drop, p_store_i_drop_array, 3)

#define ARRAY_INPLACE_OP(fn, word, n_operands, delta_expr, combine) \
void fn(DISPATCH_ARGS) { \
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, n_operands); \
	Val target_val = chain_sp[-(n_operands)]; \
	int index = (int)VAL_NUMBER(chain_sp[-(n_operands) + 1]); \
	double delta = delta_expr; \
	if (VAL_TAG(target_val) == T_ARRAY) { \
		Object *array = OBJECT_AT(VAL_DATA(target_val)); \
		if (index < 0 || index >= array->len) { \
			SYNC_REGISTERS(interp, chain_ip, chain_sp); \
			fail(interp, "array index %d out of bounds (length %d)", index, array->len); \
			return; \
		} \
		Val *element = &array->items[index]; \
		if (VAL_TAG(*element) != T_FLOAT) { \
			SYNC_REGISTERS(interp, chain_ip, chain_sp); \
			fail(interp, "array element is not a float; got %s", tag_name(VAL_TAG(*element))); \
			return; \
		} \
		*element = make_float(VAL_NUMBER(*element) combine delta); \
	} else if (VAL_TAG(target_val) == T_SEGMENT) { \
		Object *segment = OBJECT_AT(VAL_DATA(target_val)); \
		if (index < 0 || index >= segment->segment.length) { \
			SYNC_REGISTERS(interp, chain_ip, chain_sp); \
			fail(interp, "segment index %d out of bounds (length %d)", index, segment->segment.length); \
			return; \
		} \
		segment_set(segment, index, segment_get(segment, index) combine delta); \
	} else { \
		SYNC_REGISTERS(interp, chain_ip, chain_sp); \
		fail(interp, "expected an array or segment; got %s", tag_name(VAL_TAG(target_val))); \
		return; \
	} \
	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - (n_operands)); \
}

ARRAY_INPLACE_OP(p_inc_store_i, "(inc!i)", 2, 1.0, +)
ARRAY_INPLACE_OP(p_dec_store_i, "(dec!i)", 2, -1.0, +)
ARRAY_INPLACE_OP(p_add_store_i, "(+!i)", 3, VAL_NUMBER(chain_sp[-1]), +)
ARRAY_INPLACE_OP(p_sub_store_i, "(-!i)", 3, VAL_NUMBER(chain_sp[-1]), -)
ARRAY_INPLACE_OP(p_mul_store_i, "(*!i)", 3, VAL_NUMBER(chain_sp[-1]), *)
ARRAY_INPLACE_OP(p_div_store_i, "(/!i)", 3, VAL_NUMBER(chain_sp[-1]), /)
