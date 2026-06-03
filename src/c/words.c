#include "logicforth.h"
#include <time.h>

static void enter_compile_scope(Interpreter *interp);
static void leave_compile_scope(Interpreter *interp);

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
		push(interp, make_float((left).number + (right).number));
	else if (left.tag == T_STRING && right.tag == T_STRING)
		push(interp, make_string(string_concat(interp, (int)left.data, (int)right.data)));
	else if (left.tag == T_SET && right.tag == T_SET)
		push(interp, make_set(set_union(interp, (int)left.data, (int)right.data)));
	else if (left.tag == T_MATRIX && right.tag == T_MATRIX) {
		int target_handle = matrix_scalar_op(interp, left, right, scalar_add);
		if (target_handle < 0)
			return;
		push(interp, make_matrix(target_handle));
	}
	else if (left.tag == T_ARRAY && right.tag == T_ARRAY) {
		push(interp, left);
		push(interp, right);
		p_concat(interp, NULL);
	}
	else
		fail(interp, "+ : expected two floats, two strings, two sets, two matrices, or two arrays; got %s and %s", tag_name(left.tag), tag_name(right.tag));
}

void p_sub(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);

	if (left.tag == T_FLOAT && right.tag == T_FLOAT)
		push(interp, make_float((left).number - (right).number));
	else if (left.tag == T_SET && right.tag == T_SET)
		push(interp, make_set(set_difference(interp, (int)left.data, (int)right.data)));
	else if (left.tag == T_MATRIX && right.tag == T_MATRIX) {
		int target_handle = matrix_scalar_op(interp, left, right, scalar_subtract);
		if (target_handle < 0)
			return;
		push(interp, make_matrix(target_handle));
	}
	else
		fail(interp, "- : expected two floats, two sets, or two matrices; got %s and %s", tag_name(left.tag), tag_name(right.tag));
}

void p_mul(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);

	if (left.tag == T_FLOAT && right.tag == T_FLOAT)
		push(interp, make_float((left).number * (right).number));
	else if (left.tag == T_SET && right.tag == T_SET)
		push(interp, make_set(set_intersect(interp, (int)left.data, (int)right.data)));
	else if (left.tag == T_MATRIX && right.tag == T_MATRIX) {
		int target_handle = matrix_scalar_op(interp, left, right, scalar_multiply);
		if (target_handle < 0)
			return;
		push(interp, make_matrix(target_handle));
	}
	else
		fail(interp, "* : expected two floats, two sets, or two matrices; got %s and %s", tag_name(left.tag), tag_name(right.tag));
}

void p_div(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(right);
	POP(left);
	if (left.tag == T_FLOAT && right.tag == T_FLOAT) {
		if ((right).number == 0.0) {
			fail(interp, "/ : division by zero");
			return;
		}
		push(interp, make_float((left).number / (right).number));
	}
	else if (left.tag == T_MATRIX && right.tag == T_MATRIX) {
		Object *divisor = interp->objects[right.data];
		int n = divisor->matrix.rows * divisor->matrix.columns;
		for (int i = 0; i < n; i++) {
			if (divisor->matrix.elements[i] == 0.0) {
				fail(interp, "/ : division by zero (matrix element %d)", i);
				return;
			}
		}
		int target_handle = matrix_scalar_op(interp, left, right, scalar_divide);
		if (target_handle < 0)
			return;
		push(interp, make_matrix(target_handle));
	}
	else
		fail(interp, "/ : expected two floats or two matrices; got %s and %s", tag_name(left.tag), tag_name(right.tag));
}

#define BINARY_FLOAT_PRIMITIVE(name, opname, op) \
	void name(Interpreter *interp, cell *cfa) { \
		(void)cfa; \
		if (interp->dsp < 2) { \
			fail(interp, "%s: data stack underflow", opname); \
			return; \
		} \
		Val *left = &interp->data_stack[interp->dsp - 2]; \
		Val *right = &interp->data_stack[interp->dsp - 1]; \
		left->number = left->number op right->number; \
		interp->dsp--; \
	}

BINARY_FLOAT_PRIMITIVE(p_add_f, "+f", +)
BINARY_FLOAT_PRIMITIVE(p_sub_f, "-f", -)
BINARY_FLOAT_PRIMITIVE(p_mul_f, "*f", *)

void p_fmul_add(Interpreter *interp, cell *cfa) {
	(void)cfa;

	if (interp->dsp < 3) {
		fail(interp, "f*+: data stack underflow");
		return;
	}
	Val *a = &interp->data_stack[interp->dsp - 3];
	Val *b = &interp->data_stack[interp->dsp - 2];
	Val *c = &interp->data_stack[interp->dsp - 1];
	a->number = a->number * b->number + c->number;
	interp->dsp -= 2;
}

void p_fmul_sub(Interpreter *interp, cell *cfa) {
	(void)cfa;

	if (interp->dsp < 3) {
		fail(interp, "f*-: data stack underflow");
		return;
	}
	Val *a = &interp->data_stack[interp->dsp - 3];
	Val *b = &interp->data_stack[interp->dsp - 2];
	Val *c = &interp->data_stack[interp->dsp - 1];
	a->number = c->number - a->number * b->number;
	interp->dsp -= 2;
}

void p_div_f(Interpreter *interp, cell *cfa) {
	(void)cfa;

	if (interp->dsp < 2) {
		fail(interp, "/f: data stack underflow");
		return;
	}
	Val *left = &interp->data_stack[interp->dsp - 2];
	Val *right = &interp->data_stack[interp->dsp - 1];
	if (right->number == 0.0) {
		fail(interp, "/f: division by zero");
		return;
	}
	left->number = left->number / right->number;
	interp->dsp--;
}

static double scalar_negate(double x) { return -x; }

void p_neg(Interpreter *interp, cell *cfa) {
	(void)cfa;
	POP(operand);
	unary_op(interp, operand, scalar_negate, "negate");
}

Val make_bool(int is_true) { return make_float(is_true ? -1.0 : 0.0); }

int truthy(Val value) { return (value.tag == T_FLOAT) ? ((value).number != 0.0) : (value.data != 0); }

#define COMPARISON_PRIMITIVE(name, op) \
	void name(Interpreter *interp, cell *cfa) { \
		(void)cfa; \
		POP(right); \
		POP(left); \
		if (left.tag == T_FLOAT && right.tag == T_FLOAT) \
			push(interp, make_bool((left).number op (right).number)); \
		else \
			push(interp, make_bool(val_cmp(interp, left, right) op 0)); \
	}

COMPARISON_PRIMITIVE(p_eq, ==)
COMPARISON_PRIMITIVE(p_lt, <)
COMPARISON_PRIMITIVE(p_gt, >)

#define UNARY_FLOAT_PRIMITIVE(name, word_name, expr) \
	void name(Interpreter *interp, cell *cfa) { \
		(void)cfa; \
		POP(operand); \
		if (operand.tag != T_FLOAT) { \
			fail(interp, word_name ": expected a float; got %s", tag_name(operand.tag)); \
			return; \
		} \
		double n = (operand).number; \
		push(interp, make_float(expr)); \
	}

UNARY_FLOAT_PRIMITIVE(p_inc, "1+", n + 1.0)
UNARY_FLOAT_PRIMITIVE(p_dec, "1-", n - 1.0)
UNARY_FLOAT_PRIMITIVE(p_sq,  "sq", n * n)

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

	POP_INT(n, "roll", "depth");
	if (n < 0 || n >= interp->dsp) {
		fail(interp, "roll: depth %d out of range (stack has %d below it)", n, interp->dsp);
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
	} else if (value.tag == T_FRAME) {
		print_frame_pretty(interp, interp->objects[value.data], 0);
		putchar('\n');
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

	POP_INT(c, "emit", "character code");
	putchar(c);
	fflush(stdout);
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

	if (interp->rsp > 0)
		push(interp, interp->return_stack[interp->rsp - 1]);
	else
		fail(interp, "r@: return stack is empty");
}

void p_to_side(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(value);
	if (interp->side_dsp >= SIDESTACK_DEPTH) {
		fail(interp, ">side: side stack overflow (max %d)", SIDESTACK_DEPTH);
		return;
	}
	interp->side_stack[interp->side_dsp++] = value;
}

void p_side_to(Interpreter *interp, cell *cfa) {
	(void)cfa;

	if (interp->side_dsp <= 0) {
		fail(interp, "side>: side stack is empty");
		return;
	}
	push(interp, interp->side_stack[--interp->side_dsp]);
}

void p_side_drop(Interpreter *interp, cell *cfa) {
	(void)cfa;

	if (interp->side_dsp <= 0) {
		fail(interp, "side-drop: side stack is empty");
		return;
	}
	interp->side_dsp--;
}

void p_side_depth(Interpreter *interp, cell *cfa) {
	(void)cfa;

	push(interp, make_float((double)interp->side_dsp));
}



void p_execute(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP_XT(value, "execute");
	execute_cfa(interp, value);
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
		fail(interp, "shift/shift-with: no enclosing reset on the return stack");
		return -1;
	}

	int return_len = interp->rsp - mark_index - 1;
	int resume_ip = interp->ip;
	int slot = object_new_continuation(interp, &interp->return_stack[mark_index + 1],
			return_len, resume_ip);
	if (interp->error_flag)
		return -1;

	interp->objects[slot]->continuation.local_base_offset =
		interp->local_base - (mark_index + 1);

	*out_mark_index = mark_index;
	return slot;
}

static void restore_local_base_below(Interpreter *interp, int mark_index) {
	int base = interp->local_base;
	while (base > mark_index)
		base = (int)interp->return_stack[base - 1].data;
	interp->local_base = base;
}

void p_shift(Interpreter *interp, cell *cfa) {
	(void)cfa;

	int mark_index;
	int cont_slot = capture_continuation(interp, &mark_index);
	if (cont_slot < 0)
		return;

	interp->rsp = mark_index;
	restore_local_base_below(interp, mark_index);
	push(interp, make_continuation(cont_slot));
}

void p_shift_with(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP_XT(handler, "shift-with");

	int mark_index;
	int cont_slot = capture_continuation(interp, &mark_index);
	if (cont_slot < 0)
		return;

	interp->unwind_target = (int)interp->return_stack[mark_index].data;
	interp->rsp = mark_index + 1;
	restore_local_base_below(interp, mark_index);
	push(interp, make_continuation(cont_slot));

	execute_cfa(interp, handler);
	if (interp->error_flag)
		return;

	interp->unwinding = 1;
}

void p_resume(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(k);
	if (k.tag != T_CONT) {
		fail(interp, "resume: expected a continuation; got %s", tag_name(k.tag));
		return;
	}

	Object *cont = interp->objects[k.data];
	int saved_ip = interp->ip;
	int saved_running = interp->running;
	int saved_local_base = interp->local_base;

	rpush(interp, make_addr(TRAMPOLINE_SLOT + 2));

	rpush(interp, make_mark());

	int slice_base = interp->rsp;
	for (int i = 0; i < cont->continuation.return_len; i++)
		rpush(interp, cont->continuation.return_slice[i]);

	if (cont->continuation.local_base_offset >= 0)
		interp->local_base = slice_base + cont->continuation.local_base_offset;

	interp->ip = cont->continuation.resume_ip;
	interp->running = 1;
	run_inner(interp);

	interp->running = saved_running;
	interp->ip = saved_ip;
	interp->local_base = saved_local_base;
}

void p_words(Interpreter *interp, cell *cfa) {
	(void)cfa;

	int cnt = 0;
	for (int cf = interp->vocab->latest_cfa; cf != 0; cf = (int)WORD_LINK(interp->vocab, cf)) {
		fputs(&interp->vocab->name_pool[WORD_NAME(interp->vocab, cf)], stdout);
		putchar(' ');
		if (++cnt % 8 == 0)
			putchar('\n');
	}
	if (cnt % 8)
		putchar('\n');
	fflush(stdout);
}

void p_see(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP_XT(target_cfa, "see");

	const char *name = NULL;
	for (int cf = interp->vocab->latest_cfa; cf != 0; cf = (int)WORD_LINK(interp->vocab, cf)) {
		if (cf == target_cfa) {
			name = &interp->vocab->name_pool[WORD_NAME(interp->vocab, cf)];
			break;
		}
	}
	cfa_handler handler = (cfa_handler)interp->vocab->dict[target_cfa];
	if (handler == docol) {
		if (!name) {

			printf("[: ... :]  \\ anonymous, no source\n");
		} else {
			int src_idx = (int)WORD_SOURCE(interp->vocab, target_cfa);
			if (src_idx > 0)
				printf(": %s%s;\n", name, &interp->vocab->source_pool[src_idx]);
			else
				printf(": %s ... ;  \\ no source captured\n", name);
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

	leave_compile_scope(interp);
	emit_call(interp, interp->vocab->exit_cfa);
	if (interp->compiling_src_start > 0 && interp->vocab->latest_cfa != 0) {
		int src_end = interp->input_buffer_pos - 1;
		int src_len = src_end - interp->compiling_src_start;
		if (src_len < 0)
			src_len = 0;
		if (interp->vocab->source_here + src_len + 1 > SOURCE_POOL) {
			fail(interp, "source pool full (max %d bytes); definition source too large to store", SOURCE_POOL);
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

	emit_call(interp, interp->vocab->zbranch_cfa);
	push(interp, make_float((double)interp->vocab->here));
	emit(interp, 0);
}

void p_qif(Interpreter *interp, cell *cfa) {
	(void)cfa;

	emit_call(interp, interp->vocab->qzbranch_cfa);
	push(interp, make_float((double)interp->vocab->here));
	emit(interp, 0);
}

void p_then(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(slot_val);
	int slot = (int)(slot_val).number;
	interp->vocab->dict[slot] = (interp->vocab->here - slot);
}

void p_else(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(slot_val);
	int slot = (int)(slot_val).number;
	emit_call(interp, interp->vocab->branch_cfa);
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
	int back = (int)(back_val).number;
	emit_call(interp, interp->vocab->zbranch_cfa);
	emit(interp, back - interp->vocab->here);
}

void p_again(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP(back_val);
	int back = (int)(back_val).number;
	emit_call(interp, interp->vocab->branch_cfa);
	emit(interp, back - interp->vocab->here);
}

void p_qcolon(Interpreter *interp, cell *cfa) {
	(void)cfa;

	int branch_slot = -1;
	if (interp->compiling) {
		emit_call(interp, interp->vocab->branch_cfa);
		branch_slot = interp->vocab->here;
		emit(interp, 0);
	}
	int anon_cfa = interp->vocab->here;
	emit(interp, (cell)&docol);
	enter_compile_scope(interp);
	interp->compiling = 1;
	push(interp, make_float((double)anon_cfa));
	push(interp, make_float((double)branch_slot));
}

void p_qsemi(Interpreter *interp, cell *cfa) {
	(void)cfa;

	leave_compile_scope(interp);
	emit_call(interp, interp->vocab->exit_cfa);
	POP(branch_slot_val);
	POP(anon_cfa_val);
	int branch_slot = (int)(branch_slot_val).number;
	int anon_cfa = (int)(anon_cfa_val).number;
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
	if (!token) {
		fail(interp, "' : expected a word name");
		return;
	}
	int target_cfa = find(interp, token);
	if (!target_cfa) {
		fail(interp, "' : unknown word: %s", token);
		return;
	}
	Val value = make_xt(target_cfa);
	if (interp->compiling)
		emit_val_literal(interp, value);
	else
		push(interp, value);
}

static void enter_compile_scope(Interpreter *interp) {
	if (interp->n_local_scopes >= MAX_LOCAL_SCOPES) {
		fail(interp, "compile: locals nesting deeper than %d", MAX_LOCAL_SCOPES);
		return;
	}

	interp->local_scope_starts[interp->n_local_scopes] = interp->n_local_names;
	interp->local_scope_dict_starts[interp->n_local_scopes] = interp->vocab->here;
	interp->n_local_scopes++;
}

static void leave_compile_scope(Interpreter *interp) {
	if (interp->n_local_scopes <= 0)
		return;

	interp->n_local_scopes--;
	int saved_n_names = interp->local_scope_starts[interp->n_local_scopes];
	int n_locals_in_scope = interp->n_local_names - saved_n_names;

	if (n_locals_in_scope > 0) {
		emit_call(interp, interp->vocab->leave_locals_cfa);
		emit(interp, (cell)n_locals_in_scope);
	}

	if (saved_n_names == 0) {
		interp->local_names_pool_here = 0;
	} else {
		int last_offset = interp->local_name_offsets[saved_n_names - 1];
		interp->local_names_pool_here = last_offset +
			(int)strlen(&interp->local_names_pool[last_offset]) + 1;
	}
	interp->n_local_names = saved_n_names;
}

void p_colon(Interpreter *interp, cell *cfa) {
	(void)cfa;

	char *token = next_token(interp);
	if (!token) {
		fail(interp, ": expected a name for the new definition");
		return;
	}

	create_header(interp, token, 0);
	emit(interp, (cell)&docol);
	enter_compile_scope(interp);
	interp->compiling = 1;

	interp->compiling_src_start = interp->input_buffer_pos;
}

int create_variable(Interpreter *interp, const char *name) {
	create_header(interp, name, 0);
	emit(interp, (cell)&dovar);
	emit(interp, (cell)T_FLOAT);

	double zero_value = 0.0;
	cell zero_bits;
	memcpy(&zero_bits, &zero_value, 8);
	emit(interp, zero_bits);

	return interp->vocab->latest_cfa;
}


void p_variable(Interpreter *interp, cell *cfa) {
	(void)cfa;

	char *token = next_token(interp);
	if (!token) {
		fail(interp, "variable: expected a name");
		return;
	}

	create_variable(interp, token);
}

static void compile_locals_decl(Interpreter *interp, const char *opener, int force_all_receive) {
	if (!interp->compiling || interp->n_local_scopes <= 0) {
		fail(interp, "%s: only valid inside a colon definition or quotation", opener);
		return;
	}

	int scope_idx = interp->n_local_scopes - 1;
	if (interp->vocab->here != interp->local_scope_dict_starts[scope_idx]) {
		fail(interp, "%s: locals must be declared at the head of the body", opener);
		return;
	}

	int scope_start = interp->local_scope_starts[scope_idx];
	int receive_slots[MAX_LOCAL_NAMES];
	int n_received = 0;

	while (1) {
		char *token = next_token(interp);
		if (!token) {
			fail(interp, "%s: unterminated locals declaration (no closing |)", opener);
			return;
		}
		if (strcmp(token, "|") == 0)
			break;

		int has_receive_marker = 0;
		if (token[0] == '>' && token[1] != 0) {
			has_receive_marker = 1;
			token++;
		}

		for (int i = scope_start; i < interp->n_local_names; i++) {
			if (strcmp(token, &interp->local_names_pool[interp->local_name_offsets[i]]) == 0) {
				fail(interp, "%s: local '%s' declared twice", opener, token);
				return;
			}
		}

		int name_len = (int)strlen(token);
		if (interp->local_names_pool_here + name_len + 1 > LOCAL_NAMES_POOL_SIZE) {
			fail(interp, "%s: local names pool full", opener);
			return;
		}
		if (interp->n_local_names >= MAX_LOCAL_NAMES) {
			fail(interp, "%s: too many local names (max %d)", opener, MAX_LOCAL_NAMES);
			return;
		}

		int slot = interp->n_local_names - scope_start;

		int offset = interp->local_names_pool_here;
		memcpy(&interp->local_names_pool[offset], token, (size_t)name_len);
		interp->local_names_pool[offset + name_len] = 0;
		interp->local_names_pool_here += name_len + 1;
		interp->local_name_offsets[interp->n_local_names++] = offset;

		if (has_receive_marker)
			receive_slots[n_received++] = slot;
	}

	int n_declared = interp->n_local_names - scope_start;
	if (n_declared == 0)
		return;

	if (force_all_receive && n_received > 0) {
		fail(interp, "%s: do not use > markers; %s already implies all-receive", opener, opener);
		return;
	}

	if (force_all_receive || n_received == n_declared) {
		emit_call(interp, interp->vocab->enter_locals_to_cfa);
		emit(interp, (cell)n_declared);
	} else if (n_received == 0) {
		emit_call(interp, interp->vocab->enter_locals_cfa);
		emit(interp, (cell)n_declared);
	} else {
		emit_call(interp, interp->vocab->enter_locals_mixed_cfa);
		emit(interp, (cell)n_declared);
		emit(interp, (cell)n_received);
		for (int i = 0; i < n_received; i++)
			emit(interp, (cell)receive_slots[i]);
	}
}

void p_bar(Interpreter *interp, cell *cfa) {
	(void)cfa;
	compile_locals_decl(interp, "|", 0);
}

void p_bar_to(Interpreter *interp, cell *cfa) {
	(void)cfa;
	compile_locals_decl(interp, "|>", 1);
}

void p_to_var(Interpreter *interp, cell *cfa) {
	(void)cfa;
	int var_cfa = (int)interp->vocab->dict[interp->ip++];
	POP(v);
	interp->vocab->dict[var_cfa + 1] = (cell)v.tag;
	interp->vocab->dict[var_cfa + 2] = v.data;
}

void p_to(Interpreter *interp, cell *cfa) {
	(void)cfa;
	char *token = next_token(interp);
	if (!token) {
		fail(interp, "to: expected a name"); 
		return;
	}

	if (interp->compiling) {
		int local_depth, local_slot_idx;
		if (find_local(interp, token, &local_depth, &local_slot_idx)) {
			if (local_depth == 0) {
				emit_call(interp, interp->vocab->local_store_0depth_cfa);
				emit(interp, (cell)local_slot_idx);
			} else {
				emit_call(interp, interp->vocab->local_store_cfa);
				emit(interp, (cell)local_depth);
				emit(interp, (cell)local_slot_idx);
			}
			return;
		}
	}

	int target_cfa = find(interp, token);
	if (!target_cfa) {
		if (interp->compiling) {
			fail(interp, "to: unknown variable: %s; declare it with variable", token); 
		return; 
		}
		target_cfa = create_variable(interp, token);
	}

	cfa_handler h = (cfa_handler)interp->vocab->dict[target_cfa];
	if (h != dovar) {
		fail(interp, "to: %s is not a variable", token); 
		return; 
	}

	if (interp->compiling) {
		emit_call(interp, interp->vocab->to_var_cfa);
		emit(interp, (cell)target_cfa);
	} else {
		POP(v);
		interp->vocab->dict[target_cfa + 1] = (cell)v.tag;
		interp->vocab->dict[target_cfa + 2] = v.data;
	}
}

static void compile_local_unary(Interpreter *interp, const char *op,
                                int depth0_cfa, int fallback_cfa) {
	char *token = next_token(interp);
	if (!token) {
		fail(interp, "%s: expected a local name", op);
		return;
	}
	if (!interp->compiling) {
		fail(interp, "%s: only valid inside a colon definition", op);
		return;
	}
	int depth, slot;
	if (!find_local(interp, token, &depth, &slot)) {
		fail(interp, "%s: %s is not a local", op, token);
		return;
	}
	if (depth == 0) {
		emit_call(interp, depth0_cfa);
		emit(interp, (cell)slot);
	} else {
		emit_call(interp, interp->vocab->local_fetch_cfa);
		emit(interp, (cell)depth);
		emit(interp, (cell)slot);
		emit_call(interp, fallback_cfa);
		emit_call(interp, interp->vocab->local_store_cfa);
		emit(interp, (cell)depth);
		emit(interp, (cell)slot);
	}
}

void p_increment(Interpreter *interp, cell *cfa) {
	(void)cfa;
	compile_local_unary(interp, "++",
	                    interp->vocab->local_incr_0depth_cfa,
	                    interp->vocab->inc_cfa);
}

void p_decrement(Interpreter *interp, cell *cfa) {
	(void)cfa;
	compile_local_unary(interp, "--",
	                    interp->vocab->local_decr_0depth_cfa,
	                    interp->vocab->dec_cfa);
}

void p_inline(Interpreter *interp, cell *cfa) {
	(void)cfa;

	int latest = interp->vocab->latest_cfa;
	if (!latest) {
		fail(interp, "inline: no recent definition");
		return;
	}

	WORD_FLAGS(interp->vocab, latest) |= 2;
}

void p_symbol(Interpreter *interp, cell *cfa) {
	(void)cfa;

	char *token = next_token(interp);
	if (!token) {
		fail(interp, "symbol: expected a name");
		return;
	}
	if (token[0] == ':')
		token++;

	create_header(interp, token, 0);
	emit(interp, (cell)&dosym);

	emit(interp, (cell)intern_symbol(interp, token));
}

void p_string_to_symbol(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP_STRING(string, "string>symbol");
	push(interp, make_symbol(intern_symbol(interp, string->bytes)));
}

void p_forget(Interpreter *interp, cell *cfa) {
	(void)cfa;

	char *token = next_token(interp);
	if (!token) {
		fail(interp, "forget: expected a name");
		return;
	}
	int target_cfa = find(interp, token);
	if (!target_cfa) {
		fail(interp, "forget: unknown word: %s", token);
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
			if (src_end > max_src_end)
				max_src_end = src_end;
		}
	}
	interp->vocab->source_here = max_src_end;
}

int read_string_literal(Interpreter *interp) {
	int start = interp->input_buffer_pos + 1;
	int cursor = start;
	while (cursor < interp->input_buffer_len && interp->input_buffer[cursor] != '"')
		cursor++;
	if (cursor >= interp->input_buffer_len) {
		interp->need_more = 1;
		return -1;
	}
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
				if (digit_value > max_ref)
					max_ref = digit_value;
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
					fail(interp, "string interpolation: {%d} needs %d stack value(s) but only %d present",
							digit_value, digit_value + 1, interp->dsp);
					free(out_buffer);
					return object_new_string(interp, "", 0);
				}
				Val value = interp->data_stack[stack_index];
				switch (value.tag) {
					case T_FLOAT: {
						char rendered[64];
						double number = (value).number;
						int n;
						if (number == (double)(int64_t)number && number > -1e15 && number < 1e15) {
							n = snprintf(rendered, sizeof(rendered), "%lld", (long long)number);
						} else {
							n = snprintf(rendered, sizeof(rendered), "%g", number);
						}
						interp_append(&out_buffer, &capacity, &out_length, rendered, n);
						break;
					}
					case T_SYMBOL: {
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
		if (items_to_drop > interp->dsp)
			items_to_drop = interp->dsp;
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
		push(interp, make_float(function((operand).number)));
	} else if (operand.tag == T_MATRIX) {
		Object *source = interp->objects[operand.data];
		int target_handle = object_new_matrix(interp, source->matrix.rows, source->matrix.columns);
		if (interp->error_flag)
			return;

		Object *target = interp->objects[target_handle];
		int num_elements = source->matrix.rows * source->matrix.columns;
		for (int i = 0; i < num_elements; i++)
			target->matrix.elements[i] = function(source->matrix.elements[i]);

		push(interp, make_matrix(target_handle));
	} else {
		fail(interp, "%s: expected a float or a matrix; got %s", name, tag_name(operand.tag));
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

void p_now(Interpreter *interp, cell *cfa) {
	(void)cfa;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	push(interp, make_float((double)ts.tv_sec + (double)ts.tv_nsec / 1e9));
}

