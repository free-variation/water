#include "logicforth.h"
#include <unistd.h>

int object_alloc_slot(Interpreter *interp) {
	if (interp->n_objects < MAX_OBJECTS) {
		return interp->n_objects++;
	}
	if (interp->n_free_slots > 0) {
		return interp->free_slots[--interp->n_free_slots];
	}

	gc(interp);

	if (interp->n_free_slots > 0) {
		return interp->free_slots[--interp->n_free_slots];
	}
	return -1;
}

static Object *object_new(Interpreter *interp, ObjectKind kind, int *out_slot) {
	int slot = object_alloc_slot(interp);
	if (slot < 0) {
		fail(interp, "object registry full");
		*out_slot = -1;
		return NULL;
	}
	Object *obj = calloc(1, sizeof(*obj));
	obj->kind = kind;
	interp->objects[slot] = obj;
	*out_slot = slot;
	return obj;
}

int object_new_string(Interpreter *interp, const char *bytes, int length) {
	NEW_OBJECT(obj, OBJECT_STRING);
	obj->len = length;
	obj->capacity = length;
	obj->bytes = malloc((size_t)length + 1);
	memcpy(obj->bytes, bytes, (size_t)length);
	obj->bytes[length] = 0;
	return slot;
}

#define SET_INITIAL_CAPACITY 4
#define FRAME_INITIAL_CAPACITY 4

int object_new_set(Interpreter *interp) {
	NEW_OBJECT(obj, OBJECT_SET);
	obj->capacity = SET_INITIAL_CAPACITY;
	obj->items = malloc(sizeof(Val) * (size_t)obj->capacity);
	return slot;
}

int object_new_array(Interpreter *interp, int num_elements) {
	NEW_OBJECT(obj, OBJECT_ARRAY);
	obj->len = num_elements;
	obj->capacity = num_elements;
	obj->items = malloc(sizeof(Val) * (size_t)MAX(num_elements, 1));
	return slot;
}

int object_new_frame(Interpreter *interp) {
	NEW_OBJECT(obj, OBJECT_FRAME);
	obj->capacity = FRAME_INITIAL_CAPACITY;
	obj->frame.keys = malloc(sizeof(cell) * (size_t)obj->capacity);
	obj->frame.values = malloc(sizeof(Val) * (size_t)obj->capacity);
	return slot;
}

int object_new_matrix(Interpreter *interp, int num_rows, int num_columns) {
	NEW_OBJECT(obj, OBJECT_MATRIX);
	obj->matrix.rows = num_rows;
	obj->matrix.columns = num_columns;
	/* Compute the element count in size_t so a large rows*columns can't
	   overflow int before it reaches calloc. calloc already zero-fills. */
	size_t num_elements = (size_t)num_rows * (size_t)num_columns;
	obj->matrix.elements = calloc(num_elements ? num_elements : 1, sizeof(double));
	if (!obj->matrix.elements) {
		interp->objects[slot] = NULL;
		free(obj);
		fail(interp, "matrix too large to allocate");
		return -1;
	}
	return slot;
}

int object_new_continuation(Interpreter *interp, const Val *frames, int return_len, int resume_ip) {
	NEW_OBJECT(obj, OBJECT_CONTINUATION);
	obj->continuation.return_len = return_len;
	obj->continuation.resume_ip = resume_ip;
	obj->continuation.local_base_offset = -1;
	obj->continuation.return_slice = malloc(sizeof(Val) * (size_t)MAX(return_len, 1));
	memcpy(obj->continuation.return_slice, frames, sizeof(Val) * (size_t)return_len);
	return slot;
}

int val_cmp(Interpreter *interp, Val left, Val right) {

	if (VAL_TAG(left) != VAL_TAG(right))
		return (int)VAL_TAG(left) - (int)VAL_TAG(right);

	switch (VAL_TAG(left)) {
		case T_FLOAT: {
						  double left_value = VAL_NUMBER(left);
						  double right_value = VAL_NUMBER(right);
						  if (left_value < right_value)
						  	return -1;
						  if (left_value > right_value)
						  	return 1;
						  return 0;
					  }
		case T_SYMBOL: case T_XT: case T_ADDR:

					  if (VAL_DATA(left) < VAL_DATA(right))
					  	return -1;
					  if (VAL_DATA(left) > VAL_DATA(right))
					  	return 1;
					  return 0;
		case T_STRING: {
						   Object *left_string = interp->objects[VAL_DATA(left)];
						   Object *right_string = interp->objects[VAL_DATA(right)];
						   int compare_length = MIN(left_string->len, right_string->len);
						   int byte_diff = memcmp(left_string->bytes, right_string->bytes,
								   (size_t)compare_length);
						   if (byte_diff)
						   	return byte_diff;

						   return left_string->len - right_string->len;
					   }
		case T_SET: case T_ARRAY: {
									  Object *left_collection = interp->objects[VAL_DATA(left)];
									  Object *right_collection = interp->objects[VAL_DATA(right)];
									  int compare_length = MIN(left_collection->len, right_collection->len);
									  for (int i = 0; i < compare_length; i++) {
										  int element_cmp = val_cmp(interp, left_collection->items[i],
												  right_collection->items[i]);
										  if (element_cmp)
										  	return element_cmp;
									  }

									  return left_collection->len - right_collection->len;
								  }
		case T_MATRIX: {
						   Object *left_matrix = interp->objects[VAL_DATA(left)];
						   Object *right_matrix = interp->objects[VAL_DATA(right)];

						   if (left_matrix->matrix.rows != right_matrix->matrix.rows)
							   return left_matrix->matrix.rows - right_matrix->matrix.rows;
						   if (left_matrix->matrix.columns != right_matrix->matrix.columns)
							   return left_matrix->matrix.columns - right_matrix->matrix.columns;
						   int n = left_matrix->matrix.rows * left_matrix->matrix.columns;
						   for (int i = 0; i < n; i++) {
							   double a = left_matrix->matrix.elements[i];
							   double b = right_matrix->matrix.elements[i];
							   if (a < b)
							   	return -1;
							   if (a > b)
							   	return 1;
						   }
						   return 0;
					   }
		case T_FRAME: {
						  Object *left_frame = interp->objects[VAL_DATA(left)];
						  Object *right_frame = interp->objects[VAL_DATA(right)];
						  if (left_frame->len != right_frame->len)
							  return left_frame->len - right_frame->len;
						  for (int i = 0; i < left_frame->len; i++) {
							  cell left_key = left_frame->frame.keys[i];
							  cell right_key = right_frame->frame.keys[i];
							  if (left_key < right_key)
							  	return -1;
							  if (left_key > right_key)
							  	return 1;
							  int value_cmp = val_cmp(interp, left_frame->frame.values[i], right_frame->frame.values[i]);
							  if (value_cmp)
							  	return value_cmp;
						  }
						  return 0;
					  }

					  /* T_CONT, T_MARK, T_NONE have no ordering: they compare equal here, so
						 a set treats all continuations/marks as a single member. Identity-
						 based comparison is deferred to the planned logic layer. */
		default: return 0;
	}
}

void print_double(double number) {
	if (number == (double)(int64_t)number && number > -1e15 && number < 1e15)
		printf("%lld", (long long)number);
	else
		printf("%g", number);
}

#define PRINT_FIRST 10

#define PRINT_LAST 3

#define MAX_NESTING_DEPTH 100

int print_truncate = 1;

static int stdout_is_tty(void) {
	static int cached = -1;
	if (cached < 0)
		cached = isatty(fileno(stdout));
	return cached;
}

static int print_depth = 0;
static int suppress_depth_bg = 0;

static void print_depth_bg(int depth) {
	if (!stdout_is_tty() || suppress_depth_bg)
		return;
	if (depth <= 0) {
		fputs("\033[49m", stdout);
		return;
	}
	int idx = 238 + (depth - 1) * 3;
	if (idx > 250)
		idx = 250;
	printf("\033[48;5;%dm", idx);
}

static void print_depth_enter(void) { print_depth_bg(++print_depth); }
static void print_depth_leave(void) { print_depth_bg(--print_depth); }

void print_items(Interpreter *interp, Object *collection) {
	int length = collection->len;

	if (!print_truncate || length <= PRINT_FIRST + PRINT_LAST) {
		for (int i = 0; i < length; i++) {
			print_val(interp, collection->items[i]);
			putchar(' ');
		}
	} else {
		for (int i = 0; i < PRINT_FIRST; i++) {
			print_val(interp, collection->items[i]);
			putchar(' ');
		}
		fputs("... ", stdout);
		for (int i = length - PRINT_LAST; i < length; i++) {
			print_val(interp, collection->items[i]);
			putchar(' ');
		}
	}
}

void print_corners(Object *matrix) {
	double *elements = matrix->matrix.elements;
	int n = matrix->matrix.rows * matrix->matrix.columns;

	if (!print_truncate || n <= PRINT_FIRST + PRINT_LAST) {
		for (int i = 0; i < n; i++) {
			putchar(' ');
			print_double(elements[i]);
		}
	} else {
		for (int i = 0; i < PRINT_FIRST; i++) {
			putchar(' ');
			print_double(elements[i]);
		}
		fputs(" ...", stdout);
		for (int i = n - PRINT_LAST; i < n; i++) {
			putchar(' ');
			print_double(elements[i]);
		}
	}
}

#define MATRIX_DISP_FIRST_ROWS 5
#define MATRIX_DISP_LAST_ROWS 3
#define MATRIX_DISP_FIRST_COLS 5
#define MATRIX_DISP_LAST_COLS 3

void print_matrix_cell(double value) {
	printf(" %10.4g", value);
}

void print_matrix_grid(Object *m) {
	int rows = m->matrix.rows;
	int cols = m->matrix.columns;
	int rows_trunc = print_truncate
		&& rows > MATRIX_DISP_FIRST_ROWS + MATRIX_DISP_LAST_ROWS;
	int cols_trunc = print_truncate
		&& cols > MATRIX_DISP_FIRST_COLS + MATRIX_DISP_LAST_COLS;

	printf("<matrix %dx%d>\n", rows, cols);

	for (int i = 0; i < rows; i++) {
		if (rows_trunc) {
			if (i == MATRIX_DISP_FIRST_ROWS)
				fputs(" ...\n", stdout);
			if (i >= MATRIX_DISP_FIRST_ROWS && i < rows - MATRIX_DISP_LAST_ROWS)
				continue;
		}

		for (int j = 0; j < cols; j++) {
			if (cols_trunc) {
				if (j == MATRIX_DISP_FIRST_COLS)
					printf(" %10s", "...");
				if (j >= MATRIX_DISP_FIRST_COLS && j < cols - MATRIX_DISP_LAST_COLS)
					continue;
			}
			print_matrix_cell(MAT(m, i, j));
		}
		putchar('\n');
	}
}

void print_val(Interpreter *interp, Val value) {
	switch (VAL_TAG(value)) {
		case T_FLOAT: print_double(VAL_NUMBER(value)); break;
		case T_SYMBOL: printf(":%s", &interp->vocab->symbol_pool[VAL_DATA(value)]); break;
		case T_STRING: fputs(interp->objects[VAL_DATA(value)]->bytes, stdout); break;
		case T_SET:
					   print_depth_enter();
					   if (print_depth > MAX_NESTING_DEPTH) {
						   fputs("<...>", stdout);
					   } else {
						   fputs("< ", stdout);
						   print_items(interp, interp->objects[VAL_DATA(value)]);
						   putchar('>');
					   }
					   print_depth_leave();
					   break;
		case T_ARRAY:
					   print_depth_enter();
					   if (print_depth > MAX_NESTING_DEPTH) {
						   fputs("[...]", stdout);
					   } else {
						   fputs("[ ", stdout);
						   print_items(interp, interp->objects[VAL_DATA(value)]);
						   putchar(']');
					   }
					   print_depth_leave();
					   break;
		case T_XT: printf("<xt %lld>", (long long)VAL_DATA(value)); break;
		case T_ADDR: printf("<addr %lld>", (long long)VAL_DATA(value)); break;
		case T_MATRIX: {
						   Object *matrix = interp->objects[VAL_DATA(value)];
						   print_depth_enter();
						   printf("<matrix %dx%d: ", matrix->matrix.rows, matrix->matrix.columns);
						   print_corners(matrix);
						   putchar('>');
						   print_depth_leave();
						   break;
					   }
		case T_FRAME: {
						  Object *frame = interp->objects[VAL_DATA(value)];
						  print_depth_enter();
						  if (print_depth > MAX_NESTING_DEPTH) {
							  fputs("{...}", stdout);
						  } else {
							  fputs("{ ", stdout);
							  for (int i = 0; i < frame->len; i++) {
								  printf(":%s ", &interp->vocab->symbol_pool[frame->frame.keys[i]]);
								  print_val(interp, frame->frame.values[i]);
								  putchar(' ');
							  }
							  putchar('}');
						  }
						  print_depth_leave();
						  break;
					  }
		default: printf("<?>"); break;
	}
}

void print_val_compact(Interpreter *interp, Val value) {
	switch (VAL_TAG(value)) {
		case T_FLOAT: {
						  double number = VAL_NUMBER(value);
						  if (number == (double)(int64_t)number && number > -1e12 && number < 1e12)
							  printf("%lld", (long long)number);
						  else
							  printf("%.4g", number);
						  break;
					  }
		case T_STRING: {
						   Object *obj = interp->objects[VAL_DATA(value)];
						   if (obj->len <= 10)
						   	printf("\"%.*s\"", obj->len, obj->bytes);
						   else
						   	printf("\"%.9s…\"", obj->bytes);
						   break;
					   }
		case T_SYMBOL: {
						   const char *name = &interp->vocab->symbol_pool[VAL_DATA(value)];
						   int len = (int)strlen(name);
						   if (len <= 10)
						   	printf(":%s", name);
						   else
						   	printf(":%.9s…", name);
						   break;
					   }
		case T_SET:
					   print_depth_enter();
					   printf("<%d>", interp->objects[VAL_DATA(value)]->len);
					   print_depth_leave();
					   break;
		case T_ARRAY:
					   print_depth_enter();
					   printf("[%d]", interp->objects[VAL_DATA(value)]->len);
					   print_depth_leave();
					   break;
		case T_FRAME:
					   print_depth_enter();
					   printf("{%d}", interp->objects[VAL_DATA(value)]->len);
					   print_depth_leave();
					   break;
		case T_MATRIX: {
						   Object *m = interp->objects[VAL_DATA(value)];
						   print_depth_enter();
						   printf("M%dx%d", m->matrix.rows, m->matrix.columns);
						   print_depth_leave();
						   break;
					   }
		case T_XT: {
					   int target = (int)VAL_DATA(value);
					   const char *name = NULL;
					   for (int cfa = interp->vocab->latest_cfa; cfa != 0; cfa = (int)WORD_LINK(interp->vocab, cfa)) {
						   if (cfa == target) {
							   name = &interp->vocab->name_pool[WORD_NAME(interp->vocab, cfa)];
							   break;
						   }
					   }
					   if (name) {
						   int len = (int)strlen(name);
						   if (len <= 9)
						   	printf("'%s", name);
						   else
						   	printf("'%.8s…", name);
					   } else {
						   printf("'?");
					   }
					   break;
				   }
		case T_ADDR: printf("@%lld", (long long)VAL_DATA(value)); break;
		case T_CONT: fputs("k", stdout); break;
		default: fputs("?", stdout); break;
	}
}

void print_frame_pretty(Interpreter *interp, Object *frame, int indent) {
	if (indent > 2 * MAX_NESTING_DEPTH) {
		fputs("{...}", stdout);
		return;
	}
	fputs("{\n", stdout);
	for (int i = 0; i < frame->len; i++) {
		for (int s = 0; s < indent + 2; s++)
			putchar(' ');
		printf(":%s ", &interp->vocab->symbol_pool[frame->frame.keys[i]]);
		Val value = frame->frame.values[i];
		if (VAL_TAG(value) == T_FRAME)
			print_frame_pretty(interp, interp->objects[VAL_DATA(value)], indent + 2);
		else
			print_val(interp, value);
		putchar('\n');
	}
	for (int s = 0; s < indent; s++)
		putchar(' ');
	putchar('}');
}

void print_prompt_state(Interpreter *interp) {
	int tty = stdout_is_tty();
	if (tty)
		fputs(interp->error_flag ? "\033[41m" : "\033[42m", stdout);

	if (interp->error_flag) {
		printf("%d|error", interp->dsp);
	} else if (interp->dsp == 0) {
		putchar('0');
	} else {
		printf("%d|", interp->dsp);
		suppress_depth_bg = 1;
		print_val_compact(interp, interp->data_stack[interp->dsp - 1]);
		suppress_depth_bg = 0;
	}

	if (tty)
		fputs("\033[0m", stdout);
	putchar(' ');
}

int find(Interpreter *interp, const char *name) {
	int cfa = interp->vocab->latest_cfa;
	while (cfa != 0) {
		if (strcmp(&interp->vocab->name_pool[WORD_NAME(interp->vocab, cfa)], name) == 0)
			return cfa;
		cfa = (int)WORD_LINK(interp->vocab, cfa);
	}
	return 0;
}

static inline __attribute__((always_inline)) void push_variable(Interpreter *interp, int var_cfa) {
	Val value;
	value.bits = (uint64_t)interp->vocab->dict[var_cfa + 1];
	push(interp, value);
};

static inline __attribute__((always_inline)) void push_symbol(Interpreter *interp, int sym_cfa) {
	push(interp, make_symbol((int)interp->vocab->dict[sym_cfa + 1]));
}

void docol(Interpreter *interp) {
	int target_cfa = (int)interp->vocab->dict[interp->ip++];
	rpush(interp, make_addr(interp->ip));
	interp->ip = target_cfa + 1;

	DISPATCH(interp);
}

void dosym(Interpreter *interp) {
	int sym_cfa = (int)interp->vocab->dict[interp->ip++];
	push_symbol(interp, sym_cfa);

	DISPATCH(interp);
}

void dovar(Interpreter *interp) {
	int var_cfa = (int)interp->vocab->dict[interp->ip++];
	push_variable(interp, var_cfa);

	DISPATCH(interp);
}

void run_inner(Interpreter *interp) {
	int initial_rsp = interp->rsp;

	while (interp->running && !interp->error_flag) {
		if (interp->unwinding) {
			if (interp->rsp <= initial_rsp)
				break;

			Val frame = interp->return_stack[--interp->rsp];
			if (VAL_TAG(frame) == T_MARK && (int)VAL_DATA(frame) == interp->unwind_target) {
				interp->unwinding = 0;

				if (interp->rsp > 0) {
					Val ret = interp->return_stack[--interp->rsp];
					interp->ip = (int)VAL_DATA(ret);
				}
				continue;
			}
			continue;
		}

		cfa_handler handler = (cfa_handler)interp->vocab->dict[interp->ip++];
		handler(interp);
	}
}

void execute_cfa(Interpreter *interp, int cfa) {
	cfa_handler handler = (cfa_handler)interp->vocab->dict[cfa];

	if (handler == dovar) {
		push_variable(interp, cfa);
		return;
	}

	if (handler == dosym) {
		push_symbol(interp, cfa);
		return;
	}

	int saved_ip = interp->ip;
	int saved_running = interp->running;
	cell saved_slot_0 = interp->vocab->dict[TRAMPOLINE_SLOT];
	cell saved_slot_1 = interp->vocab->dict[TRAMPOLINE_SLOT + 1];
	cell saved_slot_2 = interp->vocab->dict[TRAMPOLINE_SLOT + 2];
	cell stop_handler = interp->vocab->dict[interp->vocab->stop_cfa];

	if (handler == docol) {
		interp->vocab->dict[TRAMPOLINE_SLOT] = (cell)docol;
		interp->vocab->dict[TRAMPOLINE_SLOT + 1] = (cell)cfa;
		interp->vocab->dict[TRAMPOLINE_SLOT + 2] = stop_handler;
	} else {
		interp->vocab->dict[TRAMPOLINE_SLOT] = (cell)handler;
		interp->vocab->dict[TRAMPOLINE_SLOT + 1] = stop_handler;
		interp->vocab->dict[TRAMPOLINE_SLOT + 2] = stop_handler;
	}
	interp->ip = TRAMPOLINE_SLOT;
	interp->running = 1;

	run_inner(interp);

	interp->running = saved_running;
	interp->ip = saved_ip;
	interp->vocab->dict[TRAMPOLINE_SLOT] = saved_slot_0;
	interp->vocab->dict[TRAMPOLINE_SLOT + 1] = saved_slot_1;
	interp->vocab->dict[TRAMPOLINE_SLOT + 2] = saved_slot_2;
}

void call_open(Interpreter *interp, int cfa, CallContext *ctx) {
	cfa_handler handler = (cfa_handler)interp->vocab->dict[cfa];

	if (handler == dovar || handler == dosym) {
		ctx->fast = 0;
		return;
	}

	ctx->fast = 1;
	ctx->saved_ip = interp->ip;
	ctx->saved_running = interp->running;
	ctx->saved_slot_0 = interp->vocab->dict[TRAMPOLINE_SLOT];
	ctx->saved_slot_1 = interp->vocab->dict[TRAMPOLINE_SLOT + 1];
	ctx->saved_slot_2 = interp->vocab->dict[TRAMPOLINE_SLOT + 2];

	cell stop_handler = interp->vocab->dict[interp->vocab->stop_cfa];
	if (handler == docol) {
		interp->vocab->dict[TRAMPOLINE_SLOT] = (cell)docol;
		interp->vocab->dict[TRAMPOLINE_SLOT + 1] = (cell)cfa;
		interp->vocab->dict[TRAMPOLINE_SLOT + 2] = stop_handler;
	} else {
		interp->vocab->dict[TRAMPOLINE_SLOT] = (cell)handler;
		interp->vocab->dict[TRAMPOLINE_SLOT + 1] = stop_handler;
		interp->vocab->dict[TRAMPOLINE_SLOT + 2] = stop_handler;
	}
}

void call_invoke(Interpreter *interp) {
	interp->ip = TRAMPOLINE_SLOT;
	interp->running = 1;
	run_inner(interp);
}

void call_close(Interpreter *interp, CallContext *ctx) {
	if (!ctx->fast)
		return;
	interp->running = ctx->saved_running;
	interp->ip = ctx->saved_ip;
	interp->vocab->dict[TRAMPOLINE_SLOT] = ctx->saved_slot_0;
	interp->vocab->dict[TRAMPOLINE_SLOT + 1] = ctx->saved_slot_1;
	interp->vocab->dict[TRAMPOLINE_SLOT + 2] = ctx->saved_slot_2;
}


int alloc_name(Interpreter *interp, const char *name) {
	int length = (int)strlen(name) + 1;
	if (interp->vocab->names_here + length > NAME_POOL) {
		fail(interp, "name pool full");
		return 0;
	}
	int name_offset = interp->vocab->names_here;
	memcpy(&interp->vocab->name_pool[interp->vocab->names_here], name, (size_t)length);
	interp->vocab->names_here += length;

	return name_offset;
}

int intern_symbol(Interpreter *interp, const char *name) {
	for (int i = 0; i < interp->vocab->symbol_pool_here; ) {
		if (strcmp(&interp->vocab->symbol_pool[i], name) == 0)
			return i;
		i += (int)strlen(&interp->vocab->symbol_pool[i]) + 1;
	}
	int length = (int)strlen(name) + 1;
	if (interp->vocab->symbol_pool_here + length > SYMBOL_POOL) {
		fail(interp, "symbol pool full");
		return 0;
	}
	int offset = interp->vocab->symbol_pool_here;
	memcpy(&interp->vocab->symbol_pool[interp->vocab->symbol_pool_here], name, (size_t)length);
	interp->vocab->symbol_pool_here += length;
	return offset;
}

void dict_ensure(Interpreter *interp, int extra) {
	Vocabulary *v = interp->vocab;
	if (v->here + extra <= v->dict_cap) {
		return;
	}
	int newcap = v->dict_cap;
	while (v->here + extra > newcap) {
		newcap *= 2;
	}
	v->dict = realloc(v->dict, (size_t)newcap * sizeof(cell));
	memset(v->dict + v->dict_cap, 0, (size_t)(newcap - v->dict_cap) * sizeof(cell));
	v->dict_cap = newcap;
}

int create_header(Interpreter *interp, const char *name, int flags) {
	dict_ensure(interp, 4);

	int previous_latest = interp->vocab->latest_cfa;
	int name_offset = alloc_name(interp, name);
	interp->vocab->dict[interp->vocab->here++] = previous_latest;
	interp->vocab->dict[interp->vocab->here++] = flags;
	interp->vocab->dict[interp->vocab->here++] = name_offset;
	interp->vocab->dict[interp->vocab->here++] = 0;

	interp->vocab->latest_cfa = interp->vocab->here;
	return interp->vocab->latest_cfa;
}

int define_primitive(Interpreter *interp, const char *name, cfa_handler handler, int flags) {
	int cfa = create_header(interp, name, flags);
	emit(interp, (cell)handler);
	return cfa;
}

void emit(Interpreter *interp, cell value) {
	dict_ensure(interp, 1);
	interp->vocab->dict[interp->vocab->here++] = value;
}

void emit_call(Interpreter *interp, int target_cfa) {
	cfa_handler handler = (cfa_handler)interp->vocab->dict[target_cfa];
	emit(interp, (cell)handler);

	if (handler == docol || handler == dovar || handler == dosym) {
		emit(interp, (cell)target_cfa);
	}
}

void emit_val_literal(Interpreter *interp, Val value) {
	emit_call(interp, interp->vocab->literal_cfa);
	emit(interp, (cell)value.bits);
}

void fail(Interpreter *interp, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(interp->error_message, sizeof(interp->error_message), fmt, args);
	va_end(args);
	interp->error_flag = 1;
}

const char *tag_name(Tag t) {
	switch (t) {
		case T_NONE:   return "none";
		case T_SYMBOL:    return "a symbol";
		case T_FLOAT:  return "a float";
		case T_STRING: return "a string";
		case T_SET:    return "a set";
		case T_ARRAY:  return "an array";
		case T_FRAME:  return "a frame";
		case T_MATRIX: return "a matrix";
		case T_XT:     return "an execution token";
		case T_ADDR:   return "an address";
		case T_CONT:   return "a continuation";
		case T_MARK:   return "a mark";
		default:       return "an unknown value";
	}
}

void p_exit(Interpreter *interp) {
	while (interp->rsp > 0 && VAL_TAG(interp->return_stack[interp->rsp - 1]) == T_MARK) 
		interp->rsp--;

	if (interp->rsp <= 0) {
		interp->running = 0;
		return;
	}

	Val saved_ip = interp->return_stack[--interp->rsp];
	interp->ip = (int)VAL_DATA(saved_ip);

	DISPATCH(interp);
}

void p_stop(Interpreter *interp) {
	interp->running = 0;
}

void p_literal(Interpreter *interp) {
	Val value;
	value.bits = (uint64_t)interp->vocab->dict[interp->ip++];
	push(interp, value);

	DISPATCH(interp);
}

void p_branch(Interpreter *interp) {
	interp->ip += (int)interp->vocab->dict[interp->ip];

	DISPATCH(interp);
}

#define ZBRANCH_BODY(get_condition) \
	cell offset = interp->vocab->dict[interp->ip++]; \
	get_condition; \
	int is_false = (VAL_TAG(condition) == T_FLOAT) ? (VAL_NUMBER(condition) == 0.0) \
	: (VAL_DATA(condition) == 0); \
	if (is_false) \
		interp->ip += offset - 1

void p_0branch(Interpreter *interp) {
	ZBRANCH_BODY(POP(condition));

	DISPATCH(interp);
}

void p_qzbranch(Interpreter *interp) {
	ZBRANCH_BODY(PEEK_AT(condition, 0, "?if"));

	DISPATCH(interp);
}

void p_dostr(Interpreter *interp) {
	int template_handle = (int)interp->vocab->dict[interp->ip++];
	push(interp, make_string(interpolate(interp, template_handle)));

	DISPATCH(interp);
}

void p_enter_locals(Interpreter *interp) {
	int n_locals = (int)interp->vocab->dict[interp->ip++];
	if (interp->rsp + n_locals + 1 > RETURN_STACK_DEPTH) {
		fail(interp, "return stack overflow");
		return;
	}
	interp->return_stack[interp->rsp++] = make_addr(interp->local_base);
	interp->rsp += n_locals;
	interp->local_base = interp->rsp - n_locals;

	DISPATCH(interp);
}

void p_enter_locals_to(Interpreter *interp) {
	int n_locals = (int)interp->vocab->dict[interp->ip++];
	if (interp->rsp + n_locals + 1 > RETURN_STACK_DEPTH) {
		fail(interp, "(enter-locals-to): return stack overflow");
		return;
	}
	if (interp->dsp < n_locals) {
		fail(interp, "(enter-locals-to): insufficient values on data stack; need %d", n_locals);
		return;
	}

	interp->return_stack[interp->rsp++] = make_addr(interp->local_base);
	int data_start = interp->dsp - n_locals;
	for (int i = 0; i < n_locals; i++)
		interp->return_stack[interp->rsp + i] = interp->data_stack[data_start + i];

	interp->dsp -= n_locals;
	interp->local_base = interp->rsp;
	interp->rsp += n_locals;

	DISPATCH(interp);
}

void p_enter_locals_mixed(Interpreter *interp) {
	int n_locals = (int)interp->vocab->dict[interp->ip++];
	int n_received = (int)interp->vocab->dict[interp->ip++];

	if (interp->rsp + n_locals + 1 > RETURN_STACK_DEPTH) {
		fail(interp, "(enter-locals-mixed): return stack overflow");
		return;
	}
	if (interp->dsp < n_received) {
		fail(interp, "(enter-locals-mixed): insufficient values on data stack; need %d", n_received);
		return;
	}

	interp->return_stack[interp->rsp++] = make_addr(interp->local_base);
	interp->local_base = interp->rsp;
	interp->rsp += n_locals;

	int data_start = interp->dsp - n_received;
	for (int i = 0; i < n_received; i++) {
		int slot = (int)interp->vocab->dict[interp->ip++];
		interp->return_stack[interp->local_base + slot] = interp->data_stack[data_start + i];
	}
	interp->dsp -= n_received;

	DISPATCH(interp);
}

void p_leave_locals(Interpreter *interp) {
	int n_locals = (int)interp->vocab->dict[interp->ip++];
	interp->rsp -= n_locals;
	Val saved = rpop(interp);
	interp->local_base = (int)VAL_DATA(saved);

	DISPATCH(interp);
}

static Val *local_slot(Interpreter *interp) {
	int depth = (int)interp->vocab->dict[interp->ip++];
	int slot  = (int)interp->vocab->dict[interp->ip++];

	int base = interp->local_base;
	for (int i = 0; i < depth; i++)
		base = (int)VAL_DATA(interp->return_stack[base - 1]);

	return &interp->return_stack[base + slot];
}

void p_local_fetch(Interpreter *interp) {
	push(interp, *local_slot(interp));

	DISPATCH(interp);
}

void p_local_store(Interpreter *interp) {
	*local_slot(interp) = pop(interp);

	DISPATCH(interp);
}

void p_local_fetch_0depth(Interpreter *interp) {
	push(interp, interp->return_stack[interp->local_base + (int)interp->vocab->dict[interp->ip++]]);

	DISPATCH(interp);
}

void p_local_store_0depth(Interpreter *interp) {
	interp->return_stack[interp->local_base + (int)interp->vocab->dict[interp->ip++]] = pop(interp);

	DISPATCH(interp);
}

#define LOCAL_ARITH_0DEPTH(name, word_name, expr) \
	void name(Interpreter *interp) { \
		int slot = (int)interp->vocab->dict[interp->ip++]; \
		Val *p = &interp->return_stack[interp->local_base + slot]; \
		if (VAL_TAG(*p) != T_FLOAT) { \
			fail(interp, word_name ": expected a float local; got %s", tag_name(VAL_TAG(*p))); \
			return; \
		} \
		double n = VAL_NUMBER(*p); \
		*p = make_float(expr); \
	}
LOCAL_ARITH_0DEPTH(p_local_incr_0depth, "(local+!)", n + 1.0)
LOCAL_ARITH_0DEPTH(p_local_decr_0depth, "(local-!)", n - 1.0)

void p_set(Interpreter *interp) {
	POP_INT(count, "set", "count");
	if (count < 0 || count > interp->dsp) {
		fail(interp, "set: count %d out of range (stack has %d available)", count, interp->dsp);
		return;
	}

	int set_handle = object_new_set(interp);
	if (interp->error_flag)
		return;

	int first_item = interp->dsp - count;
	for (int i = 0; i < count; i++) 
		set_add(interp, set_handle, interp->data_stack[first_item + i]);
	interp->dsp = first_item;

	push(interp, make_set(set_handle));

	DISPATCH(interp);
}

void inbuf_reset(Interpreter *interp) {
	interp->input_buffer_len = 0;
	interp->input_buffer_pos = 0;
	interp->input_buffer[0] = 0;
	interp->need_more = 0;
}

char *next_token(Interpreter *interp) {
	while (interp->input_buffer_pos < interp->input_buffer_len
	       && isspace((unsigned char)interp->input_buffer[interp->input_buffer_pos]))
		interp->input_buffer_pos++;

	if (interp->input_buffer_pos >= interp->input_buffer_len)
		return NULL;

	int start = interp->input_buffer_pos;
	while (interp->input_buffer_pos < interp->input_buffer_len
	       && !isspace((unsigned char)interp->input_buffer[interp->input_buffer_pos]))
		interp->input_buffer_pos++;

	int length = interp->input_buffer_pos - start;
	if (length >= (int)sizeof(interp->token_buffer))
		length = sizeof(interp->token_buffer) - 1;

	memcpy(interp->token_buffer, interp->input_buffer + start, (size_t)length);
	interp->token_buffer[length] = 0;
	return interp->token_buffer;
}

int parse_float(const char *text, double *out) {
	if (!*text)
		return 0;

	char *end_of_number;
	double value = strtod(text, &end_of_number);
	if (*end_of_number != 0)
		return 0;
	*out = value;
	return 1;
}

static void skip_whitespace(Interpreter *interp) {
	while (interp->input_buffer_pos < interp->input_buffer_len
			&& isspace((unsigned char)interp->input_buffer[interp->input_buffer_pos]))
		interp->input_buffer_pos++;
}

static void skip_to_char(Interpreter *interp, char delimiter) {
	while (interp->input_buffer_pos < interp->input_buffer_len
			&& interp->input_buffer[interp->input_buffer_pos] != delimiter)
		interp->input_buffer_pos++;
}

static int comment_starts_here(Interpreter *interp) {
	int next = interp->input_buffer_pos + 1;
	return next >= interp->input_buffer_len
		|| isspace((unsigned char)interp->input_buffer[next]);
}

static void compile_or_push(Interpreter *interp, Val value) {
	if (interp->compiling)
		emit_val_literal(interp, value);
	else
		push(interp, value);
	interp->fuse_prev_var = 0;
	interp->fuse_prev2_var = 0;
}

int find_local(Interpreter *interp, const char *token, int *depth_out, int *slot_out) {
	for (int scope = interp->n_local_scopes - 1; scope >= 0; scope--) {
		int slice_start = interp->local_scope_starts[scope];
		int slice_end = (scope + 1 < interp->n_local_scopes)
			? interp->local_scope_starts[scope + 1]
			: interp->n_local_names;

		for (int name_idx = slice_start; name_idx < slice_end; name_idx++) {
			const char *name = &interp->local_names_pool[interp->local_name_offsets[name_idx]];
			if (strcmp(token, name) != 0)
				continue;

			int depth = 0;
			for (int inner = scope + 1; inner < interp->n_local_scopes; inner++) {
				int inner_start = interp->local_scope_starts[inner];
				int inner_end = (inner + 1 < interp->n_local_scopes)
					? interp->local_scope_starts[inner + 1]
					: interp->n_local_names;
				if (inner_end > inner_start) 
					depth++;
			}

			*depth_out = depth;
			*slot_out = name_idx - slice_start;
			return 1;
		}
	}
	return 0;
}

void run_outer(Interpreter *interp) {
	while (!interp->error_flag) {
		skip_whitespace(interp);
		if (interp->input_buffer_pos >= interp->input_buffer_len)
			return;

		char ch = interp->input_buffer[interp->input_buffer_pos];
		if (ch == '"') {
			int n = read_string_literal(interp);
			if (n < 0)
				return;
			int handle = object_new_string(interp, interp->token_buffer, n);
			if (interp->compiling) {
				emit_call(interp, interp->vocab->dostr_cfa);
				emit(interp, (cell)handle);
			} else {
				int r = interpolate(interp, handle);
				push(interp, make_string(r));
			}
			interp->fuse_prev_var = 0;
			interp->fuse_prev2_var = 0;
			continue;
		}
		if (ch == '(' && comment_starts_here(interp)) {
			skip_to_char(interp, ')');
			if (interp->input_buffer_pos < interp->input_buffer_len) 
				interp->input_buffer_pos++;
			continue;
		}
		if (ch == '\\' && comment_starts_here(interp)) {
			skip_to_char(interp, '\n');
			continue;
		}

		char *tok = next_token(interp);
		if (!tok)
			return;

		if (interp->compiling) {
			int local_depth, local_slot_idx;
			if (find_local(interp, tok, &local_depth, &local_slot_idx)) {
				if (local_depth == 0) {
					emit_call(interp, interp->vocab->local_fetch_0depth_cfa);
					emit(interp, (cell)local_slot_idx);
				} else {
					emit_call(interp, interp->vocab->local_fetch_cfa);
					emit(interp, (cell)local_depth);
					emit(interp, (cell)local_slot_idx);
				}
				interp->fuse_prev_var = 0;
				interp->fuse_prev2_var = 0;
				continue;
			}
		}

		int cf = find(interp, tok);
		if (cf) {
			if (interp->compiling && !WORD_IS_IMMEDIATE(interp->vocab, cf)) {
				if (superword_try_fuse(interp, cf)) {
					continue;
				}
				if (WORD_IS_INLINE(interp->vocab, cf)) {
					inline_word_body(interp, cf);
					interp->fuse_prev_var = 0;
					interp->fuse_prev2_var = 0;
				} else if ((cfa_handler)interp->vocab->dict[cf] == dovar) {
					emit_call(interp, (cell)cf);
					interp->fuse_prev2_var = interp->fuse_prev_var;
					interp->fuse_prev_var = cf;
				} else {
					emit_call(interp, (cell)cf);
					interp->fuse_prev_var = 0;
					interp->fuse_prev2_var = 0;
				}
			} else {
				execute_cfa(interp, cf);
				interp->fuse_prev_var = 0;
				interp->fuse_prev2_var = 0;
			}
			continue;
		}

		if (tok[0] == ':' && tok[1] != '\0') {
			Val value = make_symbol(intern_symbol(interp, tok + 1));
			if (interp->error_flag)
				return;
			compile_or_push(interp, value);
			continue;
		}

		if (tok[0] == '/' && tok[1] != '\0') {
			char path[INPUT_BUFFER_SIZE];
			strncpy(path, tok, sizeof(path) - 1);
			path[sizeof(path) - 1] = '\0';

			int count = 0;
			for (char *p = path; *p; ) {
				while (*p == '/')
					p++;
				if (!*p)
					break;
				count++;
				while (*p && *p != '/')
					p++;
			}
			if (count == 0) {
				fail(interp, "path literal %s has no segments", tok);
				return;
			}

			int handle = object_new_array(interp, count);
			if (interp->error_flag)
				return;
			Object *path_array = interp->objects[handle];

			int idx = 0;
			for (char *p = path; *p; ) {
				while (*p == '/')
					p++;
				if (!*p)
					break;
				char *segment = p;
				while (*p && *p != '/')
					p++;
				if (*p) {
					*p = '\0';
					p++;
				}
				path_array->items[idx++] = make_symbol(intern_symbol(interp, segment));
			}

			compile_or_push(interp, make_array(handle));
			continue;
		}

		double dv;
		if (parse_float(tok, &dv)) {
			compile_or_push(interp, make_float(dv));
			continue;
		}
		fail(interp, "unknown word: %s", tok);
		return;
	}
}

void record_loaded_file(Interpreter *interp, const char *filename) {
	for (int i = 0; i < interp->n_loaded_files; i++) {
		if (strcmp(interp->loaded_files[i], filename) == 0)
			return;
	}
	if (interp->n_loaded_files >= MAX_LOADED_FILES) {
		fail(interp, "load: %d-file history limit reached", MAX_LOADED_FILES);
		return;
	}
	interp->loaded_files[interp->n_loaded_files] = strdup(filename);
	interp->n_loaded_files++;
}

void load_file(Interpreter *interp, const char *filename) {
	FILE *file = fopen(filename, "r");
	if (!file) {
		fail(interp, "cannot open %s", filename);
		return;
	}

	fseek(file, 0, SEEK_END);
	long file_size = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (file_size < 0 || file_size >= INPUT_BUFFER_SIZE) {
		fail(interp, "%s too large or invalid (%ld bytes, max %d)",
				filename, file_size, INPUT_BUFFER_SIZE - 1);
		fclose(file);
		return;
	}

	char *saved_inbuf_contents = malloc((size_t)interp->input_buffer_len + 1);
	memcpy(saved_inbuf_contents, interp->input_buffer, (size_t)interp->input_buffer_len);
	int saved_inbuf_len = interp->input_buffer_len;
	int saved_inbuf_pos = interp->input_buffer_pos;
	int saved_need_more = interp->need_more;

	size_t bytes_read = fread(interp->input_buffer, 1, (size_t)file_size, file);
	fclose(file);
	interp->input_buffer[bytes_read] = 0;
	interp->input_buffer_len = (int)bytes_read;
	interp->input_buffer_pos = 0;
	interp->need_more = 0;

	interp->load_depth++;
	run_outer(interp);
	interp->load_depth--;

	if (!interp->error_flag && interp->need_more) {
		fail(interp, "%s: unterminated string literal", filename);
	}
	if (!interp->error_flag && interp->compiling) {
		fail(interp, "%s: unterminated definition", filename);
		interp->compiling = 0;
	}

	memcpy(interp->input_buffer, saved_inbuf_contents, (size_t)saved_inbuf_len);
	interp->input_buffer[saved_inbuf_len] = 0;
	interp->input_buffer_len = saved_inbuf_len;
	interp->input_buffer_pos = saved_inbuf_pos;
	interp->need_more = saved_need_more;
	free(saved_inbuf_contents);
}

void p_load(Interpreter *interp) {
	POP_STRING(filename_obj, "load");
	gc_root_push(interp, filename_obj_val);

	const char *filename = filename_obj->bytes;
	if (interp->load_depth == 0)
		record_loaded_file(interp, filename);
	load_file(interp, filename);

	gc_root_pop(interp);

	DISPATCH(interp);
}

void p_reload(Interpreter *interp) {
	forget_user(interp);

	for (int i = 0; i < interp->n_loaded_files; i++) {
		load_file(interp, interp->loaded_files[i]);
		if (interp->error_flag)
			return;
	}

	DISPATCH(interp);
}

void mark_value(Interpreter *interp, Val value) {
	if (VAL_TAG(value) != T_STRING &&
			VAL_TAG(value) != T_SET &&
			VAL_TAG(value) != T_ARRAY &&
			VAL_TAG(value) != T_FRAME &&
			VAL_TAG(value) != T_MATRIX &&
			VAL_TAG(value) != T_CONT) return;

	int handle = (int)VAL_DATA(value);
	if (handle < 0 || handle >= MAX_OBJECTS || !interp->objects[handle] || interp->object_mark[handle])
		return;

	interp->object_mark[handle] = 1;
	Object *obj = interp->objects[handle];
	if (obj->kind == OBJECT_SET || obj->kind == OBJECT_ARRAY) {
		for (int i = 0; i < obj->len; i++)
			mark_value(interp, obj->items[i]);
	} else if (obj->kind == OBJECT_FRAME) {
		for (int i = 0; i < obj->len; i++)
			mark_value(interp, obj->frame.values[i]);
	} else if (obj->kind == OBJECT_CONTINUATION) {
		for (int i = 0; i < obj->continuation.return_len; i++)
			mark_value(interp, obj->continuation.return_slice[i]);
	}
}

static void copy_value_inner(Interpreter *interp, Val source_val, Val *copy_val, int depth) {
	int i, copy_handle;

	if (depth > MAX_NESTING_DEPTH) {
		fail(interp, "copy: structure too deeply nested (cycle?)");
		return;
	}

	switch(VAL_TAG(source_val)) {
		case T_STRING: {
						   Object *source = interp->objects[VAL_DATA(source_val)];
						   copy_handle = object_new_string(interp, source->bytes, source->len);
						   if (interp->error_flag)
						   	return;
						   *copy_val = make_string(copy_handle);
						   return;
					   }

		case T_MATRIX: {
						   Object *source = interp->objects[VAL_DATA(source_val)];
						   copy_handle = object_new_matrix(interp, source->matrix.rows, source->matrix.columns);
						   if (interp->error_flag)
						   	return;
						   Object *copy = interp->objects[copy_handle];
						   memcpy(copy->matrix.elements, source->matrix.elements, sizeof(double) * (size_t)source->matrix.rows * source->matrix.columns);
						   *copy_val = make_matrix(copy_handle);
						   return;
					   }

		case T_ARRAY:
		case T_SET: {
						Object *source = interp->objects[VAL_DATA(source_val)];
						copy_handle = (VAL_TAG(source_val) == T_ARRAY) ? object_new_array(interp, source->len) : object_new_set(interp);
						if (interp->error_flag)
							return;

						Object *copy = interp->objects[copy_handle];
						if (source->len > copy->capacity) {
							while (copy->capacity < source->len) 
								copy->capacity *= 2;
							copy->items = realloc(copy->items, sizeof(Val) * (size_t)copy->capacity);
						}

						memset(copy->items, 0, sizeof(Val) * (size_t)source->len);
						copy->len = source->len;
						*copy_val = (VAL_TAG(source_val) == T_ARRAY) ? make_array(copy_handle) : make_set(copy_handle);
						for (i = 0; i < source->len; i++)
							copy_value_inner(interp, source->items[i], &copy->items[i], depth + 1);
						return;
					}

		case T_FRAME: {
						  Object *source = interp->objects[VAL_DATA(source_val)];
						  copy_handle = object_new_frame(interp);
						  if (interp->error_flag)
						  	return;

						  Object *copy = interp->objects[copy_handle];
						  if (source->len > copy->capacity) {
							  while (copy->capacity < source->len)
								  copy->capacity *= 2;
							  copy->frame.keys = realloc(copy->frame.keys, sizeof(cell) * (size_t)copy->capacity);
							  copy->frame.values = realloc(copy->frame.values, sizeof(Val) * (size_t)copy->capacity);
						  }

						  for (i = 0; i < source->len; i++)
							  copy->frame.keys[i] = source->frame.keys[i];
						  memset(copy->frame.values, 0, sizeof(Val) * (size_t)source->len);
						  copy->len = source->len;
						  *copy_val = make_frame(copy_handle);
						  for (i = 0; i < source->len; i++)
							  copy_value_inner(interp, source->frame.values[i], &copy->frame.values[i], depth + 1);
						  return;
					  }
		default:
					  *copy_val = source_val;
					  return;
	}
}

void copy_value(Interpreter *interp, Val source_val, Val *copy_val) {
	copy_value_inner(interp, source_val, copy_val, 0);
}

void p_copy(Interpreter *interp) {
	PEEK_AT(source_val, 0, "copy");
	gc_root_push(interp, source_val);
	if (interp->error_flag)
		return;

	copy_value(interp, source_val, &interp->gc_roots[interp->n_gc_roots - 1]);
	Val copy_val = interp->gc_roots[interp->n_gc_roots - 1];
	gc_root_pop(interp);
	if (interp->error_flag)
		return;

	interp->data_stack[interp->dsp - 1] = copy_val;

	DISPATCH(interp);
}

static int op_cell_count(Vocabulary *vocab, cell *dict, int cursor) {
	cell handler = dict[cursor];

	int superword_cells = superword_cell_count(handler);
	if (superword_cells)
		return superword_cells;

	if (handler == vocab->dict[vocab->enter_locals_mixed_cfa])
		return 3 + (int)dict[cursor + 2];

	if (handler == vocab->dict[vocab->local_fetch_cfa]
	    || handler == vocab->dict[vocab->local_store_cfa])
		return 3;

	if (handler == vocab->dict[vocab->literal_cfa])
		return 2;

	if (handler == vocab->dict[vocab->dostr_cfa]
	    || handler == vocab->dict[vocab->branch_cfa]
	    || handler == vocab->dict[vocab->zbranch_cfa]
	    || handler == vocab->dict[vocab->qzbranch_cfa]
	    || handler == vocab->dict[vocab->to_var_cfa]
	    || handler == vocab->dict[vocab->enter_locals_cfa]
	    || handler == vocab->dict[vocab->enter_locals_to_cfa]
	    || handler == vocab->dict[vocab->leave_locals_cfa]
	    || handler == vocab->dict[vocab->local_fetch_0depth_cfa]
	    || handler == vocab->dict[vocab->local_store_0depth_cfa]
	    || handler == vocab->dict[vocab->local_incr_0depth_cfa]
	    || handler == vocab->dict[vocab->local_decr_0depth_cfa])
		return 2;

	return 1;
}

void inline_word_body(Interpreter *interp, int target_cfa) {
	Vocabulary *vocab = interp->vocab;
	cell exit_handler = vocab->dict[vocab->exit_cfa];
	cell branch_handler = vocab->dict[vocab->branch_cfa];
	cell docol_handler = (cell)docol;

	int cursor = target_cfa + 1;
	int depth = 0;
	int expect_docol = 0;

	while (1) {
		cell handler = vocab->dict[cursor];

		if (handler == exit_handler) {
			if (depth == 0)
				break;
			depth--;
			emit(interp, handler);
			cursor++;
			expect_docol = 0;
			continue;
		}

		if (expect_docol && handler == docol_handler) {
			depth++;
			emit(interp, handler);
			cursor++;
			expect_docol = 0;
			continue;
		}

		int n = op_cell_count(vocab, vocab->dict, cursor);
		for (int i = 0; i < n; i++)
			emit(interp, vocab->dict[cursor + i]);
		cursor += n;
		expect_docol = (handler == branch_handler);
	}
}

void mark_body(Interpreter *interp, int body_start, int body_end) {
	Vocabulary *vocab = interp->vocab;
	cell literal_ptr = vocab->dict[vocab->literal_cfa];
	cell dostr_ptr = vocab->dict[vocab->dostr_cfa];

	int cursor = body_start;
	while (cursor < body_end) {
		cell handler = vocab->dict[cursor];
		int n = op_cell_count(vocab, vocab->dict, cursor);

		if (handler == literal_ptr) {
			Val value;
			value.bits = (uint64_t)vocab->dict[cursor + 1];
			mark_value(interp, value);
		} else if (handler == dostr_ptr) {
			Val value = make_string((int)vocab->dict[cursor + 1]);
			mark_value(interp, value);
		}

		cursor += n;
	}
}

void gc(Interpreter *interp) {
	memset(interp->object_mark, 0, sizeof(interp->object_mark));

	for (int i = 0; i < interp->dsp; i++)
		mark_value(interp, interp->data_stack[i]);
	for (int i = 0; i < interp->rsp; i++)
		mark_value(interp, interp->return_stack[i]);
	for (int i = 0; i < interp->side_dsp; i++)
		mark_value(interp, interp->side_stack[i]);
	for (int i = 0; i < interp->n_gc_roots; i++)
		mark_value(interp, interp->gc_roots[i]);

	static int sorted_cfas[VOCABULARY_INIT_SIZE / 4];
	int num_cfas = 0;
	for (int cfa = interp->vocab->latest_cfa; cfa != 0; cfa = (int)WORD_LINK(interp->vocab, cfa)) {
		if (num_cfas >= (int)(sizeof sorted_cfas / sizeof sorted_cfas[0])) {
			fail(interp, "gc: vocabulary too large to scan safely");
			return;
		}
		sorted_cfas[num_cfas++] = cfa;
	}

	for (int i = 1; i < num_cfas; i++) {
		int current = sorted_cfas[i];
		int slot = i - 1;
		while (slot >= 0 && sorted_cfas[slot] > current) {
			sorted_cfas[slot + 1] = sorted_cfas[slot];
			slot--;
		}
		sorted_cfas[slot + 1] = current;
	}

	for (int i = 0; i < num_cfas; i++) {
		int cfa = sorted_cfas[i];
		int body_start = cfa + 1;
		int body_end = (i + 1 < num_cfas) ? sorted_cfas[i + 1] - 4 : interp->vocab->here;
		cfa_handler handler = (cfa_handler)interp->vocab->dict[cfa];
		if (handler == docol) {
			mark_body(interp, body_start, body_end);
		} else if (handler == dovar && body_start < body_end) {
			Val value;
			value.bits = (uint64_t)interp->vocab->dict[body_start];
			mark_value(interp, value);
		}

	}

	interp->n_free_slots = 0;
	for (int handle = 0; handle < interp->n_objects; handle++) {
		Object *obj = interp->objects[handle];
		if (obj && interp->object_mark[handle])
			continue;

		if (obj) {
			free_one_object(obj);
			interp->objects[handle] = NULL;
		}
		interp->free_slots[interp->n_free_slots++] = handle;
	}
}

static const char *handler_word_name(Interpreter *interp, cell handler) {
	for (int cfa = interp->vocab->latest_cfa; cfa != 0; cfa = (int)WORD_LINK(interp->vocab, cfa)) {
		if (interp->vocab->dict[cfa] == handler)
			return &interp->vocab->name_pool[WORD_NAME(interp->vocab, cfa)];
	}
	return NULL;
}

static const char *var_name_from_slot(Interpreter *interp, cell slot) {
	int var_cfa = (int)slot - 1;
	if (var_cfa > 0 && (cfa_handler)interp->vocab->dict[var_cfa] == dovar)
		return &interp->vocab->name_pool[WORD_NAME(interp->vocab, var_cfa)];
	return NULL;
}

static void see_compiled_body(Interpreter *interp, int body_start, int body_end) {
	Vocabulary *vocab = interp->vocab;
	int cursor = body_start;

	while (cursor < body_end) {
		cell handler = vocab->dict[cursor];
		cfa_handler handler_fn = (cfa_handler)handler;
		int cell_count = op_cell_count(vocab, vocab->dict, cursor);

		printf(" %d: ", cursor - body_start);

		if (handler_fn == docol || handler_fn == dovar) {
			int target = (int)vocab->dict[cursor + 1];
			printf("%s\n", &vocab->name_pool[WORD_NAME(vocab, target)]);
			cursor += 2;
			continue;
		}
		if (handler_fn == dosym) {
			printf(":%s\n", &vocab->symbol_pool[vocab->dict[cursor + 1]]);
			cursor += 2;
			continue;
		}

		if (superword_cell_count(handler)) {
			const char *name = handler_word_name(interp, handler);
			printf("%s", name);
			for (int operand_index = 1; operand_index < cell_count; operand_index++) {
				const char *operand_var = var_name_from_slot(interp, vocab->dict[cursor + operand_index]);
				printf(" %s", operand_var);
			}
		} else if (handler == vocab->dict[vocab->literal_cfa]) {
			Val value;
			value.bits = (uint64_t)vocab->dict[cursor + 1];
			fputs("(lit) ", stdout);
			print_val_compact(interp, value);
		} else {
			const char *name = handler_word_name(interp, handler);
			printf("%s", name);
			for (int operand_index = 1; operand_index < cell_count; operand_index++)
				printf(" %lld", (long long)vocab->dict[cursor + operand_index]);
		}

		putchar('\n');
		cursor += cell_count;
	}
}

void p_see_compiled(Interpreter *interp) {
	POP_XT(target_cfa, "see-compiled");
	Vocabulary *vocab = interp->vocab;
	const char *name = &vocab->name_pool[WORD_NAME(vocab, target_cfa)];

	if ((cfa_handler)vocab->dict[target_cfa] != docol) {
		printf("%s: not a colon definition\n", name);
		DISPATCH(interp);
	}

	int body_start = target_cfa + 1;
	int body_end = vocab->here;
	for (int cfa = vocab->latest_cfa; cfa != 0; cfa = (int)WORD_LINK(vocab, cfa)) {
		if (cfa > target_cfa && cfa - 4 < body_end)
			body_end = cfa - 4;
	}

	printf(": %s   \\ %d cells\n", name, body_end - body_start);
	see_compiled_body(interp, body_start, body_end);
	fputs(";\n", stdout);
	fflush(stdout);

	DISPATCH(interp);
}
void p_save(Interpreter *interp) {
	POP_STRING(filename_obj, "save");
	gc_root_push(interp, filename_obj_val);
	const char *filename = filename_obj->bytes;

	FILE *file = fopen(filename, "w");
	if (!file) {
		fail(interp, "cannot create %s", filename);
		gc_root_pop(interp);
		return;
	}

	static int collected_cfas[VOCABULARY_INIT_SIZE / 4];
	int num_cfas = 0;
	for (int cfa = interp->vocab->latest_cfa; cfa > interp->vocab->lib_end_latest_cfa; cfa = (int)WORD_LINK(interp->vocab, cfa)) {
		if (num_cfas < (int)(sizeof collected_cfas / sizeof collected_cfas[0]))
			collected_cfas[num_cfas++] = cfa;
	}

	fprintf(file, "\\ logicforth vocabulary\n\n");

	for (int i = num_cfas - 1; i >= 0; i--) {
		int cfa = collected_cfas[i];
		const char *name = &interp->vocab->name_pool[WORD_NAME(interp->vocab, cfa)];
		cfa_handler handler = (cfa_handler)interp->vocab->dict[cfa];

		if (handler == docol) {
			int src_offset = (int)WORD_SOURCE(interp->vocab, cfa);
			const char *body_source = (src_offset > 0) ? &interp->vocab->source_pool[src_offset] : "";
			fprintf(file, ": %s%s;\n", name, body_source);
		} else if (handler == dovar) {
			fprintf(file, "variable %s\n", name);
		} else if (handler == dosym) {
			fprintf(file, "symbol %s\n", name);
		}

	}

	fclose(file);
	gc_root_pop(interp);

	DISPATCH(interp);
}

#define IMAGE_MAGIC "LF4I"
#define IMAGE_VERSION ((uint32_t)2)

#define HANDLER_DOCOL 1
#define HANDLER_DOVAR 2
#define HANDLER_DOSYM 3

void w_u8 (FILE *f, uint8_t v) { fwrite(&v, 1, 1, f); }

void w_i32(FILE *f, int32_t v) { fwrite(&v, 4, 1, f); }

void w_i64(FILE *f, int64_t v) { fwrite(&v, 8, 1, f); }

void w_val(FILE *f, Val value) {
	w_i32(f, (int32_t)VAL_TAG(value));
	w_i64(f, VAL_DATA(value));
}

int r_u8 (FILE *f, uint8_t *v) { return fread(v, 1, 1, f) == 1; }

int r_u32(FILE *f, uint32_t *v) { return fread(v, 4, 1, f) == 1; }

int r_i32(FILE *f, int32_t *v) { return fread(v, 4, 1, f) == 1; }

int r_i64(FILE *f, int64_t *v) { return fread(v, 8, 1, f) == 1; }

int r_val(FILE *f, Val *v) {
	int32_t tag;
	int64_t data;
	if (!r_i32(f, &tag) || !r_i64(f, &data))
		return 0;
	*v = make_tagged((Tag)tag, data);
	return 1;
}

void p_save_image(Interpreter *interp) {
	POP_STRING(filename_obj, "save-image");
	gc_root_push(interp, filename_obj_val);
	const char *filename = filename_obj->bytes;

	FILE *file = fopen(filename, "wb");
	if (!file) {
		fail(interp, "cannot create %s", filename);
		gc_root_pop(interp);
		return;
	}

	int user_word_count = 0;
	/* One entry per word; each header is >=4 cells, so /4 bounds the count.
	   static keeps ~4MB off the call stack. */
	static int collected[VOCABULARY_INIT_SIZE / 4];
	for (int c = interp->vocab->latest_cfa; c >= interp->vocab->init_here; c = (int)WORD_LINK(interp->vocab, c)) {
		if (user_word_count >= (int)(sizeof collected / sizeof collected[0])) {
			fail(interp, "save-image: too many words");
			fclose(file);
			gc_root_pop(interp);
			return;
		}
		collected[user_word_count++] = c;
	}

	fwrite(IMAGE_MAGIC, 1, 4, file);
	w_i32(file, (int32_t)IMAGE_VERSION);

	int32_t user_dict_cells = interp->vocab->here - interp->vocab->init_here;
	int32_t user_namepool_bytes = interp->vocab->names_here - interp->vocab->init_names_here;
	int32_t user_sourcepool_bytes = interp->vocab->source_here - interp->vocab->init_source_here;
	int32_t user_symbolpool_bytes = interp->vocab->symbol_pool_here - interp->vocab->init_symbol_pool_here;
	w_i32(file, user_dict_cells);
	w_i32(file, user_namepool_bytes);
	w_i32(file, user_sourcepool_bytes);
	w_i32(file, user_symbolpool_bytes);
	w_i32(file, interp->vocab->latest_cfa);
	w_i32(file, interp->dsp);
	w_i32(file, interp->n_objects);

	w_i32(file, interp->vocab->init_here);
	w_i32(file, interp->vocab->init_latest_cfa);
	w_i32(file, interp->vocab->init_names_here);
	w_i32(file, interp->vocab->init_source_here);
	w_i32(file, interp->vocab->init_symbol_pool_here);

	for (int i = 0; i < user_dict_cells; i++)
		w_i64(file, (int64_t)interp->vocab->dict[interp->vocab->init_here + i]);

	w_i32(file, user_word_count);
	for (int i = 0; i < user_word_count; i++) {
		int c = collected[i];
		cfa_handler h = (cfa_handler)interp->vocab->dict[c];
		uint8_t kind = (h == docol) ? HANDLER_DOCOL
			: (h == dovar) ? HANDLER_DOVAR
			: (h == dosym) ? HANDLER_DOSYM : 0;
		if (kind == 0) {
			fail(interp, "save-image: unrecognised handler at cfa %d", c);
			fclose(file);
			gc_root_pop(interp);
			return;
		}
		w_i32(file, c);
		w_u8(file, kind);
	}

	fwrite(&interp->vocab->name_pool[interp->vocab->init_names_here], 1, (size_t)user_namepool_bytes, file);
	fwrite(&interp->vocab->source_pool[interp->vocab->init_source_here], 1, (size_t)user_sourcepool_bytes, file);
	fwrite(&interp->vocab->symbol_pool[interp->vocab->init_symbol_pool_here], 1, (size_t)user_symbolpool_bytes, file);

	for (int slot = 0; slot < interp->n_objects; slot++) {
		Object *obj = interp->objects[slot];
		if (!obj) {
			w_u8(file, 0);
			continue;
		}
		w_u8(file, 1);
		w_u8(file, (uint8_t)obj->kind);
		w_i32(file, obj->len);
		w_i32(file, obj->capacity);
		switch (obj->kind) {
			case OBJECT_STRING:
				fwrite(obj->bytes, 1, (size_t)obj->len, file);
				break;
			case OBJECT_SET:
			case OBJECT_ARRAY:
				for (int j = 0; j < obj->len; j++)
					w_val(file, obj->items[j]);
				break;
			case OBJECT_FRAME:
				for (int j = 0; j < obj->len; j++) {
					w_i64(file, (int64_t)obj->frame.keys[j]);
					w_val(file, obj->frame.values[j]);
				}
				break;
			case OBJECT_MATRIX: {
									w_i32(file, obj->matrix.rows);
									w_i32(file, obj->matrix.columns);
									size_t n = (size_t)obj->matrix.rows * (size_t)obj->matrix.columns;
									if (n > 0)
										fwrite(obj->matrix.elements, sizeof(double), n, file);
									break;
								}
			case OBJECT_CONTINUATION:
								w_i32(file, obj->continuation.return_len);
								for (int j = 0; j < obj->continuation.return_len; j++)
									w_val(file, obj->continuation.return_slice[j]);
								w_i32(file, obj->continuation.resume_ip);
								w_i32(file, obj->continuation.local_base_offset);
								break;
		}
	}

	for (int i = 0; i < interp->dsp; i++)
		w_val(file, interp->data_stack[i]);

	fclose(file);
	gc_root_pop(interp);

	DISPATCH(interp);
}

void free_one_object(Object *obj) {
	switch (obj->kind) {
		case OBJECT_STRING: free(obj->bytes); break;
		case OBJECT_SET:
		case OBJECT_ARRAY: free(obj->items); break;
		case OBJECT_FRAME: free(obj->frame.keys); free(obj->frame.values); break;
		case OBJECT_MATRIX: free(obj->matrix.elements); break;
		case OBJECT_CONTINUATION: free(obj->continuation.return_slice); break;
	}
	free(obj);
}

void forget_user(Interpreter *interp) {
	for (int i = 0; i < interp->n_objects; i++) {
		if (interp->objects[i]) {
			free_one_object(interp->objects[i]);
			interp->objects[i] = NULL;
		}
	}
	interp->n_objects = 0;
	interp->n_free_slots = 0;
	interp->dsp = 0;
	interp->rsp = 0;
	interp->vocab->here = interp->vocab->init_here;
	interp->vocab->latest_cfa = interp->vocab->init_latest_cfa;
	interp->vocab->names_here = interp->vocab->init_names_here;
	interp->vocab->source_here = interp->vocab->init_source_here;
	interp->vocab->symbol_pool_here = interp->vocab->init_symbol_pool_here;
}

void p_load_image(Interpreter *interp) {
	POP_STRING(filename_obj, "load-image");

	char filename[4096];
	int fnlen = filename_obj->len;
	if (fnlen >= (int)sizeof(filename)) {
		fail(interp, "filename too long");
		return;
	}
	memcpy(filename, filename_obj->bytes, (size_t)fnlen);
	filename[fnlen] = 0;

	FILE *file = fopen(filename, "rb");
	if (!file) {
		fail(interp, "cannot open %s", filename);
		return;
	}

	char magic[4];
	if (fread(magic, 1, 4, file) != 4 || memcmp(magic, IMAGE_MAGIC, 4) != 0) {
		fail(interp, "%s: not a logicforth image", filename);
		fclose(file);
		return;
	}
	uint32_t version;
	if (!r_u32(file, &version) || version != IMAGE_VERSION) {
		fail(interp, "%s: version %u, expected %u", filename, version, IMAGE_VERSION);
		fclose(file);
		return;
	}

	int32_t user_dict_cells, user_namepool_bytes, user_sourcepool_bytes, user_symbolpool_bytes;
	int32_t saved_latest_cfa, saved_dsp, saved_n_objects;
	if (!r_i32(file, &user_dict_cells)
			|| !r_i32(file, &user_namepool_bytes)
			|| !r_i32(file, &user_sourcepool_bytes)
			|| !r_i32(file, &user_symbolpool_bytes)
			|| !r_i32(file, &saved_latest_cfa)
			|| !r_i32(file, &saved_dsp)
			|| !r_i32(file, &saved_n_objects))
	{
		fail(interp, "%s: truncated sizes", filename);
		fclose(file);
		return;
	}

	int32_t m_here, m_latest, m_names, m_sources, m_symbols;
	if (!r_i32(file, &m_here) || !r_i32(file, &m_latest)
			|| !r_i32(file, &m_names) || !r_i32(file, &m_sources)
			|| !r_i32(file, &m_symbols))
	{
		fail(interp, "%s: truncated markers", filename);
		fclose(file);
		return;
	}
	if (m_here != interp->vocab->init_here || m_latest != interp->vocab->init_latest_cfa
			|| m_names != interp->vocab->init_names_here || m_sources != interp->vocab->init_source_here
			|| m_symbols != interp->vocab->init_symbol_pool_here)
	{
		fail(interp, "%s: interpreter bootstrap mismatch (rebuild needed)", filename);
		fclose(file);
		return;
	}

	if (user_dict_cells < 0 || interp->vocab->init_here + user_dict_cells > VOCABULARY_INIT_SIZE
			|| user_namepool_bytes < 0 || interp->vocab->init_names_here + user_namepool_bytes > NAME_POOL
			|| user_sourcepool_bytes < 0 || interp->vocab->init_source_here + user_sourcepool_bytes > SOURCE_POOL
			|| user_symbolpool_bytes < 0 || interp->vocab->init_symbol_pool_here + user_symbolpool_bytes > SYMBOL_POOL
			|| saved_dsp < 0 || saved_dsp > DATA_STACK_DEPTH
			|| saved_n_objects < 0 || saved_n_objects > MAX_OBJECTS)
	{
		fail(interp, "%s: image sizes out of bounds", filename);
		fclose(file);
		return;
	}

	forget_user(interp);

	for (int i = 0; i < user_dict_cells; i++) {
		int64_t c;
		if (!r_i64(file, &c)) {
			fail(interp, "%s: truncated dict", filename);
			goto done;
		}
		interp->vocab->dict[interp->vocab->init_here + i] = (cell)c;
	}
	interp->vocab->here = interp->vocab->init_here + user_dict_cells;

	int32_t user_word_count;
	if (!r_i32(file, &user_word_count)) {
		fail(interp, "%s: missing handler table", filename);
		goto done;
	}
	for (int i = 0; i < user_word_count; i++) {
		int32_t c;
		uint8_t kind;
		if (!r_i32(file, &c) || !r_u8(file, &kind)) {
			fail(interp, "%s: truncated handler table", filename);
			goto done;
		}
		if (c < interp->vocab->init_here || c >= interp->vocab->here) {
			fail(interp, "%s: handler cfa out of range", filename);
			goto done;
		}
		cfa_handler h = (kind == HANDLER_DOCOL) ? docol
			: (kind == HANDLER_DOVAR) ? dovar
			: (kind == HANDLER_DOSYM) ? dosym : NULL;
		if (!h) {
			fail(interp, "%s: bad handler kind %u", filename, kind);
			goto done;
		}
		interp->vocab->dict[c] = (cell)h;
	}

	if (fread(&interp->vocab->name_pool[interp->vocab->init_names_here], 1, (size_t)user_namepool_bytes, file) != (size_t)user_namepool_bytes
			|| fread(&interp->vocab->source_pool[interp->vocab->init_source_here], 1, (size_t)user_sourcepool_bytes, file) != (size_t)user_sourcepool_bytes
			|| fread(&interp->vocab->symbol_pool[interp->vocab->init_symbol_pool_here], 1, (size_t)user_symbolpool_bytes, file) != (size_t)user_symbolpool_bytes)
	{
		fail(interp, "%s: truncated pools", filename);
		goto done;
	}
	interp->vocab->names_here = interp->vocab->init_names_here + user_namepool_bytes;
	interp->vocab->source_here = interp->vocab->init_source_here + user_sourcepool_bytes;
	interp->vocab->symbol_pool_here = interp->vocab->init_symbol_pool_here + user_symbolpool_bytes;

	for (int slot = 0; slot < saved_n_objects; slot++) {
		uint8_t presence;
		if (!r_u8(file, &presence)) {
			fail(interp, "%s: truncated objects", filename);
			goto done;
		}
		if (presence == 0) {
			interp->objects[slot] = NULL;
			continue;
		}

		uint8_t kind;
		int32_t len, cap;
		if (!r_u8(file, &kind) || !r_i32(file, &len) || !r_i32(file, &cap)) {
			fail(interp, "%s: truncated object header", filename);
			goto done;
		}
		Object *obj = calloc(1, sizeof(*obj));
		obj->kind = kind;
		obj->len = len;
		obj->capacity = cap;
		switch (kind) {
			case OBJECT_STRING:
				obj->bytes = malloc((size_t)len + 1);
				if (len > 0 && fread(obj->bytes, 1, (size_t)len, file) != (size_t)len) {
					free(obj->bytes);
					free(obj);
					fail(interp, "%s: truncated string", filename);
					goto done;
				}
				obj->bytes[len] = 0;
				break;
			case OBJECT_SET:
			case OBJECT_ARRAY:
				obj->items = malloc(sizeof(Val) * (size_t)MAX(cap, 1));
				for (int j = 0; j < len; j++) {
					if (!r_val(file, &obj->items[j])) {
						free(obj->items);
						free(obj);
						fail(interp, "%s: truncated items", filename);
						goto done;
					}
				}
				break;
			case OBJECT_FRAME:
				obj->frame.keys = malloc(sizeof(cell) * (size_t)MAX(cap, 1));
				obj->frame.values = malloc(sizeof(Val) * (size_t)MAX(cap, 1));
				for (int j = 0; j < len; j++) {
					int64_t key;
					if (!r_i64(file, &key) || !r_val(file, &obj->frame.values[j])) {
						free(obj->frame.keys);
						free(obj->frame.values);
						free(obj);
						fail(interp, "%s: truncated frame", filename);
						goto done;
					}
					obj->frame.keys[j] = (cell)key;
				}
				break;
			case OBJECT_MATRIX: {
									int32_t rows, cols;
									if (!r_i32(file, &rows) || !r_i32(file, &cols)) {
										free(obj);
										fail(interp, "%s: truncated matrix header", filename);
										goto done;
									}
									obj->matrix.rows = rows;
									obj->matrix.columns = cols;
									size_t n = (size_t)rows * (size_t)cols;
									obj->matrix.elements = calloc(n > 0 ? n : 1, sizeof(double));
									if (n > 0 && fread(obj->matrix.elements, sizeof(double), n, file) != n) {
										free(obj->matrix.elements);
										free(obj);
										fail(interp, "%s: truncated matrix data", filename);
										goto done;
									}
									break;
								}
			case OBJECT_CONTINUATION: {
										  int32_t return_len;
										  if (!r_i32(file, &return_len) || return_len < 0) {
											  free(obj);
											  fail(interp, "%s: bad continuation header", filename);
											  goto done;
										  }
										  obj->continuation.return_len = return_len;
										  obj->continuation.return_slice =
											  malloc(sizeof(Val) * (size_t)MAX(return_len, 1));
										  for (int j = 0; j < return_len; j++) {
											  if (!r_val(file, &obj->continuation.return_slice[j])) {
												  free(obj->continuation.return_slice);
												  free(obj);
												  fail(interp, "%s: truncated continuation slice", filename);
												  goto done;
											  }
										  }
										  int32_t resume_ip;
										  if (!r_i32(file, &resume_ip)
												  || resume_ip < interp->vocab->init_here || resume_ip >= interp->vocab->here)
										  {
											  free(obj->continuation.return_slice);
											  free(obj);
											  fail(interp, "%s: continuation resume_ip out of range", filename);
											  goto done;
										  }
										  obj->continuation.resume_ip = resume_ip;
										  int32_t local_base_offset;
										  if (!r_i32(file, &local_base_offset)) {
											  free(obj->continuation.return_slice);
											  free(obj);
											  fail(interp, "%s: truncated continuation local_base_offset", filename);
											  goto done;
										  }
										  obj->continuation.local_base_offset = local_base_offset;
										  break;
									  }
			default:
									  free(obj);
									  fail(interp, "%s: unknown object kind %u", filename, kind);
									  goto done;
		}
		interp->objects[slot] = obj;
	}
	interp->n_objects = saved_n_objects;
	interp->n_free_slots = 0;

	for (int i = 0; i < saved_dsp; i++) {
		if (!r_val(file, &interp->data_stack[i])) {
			fail(interp, "%s: truncated stack", filename);
			goto done;
		}
	}
	interp->dsp = saved_dsp;

	interp->vocab->latest_cfa = saved_latest_cfa;

done:
	fclose(file);

	DISPATCH(interp);
}

Interpreter *interp_new(void) {
	Interpreter *interp = calloc(1, sizeof(Interpreter));
	interp->vocab = calloc(1, sizeof(Vocabulary));
	interp->vocab->dict = calloc(VOCABULARY_INIT_SIZE, sizeof(cell));
	interp->vocab->dict_cap = VOCABULARY_INIT_SIZE;
	interp->vocab->here = DICT_RESERVED;
	interp->vocab->source_here = 1;
	interp->next_mark_id = 1;
	return interp;
}

int main(void) {
	Interpreter *interp = interp_new();

	define_primitive(interp, "+", p_add, 0);
	define_primitive(interp, "-", p_sub, 0);
	define_primitive(interp, "*", p_mul, 0);
	define_primitive(interp, "/", p_div, 0);
	define_primitive(interp, "+!", p_add_inplace, 0);
	define_primitive(interp, "-!", p_sub_inplace, 0);
	define_primitive(interp, "*!", p_mul_inplace, 0);
	define_primitive(interp, "/!", p_div_inplace, 0);
	define_primitive(interp, "f+", p_add_f, 0);
	define_primitive(interp, "f-", p_sub_f, 0);
	define_primitive(interp, "f*", p_mul_f, 0);
	define_primitive(interp, "f/", p_div_f, 0);
	define_primitive(interp, "f^", p_fpow, 0);
	define_primitive(interp, "fmod", p_fmodop, 0);
	define_primitive(interp, "fabs", p_fabs, 0);
	define_primitive(interp, "fsqrt", p_fsqrt, 0);
	define_primitive(interp, "fexp", p_fexp, 0);
	define_primitive(interp, "flog", p_flog, 0);
	define_primitive(interp, "fln", p_fln, 0);
	define_primitive(interp, "fsin", p_fsin, 0);
	define_primitive(interp, "fcos", p_fcos, 0);
	define_primitive(interp, "ftan", p_ftan, 0);
	define_primitive(interp, "ftanh", p_ftanh, 0);
	define_primitive(interp, "fasin", p_fasin, 0);
	define_primitive(interp, "facos", p_facos, 0);
	define_primitive(interp, "fatan", p_fatan, 0);
	define_primitive(interp, "fround", p_fround, 0);
	define_primitive(interp, "fround-up", p_fround_up, 0);
	define_primitive(interp, "fround-down", p_fround_down, 0);
	define_primitive(interp, "ftruncate", p_ftruncate, 0);
	define_primitive(interp, "fnegate", p_fnegate, 0);
	define_primitive(interp, "f1+", p_inc, 0);
	define_primitive(interp, "f1-", p_dec, 0);
	define_primitive(interp, "fsq", p_sq, 0);
	define_primitive(interp, "negate", p_neg, 0);
	interp->vocab->inc_cfa = define_primitive(interp, "1+", p_inc_poly, 0);
	interp->vocab->dec_cfa = define_primitive(interp, "1-", p_dec_poly, 0);
	define_primitive(interp, "++", p_increment, 1);
	define_primitive(interp, "--", p_decrement, 1);
	define_primitive(interp, "sq", p_sq_poly, 0);
	define_primitive(interp, "dup", p_dup, 0);
	define_primitive(interp, "drop", p_drop, 0);
	define_primitive(interp, "swap", p_swap, 0);
	define_primitive(interp, "over", p_over, 0);
	define_primitive(interp, "rot", p_rot, 0);
	define_primitive(interp, "depth", p_depth, 0);
	define_primitive(interp, "roll", p_roll, 0);
	define_primitive(interp, "=", p_eq, 0);
	define_primitive(interp, "lt", p_lt, 0);
	define_primitive(interp, "gt", p_gt, 0);
	define_primitive(interp, "0=", p_zeq, 0);
	define_primitive(interp, "and", p_and, 0);
	define_primitive(interp, "or", p_or, 0);
	define_primitive(interp, "not", p_not, 0);
	define_primitive(interp, ".", p_dot, 0);
	define_primitive(interp, ".a", p_dot_all, 0);
	define_primitive(interp, "cr", p_cr, 0);
	define_primitive(interp, "emit", p_emit_, 0);
	define_primitive(interp, ".s", p_dots, 0);
	define_primitive(interp, "bye", p_bye, 0);
	define_primitive(interp, "clear", p_clear, 0);
	define_primitive(interp, "gc", p_gc, 0);
	define_primitive(interp, "load", p_load, 0);
	define_primitive(interp, "save", p_save, 0);
	define_primitive(interp, "save-image", p_save_image, 0);
	define_primitive(interp, "load-image", p_load_image, 0);
	define_primitive(interp, "reload", p_reload, 0);
	define_primitive(interp, ">r", p_tor, 0);
	define_primitive(interp, "r>", p_rfrom, 0);
	define_primitive(interp, "r@", p_rfetch, 0);
	define_primitive(interp, ">side", p_to_side, 0);
	define_primitive(interp, "side>", p_side_to, 0);
	define_primitive(interp, "side-drop", p_side_drop, 0);
	define_primitive(interp, "side-depth", p_side_depth, 0);
	define_primitive(interp, "@", p_frame_get, 0);
	define_primitive(interp, "!", p_frame_set, 0);
	define_primitive(interp, "keys", p_frame_keys, 0);
	define_primitive(interp, "values", p_frame_values, 0);
	define_primitive(interp, "delete-at", p_frame_delete_at, 0);
	define_primitive(interp, "has?", p_has, 0);
	define_primitive(interp, "update-at", p_update_at, 0);
	define_primitive(interp, "merge", p_merge, 0);
	define_primitive(interp, "copy", p_copy, 0);

	define_primitive(interp, "reset", p_reset, 0);
	define_primitive(interp, "shift", p_shift, 0);
	define_primitive(interp, "shift-with", p_shift_with, 0);
	define_primitive(interp, "resume", p_resume, 0);

	define_primitive(interp, "{", p_frameopen, 0);
	define_primitive(interp, "}", p_frameclose, 0);
	define_primitive(interp, "<", p_setopen, 0);
	define_primitive(interp, ">", p_setclose, 0);
	define_primitive(interp, "[", p_array_open, 0);
	define_primitive(interp, "]", p_array_close, 0);

	define_primitive(interp, "array", p_array, 0);
	define_primitive(interp, "array-of", p_array_of, 0);
	define_primitive(interp, ">frame", p_to_frame, 0);
	define_primitive(interp, "frame", p_frame, 0);
	define_primitive(interp, "take", p_take, 0);
	define_primitive(interp, "reverse", p_reverse, 0);
	define_primitive(interp, "flip", p_flip, 0);
	define_primitive(interp, "concat", p_concat, 0);
	define_primitive(interp, "destruct", p_destruct, 0);
	define_primitive(interp, "destruct-to", p_destruct_to, 0);
	define_primitive(interp, "slice!", p_slice_store, 0);
	define_primitive(interp, "to-slice", p_to_slice, 0);
	define_primitive(interp, "range", p_range, 0);
	define_primitive(interp, "size", p_size, 0);
	define_primitive(interp, "member?", p_member, 0);
	define_primitive(interp, "set", p_set, 0);
	define_primitive(interp, "union", p_union, 0);
	define_primitive(interp, "intersection", p_intersect, 0);
	define_primitive(interp, "difference", p_difference, 0);
	define_primitive(interp, "execute", p_execute, 0);
	define_primitive(interp, "map", p_map, 0);
	define_primitive(interp, "mapn", p_mapn, 0);
	define_primitive(interp, "filter", p_filter, 0);
	define_primitive(interp, "reduce", p_reduce, 0);
	define_primitive(interp, "times", p_times, 0);
	define_primitive(interp, "i-times", p_i_times, 0);

	define_primitive(interp, "words", p_words, 0);
	define_primitive(interp, "see", p_see, 0);
	define_primitive(interp, "see-compiled", p_see_compiled, 0);

	interp->vocab->exit_cfa = define_primitive(interp, "exit", p_exit, 0);
	interp->vocab->literal_cfa = define_primitive(interp, "(lit)", p_literal, 4);
	interp->vocab->branch_cfa = define_primitive(interp, "(branch)", p_branch, 4);
	interp->vocab->zbranch_cfa = define_primitive(interp, "(0branch)", p_0branch, 4);
	interp->vocab->qzbranch_cfa = define_primitive(interp, "(?0branch)", p_qzbranch, 4);
	interp->vocab->dostr_cfa = define_primitive(interp, "(dostr)", p_dostr, 4);
	interp->vocab->stop_cfa = define_primitive(interp, "(stop)", p_stop, 4);
	interp->vocab->to_var_cfa = define_primitive(interp, "(to-var)", p_to_var, 4);
	interp->vocab->enter_locals_cfa = define_primitive(interp, "(enter-locals)", p_enter_locals, 4);
	interp->vocab->enter_locals_to_cfa = define_primitive(interp, "(enter-locals-to)", p_enter_locals_to, 4);
	interp->vocab->enter_locals_mixed_cfa = define_primitive(interp, "(enter-locals-mixed)", p_enter_locals_mixed, 4);
	interp->vocab->leave_locals_cfa = define_primitive(interp, "(leave-locals)", p_leave_locals, 4);
	interp->vocab->local_fetch_cfa = define_primitive(interp, "(local@)", p_local_fetch, 4);
	interp->vocab->local_store_cfa = define_primitive(interp, "(local!)", p_local_store, 4);
	interp->vocab->local_fetch_0depth_cfa = define_primitive(interp, "(local@0)", p_local_fetch_0depth, 4);
	interp->vocab->local_store_0depth_cfa = define_primitive(interp, "(local!0)", p_local_store_0depth, 4);
	interp->vocab->local_incr_0depth_cfa  = define_primitive(interp, "(local+!0)", p_local_incr_0depth, 4);
	interp->vocab->local_decr_0depth_cfa  = define_primitive(interp, "(local-!0)", p_local_decr_0depth, 4);

	define_superwords(interp);

	define_primitive(interp, ":", p_colon, 0);
	define_primitive(interp, "variable", p_variable, 0);
	define_primitive(interp, "symbol", p_symbol, 0);
	define_primitive(interp, "string>symbol", p_string_to_symbol, 0);
	define_primitive(interp, "forget", p_forget, 0);
	define_primitive(interp, "'", p_tick, 1);
	define_primitive(interp, "to", p_to, 1);
	define_primitive(interp, ";", p_semi, 1);
	define_primitive(interp, "inline", p_inline, 0);
	define_primitive(interp, "if", p_if, 1);
	define_primitive(interp, "?if", p_qif, 1);
	define_primitive(interp, "then", p_then, 1);
	define_primitive(interp, "else", p_else, 1);
	define_primitive(interp, "begin", p_begin, 1);
	define_primitive(interp, "until", p_until, 1);
	define_primitive(interp, "again", p_again, 1);
	define_primitive(interp, "[:", p_qcolon, 1);
	define_primitive(interp, ":]", p_qsemi, 1);
	define_primitive(interp, "|", p_bar, 1);
	define_primitive(interp, "|>", p_bar_to, 1);

	define_primitive(interp, "0-matrix", p_0_matrix, 0);
	define_primitive(interp, "matrix", p_matrix, 0);
	define_primitive(interp, "dim", p_dim, 0);
	define_primitive(interp, "transpose", p_transpose, 0);
	define_primitive(interp, "diagonal-matrix", p_diagonal_matrix, 0);
	define_primitive(interp, "@i", p_at_i, 0);
	define_primitive(interp, "!i", p_store_i, 0);
	define_primitive(interp, "@j", p_at_j, 0);
	define_primitive(interp, "@i,j", p_at_ij, 0);
	define_primitive(interp, "diagonal", p_diagonal, 0);
	define_primitive(interp, "reshape", p_reshape, 0);
	define_primitive(interp, "matrix1d-range", p_matrix_range, 0);
	define_primitive(interp, "sum", p_sum, 0);
	define_primitive(interp, "row-sums", p_row_sums, 0);
	define_primitive(interp, "column-sums", p_column_sums, 0);
	define_primitive(interp, "max", p_max, 0);
	define_primitive(interp, "min", p_min, 0);
	define_primitive(interp, "row-maxes", p_row_maxes, 0);
	define_primitive(interp, "row-mins", p_row_mins, 0);
	define_primitive(interp, "column-maxes", p_column_maxes, 0);
	define_primitive(interp, "column-mins", p_column_mins, 0);

	define_primitive(interp, "dgemm-nn", p_dgemm_nn, 0);
	define_primitive(interp, "dgemm-tn", p_dgemm_tn, 0);
	define_primitive(interp, "dgemm-nt", p_dgemm_nt, 0);
	define_primitive(interp, "dgemm-tt", p_dgemm_tt, 0);

	define_primitive(interp, "abs", p_abs, 0);
	define_primitive(interp, "sqrt", p_sqrt, 0);
	define_primitive(interp, "exp", p_exp, 0);
	define_primitive(interp, "log", p_log, 0);
	define_primitive(interp, "ln", p_ln, 0);
	define_primitive(interp, "^", p_power, 0);
	define_primitive(interp, "%", p_divmod, 0);
	define_primitive(interp, "sin", p_sin, 0);
	define_primitive(interp, "cos", p_cos, 0);
	define_primitive(interp, "tan", p_tan, 0);
	define_primitive(interp, "tanh", p_tanh, 0);
	define_primitive(interp, "asin", p_asin, 0);
	define_primitive(interp, "acos", p_acos, 0);
	define_primitive(interp, "atan", p_atan, 0);
	define_primitive(interp, "round", p_round, 0);
	define_primitive(interp, "round-up", p_round_up, 0);
	define_primitive(interp, "round-down", p_round_down, 0);
	define_primitive(interp, "truncate", p_truncate, 0);

	define_primitive(interp, "now", p_now, 0);

	interp->vocab->init_here = interp->vocab->here;
	interp->vocab->init_latest_cfa = interp->vocab->latest_cfa;
	interp->vocab->init_names_here = interp->vocab->names_here;
	interp->vocab->init_source_here = interp->vocab->source_here;
	interp->vocab->init_symbol_pool_here = interp->vocab->symbol_pool_here;

	push(interp, make_string(object_new_string(interp, "src/forth/lib.l4", 16)));
	execute_cfa(interp, find(interp, "load"));
	if (interp->error_flag) {
		printf("lib.l4 load error\n");
		return 1;
	}

	interp->vocab->lib_end_latest_cfa = interp->vocab->latest_cfa;

	printf("logicforth %s\n", VERSION);
	char line[1024];

	while (fgets(line, sizeof(line), stdin)) {
		int line_len = (int)strlen(line);
		if (interp->input_buffer_len + line_len < INPUT_BUFFER_SIZE - 1) {
			memcpy(interp->input_buffer + interp->input_buffer_len, line, (size_t)line_len + 1);
			interp->input_buffer_len += line_len;
		}

		interp->error_flag = 0;
		interp->need_more = 0;
		run_outer(interp);

		if (interp->need_more)
			continue;

		if (interp->error_flag) {
			interp->compiling = 0;
			interp->dsp = 0;
			interp->rsp = 0;
			interp->compiling_src_start = 0;
			interp->n_local_scopes = 0;
			interp->n_local_names = 0;
			interp->local_names_pool_here = 0;
		}

		if (interp->compiling)
			continue;

		print_prompt_state(interp);
		if (interp->error_flag)
			fputs(interp->error_message, stdout);
		else
			fputs("ok", stdout);
		putchar('\n');
		fflush(stdout);

		inbuf_reset(interp);
	}
	return 0;
}
