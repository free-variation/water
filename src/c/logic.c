#include "water.h"

void p_lvar(DISPATCH_ARGS) {
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

void trail_undo_to(Interpreter *interp, int mark) {
	while (interp->bind_trail_top > mark) {
		int var_handle = interp->bind_trail[--interp->bind_trail_top];
		interp->lvar_stack[var_handle] = make_tagged(T_UNBOUND, 0);
	}
}

static int unify_depth(Interpreter *interp, Val left_val, Val right_val, int depth) {
	if (depth > MAX_NESTING_DEPTH) {
		fail(interp, "unify: structure too deeply nested (cycle?)");
		return 0;
	}

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
			if (!unify_depth(interp, left->items[i], right->items[i], depth + 1))
				return 0;
		MUSTTAIL return unify_depth(interp, left->items[n - 1], right->items[n - 1], depth + 1);
	}

	if (VAL_TAG(left_val) == T_PAIR && VAL_TAG(right_val) == T_PAIR) {
		Pair *left = &pairs.table[VAL_DATA(left_val)];
		Pair *right = &pairs.table[VAL_DATA(right_val)];

		if (!unify_depth(interp, left->head, right->head, depth + 1))
			return 0;
		MUSTTAIL return unify_depth(interp, left->tail, right->tail, depth + 1);
	}

	if (VAL_TAG(left_val) == T_FRAME && VAL_TAG(right_val) == T_FRAME) {
		Object *left = OBJECT_AT(VAL_DATA(left_val));
		Object *right = OBJECT_AT(VAL_DATA(right_val));

		int i = 0, j = 0;
		while (i < left->len && j < right->len) {
			cell left_key = left->frame.keys[i];
			cell right_key = right->frame.keys[j];
			if (left_key == right_key) {
				if (!unify_depth(interp, left->frame.values[i], right->frame.values[j], depth + 1))
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

int unify(Interpreter *interp, Val left_val, Val right_val) {
	return unify_depth(interp, left_val, right_val, 0);
}

static int enclosing_choice(Interpreter *interp) {
	for (int i = interp->rsp - 1; i >= 0; i--)
		if (VAL_TAG(interp->return_stack[i]) == T_MARK
				&& (VAL_DATA(interp->return_stack[i]) & 1) == PROMPT_CHOICE)
			return 1;
	return 0;
}

static void unify_outcome(Interpreter *interp, Val left, Val right, int unified) {
	if (interp->error_flag) return;

	if (unified) {
		push(interp, deref(interp, left));
	} else if (enclosing_choice(interp)) {
		backtrack(interp);
	} else {
		char *lbuf = NULL, *rbuf = NULL;
		size_t ln = 0, rn = 0;
		FILE *lf = open_memstream(&lbuf, &ln);
		if (lf) { print_val_inspect(lf, interp, deref(interp, left)); fclose(lf); }
		FILE *rf = open_memstream(&rbuf, &rn);
		if (rf) { print_val_inspect(rf, interp, deref(interp, right)); fclose(rf); }
		fail(interp, "unify: cannot unify %s with %s", lbuf ? lbuf : "?", rbuf ? rbuf : "?");
		free(lbuf);
		free(rbuf);
	}

}

void p_unify(DISPATCH_ARGS) {
	POP(right);
	POP(left);

	int unified = unify(interp, left, right);
	unify_outcome(interp, left, right, unified);

	DISPATCH(interp);
}

void p_unify_cons(DISPATCH_ARGS) {
	POP(tail);
	POP(head);
	POP(left);

	Val target = deref(interp, left);
	int unified;
	Val counterpart = target;

	if (VAL_TAG(target) == T_PAIR) {
		Pair *pair = &pairs.table[VAL_DATA(target)];
		unified = unify(interp, pair->head, head) && unify(interp, pair->tail, tail);
	} else if (VAL_TAG(target) == T_UNBOUND) {
		unified = 1;
	} else {
		int slot = object_new_pair(interp);
		if (interp->error_flag) return;

		pairs.table[slot].head = head;
		pairs.table[slot].tail = tail;
		
		counterpart = make_pair(slot);
		if (VAL_TAG(target) == T_LOGIC_VAR) {
			bind_var(interp, (int)VAL_DATA(target), counterpart);
			unified = 1;
		} else
			unified = 0;
	}

	unify_outcome(interp, left, counterpart, unified);

	DISPATCH(interp);
}

void p_matches(DISPATCH_ARGS) {
	POP(row);
	POP(pattern);

	int trail_mark = interp->bind_trail_top;
	int matched = unify(interp, pattern, row);
	trail_undo_to(interp, trail_mark);

	if (interp->error_flag) return;

	push(interp, make_bool(matched));

	DISPATCH(interp);
}

void p_deref(DISPATCH_ARGS) {
	POP(value);
	push(interp, deref(interp, value));

	DISPATCH(interp);
}

void p_amb(DISPATCH_ARGS) {
	POP_XT(branch2, "amb");
	POP_XT(branch1, "amb");

	int saved_dsp = interp->dsp;
	int saved_trail = interp->bind_trail_top;
	int saved_lvar = interp->lvar_top;

	int mark_index = interp->rsp;
	int mark_id = push_prompt(interp, PROMPT_CHOICE);

	execute_xt(interp, branch1);		

	if (interp->unwinding && interp->unwind_target == mark_id) {
		interp->unwinding = 0;
		interp->rsp = mark_index;
		interp->dsp = saved_dsp;
		trail_undo_to(interp, saved_trail);
		interp->lvar_top = saved_lvar;

		execute_xt(interp, branch2);
	} else if (!interp->unwinding)
		interp->rsp = mark_index;


	DISPATCH(interp);
}

void p_wildcard(DISPATCH_ARGS) {
	push(interp, make_tagged(T_UNBOUND, 0));

	DISPATCH(interp);
}
