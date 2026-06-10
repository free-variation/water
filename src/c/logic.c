#include "logicforth.h"

void p_lvar(Interpreter *interp) {
	int handle = object_new_logic_var(interp);
	if (interp->error_flag) return;

	push(interp, make_logic_var(handle));

	DISPATCH(interp);
}

Val deref(Interpreter *interp, Val value) {
	while (VAL_TAG(value) == T_LOGIC_VAR) {
		Object *var = interp->objects[VAL_DATA(value)];
		if (VAL_TAG(var->logic_var.binding) == T_NONE) break;
		value = var->logic_var.binding;
	}
	return value;
}

static void bind_var(Interpreter *interp, int var_handle, Val value) {
	if (interp->trail_top == interp->trail_cap) {
		interp->trail_cap *= 2;
		interp->trail = realloc(interp->trail, sizeof(int) * (size_t)interp->trail_cap);
	}

	interp->objects[var_handle]->logic_var.binding = value;
	interp->trail[interp->trail_top++] = var_handle;
}

void p_trail_mark(Interpreter *interp) {
	push(interp, make_float((double)interp->trail_top));

	DISPATCH(interp);
}

void p_trail_undo(Interpreter *interp) {
	POP_INT(mark, "trail-undo", "mark");

	while (interp->trail_top > mark) {
		int var_handle = interp->trail[--interp->trail_top];
		interp->objects[var_handle]->logic_var.binding = make_tagged(T_NONE, 0);
	}

	DISPATCH(interp);
}

int unify(Interpreter *interp, Val left_val, Val right_val) {
	left_val = deref(interp, left_val);
	right_val = deref(interp, right_val);

	if (VAL_TAG(left_val) == T_LOGIC_VAR && VAL_TAG(right_val) == T_LOGIC_VAR
			&& VAL_DATA(left_val) == VAL_DATA(right_val))
		return 1;
	
	if (VAL_TAG(left_val) == T_LOGIC_VAR) {
		bind_var(interp, (int)VAL_DATA(left_val), right_val);
		return 1;
	}

	if (VAL_TAG(right_val) == T_LOGIC_VAR) {
		bind_var(interp, (int)VAL_DATA(right_val), left_val);
		return 1;
	}

	if (VAL_TAG(left_val) == T_ARRAY && VAL_TAG(right_val) == T_ARRAY) {
		Object *left = interp->objects[VAL_DATA(left_val)];
		Object *right = interp->objects[VAL_DATA(right_val)];

		if (left->len != right->len)
			return 0;
		for (int i = 0; i < left->len; i++) 
			if (!unify(interp, left->items[i], right->items[i]))
				return 0;
		return 1;
	}

	if (VAL_TAG(left_val) == T_FRAME && VAL_TAG(right_val) == T_FRAME) {
		Object *left = interp->objects[VAL_DATA(left_val)];
		Object *right = interp->objects[VAL_DATA(right_val)];
		
		int i = 0, j = 0;
		while (i < left->len && j < right->len) {
			cell left_key = left->frame.keys[i];
			cell right_key = right->frame.keys[j];
			if (left_key == right_key) {
				if (!unify(interp, left->frame.values[i], right->frame.values[j]))
					return 0;
				i++;
				j++;
			} else if (left_key < right_key) 
				i++;
			else 
				j++;
		}
		return 1;
	}

	return val_cmp(interp, left_val, right_val) == 0;
}

void p_unify(Interpreter *interp) {
	POP(right);
	POP(left);

	if (unify(interp, left, right))
		push(interp, deref(interp, left));
	else
		backtrack(interp);

	DISPATCH(interp);
}

void p_deref(Interpreter *interp) {
	POP(value);
	push(interp, deref(interp, value));

	DISPATCH(interp);
}
