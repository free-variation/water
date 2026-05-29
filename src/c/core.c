#include "logicforth.h"
#include <unistd.h>

void gc_root_push(Interpreter *interp, Val value) {
	if (interp->n_gc_roots < MAX_GC_ROOTS) {
		interp->gc_roots[interp->n_gc_roots++] = value;
	}
}

void gc_root_pop(Interpreter *interp) {
	if (interp->n_gc_roots > 0) {
		interp->n_gc_roots--;
	}
}

int object_alloc_slot(Interpreter *interp) {
	if (interp->n_objects < MAX_OBJECTS) {
		return interp->n_objects++;
	}

	for (int i = 0; i < MAX_OBJECTS; i++) {
		if (interp->objects[i] == NULL) {
			return i;
		}
	}
	gc(interp);

	for (int i = 0; i < MAX_OBJECTS; i++) {
		if (interp->objects[i] == NULL) {
			return i;
		}
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
	obj->frame.keys   = malloc(sizeof(cell) * (size_t)obj->capacity);
	obj->frame.values = malloc(sizeof(Val)  * (size_t)obj->capacity);
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

	if (left.tag != right.tag) return (int)left.tag - (int)right.tag;

	switch (left.tag) {
		case T_FLOAT: {
						  double left_value = unpack_float(left);
						  double right_value = unpack_float(right);
						  if (left_value < right_value) return -1;
						  if (left_value > right_value) return 1;
						  return 0;
					  }
		case T_SYM: case T_XT: case T_ADDR:

					  if (left.data < right.data) return -1;
					  if (left.data > right.data) return 1;
					  return 0;
		case T_STRING: {
						   Object *left_string = interp->objects[left.data];
						   Object *right_string = interp->objects[right.data];
						   int compare_length = MIN(left_string->len, right_string->len);
						   int byte_diff = memcmp(left_string->bytes, right_string->bytes,
								   (size_t)compare_length);
						   if (byte_diff) return byte_diff;

						   return left_string->len - right_string->len;
					   }
		case T_SET: case T_ARRAY: {
									  Object *left_collection = interp->objects[left.data];
									  Object *right_collection = interp->objects[right.data];
									  int compare_length = MIN(left_collection->len, right_collection->len);
									  for (int i = 0; i < compare_length; i++) {
										  int element_cmp = val_cmp(interp, left_collection->items[i],
												  right_collection->items[i]);
										  if (element_cmp) return element_cmp;
									  }

									  return left_collection->len - right_collection->len;
								  }
		case T_MATRIX: {
						   Object *left_matrix = interp->objects[left.data];
						   Object *right_matrix = interp->objects[right.data];

						   if (left_matrix->matrix.rows != right_matrix->matrix.rows)
							   return left_matrix->matrix.rows - right_matrix->matrix.rows;
						   if (left_matrix->matrix.columns != right_matrix->matrix.columns)
							   return left_matrix->matrix.columns - right_matrix->matrix.columns;
						   int n = left_matrix->matrix.rows * left_matrix->matrix.columns;
						   for (int i = 0; i < n; i++) {
							   double a = left_matrix->matrix.elements[i];
							   double b = right_matrix->matrix.elements[i];
							   if (a < b) return -1;
							   if (a > b) return 1;
						   }
						   return 0;
					   }
		case T_FRAME: {
			Object *left_frame = interp->objects[left.data];
			Object *right_frame = interp->objects[right.data];
			if (left_frame->len != right_frame->len)
				return left_frame->len - right_frame->len;
			for (int i = 0; i < left_frame->len; i++) {
				cell left_key = left_frame->frame.keys[i];
				cell right_key = right_frame->frame.keys[i];
				if (left_key < right_key) return -1;
				if (left_key > right_key) return 1;
				int value_cmp = val_cmp(interp, left_frame->frame.values[i], right_frame->frame.values[i]);
				if (value_cmp) return value_cmp;
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

int print_truncate = 1;

static int stdout_is_tty(void) {
	static int cached = -1;
	if (cached < 0) cached = isatty(fileno(stdout));
	return cached;
}

static int print_depth = 0;

static void print_depth_bg(int depth) {
	if (!stdout_is_tty()) return;
	if (depth <= 0) { fputs("\033[49m", stdout); return; }
	int idx = 238 + (depth - 1) * 3;
	if (idx > 250) idx = 250;
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
			if (i == MATRIX_DISP_FIRST_ROWS) fputs(" ...\n", stdout);
			if (i >= MATRIX_DISP_FIRST_ROWS && i < rows - MATRIX_DISP_LAST_ROWS)
				continue;
		}

		for (int j = 0; j < cols; j++) {
			if (cols_trunc) {
				if (j == MATRIX_DISP_FIRST_COLS) printf(" %10s", "...");
				if (j >= MATRIX_DISP_FIRST_COLS && j < cols - MATRIX_DISP_LAST_COLS)
					continue;
			}
			print_matrix_cell(MAT(m, i, j));
		}
		putchar('\n');
	}
}

void print_val(Interpreter *interp, Val value) {
	switch (value.tag) {
		case T_FLOAT: print_double(unpack_float(value)); break;
		case T_SYM: fputs(&interp->vocab->symbol_pool[value.data], stdout); break;
		case T_STRING: fputs(interp->objects[value.data]->bytes, stdout); break;
		case T_SET:
			print_depth_enter();
			fputs("< ", stdout);
			print_items(interp, interp->objects[value.data]);
			putchar('>');
			print_depth_leave();
			break;
		case T_ARRAY:
			print_depth_enter();
			fputs("[ ", stdout);
			print_items(interp, interp->objects[value.data]);
			putchar(']');
			print_depth_leave();
			break;
		case T_XT: printf("<xt %lld>", (long long)value.data); break;
		case T_ADDR: printf("<addr %lld>", (long long)value.data); break;
		case T_MATRIX: {
						   Object *matrix = interp->objects[value.data];
						   print_depth_enter();
						   printf("<matrix %dx%d: ", matrix->matrix.rows, matrix->matrix.columns);
						   print_corners(matrix);
						   putchar('>');
						   print_depth_leave();
						   break;
					   }
		case T_FRAME: {
			Object *frame = interp->objects[value.data];
			print_depth_enter();
			fputs("{ ", stdout);
			for (int i = 0; i < frame->len; i++) {
				printf(":%s ", &interp->vocab->symbol_pool[frame->frame.keys[i]]);
				print_val(interp, frame->frame.values[i]);
				putchar(' ');
			}
			putchar('}');
			print_depth_leave();
			break;
		}
		default: printf("<?>"); break;
	}
}

void print_val_compact(Interpreter *interp, Val value) {
	switch (value.tag) {
		case T_FLOAT: {
						  double number = unpack_float(value);
						  if (number == (double)(int64_t)number && number > -1e12 && number < 1e12)
							  printf("%lld", (long long)number);
						  else
							  printf("%.4g", number);
						  break;
					  }
		case T_STRING: {
						   Object *obj = interp->objects[value.data];
						   if (obj->len <= 10) printf("\"%.*s\"", obj->len, obj->bytes);
						   else printf("\"%.9s…\"", obj->bytes);
						   break;
					   }
		case T_SYM: {
						const char *name = &interp->vocab->symbol_pool[value.data];
						int len = (int)strlen(name);
						if (len <= 10) fputs(name, stdout);
						else printf("%.9s…", name);
						break;
					}
		case T_SET:
			print_depth_enter();
			printf("<%d>", interp->objects[value.data]->len);
			print_depth_leave();
			break;
		case T_ARRAY:
			print_depth_enter();
			printf("[%d]", interp->objects[value.data]->len);
			print_depth_leave();
			break;
		case T_FRAME:
			print_depth_enter();
			printf("{%d}", interp->objects[value.data]->len);
			print_depth_leave();
			break;
		case T_MATRIX: {
						   Object *m = interp->objects[value.data];
						   print_depth_enter();
						   printf("M%dx%d", m->matrix.rows, m->matrix.columns);
						   print_depth_leave();
						   break;
					   }
		case T_XT: {
					   int target = (int)value.data;
					   const char *name = NULL;
					   for (int cfa = interp->vocab->latest_cfa; cfa != 0; cfa = (int)WORD_LINK(interp->vocab, cfa)) {
						   if (cfa == target) { name = &interp->vocab->name_pool[WORD_NAME(interp->vocab, cfa)]; break; }
					   }
					   if (name) {
						   int len = (int)strlen(name);
						   if (len <= 9) printf("'%s", name);
						   else printf("'%.8s…", name);
					   } else {
						   printf("'?");
					   }
					   break;
				   }
		case T_ADDR: printf("@%lld", (long long)value.data); break;
		case T_CONT: fputs("k", stdout); break;
		default: fputs("?", stdout); break;
	}
}

void print_frame_pretty(Interpreter *interp, Object *frame, int indent) {
	fputs("{\n", stdout);
	for (int i = 0; i < frame->len; i++) {
		for (int s = 0; s < indent + 2; s++) putchar(' ');
		printf(":%s ", &interp->vocab->symbol_pool[frame->frame.keys[i]]);
		Val value = frame->frame.values[i];
		if (value.tag == T_FRAME)
			print_frame_pretty(interp, interp->objects[value.data], indent + 2);
		else
			print_val(interp, value);
		putchar('\n');
	}
	for (int s = 0; s < indent; s++) putchar(' ');
	putchar('}');
}

void print_prompt_state(Interpreter *interp) {
	int tty = stdout_is_tty();
	if (tty) fputs("\033[48;5;240m", stdout);

	if (interp->error_flag) {
		printf("%d|error", interp->dsp);
	} else if (interp->dsp == 0) {
		printf("0");
	} else {
		printf("%d|", interp->dsp);
		print_val_compact(interp, interp->data_stack[interp->dsp - 1]);
	}

	if (tty) fputs("\033[49m", stdout);
	putchar(' ');
}

int find(Interpreter *interp, const char *name) {
	int cfa = interp->vocab->latest_cfa;
	while (cfa != 0) {
		if (strcmp(&interp->vocab->name_pool[WORD_NAME(interp->vocab, cfa)], name) == 0) return cfa;
		cfa = (int)WORD_LINK(interp->vocab, cfa);
	}
	return 0;
}

void docol(Interpreter *interp, cell *cfa) {
	rpush(interp, make_addr(interp->ip));
	interp->ip = (int)(cfa - interp->vocab->dict) + 1;
}

void dosym(Interpreter *interp, cell *cfa) {
	int symbol_offset = (int)cfa[1];
	push(interp, make_symbol(symbol_offset));
}

void dovar(Interpreter *interp, cell *cfa) {
	Val v;
	v.tag = (Tag)cfa[1];
	v.data = cfa[2];
	push(interp, v);
}

void run_inner(Interpreter *interp) {
	int initial_rsp = interp->rsp;

	while (interp->running && !interp->error_flag) {

		if (interp->unwinding) {

			if (interp->rsp <= initial_rsp) break;

			Val frame = interp->return_stack[--interp->rsp];
			if (frame.tag == T_MARK && (int)frame.data == interp->unwind_target) {

				interp->unwinding = 0;

				if (interp->rsp > 0) {
					Val ret = interp->return_stack[--interp->rsp];
					interp->ip = (int)ret.data;
				}
				continue;
			}

			continue;
		}

		int cfa_index = (int)interp->vocab->dict[interp->ip++];
		cfa_handler handler = (cfa_handler)interp->vocab->dict[cfa_index];
		handler(interp, &interp->vocab->dict[cfa_index]);
	}
}

void execute_cfa(Interpreter *interp, int cfa) {
	cfa_handler handler = (cfa_handler)interp->vocab->dict[cfa];
	if (handler != &docol) {
		handler(interp, &interp->vocab->dict[cfa]);
		return;
	}

	int saved_ip = interp->ip;
	int saved_running = interp->running;
	interp->vocab->dict[TRAMPOLINE_SLOT] = (cell)cfa;
	interp->vocab->dict[TRAMPOLINE_SLOT + 1] = (cell)interp->vocab->stop_cfa;
	interp->ip = TRAMPOLINE_SLOT;
	interp->running = 1;

	run_inner(interp);

	interp->running = saved_running;
	interp->ip = saved_ip;
}

int alloc_name(Interpreter *interp, const char *name) {
	int length = (int)strlen(name) + 1;
	if (interp->vocab->names_here + length > NAME_POOL) { fail(interp, "name pool full"); return 0; }

	int name_offset = interp->vocab->names_here;
	memcpy(&interp->vocab->name_pool[interp->vocab->names_here], name, (size_t)length);
	interp->vocab->names_here += length;

	return name_offset;
}

int intern_symbol(Interpreter *interp, const char *name) {
	for (int i = 0; i < interp->vocab->symbol_pool_here; ) {
		if (strcmp(&interp->vocab->symbol_pool[i], name) == 0) return i;
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

int create_header(Interpreter *interp, const char *name, int immediate) {
	dict_ensure(interp, 4);

	int previous_latest = interp->vocab->latest_cfa;
	int name_offset = alloc_name(interp, name);
	interp->vocab->dict[interp->vocab->here++] = previous_latest;
	interp->vocab->dict[interp->vocab->here++] = immediate ? 1 : 0;
	interp->vocab->dict[interp->vocab->here++] = name_offset;
	interp->vocab->dict[interp->vocab->here++] = 0;

	interp->vocab->latest_cfa = interp->vocab->here;
	return interp->vocab->latest_cfa;
}

int define_primitive(Interpreter *interp, const char *name, cfa_handler handler, int immediate) {
	int cfa = create_header(interp, name, immediate);
	emit(interp, (cell)handler);
	return cfa;
}

void emit(Interpreter *interp, cell value) {
	dict_ensure(interp, 1);
	interp->vocab->dict[interp->vocab->here++] = value;
}

void emit_val_literal(Interpreter *interp, Val value) {
	emit(interp, (cell)interp->vocab->literal_cfa);
	emit(interp, (cell)value.tag);
	emit(interp, value.data);
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
		case T_SYM:    return "a symbol";
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

void p_exit(Interpreter *interp, cell *cfa) {
	(void)cfa;

	while (interp->rsp > 0 && interp->return_stack[interp->rsp - 1].tag == T_MARK) interp->rsp--;

	if (interp->rsp <= 0) {
		interp->running = 0;
		return;
	}

	Val saved_ip = interp->return_stack[--interp->rsp];
	interp->ip = (int)saved_ip.data;
}

void p_stop(Interpreter *interp, cell *cfa) {
	(void)cfa;
	interp->running = 0;
}

void p_literal(Interpreter *interp, cell *cfa) {
	(void)cfa;
	Val literal;

	literal.tag = (Tag)interp->vocab->dict[interp->ip++];
	literal.data = interp->vocab->dict[interp->ip++];

	push(interp, literal);
}

void p_branch(Interpreter *interp, cell *cfa) {
	(void)cfa;
	interp->ip += (int)interp->vocab->dict[interp->ip];
}

void p_0branch(Interpreter *interp, cell *cfa) {
	(void)cfa;

	cell offset = interp->vocab->dict[interp->ip++];
	POP(condition);
	int is_false = (condition.tag == T_FLOAT) ? (unpack_float(condition) == 0.0)
		: (condition.data == 0);
	if (is_false) interp->ip += offset - 1;
}

void p_dostr(Interpreter *interp, cell *cfa) {
	(void)cfa;

	int template_handle = (int)interp->vocab->dict[interp->ip++];
	push(interp, make_string(interpolate(interp, template_handle)));
}

void p_enter_locals(Interpreter *interp, cell *cfa) {
	(void)cfa;

	int n_locals = (int)interp->vocab->dict[interp->ip++];
	rpush(interp, make_addr(interp->local_base));

	for (int i = 0; i < n_locals; i++)
		rpush(interp, make_float(0.0));

	interp->local_base = interp->rsp - n_locals;
}

void p_leave_locals(Interpreter *interp, cell *cfa) {
	(void)cfa;

	int n_locals = (int)interp->vocab->dict[interp->ip++];
	interp->rsp -= n_locals;
	Val saved = rpop(interp);
	interp->local_base = (int)saved.data;
}

static Val *local_slot(Interpreter *interp) {
	int depth = (int)interp->vocab->dict[interp->ip++];
	int slot  = (int)interp->vocab->dict[interp->ip++];

	int base = interp->local_base;
	for (int i = 0; i < depth; i++)
		base = (int)interp->return_stack[base - 1].data;

	return &interp->return_stack[base + slot];
}

void p_local_fetch(Interpreter *interp, cell *cfa) {
	(void)cfa;
	push(interp, *local_slot(interp));
}

void p_local_store(Interpreter *interp, cell *cfa) {
	(void)cfa;
	*local_slot(interp) = pop(interp);
}

void p_set(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP_INT(count, "set", "count");
	if (count < 0 || count > interp->dsp) {
		fail(interp, "set: count %d out of range (stack has %d available)", count, interp->dsp);
		return;
	}

	int set_handle = object_new_set(interp);
	if (interp->error_flag) return;

	int first_item = interp->dsp - count;
	for (int i = 0; i < count; i++) set_add(interp, set_handle, interp->data_stack[first_item + i]);
	interp->dsp = first_item;

	push(interp, make_set(set_handle));
}

void inbuf_reset(Interpreter *interp) {
	interp->input_buffer_len = 0;
	interp->input_buffer_pos = 0;
	interp->input_buffer[0] = 0;
	interp->need_more = 0;
}

char *next_token(Interpreter *interp) {
	while (interp->input_buffer_pos < interp->input_buffer_len && isspace((unsigned char)interp->input_buffer[interp->input_buffer_pos])) interp->input_buffer_pos++;
	if (interp->input_buffer_pos >= interp->input_buffer_len) return NULL;
	int start = interp->input_buffer_pos;
	while (interp->input_buffer_pos < interp->input_buffer_len && !isspace((unsigned char)interp->input_buffer[interp->input_buffer_pos])) interp->input_buffer_pos++;
	int length = interp->input_buffer_pos - start;
	if (length >= (int)sizeof(interp->token_buffer)) length = sizeof(interp->token_buffer) - 1;
	memcpy(interp->token_buffer, interp->input_buffer + start, (size_t)length);
	interp->token_buffer[length] = 0;
	return interp->token_buffer;
}

int parse_float(const char *text, double *out) {
	if (!*text) return 0;

	char *end_of_number;
	double value = strtod(text, &end_of_number);
	if (*end_of_number != 0) return 0;
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
	if (interp->compiling) emit_val_literal(interp, value);
	else push(interp, value);
}

int find_local(Interpreter *interp, const char *token, int *depth_out, int *slot_out) {
	for (int scope = interp->n_local_scopes - 1; scope >= 0; scope--) {
		int slice_start = interp->local_scope_starts[scope];
		int slice_end = (scope + 1 < interp->n_local_scopes)
			? interp->local_scope_starts[scope + 1]
			: interp->n_local_names;

		for (int name_idx = slice_start; name_idx < slice_end; name_idx++) {
			const char *name = &interp->local_names_pool[interp->local_name_offsets[name_idx]];
			if (strcmp(token, name) != 0) continue;

			int depth = 0;
			for (int inner = scope + 1; inner < interp->n_local_scopes; inner++) {
				int inner_start = interp->local_scope_starts[inner];
				int inner_end = (inner + 1 < interp->n_local_scopes)
					? interp->local_scope_starts[inner + 1]
					: interp->n_local_names;
				if (inner_end > inner_start) depth++;
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
		if (interp->input_buffer_pos >= interp->input_buffer_len) return;

		char ch = interp->input_buffer[interp->input_buffer_pos];
		if (ch == '"') {
			int n = read_string_literal(interp);
			if (n < 0) return;
			int handle = object_new_string(interp, interp->token_buffer, n);
			if (interp->compiling) {
				emit(interp, (cell)interp->vocab->dostr_cfa);
				emit(interp, (cell)handle);
			} else {
				int r = interpolate(interp, handle);
				push(interp, make_string(r));
			}
			continue;
		}
		if (ch == '(' && comment_starts_here(interp)) {
			skip_to_char(interp, ')');
			if (interp->input_buffer_pos < interp->input_buffer_len) interp->input_buffer_pos++;
			continue;
		}
		if (ch == '\\' && comment_starts_here(interp)) {
			skip_to_char(interp, '\n');
			continue;
		}

		char *tok = next_token(interp);
		if (!tok) return;

		if (interp->compiling) {
			int local_depth, local_slot_idx;
			if (find_local(interp, tok, &local_depth, &local_slot_idx)) {
				emit(interp, (cell)interp->vocab->local_fetch_cfa);
				emit(interp, (cell)local_depth);
				emit(interp, (cell)local_slot_idx);
				continue;
			}
		}

		int cf = find(interp, tok);
		if (cf) {
			if (interp->compiling && !WORD_IS_IMMEDIATE(interp->vocab, cf)) emit(interp, (cell)cf);
			else execute_cfa(interp, cf);
			continue;
		}

		if (tok[0] == ':' && tok[1] != '\0') {
			Val value = make_symbol(intern_symbol(interp, tok + 1));
			if (interp->error_flag) return;
			compile_or_push(interp, value);
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
		if (strcmp(interp->loaded_files[i], filename) == 0) return;
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

void p_load(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP_STRING(filename_obj, "load");
	gc_root_push(interp, filename_obj_val);

	const char *filename = filename_obj->bytes;
	if (interp->load_depth == 0)
		record_loaded_file(interp, filename);
	load_file(interp, filename);

	gc_root_pop(interp);
}

void p_reload(Interpreter *interp, cell *cfa) {
	(void)cfa;

	forget_user(interp);

	for (int i = 0; i < interp->n_loaded_files; i++) {
		load_file(interp, interp->loaded_files[i]);
		if (interp->error_flag) return;
	}
}

void mark_val(Interpreter *interp, Val value) {
	if (value.tag != T_STRING &&
			value.tag != T_SET &&
			value.tag != T_ARRAY &&
			value.tag != T_FRAME &&
			value.tag != T_MATRIX &&
			value.tag != T_CONT) return;

	int handle = (int)value.data;
	if (handle < 0 || handle >= MAX_OBJECTS || !interp->objects[handle] || interp->object_mark[handle]) return;

	interp->object_mark[handle] = 1;
	Object *obj = interp->objects[handle];
	if (obj->kind == OBJECT_SET || obj->kind == OBJECT_ARRAY) {
		for (int i = 0; i < obj->len; i++) mark_val(interp, obj->items[i]);
	} else if (obj->kind == OBJECT_FRAME) {
		for (int i = 0; i < obj->len; i++) mark_val(interp, obj->frame.values[i]);
	} else if (obj->kind == OBJECT_CONTINUATION) {
		for (int i = 0; i < obj->continuation.return_len; i++)
			mark_val(interp, obj->continuation.return_slice[i]);
	}
}

void mark_body(Interpreter *interp, int body_start, int body_end) {
	int cursor = body_start;

	while (cursor < body_end) {
		cell ref = interp->vocab->dict[cursor];
		if (ref == (cell)interp->vocab->literal_cfa && cursor + 2 < body_end) {
			Tag tag = (Tag)interp->vocab->dict[cursor + 1];
			Val value; value.tag = tag; value.data = interp->vocab->dict[cursor + 2];
			mark_val(interp, value);
			cursor += 3;
		} else if (ref == (cell)interp->vocab->dostr_cfa && cursor + 1 < body_end) {
			Val value; value.tag = T_STRING; value.data = interp->vocab->dict[cursor + 1];
			mark_val(interp, value);
			cursor += 2;
		} else if ((ref == (cell)interp->vocab->branch_cfa
					|| ref == (cell)interp->vocab->zbranch_cfa) && cursor + 1 < body_end) {
			cursor += 2;
		} else if ((ref == (cell)interp->vocab->to_var_cfa
					|| ref == (cell)interp->vocab->enter_locals_cfa
					|| ref == (cell)interp->vocab->leave_locals_cfa) && cursor + 1 < body_end) {
			cursor += 2;
		} else if ((ref == (cell)interp->vocab->local_fetch_cfa
					|| ref == (cell)interp->vocab->local_store_cfa) && cursor + 2 < body_end) {
			cursor += 3;
		} else {
			cursor++;
		}
	}
}

void gc(Interpreter *interp) {
	memset(interp->object_mark, 0, sizeof(interp->object_mark));

	for (int i = 0; i < interp->dsp; i++) mark_val(interp, interp->data_stack[i]);
	for (int i = 0; i < interp->rsp; i++) mark_val(interp, interp->return_stack[i]);
	for (int i = 0; i < interp->side_dsp; i++) mark_val(interp, interp->side_stack[i]);
	for (int i = 0; i < interp->n_gc_roots; i++) mark_val(interp, interp->gc_roots[i]);

	static int sorted_cfas[VOCABULARY_INIT_SIZE / 4];
	int num_cfas = 0;
	for (int cfa = interp->vocab->latest_cfa; cfa != 0; cfa = (int)WORD_LINK(interp->vocab, cfa)) {
		if (num_cfas >= (int)(sizeof sorted_cfas / sizeof sorted_cfas[0])) {
			/* A partial scan would leave some word bodies unmarked and free
			   objects they still reference. Abort the sweep instead — leaking
			   is recoverable, a use-after-free is not. */
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
		} else if (handler == dovar && body_start + 1 < body_end) {
			Val value;
			value.tag = (Tag)interp->vocab->dict[body_start];
			value.data = interp->vocab->dict[body_start + 1];
			mark_val(interp, value);
		}

	}

	for (int handle = 0; handle < interp->n_objects; handle++) {
		Object *obj = interp->objects[handle];
		if (!obj || interp->object_mark[handle]) continue;

		switch (obj->kind) {
			case OBJECT_STRING: free(obj->bytes); break;
			case OBJECT_SET:
			case OBJECT_ARRAY: free(obj->items); break;
			case OBJECT_FRAME: free(obj->frame.keys); free(obj->frame.values); break;
			case OBJECT_MATRIX: free(obj->matrix.elements); break;
			case OBJECT_CONTINUATION: free(obj->continuation.return_slice); break;
		}
		free(interp->objects[handle]);
		interp->objects[handle] = NULL;
	}
}

void p_save(Interpreter *interp, cell *cfa) {
	(void)cfa;

	POP_STRING(filename_obj, "save");
	gc_root_push(interp, filename_obj_val);
	const char *filename = filename_obj->bytes;

	FILE *file = fopen(filename, "w");
	if (!file) {
		fail(interp, "cannot create %s", filename);
		gc_root_pop(interp);
		return;
	}

	/* One entry per word; each header is >=4 cells, so /4 bounds the count.
	   static (like gc's sorted_cfas) keeps ~4MB off the call stack. */
	static int collected_cfas[VOCABULARY_INIT_SIZE / 4];
	int num_cfas = 0;
	for (int cfa = interp->vocab->latest_cfa; cfa > interp->vocab->lib_end_latest_cfa; cfa = (int)WORD_LINK(interp->vocab, cfa)) {
		if (num_cfas < (int)(sizeof collected_cfas / sizeof collected_cfas[0])) collected_cfas[num_cfas++] = cfa;
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
}

#define IMAGE_MAGIC "LF4I"
#define IMAGE_VERSION ((uint32_t)1)

#define HANDLER_DOCOL 1

#define HANDLER_DOVAR 2

#define HANDLER_DOSYM 3

void w_u8 (FILE *f, uint8_t v) { fwrite(&v, 1, 1, f); }

void w_i32(FILE *f, int32_t v) { fwrite(&v, 4, 1, f); }

void w_i64(FILE *f, int64_t v) { fwrite(&v, 8, 1, f); }

void w_val(FILE *f, Val v) {
	w_i32(f, (int32_t)v.tag);
	w_i64(f, v.data);
}

int r_u8 (FILE *f, uint8_t *v) { return fread(v, 1, 1, f) == 1; }

int r_u32(FILE *f, uint32_t *v) { return fread(v, 4, 1, f) == 1; }

int r_i32(FILE *f, int32_t *v) { return fread(v, 4, 1, f) == 1; }

int r_i64(FILE *f, int64_t *v) { return fread(v, 8, 1, f) == 1; }

int r_val(FILE *f, Val *v) {
	int32_t tag;
	int64_t data;
	if (!r_i32(f, &tag) || !r_i64(f, &data)) return 0;
	v->tag = (Tag)tag;
	v->data = data;
	return 1;
}

void p_save_image(Interpreter *interp, cell *cfa) {
	(void)cfa;

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
				for (int j = 0; j < obj->len; j++) w_val(file, obj->items[j]);
				break;
			case OBJECT_MATRIX: {
									w_i32(file, obj->matrix.rows);
									w_i32(file, obj->matrix.columns);
									size_t n = (size_t)obj->matrix.rows * (size_t)obj->matrix.columns;
									if (n > 0) fwrite(obj->matrix.elements, sizeof(double), n, file);
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

	for (int i = 0; i < interp->dsp; i++) w_val(file, interp->data_stack[i]);

	fclose(file);
	gc_root_pop(interp);
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
	interp->dsp = 0;
	interp->rsp = 0;
	interp->vocab->here = interp->vocab->init_here;
	interp->vocab->latest_cfa = interp->vocab->init_latest_cfa;
	interp->vocab->names_here = interp->vocab->init_names_here;
	interp->vocab->source_here = interp->vocab->init_source_here;
	interp->vocab->symbol_pool_here = interp->vocab->init_symbol_pool_here;
}

void p_load_image(Interpreter *interp, cell *cfa) {
	(void)cfa;

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
	define_primitive(interp, "negate", p_neg, 0);
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
	define_primitive(interp, ".", p_dot, 0);
	define_primitive(interp, ".a", p_dot_all, 0);
	define_primitive(interp, "cr", p_cr, 0);
	define_primitive(interp, "emit", p_emit_, 0);
	define_primitive(interp, ".s", p_dots, 0);
	define_primitive(interp, "bye", p_bye, 0);
	define_primitive(interp, "clear", p_clear, 0);
	define_primitive(interp, "gc",	 p_gc, 0);
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
	define_primitive(interp, "@", p_fetch, 0);
	define_primitive(interp, "!", p_store, 0);

	define_primitive(interp, "reset", 		p_reset, 0);
	define_primitive(interp, "shift", 		p_shift, 0);
	define_primitive(interp, "shift-with", 	p_shift_with, 0);
	define_primitive(interp, "resume", 	p_resume, 0);

	define_primitive(interp, "{", p_frameopen, 0);
	define_primitive(interp, "}", p_frameclose, 0);
	define_primitive(interp, "<", p_setopen, 0);
	define_primitive(interp, ">", p_setclose, 0);
	define_primitive(interp, "[", p_array_open, 0);
	define_primitive(interp, "]", p_array_close, 0);

	define_primitive(interp, "array",		 p_array, 0);
	define_primitive(interp, "array-of",	 p_array_of, 0);
	define_primitive(interp, ">frame",		 p_to_frame, 0);
	define_primitive(interp, "take",		 p_take, 0);
	define_primitive(interp, "reverse",	 p_reverse, 0);
	define_primitive(interp, "concat",	 p_concat, 0);
	define_primitive(interp, "range",	 p_range, 0);
	define_primitive(interp, "cardinality", p_cardinality, 0);
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
	define_primitive(interp, "words", p_words, 0);
	define_primitive(interp, "see", p_see, 0);

	interp->vocab->exit_cfa = define_primitive(interp, "exit", p_exit, 0);
	interp->vocab->literal_cfa = define_primitive(interp, "(lit)", p_literal, 0);
	interp->vocab->branch_cfa = define_primitive(interp, "(branch)", p_branch, 0);
	interp->vocab->zbranch_cfa = define_primitive(interp, "(0branch)", p_0branch, 0);
	interp->vocab->dostr_cfa = define_primitive(interp, "(dostr)", p_dostr, 0);
	interp->vocab->stop_cfa = define_primitive(interp, "(stop)", p_stop, 0);
	interp->vocab->to_var_cfa = define_primitive(interp, "(to-var)", p_to_var, 0);
	interp->vocab->enter_locals_cfa = define_primitive(interp, "(enter-locals)", p_enter_locals, 0);
	interp->vocab->leave_locals_cfa = define_primitive(interp, "(leave-locals)", p_leave_locals, 0);
	interp->vocab->local_fetch_cfa  = define_primitive(interp, "(local@)", p_local_fetch, 0);
	interp->vocab->local_store_cfa  = define_primitive(interp, "(local!)", p_local_store, 0);

	define_primitive(interp, ":", p_colon, 0);
	define_primitive(interp, "variable", p_variable, 0);
	define_primitive(interp, "symbol", p_symbol, 0);
	define_primitive(interp, "string>symbol", p_string_to_symbol, 0);
	define_primitive(interp, "forget", p_forget, 0);
	define_primitive(interp, "'", p_tick, 1);
	define_primitive(interp, "to", p_to, 1);

	define_primitive(interp, ";", p_semi, 1);
	define_primitive(interp, "if", p_if, 1);
	define_primitive(interp, "then", p_then, 1);
	define_primitive(interp, "else", p_else, 1);
	define_primitive(interp, "begin", p_begin, 1);
	define_primitive(interp, "until", p_until, 1);
	define_primitive(interp, "again", p_again, 1);
	define_primitive(interp, "[:", p_qcolon, 1);
	define_primitive(interp, ":]", p_qsemi, 1);
	define_primitive(interp, "|", p_bar, 1);

	define_primitive(interp, "0-matrix",		p_0_matrix, 0);
	define_primitive(interp, "matrix",			p_matrix, 0);
	define_primitive(interp, "dim",				p_dim, 0);
	define_primitive(interp, "transpose",		p_transpose, 0);
	define_primitive(interp, "diagonal-matrix",	p_diagonal_matrix, 0);
	define_primitive(interp, "@i", 	p_at_i, 0);
	define_primitive(interp, "@j", 	p_at_j, 0);
	define_primitive(interp, "@i,j", 	p_at_ij, 0);
	define_primitive(interp, "diagonal",		p_diagonal, 0);
	define_primitive(interp, "reshape",			p_reshape, 0);
	define_primitive(interp, "sum",				p_sum, 0);
	define_primitive(interp, "row-sums",		p_row_sums, 0);
	define_primitive(interp, "column-sums",		p_column_sums, 0);
	define_primitive(interp, "max",				p_max, 0);
	define_primitive(interp, "min",				p_min, 0);
	define_primitive(interp, "row-maxes",		p_row_maxes, 0);
	define_primitive(interp, "row-mins",		p_row_mins, 0);
	define_primitive(interp, "column-maxes",	p_column_maxes, 0);
	define_primitive(interp, "column-mins",		p_column_mins, 0);

	define_primitive(interp, "dgemm-nn", 	p_dgemm_nn, 0);
	define_primitive(interp, "dgemm-tn", 	p_dgemm_tn, 0);
	define_primitive(interp, "dgemm-nt", 	p_dgemm_nt, 0);
	define_primitive(interp, "dgemm-tt", 	p_dgemm_tt, 0);

	define_primitive(interp, "abs",		p_abs, 0);
	define_primitive(interp, "sqrt",	p_sqrt, 0);
	define_primitive(interp, "exp",		p_exp, 0);
	define_primitive(interp, "log",		p_log, 0);
	define_primitive(interp, "sin",		p_sin, 0);
	define_primitive(interp, "cos",		p_cos, 0);
	define_primitive(interp, "tan",		p_tan, 0);
	define_primitive(interp, "tanh",	p_tanh, 0);

	interp->vocab->init_here = interp->vocab->here;
	interp->vocab->init_latest_cfa = interp->vocab->latest_cfa;
	interp->vocab->init_names_here = interp->vocab->names_here;
	interp->vocab->init_source_here = interp->vocab->source_here;
	interp->vocab->init_symbol_pool_here = interp->vocab->symbol_pool_here;

	push(interp, make_string(object_new_string(interp, "src/forth/lib.l4", 16)));
	p_load(interp, NULL);
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

		if (interp->need_more) continue;

		if (interp->error_flag) {
			interp->compiling = 0;
			interp->dsp = 0;
			interp->rsp = 0;
			interp->compiling_src_start = 0;
			interp->n_local_scopes = 0;
			interp->n_local_names = 0;
			interp->local_names_pool_here = 0;
		}

		if (interp->compiling) continue;

		print_prompt_state(interp);
		if (interp->error_flag) fputs(interp->error_message, stdout);
		else fputs("ok", stdout);
		putchar('\n');
		fflush(stdout);

		inbuf_reset(interp);
	}
	return 0;
}
