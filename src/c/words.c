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

#define BROADCAST_SCALAR_OP_MATRIX(op) \
	do { \
		double scalar = VAL_NUMBER(left); \
		Object *matrix_source = interp->objects[VAL_DATA(right)]; \
		int target_handle = object_new_matrix(interp, matrix_source->matrix.rows, matrix_source->matrix.columns); \
		if (interp->error_flag) return; \
		Object *target = interp->objects[target_handle]; \
		size_t num_elements = (size_t)matrix_source->matrix.rows * (size_t)matrix_source->matrix.columns; \
		const double * restrict source_elements = matrix_source->matrix.elements; \
		double * restrict target_elements = target->matrix.elements; \
		for (size_t i = 0; i < num_elements; i++) \
			target_elements[i] = scalar op source_elements[i]; \
		push(interp, make_matrix(target_handle)); \
	} while (0)

#define BROADCAST_MATRIX_OP_SCALAR(op) \
	do { \
		double scalar = VAL_NUMBER(right); \
		Object *matrix_source = interp->objects[VAL_DATA(left)]; \
		int target_handle = object_new_matrix(interp, matrix_source->matrix.rows, matrix_source->matrix.columns); \
		if (interp->error_flag) return; \
		Object *target = interp->objects[target_handle]; \
		size_t num_elements = (size_t)matrix_source->matrix.rows * (size_t)matrix_source->matrix.columns; \
		const double * restrict source_elements = matrix_source->matrix.elements; \
		double * restrict target_elements = target->matrix.elements; \
		for (size_t i = 0; i < num_elements; i++) \
			target_elements[i] = source_elements[i] op scalar; \
		push(interp, make_matrix(target_handle)); \
	} while (0)

void p_add(Interpreter *interp) {
	POP(right);
	POP(left);

	if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT)
		push(interp, make_float(VAL_NUMBER(left) + VAL_NUMBER(right)));
	else if (VAL_TAG(left) == T_STRING && VAL_TAG(right) == T_STRING)
		push(interp, make_string(string_concat(interp, (int)VAL_DATA(left), (int)VAL_DATA(right))));
	else if (VAL_TAG(left) == T_SET && VAL_TAG(right) == T_SET)
		push(interp, make_set(set_union(interp, (int)VAL_DATA(left), (int)VAL_DATA(right))));
	else if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_MATRIX) {
		int target_handle = matrix_add(interp, left, right);
		if (target_handle < 0)
			return;
		push(interp, make_matrix(target_handle));
	}
	else if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_MATRIX)
		BROADCAST_SCALAR_OP_MATRIX(+);
	else if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_FLOAT)
		BROADCAST_MATRIX_OP_SCALAR(+);
	else if (VAL_TAG(left) == T_ARRAY && VAL_TAG(right) == T_ARRAY) {
		push(interp, left);
		push(interp, right);
		execute_cfa(interp, find(interp, "concat"));
	}
	else
		fail(interp, "+ : expected two floats, two strings, two sets, two matrices, scalar/matrix, or two arrays; got %s and %s", tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));

	DISPATCH(interp);
}

void p_sub(Interpreter *interp) {
	POP(right);
	POP(left);

	if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT)
		push(interp, make_float(VAL_NUMBER(left) - VAL_NUMBER(right)));
	else if (VAL_TAG(left) == T_SET && VAL_TAG(right) == T_SET)
		push(interp, make_set(set_difference(interp, (int)VAL_DATA(left), (int)VAL_DATA(right))));
	else if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_MATRIX) {
		int target_handle = matrix_sub(interp, left, right);
		if (target_handle < 0)
			return;
		push(interp, make_matrix(target_handle));
	}
	else if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_MATRIX)
		BROADCAST_SCALAR_OP_MATRIX(-);
	else if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_FLOAT)
		BROADCAST_MATRIX_OP_SCALAR(-);
	else
		fail(interp, "- : expected two floats, two sets, two matrices, or scalar/matrix; got %s and %s", tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));

	DISPATCH(interp);
}

void p_mul(Interpreter *interp) {
	POP(right);
	POP(left);

	if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT)
		push(interp, make_float(VAL_NUMBER(left) * VAL_NUMBER(right)));
	else if (VAL_TAG(left) == T_SET && VAL_TAG(right) == T_SET)
		push(interp, make_set(set_intersect(interp, (int)VAL_DATA(left), (int)VAL_DATA(right))));
	else if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_MATRIX) {
		int target_handle = matrix_mul(interp, left, right);
		if (target_handle < 0)
			return;
		push(interp, make_matrix(target_handle));
	}
	else if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_MATRIX)
		BROADCAST_SCALAR_OP_MATRIX(*);
	else if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_FLOAT)
		BROADCAST_MATRIX_OP_SCALAR(*);
	else
		fail(interp, "* : expected two floats, two sets, two matrices, or scalar/matrix; got %s and %s", tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));

	DISPATCH(interp);
}

void p_div(Interpreter *interp) {
	POP(right);
	POP(left);
	if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT) {
		if (VAL_NUMBER(right) == 0.0) {
			fail(interp, "/ : division by zero");
			return;
		}
		push(interp, make_float(VAL_NUMBER(left) / VAL_NUMBER(right)));
	}
	else if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_MATRIX) {
		Object *divisor = interp->objects[VAL_DATA(right)];
		int n = divisor->matrix.rows * divisor->matrix.columns;
		for (int i = 0; i < n; i++) {
			if (divisor->matrix.elements[i] == 0.0) {
				fail(interp, "/ : division by zero (matrix element %d)", i);
				return;
			}
		}
		int target_handle = matrix_div(interp, left, right);
		if (target_handle < 0)
			return;
		push(interp, make_matrix(target_handle));
	}
	else if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_MATRIX)
		BROADCAST_SCALAR_OP_MATRIX(/);
	else if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_FLOAT) {
		if (VAL_NUMBER(right) == 0.0) {
			fail(interp, "/ : division by zero");
			return;
		}
		BROADCAST_MATRIX_OP_SCALAR(/);
	}
	else
		fail(interp, "/ : expected two floats, two matrices, or scalar/matrix; got %s and %s", tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));

	DISPATCH(interp);
}

#define INPLACE_OP(name, word, op) \
	void name(Interpreter *interp) { \
		POP(right); \
		POP(left); \
		if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_MATRIX) { \
			Object *left_matrix = interp->objects[VAL_DATA(left)]; \
			Object *right_matrix = interp->objects[VAL_DATA(right)]; \
			if (left_matrix->matrix.rows != right_matrix->matrix.rows || left_matrix->matrix.columns != right_matrix->matrix.columns) { \
				fail(interp, word ": matrix shapes differ (%dx%d vs %dx%d)", left_matrix->matrix.rows, left_matrix->matrix.columns, right_matrix->matrix.rows, right_matrix->matrix.columns); \
				return; \
			} \
			size_t num_elements = (size_t)left_matrix->matrix.rows * (size_t)left_matrix->matrix.columns; \
			double * restrict left_elements = left_matrix->matrix.elements; \
			const double * restrict right_elements = right_matrix->matrix.elements; \
			for (size_t i = 0; i < num_elements; i++) \
				left_elements[i] = left_elements[i] op right_elements[i]; \
			push(interp, left); \
		} else if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_FLOAT) { \
			double scalar = VAL_NUMBER(right); \
			Object *matrix = interp->objects[VAL_DATA(left)]; \
			size_t num_elements = (size_t)matrix->matrix.rows * (size_t)matrix->matrix.columns; \
			double * restrict elements = matrix->matrix.elements; \
			for (size_t i = 0; i < num_elements; i++) \
				elements[i] = elements[i] op scalar; \
			push(interp, left); \
		} else if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_MATRIX) { \
			double scalar = VAL_NUMBER(left); \
			Object *matrix = interp->objects[VAL_DATA(right)]; \
			size_t num_elements = (size_t)matrix->matrix.rows * (size_t)matrix->matrix.columns; \
			double * restrict elements = matrix->matrix.elements; \
			for (size_t i = 0; i < num_elements; i++) \
				elements[i] = scalar op elements[i]; \
			push(interp, right); \
		} else { \
			fail(interp, word ": expected a matrix operand; got %s and %s", tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right))); \
		} \
		DISPATCH(interp); \
	}

INPLACE_OP(p_add_inplace, "+!", +)
INPLACE_OP(p_sub_inplace, "-!", -)
INPLACE_OP(p_mul_inplace, "*!", *)
INPLACE_OP(p_div_inplace, "/!", /)

#define BINARY_FLOAT_OP(name, opname, op) \
	void name(Interpreter *interp) { \
		if (interp->dsp < 2) { \
			fail(interp, "%s: data stack underflow", opname); \
			return; \
		} \
		Val *left = &interp->data_stack[interp->dsp - 2]; \
		Val *right = &interp->data_stack[interp->dsp - 1]; \
		left->number = left->number op right->number; \
		interp->dsp--; \
		DISPATCH(interp); \
	}

BINARY_FLOAT_OP(p_add_f, "f+", +)
BINARY_FLOAT_OP(p_sub_f, "f-", -)
BINARY_FLOAT_OP(p_mul_f, "f*", *)

void p_div_f(Interpreter *interp) {
	if (interp->dsp < 2) {
		fail(interp, "f/: data stack underflow");
		return;
	}
	Val *left = &interp->data_stack[interp->dsp - 2];
	Val *right = &interp->data_stack[interp->dsp - 1];
	if (right->number == 0.0) {
		fail(interp, "f/: division by zero");
		return;
	}
	left->number = left->number / right->number;
	interp->dsp--;

	DISPATCH(interp);
}

static double scalar_negate(double x) { return -x; }
static double scalar_inc(double x) { return x + 1.0; }
static double scalar_dec(double x) { return x - 1.0; }
static double scalar_sq(double x) { return x * x; }

void p_neg(Interpreter *interp) {
	POP(operand);
	unary_op(interp, operand, scalar_negate, "negate");

	DISPATCH(interp);
}

#define COMPARISON_OP(name, op) \
	void name(Interpreter *interp) { \
		POP(right); \
		POP(left); \
		if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT) \
			push(interp, make_bool(VAL_NUMBER(left) op VAL_NUMBER(right))); \
		else \
			push(interp, make_bool(val_cmp(interp, left, right) op 0)); \
		DISPATCH(interp); \
	}

COMPARISON_OP(p_eq, ==)
COMPARISON_OP(p_lt, <)
COMPARISON_OP(p_gt, >)

#define UNARY_FLOAT_OP(name, opname, expr) \
	void name(Interpreter *interp) { \
		if (interp->dsp < 1) { \
			fail(interp, "%s: data stack underflow", opname); \
			return; \
		} \
		Val *top = &interp->data_stack[interp->dsp - 1]; \
		double n = top->number; \
		top->number = (expr); \
		DISPATCH(interp); \
	}

void p_zeq(Interpreter *interp) {
	POP(operand);
	push(interp, make_bool(!truthy(operand)));

	DISPATCH(interp);
}

void p_and(Interpreter *interp) {
	POP(right);
	POP(left);
	push(interp, make_bool(truthy(left) && truthy(right)));

	DISPATCH(interp);
}

void p_or(Interpreter *interp) {
	POP(right);
	POP(left);
	push(interp, make_bool(truthy(left) || truthy(right)));

	DISPATCH(interp);
}

void p_not(Interpreter *interp) {
	POP(operand);
	push(interp, make_bool(!truthy(operand)));

	DISPATCH(interp);
}

void p_dup(Interpreter *interp) {
	POP(top);
	push(interp, top);
	push(interp, top);

	DISPATCH(interp);
}

void p_drop(Interpreter *interp) {
	(void)pop(interp);

	DISPATCH(interp);
}

void p_swap(Interpreter *interp) {
	POP(top);
	POP(second);
	push(interp, top);
	push(interp, second);

	DISPATCH(interp);
}

void p_over(Interpreter *interp) {
	POP(top);
	POP(second);
	push(interp, second);
	push(interp, top);
	push(interp, second);

	DISPATCH(interp);
}

void p_rot(Interpreter *interp) {
	POP(top);
	POP(middle);
	POP(bottom);
	push(interp, middle);
	push(interp, top);
	push(interp, bottom);

	DISPATCH(interp);
}

void p_depth(Interpreter *interp) {
	push(interp, make_float((double)interp->dsp));

	DISPATCH(interp);
}

void p_roll(Interpreter *interp) {
	POP_INT(n, "roll", "depth");
	if (n < 0 || n >= interp->dsp) {
		fail(interp, "roll: depth %d out of range (stack has %d below it)", n, interp->dsp);
		return;
	}

	int src = interp->dsp - 1 - n;
	Val value = interp->data_stack[src];
	memmove(&interp->data_stack[src], &interp->data_stack[src + 1], (size_t)n * sizeof(Val));
	interp->data_stack[interp->dsp - 1] = value;

	DISPATCH(interp);
}

void p_dot(Interpreter *interp) {
	POP(value);
	if (VAL_TAG(value) == T_MATRIX) {
		print_matrix_grid(interp->objects[VAL_DATA(value)]);
	} else if (VAL_TAG(value) == T_FRAME) {
		print_frame_pretty(interp, interp->objects[VAL_DATA(value)], 0);
		putchar('\n');
	} else if (VAL_TAG(value) == T_ARRAY) {
		pretty_print_array(interp, value);
	} else {
		print_val(interp, value);
		putchar(' ');
	}
	fflush(stdout);

	DISPATCH(interp);
}

void p_dot_all(Interpreter *interp) {
	int saved = print_truncate;
	print_truncate = 0;
	POP(value);
	if (VAL_TAG(value) == T_MATRIX) {
		print_matrix_grid(interp->objects[VAL_DATA(value)]);
	} else {
		print_val(interp, value);
		putchar(' ');
	}
	fflush(stdout);
	print_truncate = saved;

	DISPATCH(interp);
}

void p_cr(Interpreter *interp) {
	(void)interp;

	putchar('\n');
	fflush(stdout);

	DISPATCH(interp);
}

void p_emit_(Interpreter *interp) {
	POP_INT(c, "emit", "character code");
	putchar(c);
	fflush(stdout);

	DISPATCH(interp);
}

void p_dots(Interpreter *interp) {
	for (int i = 0; i < interp->dsp; i++) {
		print_val(interp, interp->data_stack[i]);
		putchar(' ');
	}
	fflush(stdout);

	DISPATCH(interp);
}

void p_bye(Interpreter *interp) {
	(void)interp;

	exit(0);
}

void p_tor(Interpreter *interp) {
	POP(value);
	rpush(interp, value);

	DISPATCH(interp);
}

void p_rfrom(Interpreter *interp) {
	push(interp, rpop(interp));

	DISPATCH(interp);
}

void p_rfetch(Interpreter *interp) {
	if (interp->rsp > 0)
		push(interp, interp->return_stack[interp->rsp - 1]);
	else
		fail(interp, "r@: return stack is empty");

	DISPATCH(interp);
}

void p_to_side(Interpreter *interp) {
	POP(value);
	if (interp->side_dsp >= SIDESTACK_DEPTH) {
		fail(interp, ">side: side stack overflow (max %d)", SIDESTACK_DEPTH);
		return;
	}
	interp->side_stack[interp->side_dsp++] = value;

	DISPATCH(interp);
}

void p_side_to(Interpreter *interp) {
	if (interp->side_dsp <= 0) {
		fail(interp, "side>: side stack is empty");
		return;
	}
	push(interp, interp->side_stack[--interp->side_dsp]);

	DISPATCH(interp);
}

void p_side_drop(Interpreter *interp) {
	if (interp->side_dsp <= 0) {
		fail(interp, "side-drop: side stack is empty");
		return;
	}
	interp->side_dsp--;

	DISPATCH(interp);
}

void p_side_depth(Interpreter *interp) {
	push(interp, make_float((double)interp->side_dsp));

	DISPATCH(interp);
}

void p_execute(Interpreter *interp) {
	POP_XT(value, "execute");
	execute_cfa(interp, value);

	DISPATCH(interp);
}

void p_reset(Interpreter *interp) {
	Val mark = make_tagged(T_MARK, interp->next_mark_id++);
	rpush(interp, mark);

	DISPATCH(interp);
}

int capture_continuation(Interpreter *interp, int *out_mark_index) {
	int mark_index = interp->rsp - 1;
	while (mark_index >= 0 && VAL_TAG(interp->return_stack[mark_index]) != T_MARK) {
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
		base = (int)VAL_DATA(interp->return_stack[base - 1]);
	interp->local_base = base;
}

void p_shift(Interpreter *interp) {
	int mark_index;
	int cont_slot = capture_continuation(interp, &mark_index);
	if (cont_slot < 0)
		return;

	interp->rsp = mark_index;
	restore_local_base_below(interp, mark_index);
	push(interp, make_continuation(cont_slot));

	DISPATCH(interp);
}

void p_shift_with(Interpreter *interp) {
	POP_XT(handler, "shift-with");

	int mark_index;
	int cont_slot = capture_continuation(interp, &mark_index);
	if (cont_slot < 0)
		return;

	interp->unwind_target = (int)VAL_DATA(interp->return_stack[mark_index]);
	interp->rsp = mark_index + 1;
	restore_local_base_below(interp, mark_index);
	push(interp, make_continuation(cont_slot));

	execute_cfa(interp, handler);
	if (interp->error_flag)
		return;

	interp->unwinding = 1;
}

void p_resume(Interpreter *interp) {
	POP(k);
	if (VAL_TAG(k) != T_CONT) {
		fail(interp, "resume: expected a continuation; got %s", tag_name(VAL_TAG(k)));
		return;
	}

	Object *cont = interp->objects[VAL_DATA(k)];
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

	DISPATCH(interp);
}

void p_words(Interpreter *interp) {
	int cnt = 0;
	for (int cf = interp->vocab->latest_cfa; cf != 0; cf = (int)WORD_LINK(interp->vocab, cf)) {
		if (WORD_IS_INTERNAL(interp->vocab, cf))
			continue;
		fputs(&interp->vocab->name_pool[WORD_NAME(interp->vocab, cf)], stdout);
		putchar(' ');
		if (++cnt % 8 == 0)
			putchar('\n');
	}
	if (cnt % 8)
		putchar('\n');
	fflush(stdout);

	DISPATCH(interp);
}

void p_see(Interpreter *interp) {
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
		Val value;
		value.bits = (uint64_t)interp->vocab->dict[target_cfa + 1];
		printf("variable %s  \\ current value: ", name ? name : "?");
		print_val(interp, value);
		putchar('\n');
	} else if (handler == dosym) {
		printf("symbol %s\n", name ? name : "?");
	} else {
		printf("%s is a primitive\n", name ? name : "?");
	}
	fflush(stdout);

	DISPATCH(interp);
}

void p_semi(Interpreter *interp) {
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

	DISPATCH(interp);
}

void p_if(Interpreter *interp) {
	emit_call(interp, interp->vocab->zbranch_cfa);
	push(interp, make_float((double)interp->vocab->here));
	emit(interp, 0);

	DISPATCH(interp);
}

void p_qif(Interpreter *interp) {
	emit_call(interp, interp->vocab->qzbranch_cfa);
	push(interp, make_float((double)interp->vocab->here));
	emit(interp, 0);

	DISPATCH(interp);
}

void p_then(Interpreter *interp) {
	POP(slot_val);
	int slot = (int)VAL_NUMBER(slot_val);
	interp->vocab->dict[slot] = (interp->vocab->here - slot);

	DISPATCH(interp);
}

void p_else(Interpreter *interp) {
	POP(slot_val);
	int slot = (int)VAL_NUMBER(slot_val);
	emit_call(interp, interp->vocab->branch_cfa);
	push(interp, make_float((double)interp->vocab->here));
	emit(interp, 0);
	interp->vocab->dict[slot] = (interp->vocab->here - slot);

	DISPATCH(interp);
}

void p_begin(Interpreter *interp) {
	push(interp, make_float((double)interp->vocab->here));

	DISPATCH(interp);
}

void p_until(Interpreter *interp) {
	POP(back_val);
	int back = (int)VAL_NUMBER(back_val);
	emit_call(interp, interp->vocab->zbranch_cfa);
	emit(interp, back - interp->vocab->here);

	DISPATCH(interp);
}

void p_again(Interpreter *interp) {
	POP(back_val);
	int back = (int)VAL_NUMBER(back_val);
	emit_call(interp, interp->vocab->branch_cfa);
	emit(interp, back - interp->vocab->here);

	DISPATCH(interp);
}

void p_while(Interpreter *interp) {
	emit_call(interp, interp->vocab->zbranch_cfa);
	push(interp, make_float((double)interp->vocab->here));
	emit(interp, 0);

	DISPATCH(interp);
}

void p_repeat(Interpreter *interp) {
	POP(exit_slot_val);
	POP(back_val);
	int exit_slot = (int)VAL_NUMBER(exit_slot_val);
	int back = (int)VAL_NUMBER(back_val);
	emit_call(interp, interp->vocab->branch_cfa);
	emit(interp, back - interp->vocab->here);
	interp->vocab->dict[exit_slot] = (interp->vocab->here - exit_slot);

	DISPATCH(interp);
}

void p_qcolon(Interpreter *interp) {
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

	DISPATCH(interp);
}

void p_qsemi(Interpreter *interp) {
	leave_compile_scope(interp);
	emit_call(interp, interp->vocab->exit_cfa);
	POP(branch_slot_val);
	POP(anon_cfa_val);
	int branch_slot = (int)VAL_NUMBER(branch_slot_val);
	int anon_cfa = (int)VAL_NUMBER(anon_cfa_val);
	if (branch_slot < 0) {
		interp->compiling = 0;
		push(interp, make_xt(anon_cfa));
	} else {
		interp->vocab->dict[branch_slot] = (interp->vocab->here - branch_slot);
		emit_val_literal(interp, make_xt(anon_cfa));
	}

	DISPATCH(interp);
}

void p_tick(Interpreter *interp) {
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

	DISPATCH(interp);
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

void p_colon(Interpreter *interp) {
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

	DISPATCH(interp);
}

int create_variable(Interpreter *interp, const char *name) {
	create_header(interp, name, 0);
	emit(interp, (cell)&dovar);
	emit(interp, (cell)make_float(0.0).bits);

	return interp->vocab->latest_cfa;
}


void p_variable(Interpreter *interp) {
	char *token = next_token(interp);
	if (!token) {
		fail(interp, "variable: expected a name");
		return;
	}

	create_variable(interp, token);

	DISPATCH(interp);
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

void p_bar(Interpreter *interp) {
	compile_locals_decl(interp, "|", 0);

	DISPATCH(interp);
}

void p_bar_to(Interpreter *interp) {
	compile_locals_decl(interp, "|>", 1);

	DISPATCH(interp);
}

void p_to_var(Interpreter *interp) {
	int var_cfa = (int)interp->vocab->dict[interp->ip++];
	POP(v);
	interp->vocab->dict[var_cfa + 1] = (cell)v.bits;

	DISPATCH(interp);
}

void p_to(Interpreter *interp) {
	char *token = next_token(interp);
	if (!token) {
		fail(interp, "to: expected a name"); 
		return;
	}

	if (interp->compiling) {
		int local_depth, local_slot_idx;
		if (find_local(interp, token, &local_depth, &local_slot_idx)) {
			if (try_fuse_local_acc(interp, local_depth, local_slot_idx))
				return;
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
		if (!superword_try_fuse_store(interp, target_cfa)) {
			emit_call(interp, interp->vocab->to_var_cfa);
			emit(interp, (cell)target_cfa);
		}
	} else {
		POP(value);
		interp->vocab->dict[target_cfa + 1] = (cell)value.bits;
	}

	DISPATCH(interp);
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

void p_increment(Interpreter *interp) {
	compile_local_unary(interp, "++",
	                    interp->vocab->local_incr_0depth_cfa,
	                    interp->vocab->inc_cfa);

	DISPATCH(interp);
}

void p_decrement(Interpreter *interp) {
	compile_local_unary(interp, "--",
	                    interp->vocab->local_decr_0depth_cfa,
	                    interp->vocab->dec_cfa);

	DISPATCH(interp);
}

void p_inline(Interpreter *interp) {
	int latest = interp->vocab->latest_cfa;
	if (!latest) {
		fail(interp, "inline: no recent definition");
		return;
	}

	WORD_FLAGS(interp->vocab, latest) |= 2;

	DISPATCH(interp);
}

void p_symbol(Interpreter *interp) {
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

	DISPATCH(interp);
}

void p_string_to_symbol(Interpreter *interp) {
	POP_STRING(string, "string>symbol");
	push(interp, make_symbol(intern_symbol(interp, string->bytes)));

	DISPATCH(interp);
}

void p_forget(Interpreter *interp) {
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

	DISPATCH(interp);
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
				switch (VAL_TAG(value)) {
					case T_FLOAT: {
						char rendered[64];
						double number = VAL_NUMBER(value);
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
						const char *name = &interp->vocab->symbol_pool[VAL_DATA(value)];
						interp_append(&out_buffer, &capacity, &out_length, name, (int)strlen(name));
						break;
					}
					case T_STRING: {
						Object *string_obj = interp->objects[VAL_DATA(value)];
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

void p_gc(Interpreter *interp) {
	gc(interp);

	DISPATCH(interp);
}

void p_clear(Interpreter *interp) {
	interp->dsp = 0;

	DISPATCH(interp);
}

void unary_op(Interpreter *interp, Val operand, double (*function)(double), const char *name) {
	if (VAL_TAG(operand) == T_FLOAT) {
		push(interp, make_float(function(VAL_NUMBER(operand))));
	} else if (VAL_TAG(operand) == T_MATRIX) {
		Object *source = interp->objects[VAL_DATA(operand)];
		int target_handle = object_new_matrix(interp, source->matrix.rows, source->matrix.columns);
		if (interp->error_flag)
			return;

		Object *target = interp->objects[target_handle];
		int num_elements = source->matrix.rows * source->matrix.columns;
		for (int i = 0; i < num_elements; i++)
			target->matrix.elements[i] = function(source->matrix.elements[i]);

		push(interp, make_matrix(target_handle));
	} else {
		fail(interp, "%s: expected a float or a matrix; got %s", name, tag_name(VAL_TAG(operand)));
	}
}

#define UNARY_MATH_OP_NAMED(cname, func, word) \
	void p_##cname(Interpreter *interp) { \
		POP(operand); \
		unary_op(interp, operand, func, word); \
		DISPATCH(interp); \
	}
#define UNARY_MATH_OP(name, func) UNARY_MATH_OP_NAMED(name, func, #name)

UNARY_MATH_OP(abs, fabs)
UNARY_MATH_OP(sqrt, sqrt)
UNARY_MATH_OP(exp, exp)
UNARY_MATH_OP(log, log10)
UNARY_MATH_OP(ln, log)
UNARY_MATH_OP(sin, sin)
UNARY_MATH_OP(cos, cos)
UNARY_MATH_OP(tan, tan)
UNARY_MATH_OP(tanh, tanh)
UNARY_MATH_OP(asin, asin)
UNARY_MATH_OP(acos, acos)
UNARY_MATH_OP(atan, atan)
UNARY_MATH_OP(round, round)
UNARY_MATH_OP(truncate, trunc)
UNARY_MATH_OP_NAMED(round_up, ceil, "round-up")
UNARY_MATH_OP_NAMED(round_down, floor, "round-down")
UNARY_MATH_OP_NAMED(inc_poly, scalar_inc, "1+")
UNARY_MATH_OP_NAMED(dec_poly, scalar_dec, "1-")
UNARY_MATH_OP_NAMED(sq_poly, scalar_sq, "sq")

void binary_op(Interpreter *interp, Val left, Val right, scalar_operator function, const char *name) {
	if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_FLOAT) {
		push(interp, make_float(function(VAL_NUMBER(left), VAL_NUMBER(right))));
		return;
	}

	if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_MATRIX) {
		Object *a = interp->objects[VAL_DATA(left)];
		Object *b = interp->objects[VAL_DATA(right)];
		if (a->matrix.rows != b->matrix.rows || a->matrix.columns != b->matrix.columns) {
			fail(interp, "%s: matrix shapes differ (%dx%d vs %dx%d)", name,
			     a->matrix.rows, a->matrix.columns, b->matrix.rows, b->matrix.columns);
			return;
		}
		int target_handle = object_new_matrix(interp, a->matrix.rows, a->matrix.columns);
		if (interp->error_flag) return;
		Object *target = interp->objects[target_handle];
		int num_elements = a->matrix.rows * a->matrix.columns;
		for (int i = 0; i < num_elements; i++)
			target->matrix.elements[i] = function(a->matrix.elements[i], b->matrix.elements[i]);
		push(interp, make_matrix(target_handle));
		return;
	}

	if (VAL_TAG(left) == T_MATRIX && VAL_TAG(right) == T_FLOAT) {
		double scalar = VAL_NUMBER(right);
		Object *source = interp->objects[VAL_DATA(left)];
		int target_handle = object_new_matrix(interp, source->matrix.rows, source->matrix.columns);
		if (interp->error_flag) return;
		Object *target = interp->objects[target_handle];
		int num_elements = source->matrix.rows * source->matrix.columns;
		for (int i = 0; i < num_elements; i++)
			target->matrix.elements[i] = function(source->matrix.elements[i], scalar);
		push(interp, make_matrix(target_handle));
		return;
	}

	if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_MATRIX) {
		double scalar = VAL_NUMBER(left);
		Object *source = interp->objects[VAL_DATA(right)];
		int target_handle = object_new_matrix(interp, source->matrix.rows, source->matrix.columns);
		if (interp->error_flag) return;
		Object *target = interp->objects[target_handle];
		int num_elements = source->matrix.rows * source->matrix.columns;
		for (int i = 0; i < num_elements; i++)
			target->matrix.elements[i] = function(scalar, source->matrix.elements[i]);
		push(interp, make_matrix(target_handle));
		return;
	}

	fail(interp, "%s: expected floats or matrices; got %s and %s", name,
	     tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));
}

void p_power(Interpreter *interp) {
	POP(right);
	POP(left);
	binary_op(interp, left, right, pow, "^");

	DISPATCH(interp);
}

void p_divmod(Interpreter *interp) {
	POP(right);
	POP(left);
	if (VAL_TAG(left) != T_FLOAT || VAL_TAG(right) != T_FLOAT) {
		fail(interp, "%%: expected two floats; got %s and %s", tag_name(VAL_TAG(left)), tag_name(VAL_TAG(right)));
		return;
	}

	double dividend = VAL_NUMBER(left);
	double divisor = VAL_NUMBER(right);
	if (divisor == 0.0) {
		fail(interp, "%%: division by zero");
		return;
	}

	double quotient = trunc(dividend / divisor);
	double remainder = dividend - quotient * divisor;
	push(interp, make_float(remainder));
	push(interp, make_float(quotient));

	DISPATCH(interp);
}

UNARY_FLOAT_OP(p_fabs, "fabs", fabs(n))
UNARY_FLOAT_OP(p_fsqrt, "fsqrt", sqrt(n))
UNARY_FLOAT_OP(p_fexp, "fexp", exp(n))
UNARY_FLOAT_OP(p_flog, "flog", log10(n))
UNARY_FLOAT_OP(p_fln, "fln", log(n))
UNARY_FLOAT_OP(p_fsin, "fsin", sin(n))
UNARY_FLOAT_OP(p_fcos, "fcos", cos(n))
UNARY_FLOAT_OP(p_ftan, "ftan", tan(n))
UNARY_FLOAT_OP(p_ftanh, "ftanh", tanh(n))
UNARY_FLOAT_OP(p_fasin, "fasin", asin(n))
UNARY_FLOAT_OP(p_facos, "facos", acos(n))
UNARY_FLOAT_OP(p_fatan, "fatan", atan(n))
UNARY_FLOAT_OP(p_fround, "fround", round(n))
UNARY_FLOAT_OP(p_ftruncate, "ftruncate", trunc(n))
UNARY_FLOAT_OP(p_fnegate, "fnegate", -n)
UNARY_FLOAT_OP(p_fround_up, "fround-up", ceil(n))
UNARY_FLOAT_OP(p_fround_down, "fround-down", floor(n))
UNARY_FLOAT_OP(p_inc, "f1+", n + 1.0)
UNARY_FLOAT_OP(p_dec, "f1-", n - 1.0)
UNARY_FLOAT_OP(p_sq, "fsq", n * n)

void p_fpow(Interpreter *interp) {
	if (interp->dsp < 2) {
		fail(interp, "f^: data stack underflow");
		return;
	}
	Val *left = &interp->data_stack[interp->dsp - 2];
	Val *right = &interp->data_stack[interp->dsp - 1];
	left->number = pow(left->number, right->number);
	interp->dsp--;

	DISPATCH(interp);
}

void p_fmodop(Interpreter *interp) {
	if (interp->dsp < 2) {
		fail(interp, "fmod: data stack underflow");
		return;
	}
	Val *left = &interp->data_stack[interp->dsp - 2];
	Val *right = &interp->data_stack[interp->dsp - 1];
	left->number = fmod(left->number, right->number);
	interp->dsp--;

	DISPATCH(interp);
}

void p_now(Interpreter *interp) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	push(interp, make_float((double)ts.tv_sec + (double)ts.tv_nsec / 1e9));

	DISPATCH(interp);
}

