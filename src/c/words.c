#include "logicforth.h"

int string_concat(Interpreter *interp, int left_handle, int right_handle) {
	Object *left = interp->objects[left_handle];
	Object *right = interp->objects[right_handle];
	int combined_length = left->len + right->len;

	char *buffer = malloc((size_t)combined_length + 1);
	memcpy(buffer, left->bytes, (size_t)left->len);
	memcpy(buffer + left->len, right->bytes, (size_t)right->len);
	int result_handle = object_new_string(interp, buffer, combined_length);
	free(buffer);

	return result_handle;
}

void p_add(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);

	if (left.tag == T_FLOAT && right.tag == T_FLOAT)
		push(interp, make_float(unpack_float(left) + unpack_float(right)));
	else if (left.tag == T_STRING && right.tag == T_STRING)
		push(interp, make_string(string_concat(interp, (int)left.data, (int)right.data)));
	else if (left.tag == T_SET && right.tag == T_SET)
		push(interp, make_set(set_union(interp, (int)left.data, (int)right.data)));
	else if (left.tag == T_MATRIX && right.tag == T_MATRIX) {
		int target_handle = matrix_scalar_op(interp, left, right, scalar_add);
		if (target_handle < 0) return;
		push(interp, make_matrix(target_handle));
	}
	else type_error(interp, "+");
}

void p_sub(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);

	if (left.tag == T_FLOAT && right.tag == T_FLOAT)
		push(interp, make_float(unpack_float(left) - unpack_float(right)));
	else if (left.tag == T_SET && right.tag == T_SET)
		push(interp, make_set(set_difference(interp, (int)left.data, (int)right.data)));
	else if (left.tag == T_MATRIX && right.tag == T_MATRIX) {
		int target_handle = matrix_scalar_op(interp, left, right, scalar_subtract);
		if (target_handle < 0) return;
		push(interp, make_matrix(target_handle));
	}
	else type_error(interp, "-");
}

void p_mul(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);

	if (left.tag == T_FLOAT && right.tag == T_FLOAT)
		push(interp, make_float(unpack_float(left) * unpack_float(right)));
	else if (left.tag == T_SET && right.tag == T_SET)
		push(interp, make_set(set_intersect(interp, (int)left.data, (int)right.data)));
	else if (left.tag == T_MATRIX && right.tag == T_MATRIX) {
		int target_handle = matrix_scalar_op(interp, left, right, scalar_multiply);
		if (target_handle < 0) return;
		push(interp, make_matrix(target_handle));
	}
	else type_error(interp, "*");
}

void p_div(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);
	if (left.tag == T_FLOAT && right.tag == T_FLOAT && unpack_float(right) != 0.0)
		push(interp, make_float(unpack_float(left) / unpack_float(right)));
	else if (left.tag == T_MATRIX && right.tag == T_MATRIX) {
		/* Match the scalar path: a zero divisor is an error, not silent inf/nan. */
		Object *divisor = interp->objects[right.data];
		int n = divisor->matrix.rows * divisor->matrix.columns;
		for (int i = 0; i < n; i++) {
			if (divisor->matrix.elements[i] == 0.0) {
				type_error(interp, "/");
				return;
			}
		}
		int target_handle = matrix_scalar_op(interp, left, right, scalar_divide);
		if (target_handle < 0) return;
		push(interp, make_matrix(target_handle));
	}
	else type_error(interp, "/");
}

static double scalar_negate(double x) { return -x; }

void p_neg(Interpreter *interp, cell *cfa) {
	(void)cfa;
	POP(operand);
	unary_op(interp, operand, scalar_negate, "negate");
}

Val make_bool(int is_true) { return make_float(is_true ? -1.0 : 0.0); }

int truthy(Val value) { return (value.tag == T_FLOAT) ? (unpack_float(value) != 0.0) : (value.data != 0); }

void p_eq(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);
	push(interp, make_bool(val_cmp(interp, left, right) == 0));
}

void p_lt(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);
	push(interp, make_bool(val_cmp(interp, left, right) < 0));
}

void p_gt(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);
	push(interp, make_bool(val_cmp(interp, left, right) > 0));
}

void p_zeq(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(operand);
	push(interp, make_bool(!truthy(operand)));
}

void p_dup(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(top);
	push(interp, top);
	push(interp, top);
}

void p_drop(Interpreter *interp, cell *cfa) {
	(void)cfa;

	(void)pop(interp);
}

void p_swap(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(top);
	POP(second);
	push(interp, top);
	push(interp, second);
}

void p_over(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(top);
	POP(second);
	push(interp, second);
	push(interp, top);
	push(interp, second);
}

void p_rot(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(top);
	POP(middle);
	POP(bottom);
	push(interp, middle);
	push(interp, top);
	push(interp, bottom);
}

void p_depth(Interpreter *interp, cell *cfa) {
	(void)cfa;

	push(interp, make_float((double)interp->dsp));
}

void p_roll(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(n_val);
	if (n_val.tag != T_FLOAT) {
		type_error(interp, "roll");
		return;
	}

	int n = (int)unpack_float(n_val);
	if (n < 0 || n >= interp->dsp) {
		fail(interp, "roll: index out of range");
		return;
	}

	int src = interp->dsp - 1 - n;
	Val v = interp->data_stack[src];
	memmove(&interp->data_stack[src], &interp->data_stack[src + 1], (size_t)n * sizeof(Val));
	interp->data_stack[interp->dsp - 1] = v;
}

void p_dot(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(value);
	if (value.tag == T_MATRIX) {
		print_matrix_grid(interp->objects[value.data]);
	} else {
		print_val(interp, value);
		putchar(' ');
	}
	fflush(stdout);
}

void p_dot_all(Interpreter *interp, cell *cfa) {
	(void)cfa;

	int saved = print_truncate;
	print_truncate = 0;
	POP(value);
	if (value.tag == T_MATRIX) {
		print_matrix_grid(interp->objects[value.data]);
	} else {
		print_val(interp, value);
		putchar(' ');
	}
	fflush(stdout);
	print_truncate = saved;
}

void p_cr(Interpreter *interp, cell *cfa) {
	(void)interp;
	(void)cfa;

	putchar('\n');
	fflush(stdout);
}

void p_emit_(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(character);
	if (character.tag == T_FLOAT) {
		putchar((int)unpack_float(character));
		fflush(stdout);
	} else type_error(interp, "emit");
}

void p_dots(Interpreter *interp, cell *cfa) {
	(void)cfa;

	for (int i = 0; i < interp->dsp; i++) {
		print_val(interp, interp->data_stack[i]);
		putchar(' ');
	}
	fflush(stdout);
}

void p_bye(Interpreter *interp, cell *cfa) {
	(void)interp;
	(void)cfa;

	exit(0);
}

void p_tor(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(value);
	rpush(interp, value);
}

void p_rfrom(Interpreter *interp, cell *cfa) {
	(void)cfa;

	push(interp, rpop(interp));
}

void p_rfetch(Interpreter *interp, cell *cfa) {
	(void)cfa;

	if (interp->rsp > 0) push(interp, interp->return_stack[interp->rsp - 1]);
	else fail(interp, "return stack empty");
}

void p_to_side(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(value);
	if (interp->side_dsp >= SIDESTACK_DEPTH) {
		fail(interp, "side stack overflow");
		return;
	}
	interp->side_stack[interp->side_dsp++] = value;
}

void p_side_to(Interpreter *interp, cell *cfa) {
	(void)cfa;

	if (interp->side_dsp <= 0) {
		fail(interp, "side stack underflow");
		return;
	}
	push(interp, interp->side_stack[--interp->side_dsp]);
}

void p_side_drop(Interpreter *interp, cell *cfa) {
	(void)cfa;

	if (interp->side_dsp <= 0) {
		fail(interp, "side stack underflow");
		return;
	}
	interp->side_dsp--;
}

void p_side_depth(Interpreter *interp, cell *cfa) {
	(void)cfa;

	push(interp, make_float((double)interp->side_dsp));
}

void p_fetch(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(address);
	if (address.tag != T_ADDR) {
		type_error(interp, "@");
		return;
	}
	int cell_index = (int)address.data;
	Val loaded;
	loaded.tag = (Tag)interp->vocab->dict[cell_index];
	loaded.data = interp->vocab->dict[cell_index + 1];
	push(interp, loaded);
}

void p_store(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(addr);
	POP(value);
	if (addr.tag != T_ADDR) {
		type_error(interp, "!");
		return;
	}
	int cell_index = (int)addr.data;
	interp->vocab->dict[cell_index] = (cell)value.tag;
	interp->vocab->dict[cell_index + 1] = value.data;
}

void p_execute(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(value);
	if (value.tag != T_XT) {
		type_error(interp, "execute");
		return;
	}
	execute_cfa(interp, (int)value.data);
}

void p_reset(Interpreter *interp, cell *cfa) {
	(void)cfa;

	Val mark = make_mark();
	mark.data = interp->next_mark_id++;
	rpush(interp, mark);
}

int capture_continuation(Interpreter *interp, int *out_mark_index) {
	int mark_index = interp->rsp - 1;
	while (mark_index >= 0 && interp->return_stack[mark_index].tag != T_MARK) {
		mark_index--;
	}
	if (mark_index < 0) {
		fail(interp, "shift outside reset");
		return -1;
	}

	int return_len = interp->rsp - mark_index - 1;
	int resume_ip = interp->ip;
	int slot = object_new_continuation(interp, &interp->return_stack[mark_index + 1],
			return_len, resume_ip);
	if (interp->error_flag) return -1;

	*out_mark_index = mark_index;
	return slot;
}

void p_shift(Interpreter *interp, cell *cfa) {
	(void)cfa;

	int mark_index;
	int cont_slot = capture_continuation(interp, &mark_index);
	if (cont_slot < 0) return;

	interp->rsp = mark_index;
	push(interp, make_continuation(cont_slot));
}

void p_shift_with(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(handler);
	if (handler.tag != T_XT) {
		type_error(interp, "shift-with");
		return;
	}

	int mark_index;
	int cont_slot = capture_continuation(interp, &mark_index);
	if (cont_slot < 0) return;

	interp->unwind_target = (int)interp->return_stack[mark_index].data;
	interp->rsp = mark_index + 1;
	push(interp, make_continuation(cont_slot));

	execute_cfa(interp, (int)handler.data);
	if (interp->error_flag) return;

	interp->unwinding = 1;
}

void p_resume(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(k);
	if (k.tag != T_CONT) {
		type_error(interp, "resume");
		return;
	}

	Object *cont = interp->objects[k.data];
	int saved_ip = interp->ip;
	int saved_running = interp->running;

	interp->vocab->dict[TRAMPOLINE_SLOT + 1] = (cell)interp->vocab->stop_cfa;
	rpush(interp, make_addr(TRAMPOLINE_SLOT + 1));

	rpush(interp, make_mark());

	for (int i = 0; i < cont->continuation.return_len; i++)
		rpush(interp, cont->continuation.return_slice[i]);

	interp->ip = cont->continuation.resume_ip;
	interp->running = 1;
	run_inner(interp);

	interp->running = saved_running;
	interp->ip = saved_ip;
}

void p_words(Interpreter *interp, cell *cfa) {
	(void)cfa;

	int cnt = 0;
	for (int cf = interp->vocab->latest_cfa; cf != 0; cf = (int)WORD_LINK(interp->vocab, cf)) {
		fputs(&interp->vocab->name_pool[WORD_NAME(interp->vocab, cf)], stdout);
		putchar(' ');
		if (++cnt % 8 == 0) putchar('\n');
	}
	if (cnt % 8) putchar('\n');
	fflush(stdout);
}

void p_see(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(xt);
	if (xt.tag != T_XT) {
		type_error(interp, "see");
		return;
	}
	int target_cfa = (int)xt.data;

	const char *name = NULL;
	for (int cf = interp->vocab->latest_cfa; cf != 0; cf = (int)WORD_LINK(interp->vocab, cf)) {
		if (cf == target_cfa) { name = &interp->vocab->name_pool[WORD_NAME(interp->vocab, cf)]; break; }
	}
	cfa_handler handler = (cfa_handler)interp->vocab->dict[target_cfa];
	if (handler == docol) {
		if (!name) {

			printf("[: ... :]  \\ anonymous, no source\n");
		} else {
			int src_idx = (int)WORD_SOURCE(interp->vocab, target_cfa);
			if (src_idx > 0) printf(": %s%s;\n", name, &interp->vocab->source_pool[src_idx]);
			else printf(": %s ... ;  \\ no source captured\n", name);
		}
	} else if (handler == dovar) {
		Val val; val.tag = (Tag)interp->vocab->dict[target_cfa + 1]; val.data = interp->vocab->dict[target_cfa + 2];
		printf("variable %s  \\ current value: ", name ? name : "?");
		print_val(interp, val);
		putchar('\n');
	} else if (handler == dosym) {
		printf("symbol %s\n", name ? name : "?");
	} else {
		printf("%s is a primitive\n", name ? name : "?");
	}
	fflush(stdout);
}

void p_semi(Interpreter *interp, cell *cfa) {
	(void)cfa;

	emit(interp, (cell)interp->vocab->exit_cfa);
	if (interp->compiling_src_start > 0 && interp->vocab->latest_cfa != 0) {
		int src_end = interp->input_buffer_pos - 1;
		int src_len = src_end - interp->compiling_src_start;
		if (src_len < 0) src_len = 0;
		if (interp->vocab->source_here + src_len + 1 > SOURCE_POOL) {
			fail(interp, "source pool full");
		} else {
			int source_offset = interp->vocab->source_here;
			memcpy(&interp->vocab->source_pool[interp->vocab->source_here],
					&interp->input_buffer[interp->compiling_src_start],
					(size_t)src_len);
			interp->vocab->source_pool[interp->vocab->source_here + src_len] = 0;
			interp->vocab->source_here += src_len + 1;
			WORD_SOURCE(interp->vocab, interp->vocab->latest_cfa) = source_offset;
		}
	}
	interp->compiling = 0;
	interp->compiling_src_start = 0;
}

void p_if(Interpreter *interp, cell *cfa) {
	(void)cfa;

	emit(interp, (cell)interp->vocab->zbranch_cfa);
	push(interp, make_float((double)interp->vocab->here));
	emit(interp, 0);
}

void p_then(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(slot_val);
	int slot = (int)unpack_float(slot_val);
	interp->vocab->dict[slot] = (interp->vocab->here - slot);
}

void p_else(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(slot_val);
	int slot = (int)unpack_float(slot_val);
	emit(interp, (cell)interp->vocab->branch_cfa);
	push(interp, make_float((double)interp->vocab->here));
	emit(interp, 0);
	interp->vocab->dict[slot] = (interp->vocab->here - slot);
}

void p_begin(Interpreter *interp, cell *cfa) {
	(void)cfa;
	push(interp, make_float((double)interp->vocab->here));
}

void p_until(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(back_val);
	int back = (int)unpack_float(back_val);
	emit(interp, (cell)interp->vocab->zbranch_cfa);
	emit(interp, back - interp->vocab->here);
}

void p_again(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(back_val);
	int back = (int)unpack_float(back_val);
	emit(interp, (cell)interp->vocab->branch_cfa);
	emit(interp, back - interp->vocab->here);
}

void p_qcolon(Interpreter *interp, cell *cfa) {
	(void)cfa;

	int branch_slot = -1;
	if (interp->compiling) {
		emit(interp, (cell)interp->vocab->branch_cfa);
		branch_slot = interp->vocab->here;
		emit(interp, 0);
	}
	int anon_cfa = interp->vocab->here;
	emit(interp, (cell)&docol);
	interp->compiling = 1;
	push(interp, make_float((double)anon_cfa));
	push(interp, make_float((double)branch_slot));
}

void p_qsemi(Interpreter *interp, cell *cfa) {
	(void)cfa;

	emit(interp, (cell)interp->vocab->exit_cfa);
	POP(branch_slot_val);
	POP(anon_cfa_val);
	int branch_slot = (int)unpack_float(branch_slot_val);
	int anon_cfa = (int)unpack_float(anon_cfa_val);
	if (branch_slot < 0) {
		interp->compiling = 0;
		push(interp, make_xt(anon_cfa));
	} else {
		interp->vocab->dict[branch_slot] = (interp->vocab->here - branch_slot);
		emit_val_literal(interp, make_xt(anon_cfa));
	}
}

void p_tick(Interpreter *interp, cell *cfa) {
	(void)cfa;

	char *token = next_token(interp);
	if (!token) { type_error(interp, "'"); return; }
	int target_cfa = find(interp, token);
	if (!target_cfa) { fail(interp, "%s", token); return; }
	Val value = make_xt(target_cfa);
	if (interp->compiling) emit_val_literal(interp, value);
	else push(interp, value);
}

void p_colon(Interpreter *interp, cell *cfa) {
	(void)cfa;

	char *token = next_token(interp);
	if (!token) {
		fail(interp, ": needs a name");
		return;
	}

	create_header(interp, token, 0);
	emit(interp, (cell)&docol);
	interp->compiling = 1;

	interp->compiling_src_start = interp->input_buffer_pos;
}

void p_variable(Interpreter *interp, cell *cfa) {
	(void)cfa;

	char *token = next_token(interp);
	if (!token) {
		fail(interp, "variable needs a name");
		return;
	}

	create_header(interp, token, 0);
	emit(interp, (cell)&dovar);
	emit(interp, (cell)T_FLOAT);

	double zero_value = 0.0;
	cell zero_bits;
	memcpy(&zero_bits, &zero_value, 8);
	emit(interp, zero_bits);
}

void p_symbol(Interpreter *interp, cell *cfa) {
	(void)cfa;

	char *token = next_token(interp);
	if (!token) {
		fail(interp, "symbol needs a name");
		return;
	}

	create_header(interp, token, 0);
	emit(interp, (cell)&dosym);

	emit(interp, (cell)intern_symbol(interp, token));
}

void p_string_to_symbol(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(value);
	if (value.tag != T_STRING) {
		type_error(interp, "string>symbol");
		return;
	}

	Object *string = interp->objects[value.data];
	push(interp, make_symbol(intern_symbol(interp, string->bytes)));
}

void p_forget(Interpreter *interp, cell *cfa) {
	(void)cfa;

	char *token = next_token(interp);
	if (!token) {
		fail(interp, "forget needs a name");
		return;
	}
	int target_cfa = find(interp, token);
	if (!target_cfa) {
		fail(interp, "%s", token);
		return;
	}
	interp->vocab->here = target_cfa - 4;
	interp->vocab->names_here = (int)WORD_NAME(interp->vocab, target_cfa);
	interp->vocab->latest_cfa = (int)WORD_LINK(interp->vocab, target_cfa);

	int max_src_end = 1;
	for (int surviving_cfa = interp->vocab->latest_cfa; surviving_cfa != 0; surviving_cfa = (int)WORD_LINK(interp->vocab, surviving_cfa)) {
		int src_offset = (int)WORD_SOURCE(interp->vocab, surviving_cfa);
		if (src_offset > 0) {
			int src_end = src_offset + (int)strlen(&interp->vocab->source_pool[src_offset]) + 1;
			if (src_end > max_src_end) max_src_end = src_end;
		}
	}
	interp->vocab->source_here = max_src_end;
}

int read_string_literal(Interpreter *interp) {
	int start = interp->input_buffer_pos + 1;
	int cursor = start;
	while (cursor < interp->input_buffer_len && interp->input_buffer[cursor] != '"') cursor++;
	if (cursor >= interp->input_buffer_len) { interp->need_more = 1; return -1; }
	int length = cursor - start;
	memcpy(interp->token_buffer, interp->input_buffer + start, (size_t)length);
	interp->token_buffer[length] = 0;
	interp->input_buffer_pos = cursor + 1;
	return length;
}

static void interp_append(char **buffer, int *capacity, int *length, const char *src, int n) {
	if (*length + n > *capacity) {
		while (*length + n > *capacity) {
			*capacity *= 2;
		}
		*buffer = realloc(*buffer, (size_t)*capacity);
	}
	memcpy(*buffer + *length, src, (size_t)n);
	*length += n;
}

int interpolate(Interpreter *interp, int template_handle) {
	Object *template = interp->objects[template_handle];

	int max_ref = -1, any_placeholders = 0;
	for (int cursor = 0; cursor < template->len; ) {
		if (template->bytes[cursor] == '{') {
			int scan = cursor + 1, digit_value = 0, saw_digit = 0;
			while (scan < template->len && isdigit((unsigned char)template->bytes[scan])) {
				digit_value = digit_value * 10 + (template->bytes[scan] - '0');
				scan++;
				saw_digit = 1;
			}
			if (saw_digit && scan < template->len && template->bytes[scan] == '}') {
				if (digit_value > max_ref) max_ref = digit_value;
				any_placeholders = 1;
				cursor = scan + 1;
				continue;
			}
		}
		cursor++;
	}

	int capacity = template->len + 64;
	char *out_buffer = malloc((size_t)capacity);
	int out_length = 0;
	for (int cursor = 0; cursor < template->len; ) {
		if (template->bytes[cursor] == '{') {
			int scan = cursor + 1, digit_value = 0, saw_digit = 0;
			while (scan < template->len && isdigit((unsigned char)template->bytes[scan])) {
				digit_value = digit_value * 10 + (template->bytes[scan] - '0');
				scan++;
				saw_digit = 1;
			}
			if (saw_digit && scan < template->len && template->bytes[scan] == '}') {
				int stack_index = interp->dsp - 1 - digit_value;
				if (stack_index < 0) {
					type_error(interp, "string interpolation: stack too shallow");
					free(out_buffer);
					return object_new_string(interp, "", 0);
				}
				Val value = interp->data_stack[stack_index];
				switch (value.tag) {
					case T_FLOAT: {
						char rendered[64];
						double number = unpack_float(value);
						int n;
						if (number == (double)(int64_t)number && number > -1e15 && number < 1e15) {
							n = snprintf(rendered, sizeof(rendered), "%lld", (long long)number);
						} else {
							n = snprintf(rendered, sizeof(rendered), "%g", number);
						}
						interp_append(&out_buffer, &capacity, &out_length, rendered, n);
						break;
					}
					case T_SYM: {
						const char *name = &interp->vocab->symbol_pool[value.data];
						interp_append(&out_buffer, &capacity, &out_length, name, (int)strlen(name));
						break;
					}
					case T_STRING: {
						Object *string_obj = interp->objects[value.data];
						interp_append(&out_buffer, &capacity, &out_length, string_obj->bytes, string_obj->len);
						break;
					}
					default:
						interp_append(&out_buffer, &capacity, &out_length, "<?>", 3);
						break;
				}
				cursor = scan + 1;
				continue;
			}
		}
		interp_append(&out_buffer, &capacity, &out_length, &template->bytes[cursor], 1);
		cursor++;
	}

	if (any_placeholders && max_ref >= 0) {
		int items_to_drop = max_ref + 1;
		if (items_to_drop > interp->dsp) items_to_drop = interp->dsp;
		interp->dsp -= items_to_drop;
	}

	int result_handle = object_new_string(interp, out_buffer, out_length);
	free(out_buffer);
	return result_handle;
}

void p_gc(Interpreter *interp, cell *cfa) {
	(void)cfa;

	gc(interp);
}

void p_clear(Interpreter *interp, cell *cfa) {
	(void)cfa;

	interp->dsp = 0;
}

void unary_op(Interpreter *interp, Val operand, double (*function)(double), const char *name) {
	if (operand.tag == T_FLOAT) {
		push(interp, make_float(function(unpack_float(operand))));
	} else if (operand.tag == T_MATRIX) {
		Object *source = interp->objects[operand.data];
		int target_handle = object_new_matrix(interp, source->matrix.rows, source->matrix.columns);
		if (interp->error_flag) return;

		Object *target = interp->objects[target_handle];
		int num_elements = source->matrix.rows * source->matrix.columns;
		for (int i = 0; i < num_elements; i++)
			target->matrix.elements[i] = function(source->matrix.elements[i]);

		push(interp, make_matrix(target_handle));
	} else {
		type_error(interp, name);
	}
}

void p_abs(Interpreter *interp, cell *cfa) {
	(void)cfa;
	POP(operand);
	unary_op(interp, operand, fabs, "abs");
}

void p_sqrt(Interpreter *interp, cell *cfa) {
	(void)cfa;
	POP(operand);
	unary_op(interp, operand, sqrt, "sqrt");
}

void p_exp(Interpreter *interp, cell *cfa) {
	(void)cfa;
	POP(operand);
	unary_op(interp, operand, exp, "exp");
}

void p_log(Interpreter *interp, cell *cfa) {
	(void)cfa;
	POP(operand);
	unary_op(interp, operand, log, "log");
}

void p_sin(Interpreter *interp, cell *cfa) {
	(void)cfa;
	POP(operand);
	unary_op(interp, operand, sin, "sin");
}

void p_cos(Interpreter *interp, cell *cfa) {
	(void)cfa;
	POP(operand);
	unary_op(interp, operand, cos, "cos");
}

void p_tan(Interpreter *interp, cell *cfa) {
	(void)cfa;
	POP(operand);
	unary_op(interp, operand, tan, "tan");
}

void p_tanh(Interpreter *interp, cell *cfa) {
	(void)cfa;
	POP(operand);
	unary_op(interp, operand, tanh, "tanh");
}
