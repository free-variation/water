#include "telic.h"

void p_lvar(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	int handle = object_new_logic_var(interp);
	if (interp->error_flag) return;

	chain_sp[0] = make_logic_var(handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
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

static int unify_depth(Interpreter *interp, Val left_val, Val right_val, int depth);

static int rest_position(Interpreter *interp, Object *pattern) {
	int last = pattern->len - 1;
	for (int i = 0; i < last; i++)
		if (VAL_TAG(pattern->items[i]) == T_REST) {
			fail(interp, "rest must be the last element of its array; found it at %d of %d", i, pattern->len);
			return -1;
		}
	return last >= 0 && VAL_TAG(pattern->items[last]) == T_REST ? last : pattern->len;
}

static int unify_rest(Interpreter *interp, Val pattern_val, Val subject_val, int n_prefix, int depth) {
	Object *subject = OBJECT_AT(VAL_DATA(subject_val));
	if (subject->len < n_prefix)
		return 0;

	for (int i = 0; i < n_prefix; i++)
		if (!unify_depth(interp, OBJECT_AT(VAL_DATA(pattern_val))->items[i], subject->items[i], depth + 1))
			return 0;

	Val marker = OBJECT_AT(VAL_DATA(pattern_val))->items[n_prefix];
	if (rest_is_wildcard(marker))
		return 1;

	gc_root_push(interp, pattern_val);
	gc_root_push(interp, subject_val);
	if (interp->error_flag)
		return 0;
	int n_remaining = subject->len - n_prefix;
	int remaining_handle = object_new_array(interp, n_remaining);
	gc_root_pop(interp);
	gc_root_pop(interp);
	if (interp->error_flag)
		return 0;

	subject = OBJECT_AT(VAL_DATA(subject_val));
	memcpy(OBJECT_AT(remaining_handle)->items, subject->items + n_prefix, sizeof(Val) * (size_t)n_remaining);

	return unify_depth(interp, make_logic_var((int)VAL_DATA(marker)), make_array(remaining_handle), depth + 1);
}

static int unify_depth(Interpreter *interp, Val left_val, Val right_val, int depth) {
	if (depth > MAX_NESTING_DEPTH) {
		fail(interp, "structure too deeply nested (cycle?)");
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

		int left_rest = rest_position(interp, left);
		if (left_rest < 0)
			return 0;
		int right_rest = rest_position(interp, right);
		if (right_rest < 0)
			return 0;
		if (left_rest < left->len && right_rest < right->len) {
			fail(interp, "cannot unify two rest patterns");
			return 0;
		}
		if (left_rest < left->len)
			return unify_rest(interp, left_val, right_val, left_rest, depth);
		if (right_rest < right->len)
			return unify_rest(interp, right_val, left_val, right_rest, depth);

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

static void unify_outcome(Interpreter *interp, Val left, Val right, int unified) {
	if (interp->error_flag) return;

	if (unified) {
		push(interp, deref(interp, left));
	} else if (prompt_index(interp, PROMPT_CHOICE) >= 0) {
		backtrack(interp);
	} else {
		char *lbuf = NULL, *rbuf = NULL;
		size_t ln = 0, rn = 0;
		FILE *lf = open_memstream(&lbuf, &ln);
		if (lf) { print_val_inspect(lf, interp, deref(interp, left)); fclose(lf); }
		FILE *rf = open_memstream(&rbuf, &rn);
		if (rf) { print_val_inspect(rf, interp, deref(interp, right)); fclose(rf); }
		fail(interp, "cannot unify %s with %s", lbuf ? lbuf : "?", rbuf ? rbuf : "?");
		free(lbuf);
		free(rbuf);
	}

}

void p_unify(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val right = chain_sp[-1];
	Val left = chain_sp[-2];
	SYNC_REGISTERS(interp, chain_ip, chain_sp - 2);

	int unified = unify(interp, left, right);
	unify_outcome(interp, left, right, unified);

	DISPATCH(interp);
}

void p_matches(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val row = chain_sp[-1];
	Val pattern = chain_sp[-2];

	int trail_mark = interp->bind_trail_top;
	int matched = unify(interp, pattern, row);
	trail_undo_to(interp, trail_mark);

	if (interp->error_flag) return;

	chain_sp[-2] = make_bool(matched);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_unify_keep(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val row = chain_sp[-1];
	Val pattern = chain_sp[-2];

	int trail_mark = interp->bind_trail_top;
	int matched = unify(interp, pattern, row);
	if (!matched)
		trail_undo_to(interp, trail_mark);

	if (interp->error_flag) return;

	chain_sp[-2] = make_bool(matched);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_deref(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	chain_sp[-1] = deref(interp, chain_sp[-1]);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_amb(DISPATCH_ARGS) {
	POP_CALLABLE(branch2, "amb");
	POP_CALLABLE(branch1, "amb");

	gc_root_push(interp, branch1_val);
	gc_root_push(interp, branch2_val);
	if (interp->error_flag) {
		gc_root_pop(interp);
		gc_root_pop(interp);
		return;
	}

	int saved_dsp = interp->dsp;
	int saved_trail = interp->bind_trail_top;
	int saved_lvar = interp->lvar_top;

	int mark_index = interp->rsp;
	int mark_id = push_prompt(interp, PROMPT_CHOICE);

	push_curried_bindings(interp, branch1_val);
	if (!interp->error_flag)
		execute_xt(interp, branch1);

	if (interp->unwinding && interp->unwind_target == mark_id) {
		interp->unwinding = 0;
		interp->rsp = mark_index;
		interp->dsp = saved_dsp;
		trail_undo_to(interp, saved_trail);
		interp->lvar_top = saved_lvar;

		push_curried_bindings(interp, branch2_val);
		if (!interp->error_flag)
			execute_xt(interp, branch2);
	} else if (!interp->unwinding)
		interp->rsp = mark_index;

	gc_root_pop(interp);
	gc_root_pop(interp);

	DISPATCH(interp);
}

void p_rest(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val variable = chain_sp[-1];
	if (VAL_TAG(variable) == T_UNBOUND)
		chain_sp[-1] = make_rest(REST_WILDCARD);
	else if (VAL_TAG(variable) == T_LOGIC_VAR)
		chain_sp[-1] = make_rest(VAL_DATA(variable));
	else {
		fail(interp, "expected a logic variable or _; got %s", tag_name(VAL_TAG(variable)));
		return;
	}

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_wildcard(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	chain_sp[0] = make_tagged(T_UNBOUND, 0);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}
