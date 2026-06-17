#include "logicforth.h"

void p_lvar(Interpreter *interp) {
	int handle = object_new_logic_var(interp);
	if (interp->error_flag) return;

	push(interp, make_logic_var(handle));

	DISPATCH(interp);
}

Val deref(Interpreter *interp, Val value) {
	while (VAL_TAG(value) == T_LOGIC_VAR) {
		Val binding = interp->lvar_stack[VAL_DATA(value)];
		if (VAL_TAG(binding) == T_UNBOUND)
			break;
		value = binding;
	}
	return value;
}

static void bind_var(Interpreter *interp, int var_handle, Val value) {
	GROW_IF_FULL_SYS(interp->bind_trail_top, interp->bind_trail_cap, interp->bind_trail);

	interp->lvar_stack[var_handle] = value;
	interp->bind_trail[interp->bind_trail_top++] = var_handle;
}

static void trail_undo_to(Interpreter *interp, int mark) {
	while (interp->bind_trail_top > mark) {
		int var_handle = interp->bind_trail[--interp->bind_trail_top];
		interp->lvar_stack[var_handle] = make_tagged(T_UNBOUND, 0);
	}
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

	if (VAL_TAG(left_val) == T_UNBOUND || VAL_TAG(right_val) == T_UNBOUND)
		return 1;

	if (VAL_TAG(left_val) == T_ARRAY && VAL_TAG(right_val) == T_ARRAY) {
		Object *left = OBJECT_AT(VAL_DATA(left_val));
		Object *right = OBJECT_AT(VAL_DATA(right_val));

		if (left->len != right->len)
			return 0;
		int n = left->len;
		if (n == 0)
			return 1;
		for (int i = 0; i < n - 1; i++)
			if (!unify(interp, left->items[i], right->items[i]))
				return 0;
		__attribute__((musttail)) return unify(interp, left->items[n - 1], right->items[n - 1]);
	}

	if (VAL_TAG(left_val) == T_PAIR && VAL_TAG(right_val) == T_PAIR) {
		Pair *left = &pairs.table[VAL_DATA(left_val)];
		Pair *right = &pairs.table[VAL_DATA(right_val)];

		if (!unify(interp, left->head, right->head))
			return 0;
		__attribute__((musttail)) return unify(interp, left->tail, right->tail);
	}

	if (VAL_TAG(left_val) == T_FRAME && VAL_TAG(right_val) == T_FRAME) {
		Object *left = OBJECT_AT(VAL_DATA(left_val));
		Object *right = OBJECT_AT(VAL_DATA(right_val));

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

void p_matches(Interpreter *interp) {
	POP(row);
	POP(pattern);

	int trail_mark = interp->bind_trail_top;
	int matched = unify(interp, pattern, row);
	trail_undo_to(interp, trail_mark);

	push(interp, make_bool(matched));

	DISPATCH(interp);
}

void p_deref(Interpreter *interp) {
	POP(value);
	push(interp, deref(interp, value));

	DISPATCH(interp);
}

void p_amb(Interpreter *interp) {
	POP_XT(branch2, "amb");
	POP_XT(branch1, "amb");

	int saved_dsp = interp->dsp;
	int saved_trail = interp->bind_trail_top;
	int saved_lvar = interp->lvar_top;

	int mark_index = interp->rsp;
	int mark_id = push_prompt(interp, PROMPT_CHOICE);

	execute_cfa(interp, branch1);		

	if (interp->unwinding && interp->unwind_target == mark_id) {
		interp->unwinding = 0;
		interp->rsp = mark_index;
		interp->dsp = saved_dsp;
		trail_undo_to(interp, saved_trail);
		interp->lvar_top = saved_lvar;

		execute_cfa(interp, branch2);
	} else if (!interp->unwinding)
		interp->rsp = mark_index;


	DISPATCH(interp);
}

void p_wildcard(Interpreter *interp) {
	push(interp, make_tagged(T_UNBOUND, 0));

	DISPATCH(interp);
}
