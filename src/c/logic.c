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
