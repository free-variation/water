#include "logicforth.h"
#include "lib_embed.h"
#include "isocline.h"


Vocabulary vocab;
Compiler compiler;
Arena arena;
PairPool pairs;

int in_parallel;
static _Thread_local AllocContext thread_alloc;
static AllocContext main_alloc;
static pthread_mutex_t intern_lock = PTHREAD_MUTEX_INITIALIZER;


static void arena_init(void) {
	arena.base = mmap(NULL, ARENA_RESERVE, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANON, -1, 0);

	if (arena.base == MAP_FAILED) {
		fprintf(stderr, "logicforth: arena mmap failed\n");
		exit(1);
	}

	arena.used = 0;
	arena.reserved = ARENA_RESERVE;
	main_alloc.slab_next = main_alloc.slab_end = arena.base;

	arena.max_objects = MAX_OBJECTS;
	arena.objects_cap = OBJECTS_INIT_CAP;
	arena.objects = calloc(arena.objects_cap, sizeof(Object *));
	arena.n_objects = 0;
	main_alloc.slot_next = arena.n_objects;
	main_alloc.slot_end = arena.n_objects;
	arena.free_slots = malloc(sizeof(int) * (size_t)arena.objects_cap);
	arena.n_free_slots = 0;
}

static inline void *arena_bump(AllocContext *ctx, size_t advance_bytes) {
	if ((size_t)(ctx->slab_end - ctx->slab_next) < advance_bytes) {
		size_t slab_claim_bytes = advance_bytes > SLAB_BYTES ? advance_bytes : SLAB_BYTES;
		size_t claimed = atomic_fetch_add(&arena.used, slab_claim_bytes);
		if (claimed + slab_claim_bytes > arena.reserved) {
			fprintf(stderr, "logicforth: arena exhausted\n");
			exit(1);
		}
		ctx->slab_next = arena.base + claimed;
		ctx->slab_end = ctx->slab_next + slab_claim_bytes;
	}

	void *allocation = ctx->slab_next;
	ctx->slab_next += advance_bytes;

	return allocation;
}

static inline void *arena_alloc(size_t bytes) {
	size_t advance_bytes = (bytes + (ARENA_ALIGNMENT - 1)) & ~(size_t)(ARENA_ALIGNMENT - 1);
	if (in_parallel)
		return arena_bump(&thread_alloc, advance_bytes);
	return arena_bump(&main_alloc, advance_bytes);
}


static inline int size_class_index(size_t bytes) {
	if (bytes <= 16) return 4;
	return 64 - __builtin_clzll(bytes - 1);
}

static void *arena_alloc_sized(size_t bytes) {
	AllocContext *context = in_parallel ? &thread_alloc : &main_alloc;
	int class_index = size_class_index(bytes);
	void *recycled_block = context->size_class_free[class_index];

	if (recycled_block) {
		context->size_class_free[class_index] = *(void **)recycled_block;
		return recycled_block;
	}

	return arena_alloc((size_t)1 << class_index);
}

static void arena_free_sized(void *block, size_t bytes) {
	AllocContext *context = in_parallel ? &thread_alloc : &main_alloc;
	int class_index = size_class_index(bytes);
	*(void **)block = context->size_class_free[class_index];
	context->size_class_free[class_index] = block;
}

void *arena_malloc(size_t bytes) {
	void *block = arena_alloc_sized(bytes + ARENA_ALIGNMENT);
	*(size_t *)block = bytes + ARENA_ALIGNMENT;

	return (char *)block + ARENA_ALIGNMENT;
}

void arena_free(void *payload) {
	if (!payload) return;
	void *block = (char *)payload - ARENA_ALIGNMENT;

	arena_free_sized(block, *(size_t *)block);
}

void *arena_realloc(void *payload, size_t bytes) {
	if (!payload) 
		return arena_malloc(bytes);

	void *block = (char *)payload - ARENA_ALIGNMENT;
	size_t old_total = *(size_t *)block;
	size_t new_total = bytes + ARENA_ALIGNMENT;

	if (size_class_index(new_total) == size_class_index(old_total)) {
		*(size_t *)block = new_total;
		return payload;
	}

	void *grown = arena_malloc(bytes);
	size_t old_payload_bytes = old_total - ARENA_ALIGNMENT;
	memcpy(grown, payload, old_payload_bytes < bytes ? old_payload_bytes : bytes);
	
	arena_free(payload);
	
	return grown;
}

static Object *arena_alloc_object(void) {
	AllocContext *context = in_parallel ? &thread_alloc : &main_alloc;
	Object *fresh;
	if (context->freed_object_structs) {
		fresh = context->freed_object_structs;
		context->freed_object_structs = *(void **)fresh;
	} else {
		fresh = arena_alloc(sizeof(Object));
	}
	memset(fresh, 0, sizeof(Object));
	return fresh;
}

static void arena_free_object(Object *obj) {
	AllocContext *context = in_parallel ? &thread_alloc : &main_alloc;
	*(void **)obj = context->freed_object_structs;
	context->freed_object_structs = obj;
}

int object_alloc_slot(Interpreter *interp) {
	if (in_parallel) {
		if (thread_alloc.slot_next >= thread_alloc.slot_end) {
			int claimed = atomic_fetch_add(&arena.n_objects, SLOTS_PER_CLAIM);
			if (claimed + SLOTS_PER_CLAIM > arena.objects_cap) {
				fail(interp, "object table full in parallel region");
				return -1;
			}

			thread_alloc.slot_next = claimed;
			thread_alloc.slot_end = claimed + SLOTS_PER_CLAIM;
		}

		return thread_alloc.slot_next++;
	}

	if (main_alloc.slot_next < main_alloc.slot_end)
		return main_alloc.slot_next++;

	if (arena.n_objects < arena.max_objects) {
		int claim = arena.max_objects - arena.n_objects;
		if (claim > SLOTS_PER_CLAIM) 
			claim = SLOTS_PER_CLAIM;
		if (arena.n_objects + claim > arena.objects_cap) {
			int new_cap = arena.objects_cap * 2;
			if (new_cap < arena.n_objects + claim)
				new_cap = arena.n_objects + claim;
			if (new_cap > arena.max_objects)
				new_cap = arena.max_objects;
			GROW_OBJECT_TABLE(new_cap);
		}
		
		main_alloc.slot_next = arena.n_objects;
		arena.n_objects += claim;
		main_alloc.slot_end = arena.n_objects;

		return main_alloc.slot_next++;
	}

	if (arena.n_free_slots > 0) {
		return arena.free_slots[--arena.n_free_slots];
	}

	if (interp->gc_disabled)
		return -1;

	gc(interp);

	if (arena.n_free_slots > 0) {
		return arena.free_slots[--arena.n_free_slots];
	}

	return -1;
}

void abort_parallel_region(size_t saved_used, int saved_n_objects, int saved_n_pairs) {
	arena.used = saved_used;
	arena.n_objects = saved_n_objects;
	pairs.n_pairs = saved_n_pairs;
	memset(&thread_alloc, 0, sizeof thread_alloc);
}

void reset_thread_alloc(void) {
	memset(&thread_alloc, 0, sizeof thread_alloc);
}

static Object *object_new(Interpreter *interp, ObjectKind kind, int *out_slot) {
	int slot = object_alloc_slot(interp);
	if (slot < 0) {
		fail(interp, "object registry full");
		*out_slot = -1;
		return NULL;
	}

	Object *fresh_object = arena_alloc_object();
	fresh_object->kind = kind;
	arena.objects[slot] = fresh_object;
	*out_slot = slot;

	return fresh_object;
}

int object_new_string(Interpreter *interp, const char *bytes, int length) {
	NEW_OBJECT(obj, OBJECT_STRING);
	obj->len = length;
	obj->capacity = length;
	obj->bytes = arena_malloc((size_t)length + 1);
	memcpy(obj->bytes, bytes, (size_t)length);
	obj->bytes[length] = 0;
	return slot;
}

int object_new_string_uninit(Interpreter *interp, int length) {
	NEW_OBJECT(obj, OBJECT_STRING);
	obj->len = length;
	obj->capacity = length;
	obj->bytes = arena_malloc((size_t)length + 1);
	obj->bytes[length] = 0;
	return slot;
}

#define SET_INITIAL_CAPACITY 4
#define FRAME_INITIAL_CAPACITY 4

int object_new_set(Interpreter *interp) {
	NEW_OBJECT(obj, OBJECT_SET);
	obj->capacity = SET_INITIAL_CAPACITY;
	obj->items = arena_malloc(sizeof(Val) * (size_t)obj->capacity);
	return slot;
}

static long alloc_count_array = 0;
static long alloc_count_lvar = 0;

int object_new_array(Interpreter *interp, int num_elements) {
	alloc_count_array++;
	NEW_OBJECT(obj, OBJECT_ARRAY);
	obj->len = num_elements;
	obj->capacity = num_elements;
	obj->items = arena_malloc(sizeof(Val) * (size_t)MAX(num_elements, 1));
	return slot;
}

int object_new_pair(Interpreter *interp) {
	int slot;

	if (in_parallel) {
		if (thread_alloc.pair_next >= thread_alloc.pair_end) {
			int claimed = atomic_fetch_add(&pairs.n_pairs, SLOTS_PER_CLAIM);
			if (claimed + SLOTS_PER_CLAIM > pairs.pairs_cap) {
				fail(interp, "pair table full in parallel region");
				return -1;
			}
			
			thread_alloc.pair_next = claimed;
			thread_alloc.pair_end = claimed + SLOTS_PER_CLAIM;
		}

		slot = thread_alloc.pair_next++;
		INIT_PAIR(slot);
		return slot;
	}

	if (pairs.free_count > 0) {
		slot = pairs.free_list[--pairs.free_count];
		INIT_PAIR(slot);
		return slot;
	}

	if (pairs.n_pairs == pairs.pairs_cap) {
		if (!interp->gc_disabled)
			gc(interp);
		if (pairs.free_count > 0) {
			slot = pairs.free_list[--pairs.free_count];
			INIT_PAIR(slot);
			return slot;
		}

		GROW_PAIR_TABLE(pairs.pairs_cap * 2);
	}

	slot = pairs.n_pairs++;
	INIT_PAIR(slot);
	return slot;
}

int object_new_frame(Interpreter *interp) {
	NEW_OBJECT(obj, OBJECT_FRAME);
	obj->capacity = FRAME_INITIAL_CAPACITY;
	obj->frame.keys = arena_malloc(sizeof(cell) * (size_t)obj->capacity);
	obj->frame.values = arena_malloc(sizeof(Val) * (size_t)obj->capacity);

	return slot;
}

int object_new_matrix(Interpreter *interp, int num_rows, int num_columns) {
	NEW_OBJECT(obj, OBJECT_MATRIX);
	obj->matrix.rows = num_rows;
	obj->matrix.columns = num_columns;
	size_t num_elements = (size_t)num_rows * (size_t)num_columns;
	
	obj->matrix.elements = calloc(num_elements ? num_elements : 1, sizeof(double));
	if (!obj->matrix.elements) {
		arena_free_object(obj);
		arena.objects[slot] = NULL;
		fail(interp, "matrix too large to allocate");
		return -1;
	}
	return slot;
}

int object_new_logic_var(Interpreter *interp) {
	alloc_count_lvar++;


	GROW_IF_FULL_SYS(interp->lvar_top, interp->lvar_cap, interp->lvar_stack);

	int id = interp->lvar_top++;
	interp->lvar_stack[id] = make_tagged(T_UNBOUND, 0);
	return id;
}

int object_new_continuation(Interpreter *interp, const Val *frames, int return_len, int resume_ip) {
	NEW_OBJECT(obj, OBJECT_CONTINUATION);
	obj->continuation.return_len = return_len;
	obj->continuation.resume_ip = resume_ip;
	obj->continuation.local_base_offset = -1;
	obj->continuation.capture_generation = vocab.forget_generation;
	obj->continuation.return_slice = malloc(sizeof(Val) * (size_t)MAX(return_len, 1));
	memcpy(obj->continuation.return_slice, frames, sizeof(Val) * (size_t)return_len);
	return slot;
}

static void *worker_entry(void *parallel_task) {
	ParallelTask *task = parallel_task;

	for (;;) {
			int start_index = atomic_fetch_add(&task->next_index, task->items_per_claim);
			if (start_index >= task->n_items) 
				break;
			int end_index = MIN(start_index + task->items_per_claim, task->n_items);
			task->kernel(start_index, end_index, task->context);
	}

	return NULL;
}

void parallel_for(int n_items, int n_threads, int items_per_claim,
		void (*kernel)(int start_index, int end_index, void *context),
		void *context) {
	CLAMP(n_threads, 1, MAX_WORKER_THREADS);
	if (n_threads > n_items)
		n_threads = n_items > 0 ? n_items : 1;

	ParallelTask task = {
		.n_items = n_items,
		.items_per_claim = items_per_claim,
		.next_index = 0,
		.kernel = kernel,
		.context = context,
	};

	pthread_t threads[MAX_WORKER_THREADS];
	int created = 0;

	for (int worker = 1; worker < n_threads; worker++)
		if (pthread_create(&threads[created], NULL, worker_entry, &task) == 0)
			created++;

	worker_entry(&task);
	for (int worker = 0; worker < created; worker++)
		pthread_join(threads[worker], NULL);
}

int val_cmp_depth(Interpreter *interp, Val left, Val right, int depth) {
	if (depth > MAX_NESTING_DEPTH) {
		fail(interp, "compare: structure too deeply nested (cycle?)");
		return 0;
	}

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
		case T_SYMBOL: case T_XT: case T_ADDR: case T_LOGIC_VAR:

					  if (VAL_DATA(left) < VAL_DATA(right))
					  	return -1;
					  if (VAL_DATA(left) > VAL_DATA(right))
					  	return 1;
					  return 0;
		case T_STRING: {
						   Object *left_string = OBJECT_AT(VAL_DATA(left));
						   Object *right_string = OBJECT_AT(VAL_DATA(right));
						   int compare_length = MIN(left_string->len, right_string->len);
						   int byte_diff = memcmp(left_string->bytes, right_string->bytes,
								   (size_t)compare_length);
						   if (byte_diff)
						   	return byte_diff;

						   return left_string->len - right_string->len;
					   }
		case T_SET: case T_ARRAY: {
									  Object *left_collection = OBJECT_AT(VAL_DATA(left));
									  Object *right_collection = OBJECT_AT(VAL_DATA(right));
									  int compare_length = MIN(left_collection->len, right_collection->len);
									  for (int i = 0; i < compare_length; i++) {
										  int element_cmp = val_cmp_depth(interp, left_collection->items[i],
												  right_collection->items[i], depth + 1);
										  if (element_cmp)
										  	return element_cmp;
									  }

									  return left_collection->len - right_collection->len;
								  }
		case T_PAIR: {
			Pair *left_pair = &pairs.table[VAL_DATA(left)];
			Pair *right_pair = &pairs.table[VAL_DATA(right)];
			int head_cmp = val_cmp_depth(interp, left_pair->head, right_pair->head, depth + 1);
			if (head_cmp)
				return head_cmp;
			return val_cmp_depth(interp, left_pair->tail, right_pair->tail, depth + 1);
		}

		case T_MATRIX: {
						   Object *left_matrix = OBJECT_AT(VAL_DATA(left));
						   Object *right_matrix = OBJECT_AT(VAL_DATA(right));

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
						  Object *left_frame = OBJECT_AT(VAL_DATA(left));
						  Object *right_frame = OBJECT_AT(VAL_DATA(right));
						  if (left_frame->len != right_frame->len)
							  return left_frame->len - right_frame->len;
						  for (int i = 0; i < left_frame->len; i++) {
							  cell left_key = left_frame->frame.keys[i];
							  cell right_key = right_frame->frame.keys[i];
							  if (left_key < right_key)
							  	return -1;
							  if (left_key > right_key)
							  	return 1;
							  int value_cmp = val_cmp_depth(interp, left_frame->frame.values[i], right_frame->frame.values[i], depth + 1);
							  if (value_cmp)
							  	return value_cmp;
						  }
						  return 0;
					  }

		default: return 0;
	}
}

int val_cmp(Interpreter *interp, Val left, Val right) {
	return val_cmp_depth(interp, left, right, 0);
}

void print_double(double number) {
	if (number == (double)(int64_t)number && number > -1e15 && number < 1e15)
		printf("%lld", (long long)number);
	else
		printf("%g", number);
}


int print_truncate = 1;

static int stdout_is_tty(void) {
	static int cached = -1;
	if (cached < 0)
		cached = isatty(fileno(stdout));
	return cached;
}

static int print_depth = 0;

static void print_depth_enter(void) { print_depth++; }
static void print_depth_leave(void) { print_depth--; }

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
	value = deref(interp, value);
	switch (VAL_TAG(value)) {
		case T_NONE: fputs("null", stdout); break;
		case T_UNBOUND: fputs("_", stdout); break;
		case T_FLOAT: print_double(VAL_NUMBER(value)); break;
		case T_SYMBOL: printf(":%s", &vocab.symbol_pool[VAL_DATA(value)]); break;
		case T_STRING: {
			Object *str = OBJECT_AT(VAL_DATA(value));
			if (print_depth > 0)
				printf("\"%s\"", str->bytes);
			else
				fputs(str->bytes, stdout);
			break;
		}
		case T_SET:
					   print_depth_enter();
					   if (print_depth > MAX_NESTING_DEPTH) {
						   fputs("<...>", stdout);
					   } else {
						   fputs("< ", stdout);
						   print_items(interp, OBJECT_AT(VAL_DATA(value)));
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
						   print_items(interp, OBJECT_AT(VAL_DATA(value)));
						   putchar(']');
					   }
					   print_depth_leave();
					   break;
		case T_PAIR: {
			print_depth_enter();
			if (print_depth > MAX_NESTING_DEPTH) {
				fputs("[(...)]", stdout);
			} else {
				fputs("[( ", stdout);
				Val cur = value;
				int count = 0;
				while (VAL_TAG(cur) == T_PAIR && count < LIST_PRINT_MAX) {
					Pair *pair = &pairs.table[VAL_DATA(cur)];
					print_val(interp, pair->head);
					putchar(' ');
					cur = deref(interp, pair->tail);
					count++;
				}
				if (count == LIST_PRINT_MAX)
					fputs("... ", stdout);
				else {
					print_val(interp, cur);
					putchar(' ');
				}
				fputs(")]", stdout);
			}
			print_depth_leave();
			break;
		}
		case T_XT: printf("<xt %lld>", (long long)VAL_DATA(value)); break;
		case T_ADDR: printf("<addr %lld>", (long long)VAL_DATA(value)); break;
		case T_STREAM: printf("<stream %lld>", (long long)VAL_DATA(value)); break;
		case T_DB: printf("<database %lld>", (long long)VAL_DATA(value)); break;
		case T_LOGIC_VAR: printf("_%d", (int)VAL_DATA(value)); break;
		case T_MATRIX: {
						   Object *matrix = OBJECT_AT(VAL_DATA(value));
						   print_depth_enter();
						   printf("<matrix %dx%d: ", matrix->matrix.rows, matrix->matrix.columns);
						   print_corners(matrix);
						   putchar('>');
						   print_depth_leave();
						   break;
					   }
		case T_FRAME: {
						  Object *frame = OBJECT_AT(VAL_DATA(value));
						  print_depth_enter();
						  if (print_depth > MAX_NESTING_DEPTH) {
							  fputs("{...}", stdout);
						  } else {
							  fputs("{ ", stdout);
							  for (int i = 0; i < frame->len; i++) {
								  printf(":%s ", &vocab.symbol_pool[frame->frame.keys[i]]);
								  print_val(interp, frame->frame.values[i]);
								  putchar(' ');
							  }
							  putchar('}');
						  }
						  print_depth_leave();
						  break;
					  }
		case T_MARK: {
						 int bracket = (int)VAL_DATA(value);
						 if (bracket == '(')
							 fputs("[(", stdout);
						 else
							 putchar(bracket == '{' || bracket == '[' || bracket == '<' ? bracket : '?');
						 break;
					 }
		default: printf("<?>"); break;
	}
}

static int array_has_nested(Object *arr) {
	for (int i = 0; i < arr->len; i++)
		if (VAL_TAG(arr->items[i]) == T_ARRAY)
			return 1;
	return 0;
}

static void pp_value(Interpreter *interp, Val value, int indent) {
	if (VAL_TAG(value) != T_ARRAY) {
		print_val(interp, value);
		return;
	}
	Object *arr = OBJECT_AT(VAL_DATA(value));
	if (!array_has_nested(arr)) {
		print_val(interp, value);
		return;
	}

	int n = arr->len;
	int trunc = print_truncate && n > PRINT_FIRST + PRINT_LAST;
	int child_indent = indent + 2;
	fputs("[ ", stdout);
	print_depth_enter();
	int first = 1;
	for (int i = 0; i < n; i++) {
		if (trunc && i == PRINT_FIRST) {
			putchar('\n');
			for (int s = 0; s < child_indent; s++)
				putchar(' ');
			fputs("...", stdout);
		}
		if (trunc && i >= PRINT_FIRST && i < n - PRINT_LAST)
			continue;
		if (!first) {
			putchar('\n');
			for (int s = 0; s < child_indent; s++)
				putchar(' ');
		}
		first = 0;
		pp_value(interp, arr->items[i], child_indent);
	}
	print_depth_leave();
	fputs(" ]", stdout);
}

void pretty_print_array(Interpreter *interp, Val value) {
	Object *arr = OBJECT_AT(VAL_DATA(value));
	if (!array_has_nested(arr)) {
		print_val(interp, value);
		putchar(' ');
		return;
	}
	pp_value(interp, value, 0);
	putchar(' ');
}

void print_val_inspect(Interpreter *interp, Val value) {
	print_depth_enter();
	print_val(interp, value);
	print_depth_leave();
}

void print_val_compact(Interpreter *interp, Val value) {
	value = deref(interp, value);
	switch (VAL_TAG(value)) {
		case T_NONE: fputs("null", stdout); break;
		case T_UNBOUND: fputs("_", stdout); break;
		case T_FLOAT: {
						  double number = VAL_NUMBER(value);
						  if (number == (double)(int64_t)number && number > -1e12 && number < 1e12)
							  printf("%lld", (long long)number);
						  else
							  printf("%.4g", number);
						  break;
					  }
		case T_STRING: {
						   Object *obj = OBJECT_AT(VAL_DATA(value));
						   if (obj->len <= 10)
						   	printf("\"%.*s\"", obj->len, obj->bytes);
						   else
						   	printf("\"%.9s…\"", obj->bytes);
						   break;
					   }
		case T_SYMBOL: {
						   const char *name = &vocab.symbol_pool[VAL_DATA(value)];
						   int len = (int)strlen(name);
						   if (len <= 10)
						   	printf(":%s", name);
						   else
						   	printf(":%.9s…", name);
						   break;
					   }
		case T_SET:
					   print_depth_enter();
					   printf("<%d>", OBJECT_AT(VAL_DATA(value))->len);
					   print_depth_leave();
					   break;
		case T_ARRAY:
					   print_depth_enter();
					   printf("[%d]", OBJECT_AT(VAL_DATA(value))->len);
					   print_depth_leave();
					   break;
		case T_PAIR:
					   fputs("[(…)]", stdout);
					   break;
		case T_FRAME:
					   print_depth_enter();
					   printf("{%d}", OBJECT_AT(VAL_DATA(value))->len);
					   print_depth_leave();
					   break;
		case T_MATRIX: {
						   Object *m = OBJECT_AT(VAL_DATA(value));
						   print_depth_enter();
						   printf("M%dx%d", m->matrix.rows, m->matrix.columns);
						   print_depth_leave();
						   break;
					   }
		case T_XT: {
					   int target = (int)VAL_DATA(value);
					   const char *name = NULL;
					   for (int cfa = vocab.latest_cfa; cfa != 0; cfa = (int)WORD_LINK(cfa)) {
						   if (cfa == target) {
							   name = &vocab.name_pool[WORD_NAME(cfa)];
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
		case T_LOGIC_VAR: printf("_%d", (int)VAL_DATA(value)); break;
		case T_MARK: {
						 int bracket = (int)VAL_DATA(value);
						 if (bracket == '(')
							 fputs("[(", stdout);
						 else
							 putchar(bracket == '{' || bracket == '[' || bracket == '<' ? bracket : '?');
						 break;
					 }
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
		printf(":%s ", &vocab.symbol_pool[frame->frame.keys[i]]);
		Val value = frame->frame.values[i];
		if (VAL_TAG(value) == T_FRAME)
			print_frame_pretty(interp, OBJECT_AT(VAL_DATA(value)), indent + 2);
		else
			print_val(interp, value);
		putchar('\n');
	}
	for (int s = 0; s < indent; s++)
		putchar(' ');
	putchar('}');
}

void print_prompt_state(Interpreter *interp) {
	if (stdout_is_tty())
		fputs("-> ", stdout);

	if (interp->error_flag) {
		printf("%d|error", interp->dsp);
	} else if (interp->dsp == 0) {
		putchar('0');
	} else {
		printf("%d|", interp->dsp);
		print_val_compact(interp, interp->data_stack[interp->dsp - 1]);
	}

	putchar(' ');
}

int find(const char *name) {
	int cfa = vocab.latest_cfa;
	while (cfa != 0) {
		if (strcmp(&vocab.name_pool[WORD_NAME(cfa)], name) == 0)
			return cfa;
		cfa = (int)WORD_LINK(cfa);
	}
	return 0;
}

const char *name_of(int cfa) {
	for (int cf = vocab.latest_cfa; cf != 0; cf = (int)WORD_LINK(cf)) {
		if (cf == cfa) {
			return &vocab.name_pool[WORD_NAME(cf)];
		}
	}
	return NULL;
}


static inline __attribute__((always_inline)) void push_variable(Interpreter *interp, int var_cfa) {
	Val value;
	value.bits = (uint64_t)vocab.dict[var_cfa + 1];
	push(interp, value);
};

static inline __attribute__((always_inline)) void push_symbol(Interpreter *interp, int sym_cfa) {
	push(interp, make_symbol((int)vocab.dict[sym_cfa + 1]));
}

void docol(Interpreter *interp) {
	int target_cfa = (int)vocab.dict[interp->ip++];
	rpush(interp, make_addr(interp->ip));
	interp->ip = target_cfa + 1;

	DISPATCH(interp);
}

void dosym(Interpreter *interp) {
	int sym_cfa = (int)vocab.dict[interp->ip++];
	push_symbol(interp, sym_cfa);

	DISPATCH(interp);
}

void dovar(Interpreter *interp) {
	int var_cfa = (int)vocab.dict[interp->ip++];
	push_variable(interp, var_cfa);

	DISPATCH(interp);
}

static void unwind_locals_scopes(Interpreter *interp) {
	while (interp->local_base > interp->run_floor 
			&& interp->rsp - 1 >= interp->local_base
			&& interp->rsp - 1 < interp->local_base + saved_n_locals(interp->return_stack[interp->local_base - 1])) {
		int enclosing_base = saved_local_base(interp->return_stack[interp->local_base - 1]);
		interp->rsp = interp->local_base - 1;
		interp->local_base = enclosing_base;
	}
}

void run_inner(Interpreter *interp, int floor) {
	if (interp->call_depth >= MAX_CALL_DEPTH) {
		fail(interp, "call stack too deep (runaway recursion via execute/resume/amb?)");
		return;
	}
	interp->call_depth++;
	int saved_floor = interp->run_floor;
	interp->run_floor = floor;

	while (interp->running && !interp->error_flag) {
		if (interp->unwinding) {
			if (interp->rsp <= floor)
				break;

			Val frame = interp->return_stack[--interp->rsp];
			if (VAL_TAG(frame) == T_MARK && (int)VAL_DATA(frame) == interp->unwind_target) {
				interp->unwinding = 0;

				unwind_locals_scopes(interp);

				if (interp->rsp > 0) {
					Val ret = interp->return_stack[--interp->rsp];
					interp->ip = (int)VAL_DATA(ret);
				}
				continue;
			}
			continue;
		}

		cfa_handler handler = (cfa_handler)vocab.dict[interp->ip++];
		handler(interp);
	}

	interp->run_floor = saved_floor;
	interp->call_depth--;
}

void execute_cfa(Interpreter *interp, int cfa) {
	cfa_handler handler = (cfa_handler)vocab.dict[cfa];

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
	cell saved_slot_0 = vocab.dict[interp->trampoline_base];
	cell saved_slot_1 = vocab.dict[interp->trampoline_base + 1];
	cell saved_slot_2 = vocab.dict[interp->trampoline_base + 2];
	cell stop_handler = vocab.dict[vocab.stop_cfa];

	if (handler == docol) {
		vocab.dict[interp->trampoline_base] = (cell)docol;
		vocab.dict[interp->trampoline_base + 1] = (cell)cfa;
		vocab.dict[interp->trampoline_base + 2] = stop_handler;
	} else {
		vocab.dict[interp->trampoline_base] = (cell)handler;
		vocab.dict[interp->trampoline_base + 1] = stop_handler;
		vocab.dict[interp->trampoline_base + 2] = stop_handler;
	}
	interp->ip = interp->trampoline_base;
	interp->running = 1;

	run_inner(interp, interp->rsp);

	interp->running = saved_running;
	interp->ip = saved_ip;
	vocab.dict[interp->trampoline_base] = saved_slot_0;
	vocab.dict[interp->trampoline_base + 1] = saved_slot_1;
	vocab.dict[interp->trampoline_base + 2] = saved_slot_2;
}

void call_open(Interpreter *interp, int cfa, CallContext *ctx) {
	cfa_handler handler = (cfa_handler)vocab.dict[cfa];

	if (handler == dovar || handler == dosym) {
		ctx->fast = 0;
		return;
	}

	ctx->fast = 1;
	ctx->saved_ip = interp->ip;
	ctx->saved_running = interp->running;
	ctx->saved_slot_0 = vocab.dict[interp->trampoline_base];
	ctx->saved_slot_1 = vocab.dict[interp->trampoline_base + 1];
	ctx->saved_slot_2 = vocab.dict[interp->trampoline_base + 2];

	cell stop_handler = vocab.dict[vocab.stop_cfa];
	if (handler == docol) {
		vocab.dict[interp->trampoline_base] = (cell)docol;
		vocab.dict[interp->trampoline_base + 1] = (cell)cfa;
		vocab.dict[interp->trampoline_base + 2] = stop_handler;
	} else {
		vocab.dict[interp->trampoline_base] = (cell)handler;
		vocab.dict[interp->trampoline_base + 1] = stop_handler;
		vocab.dict[interp->trampoline_base + 2] = stop_handler;
	}
}

void call_invoke(Interpreter *interp) {
	interp->ip = interp->trampoline_base;
	interp->running = 1;
	run_inner(interp, interp->rsp);
}

void call_close(Interpreter *interp, CallContext *ctx) {
	if (!ctx->fast)
		return;
	interp->running = ctx->saved_running;
	interp->ip = ctx->saved_ip;
	vocab.dict[interp->trampoline_base] = ctx->saved_slot_0;
	vocab.dict[interp->trampoline_base + 1] = ctx->saved_slot_1;
	vocab.dict[interp->trampoline_base + 2] = ctx->saved_slot_2;
}


int alloc_name(Interpreter *interp, const char *name) {
	int length = (int)strlen(name) + 1;
	if (vocab.names_here + length > NAME_POOL) {
		fail(interp, "name pool full");
		return 0;
	}
	int name_offset = vocab.names_here;
	memcpy(&vocab.name_pool[vocab.names_here], name, (size_t)length);
	vocab.names_here += length;

	return name_offset;
}

static unsigned int symbol_hash_index(const char *name) {
	unsigned int hash = 2166136261u;
	for (const unsigned char *byte = (const unsigned char *)name; *byte; byte++) {
		hash ^= *byte;
		hash *= 16777619u;
	}
	return hash & (SYMBOL_HASH_SIZE - 1);
}

void rebuild_symbol_hash(void) {
	memset(vocab.symbol_hash, 0, sizeof(vocab.symbol_hash));
	for (int offset = 0; offset < vocab.symbol_pool_here; ) {
		const char *name = &vocab.symbol_pool[offset];
		unsigned int index = symbol_hash_index(name);
		while (vocab.symbol_hash[index] != 0)
			index = (index + 1) & (SYMBOL_HASH_SIZE - 1);
		vocab.symbol_hash[index] = offset + 1;
		offset += (int)strlen(name) + 1;
	}
}

static int probe_symbol(const char *name, unsigned int *empty_slot) {
	unsigned int index = symbol_hash_index(name);
	
	for (int probe = 0; probe < SYMBOL_HASH_SIZE; probe++) {
		int slot = atomic_load_explicit(&vocab.symbol_hash[index], memory_order_acquire);
		if (slot == 0) {
			*empty_slot = index;
			return -1;
		}

		if (strcmp(&vocab.symbol_pool[slot - 1], name) == 0)
			return slot -1;

		index = (index + 1) & (SYMBOL_HASH_SIZE - 1);
	}

	return -2;
}

int intern_symbol(Interpreter *interp, const char *name) {
	unsigned int index;
	int symbol_offset = probe_symbol(name, &index);
	if (symbol_offset >= 0)
		return symbol_offset;

	if (in_parallel)
		pthread_mutex_lock(&intern_lock);

	symbol_offset = probe_symbol(name, &index);
	if (symbol_offset == -1) {
		int name_bytes = (int)strlen(name) + 1;
		if (vocab.symbol_pool_here + name_bytes > SYMBOL_POOL) {
			fail(interp, "symbol pool full");
			symbol_offset = 0;
		} else {
			symbol_offset = vocab.symbol_pool_here;
			memcpy(&vocab.symbol_pool[symbol_offset], name, (size_t)name_bytes);
			vocab.symbol_pool_here += name_bytes;
			atomic_store_explicit(&vocab.symbol_hash[index], symbol_offset + 1, memory_order_release);
		}
	} else if (symbol_offset == -2) {
		fail(interp, "symbol table full");
		symbol_offset = 0;
	}

	if (in_parallel)
		pthread_mutex_unlock(&intern_lock);
	return symbol_offset;
}

void dict_ensure(Interpreter *interp, int extra) {
	(void)interp;
	if (vocab.here + extra > VOCABULARY_INIT_SIZE) {
		fprintf(stderr, "logicforth: dictionary full\n");
		exit(1);
	}
}

int create_header(Interpreter *interp, const char *name, int flags) {
	dict_ensure(interp, 4);

	int previous_latest = vocab.latest_cfa;
	int name_offset = alloc_name(interp, name);
	vocab.dict[vocab.here++] = previous_latest;
	vocab.dict[vocab.here++] = flags;
	vocab.dict[vocab.here++] = name_offset;
	vocab.dict[vocab.here++] = 0;

	vocab.latest_cfa = vocab.here;
	return vocab.latest_cfa;
}

int define_primitive(Interpreter *interp, const char *name, cfa_handler handler, int flags) {
	int cfa = create_header(interp, name, flags);
	emit(interp, (cell)handler);
	if (compiler.n_handlers < MAX_HANDLERS)
		compiler.handler_registry[compiler.n_handlers++] = (void *)handler;
	return cfa;
}

void emit(Interpreter *interp, cell value) {
	dict_ensure(interp, 1);
	vocab.dict[vocab.here++] = value;
	compiler.fuse_prev_cmp = 0;
}

void emit_call(Interpreter *interp, int target_cfa) {
	cfa_handler handler = (cfa_handler)vocab.dict[target_cfa];
	emit(interp, (cell)handler);

	if (handler == docol || handler == dovar || handler == dosym) {
		emit(interp, (cell)target_cfa);
	}
}

void emit_val_literal(Interpreter *interp, Val value) {
	emit_call(interp, vocab.literal_cfa);
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
		case T_PAIR:   return "a pair";
		case T_FRAME:  return "a frame";
		case T_MATRIX: return "a matrix";
		case T_XT:     return "an execution token";
		case T_ADDR:   return "an address";
		case T_STREAM: return "a stream";
		case T_CONT:   return "a continuation";
		case T_MARK:   return "a mark";
		case T_LOGIC_VAR: return "a logic variable";
		case T_DB: return "a database";
		default:       return "an unknown value";
	}
}

void p_exit(Interpreter *interp) {
	while (interp->rsp > 0 && VAL_TAG(interp->return_stack[interp->rsp - 1]) == T_MARK) 
		interp->rsp--;


	unwind_locals_scopes(interp);

	if (interp->rsp <= interp->run_floor) {
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

void p_alloc_stats(Interpreter *interp) {
	printf("lvars=%ld arrays=%ld\n", alloc_count_lvar, alloc_count_array);
	alloc_count_lvar = 0;
	alloc_count_array = 0;

	DISPATCH(interp);
}

void p_literal(Interpreter *interp) {
	Val value;
	value.bits = (uint64_t)vocab.dict[interp->ip++];
	push(interp, value);

	DISPATCH(interp);
}

void p_branch(Interpreter *interp) {
	interp->ip += (int)vocab.dict[interp->ip];

	DISPATCH(interp);
}

#define ZBRANCH_BODY(get_condition) \
	cell offset = vocab.dict[interp->ip++]; \
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
	int template_handle = (int)vocab.dict[interp->ip++];
	push(interp, make_string(interpolate(interp, template_handle)));

	DISPATCH(interp);
}

void p_enter_locals(Interpreter *interp) {
	int n_locals = (int)vocab.dict[interp->ip++];
	if (interp->rsp + n_locals + 1 > RETURN_STACK_DEPTH) {
		fail(interp, "return stack overflow");
		return;
	}
	interp->return_stack[interp->rsp++] = make_locals_header(interp->local_base, n_locals);
	interp->rsp += n_locals;
	interp->local_base = interp->rsp - n_locals;

	DISPATCH(interp);
}

void p_enter_locals_to(Interpreter *interp) {
	int n_locals = (int)vocab.dict[interp->ip++];
	if (interp->rsp + n_locals + 1 > RETURN_STACK_DEPTH) {
		fail(interp, "(enter-locals-to): return stack overflow");
		return;
	}
	if (interp->dsp < n_locals) {
		fail(interp, "(enter-locals-to): insufficient values on data stack; need %d", n_locals);
		return;
	}

	interp->return_stack[interp->rsp++] = make_locals_header(interp->local_base, n_locals);
	int data_start = interp->dsp - n_locals;
	for (int i = 0; i < n_locals; i++)
		interp->return_stack[interp->rsp + i] = interp->data_stack[data_start + i];

	interp->dsp -= n_locals;
	interp->local_base = interp->rsp;
	interp->rsp += n_locals;

	DISPATCH(interp);
}

void p_enter_locals_mixed(Interpreter *interp) {
	int n_locals = (int)vocab.dict[interp->ip++];
	int n_received = (int)vocab.dict[interp->ip++];

	if (interp->rsp + n_locals + 1 > RETURN_STACK_DEPTH) {
		fail(interp, "(enter-locals-mixed): return stack overflow");
		return;
	}
	if (interp->dsp < n_received) {
		fail(interp, "(enter-locals-mixed): insufficient values on data stack; need %d", n_received);
		return;
	}

	interp->return_stack[interp->rsp++] = make_locals_header(interp->local_base, n_locals);
	interp->local_base = interp->rsp;
	interp->rsp += n_locals;

	int data_start = interp->dsp - n_received;
	for (int i = 0; i < n_received; i++) {
		int slot = (int)vocab.dict[interp->ip++];
		interp->return_stack[interp->local_base + slot] = interp->data_stack[data_start + i];
	}
	interp->dsp -= n_received;

	DISPATCH(interp);
}

void p_leave_locals(Interpreter *interp) {
	int n_locals = (int)vocab.dict[interp->ip++];
	interp->rsp -= n_locals;
	Val locals_header = rpop(interp);
	interp->local_base = saved_local_base(locals_header);

	DISPATCH(interp);
}

static Val *local_slot(Interpreter *interp) {
	int depth = (int)vocab.dict[interp->ip++];
	int slot  = (int)vocab.dict[interp->ip++];

	int base = interp->local_base;
	for (int i = 0; i < depth; i++)
		base = saved_local_base(interp->return_stack[base - 1]);

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
	push(interp, interp->return_stack[interp->local_base + (int)vocab.dict[interp->ip++]]);

	DISPATCH(interp);
}

void p_local_store_0depth(Interpreter *interp) {
	interp->return_stack[interp->local_base + (int)vocab.dict[interp->ip++]] = pop(interp);

	DISPATCH(interp);
}

#define LOCAL_ARITH_0DEPTH(name, word_name, expr) \
	void name(Interpreter *interp) { \
		int slot = (int)vocab.dict[interp->ip++]; \
		Val *p = &interp->return_stack[interp->local_base + slot]; \
		if (VAL_TAG(*p) != T_FLOAT) { \
			fail(interp, word_name ": expected a float local; got %s", tag_name(VAL_TAG(*p))); \
			return; \
		} \
		double n = VAL_NUMBER(*p); \
		*p = make_float(expr); \
		DISPATCH(interp); \
	}
LOCAL_ARITH_0DEPTH(p_local_incr_0depth, "(local+!)", n + 1.0)
LOCAL_ARITH_0DEPTH(p_local_decr_0depth, "(local-!)", n - 1.0)

#define UNSAFE_LOCAL_ARITH_0DEPTH(name, expr) \
	void name(Interpreter *interp) { \
		int slot = (int)vocab.dict[interp->ip++]; \
		Val *p = &interp->return_stack[interp->local_base + slot]; \
		double n = p->number; \
		p->number = (expr); \
		DISPATCH(interp); \
	}
UNSAFE_LOCAL_ARITH_0DEPTH(p_local_finc_0depth, n + 1.0)
UNSAFE_LOCAL_ARITH_0DEPTH(p_local_fdec_0depth, n - 1.0)

#define LOCAL_ACC_OP(suffix, op) \
	static int local_acc_##suffix##_0_cfa; \
	static int local_acc_##suffix##_cfa; \
	static void p_local_acc_##suffix##_0(Interpreter *interp) { \
		int slot = (int)vocab.dict[interp->ip++]; \
		Val *p = &interp->return_stack[interp->local_base + slot]; \
		double x = pop(interp).number; \
		p->number = x op p->number; \
		DISPATCH(interp); \
	} \
	static void p_local_acc_##suffix(Interpreter *interp) { \
		int depth = (int)vocab.dict[interp->ip++]; \
		int slot = (int)vocab.dict[interp->ip++]; \
		int base = interp->local_base; \
		for (int i = 0; i < depth; i++) \
			base = saved_local_base(interp->return_stack[base - 1]); \
		Val *p = &interp->return_stack[base + slot]; \
		double x = pop(interp).number; \
		p->number = x op p->number; \
		DISPATCH(interp); \
	}
LOCAL_ACC_OP(add, +)
LOCAL_ACC_OP(sub, -)
LOCAL_ACC_OP(mul, *)
LOCAL_ACC_OP(div, /)

int try_fuse_local_acc(Interpreter *interp, int depth, int slot) {
	cell *dict = vocab.dict;
	int here = vocab.here;
	if (here < 1)
		return 0;

	cfa_handler binop = (cfa_handler)dict[here - 1];
	int cfa0, cfag;
	if (binop == p_add_f) { cfa0 = local_acc_add_0_cfa; cfag = local_acc_add_cfa; }
	else if (binop == p_sub_f) { cfa0 = local_acc_sub_0_cfa; cfag = local_acc_sub_cfa; }
	else if (binop == p_mul_f) { cfa0 = local_acc_mul_0_cfa; cfag = local_acc_mul_cfa; }
	else if (binop == p_div_f) { cfa0 = local_acc_div_0_cfa; cfag = local_acc_div_cfa; }
	else return 0;

	if (depth == 0) {
		if (here < 3)
			return 0;
		if ((cfa_handler)dict[here - 3] != p_local_fetch_0depth)
			return 0;
		if ((int)dict[here - 2] != slot)
			return 0;
		vocab.here -= 3;
		emit_call(interp, cfa0);
		emit(interp, (cell)slot);
		return 1;
	}

	if (here < 4)
		return 0;
	if ((cfa_handler)dict[here - 4] != p_local_fetch)
		return 0;
	if ((int)dict[here - 3] != depth)
		return 0;
	if ((int)dict[here - 2] != slot)
		return 0;
	vocab.here -= 4;
	emit_call(interp, cfag);
	emit(interp, (cell)depth);
	emit(interp, (cell)slot);
	return 1;
}

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

void inbuf_reset(void) {
	compiler.input_buffer_len = 0;
	compiler.input_buffer_pos = 0;
	compiler.input_buffer[0] = 0;
	compiler.need_more = 0;
}

char *next_token(void) {
	while (compiler.input_buffer_pos < compiler.input_buffer_len
	       && isspace((unsigned char)compiler.input_buffer[compiler.input_buffer_pos]))
		compiler.input_buffer_pos++;

	if (compiler.input_buffer_pos >= compiler.input_buffer_len)
		return NULL;

	int start = compiler.input_buffer_pos;
	while (compiler.input_buffer_pos < compiler.input_buffer_len
	       && !isspace((unsigned char)compiler.input_buffer[compiler.input_buffer_pos]))
		compiler.input_buffer_pos++;

	int length = compiler.input_buffer_pos - start;
	if (length >= (int)sizeof(compiler.token_buffer))
		length = sizeof(compiler.token_buffer) - 1;

	memcpy(compiler.token_buffer, compiler.input_buffer + start, (size_t)length);
	compiler.token_buffer[length] = 0;
	return compiler.token_buffer;
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

static void skip_whitespace(void) {
	while (compiler.input_buffer_pos < compiler.input_buffer_len
			&& isspace((unsigned char)compiler.input_buffer[compiler.input_buffer_pos]))
		compiler.input_buffer_pos++;
}

static void skip_to_char(char delimiter) {
	while (compiler.input_buffer_pos < compiler.input_buffer_len
			&& compiler.input_buffer[compiler.input_buffer_pos] != delimiter)
		compiler.input_buffer_pos++;
}

static int comment_starts_here(void) {
	int next = compiler.input_buffer_pos + 1;
	return next >= compiler.input_buffer_len
		|| isspace((unsigned char)compiler.input_buffer[next]);
}

static void compile_or_push(Interpreter *interp, Val value) {
	if (compiler.compiling)
		emit_val_literal(interp, value);
	else
		push(interp, value);
	compiler.fuse_prev_var = 0;
	compiler.fuse_prev2_var = 0;
}

static void path_append(int handle, Val element) {
	Object *path = OBJECT_AT(handle);
	GROW_IF_FULL(path->len, path->capacity, path->items);
	path->items[path->len++] = element;
}

static int parse_path_predicate(Interpreter *interp, char *text) {
	char *op = strpbrk(text, "=<>");
	int op_code = PRED_EQ;
	char *value = NULL;
	if (op) {
		op_code = (*op == '<') ? PRED_LT : (*op == '>') ? PRED_GT : PRED_EQ;
		*op = '\0';
		value = op + 1;
	}
	if (text[0] == '\0') {
		fail(interp, "path predicate: empty key");
		return -1;
	}
	Val key_val;
	int key_rooted = 0;
	if (strchr(text, '/')) {
		int subpath = object_new_array(interp, 0);
		if (interp->error_flag)
			return -1;
		gc_root_push(interp, make_array(subpath));
		key_rooted = 1;
		for (char *q = text; *q; ) {
			char *seg = q;
			while (*q && *q != '/')
				q++;
			char saved = *q;
			*q = '\0';
			if (seg[0] != '\0')
				path_append(subpath, make_symbol(intern_symbol(interp, seg)));
			*q = saved;
			if (*q == '/')
				q++;
		}
		key_val = make_array(subpath);
	} else {
		key_val = make_symbol(intern_symbol(interp, text));
	}

	int handle;
	if (!op) {
		handle = object_new_array(interp, 2);
		if (interp->error_flag) {
			if (key_rooted) gc_root_pop(interp);
			return -1;
		}
		Object *predicate = OBJECT_AT(handle);
		predicate->items[0] = make_float(PRED_EXISTS);
		predicate->items[1] = key_val;
		if (key_rooted) gc_root_pop(interp);
		return handle;
	}

	if (value[0] == '\0') {
		fail(interp, "path predicate [%s…]: empty value", text);
		if (key_rooted) gc_root_pop(interp);
		return -1;
	}
	Val compare;
	double number;
	if (value[0] == ':')
		compare = make_symbol(intern_symbol(interp, value + 1));
	else if (parse_float(value, &number))
		compare = make_float(number);
	else
		compare = make_symbol(intern_symbol(interp, value));

	handle = object_new_array(interp, 3);
	if (interp->error_flag) {
		if (key_rooted) gc_root_pop(interp);
		return -1;
	}
	Object *predicate = OBJECT_AT(handle);
	predicate->items[0] = make_float(op_code);
	predicate->items[1] = key_val;
	predicate->items[2] = compare;
	if (key_rooted) gc_root_pop(interp);
	return handle;
}

int find_local(const char *token, int *depth_out, int *slot_out) {
	for (int scope = compiler.n_local_scopes - 1; scope >= 0; scope--) {
		int slice_start = compiler.local_scope_starts[scope];
		int slice_end = (scope + 1 < compiler.n_local_scopes)
			? compiler.local_scope_starts[scope + 1]
			: compiler.n_local_names;

		for (int name_idx = slice_start; name_idx < slice_end; name_idx++) {
			const char *name = &compiler.local_names_pool[compiler.local_name_offsets[name_idx]];
			if (strcmp(token, name) != 0)
				continue;

			int depth = 0;
			for (int inner = scope + 1; inner < compiler.n_local_scopes; inner++) {
				int inner_start = compiler.local_scope_starts[inner];
				int inner_end = (inner + 1 < compiler.n_local_scopes)
					? compiler.local_scope_starts[inner + 1]
					: compiler.n_local_names;
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
		skip_whitespace();
		if (compiler.input_buffer_pos >= compiler.input_buffer_len)
			return;

		char lead_char = compiler.input_buffer[compiler.input_buffer_pos];
		if (lead_char == '"') {
			int literal_len = read_string_literal();
			if (literal_len < 0)
				return;
			int handle = object_new_string(interp, compiler.token_buffer, literal_len);
			if (compiler.compiling) {
				emit_val_literal(interp, make_string(handle));
			} else {
				push(interp, make_string(handle));
			}
			compiler.fuse_prev_var = 0;
			compiler.fuse_prev2_var = 0;
			continue;
		}
		if (lead_char == '(' && comment_starts_here()) {
			skip_to_char(')');
			if (compiler.input_buffer_pos < compiler.input_buffer_len)
				compiler.input_buffer_pos++;
			continue;
		}
		if (lead_char == '\\' && comment_starts_here()) {
			skip_to_char('\n');
			continue;
		}

		char *tok = next_token();
		if (!tok)
			return;

		if (compiler.compiling) {
			int local_depth, local_slot_idx;
			if (find_local(tok, &local_depth, &local_slot_idx)) {
				if (local_depth == 0) {
					emit_call(interp, vocab.local_fetch_0depth_cfa);
					emit(interp, (cell)local_slot_idx);
				} else {
					emit_call(interp, vocab.local_fetch_cfa);
					emit(interp, (cell)local_depth);
					emit(interp, (cell)local_slot_idx);
				}
				compiler.fuse_prev_var = 0;
				compiler.fuse_prev2_var = 0;
				continue;
			}
		}

		int cf = find(tok);
		if (cf) {
			if (compiler.compiling && !WORD_IS_IMMEDIATE(cf)) {
				if (superword_try_fuse(interp, cf)) {
					continue;
				}
				if (WORD_IS_INLINE(cf)) {
					inline_word_body(interp, cf);
					compiler.fuse_prev_var = 0;
					compiler.fuse_prev2_var = 0;
				} else if ((cfa_handler)vocab.dict[cf] == dovar) {
					emit_call(interp, (cell)cf);
					compiler.fuse_prev2_var = compiler.fuse_prev_var;
					compiler.fuse_prev_var = cf;
				} else {
					emit_call(interp, (cell)cf);
					compiler.fuse_prev_var = 0;
					compiler.fuse_prev2_var = 0;
					if (cf == vocab.eq_cfa || cf == vocab.lt_cfa
							|| cf == vocab.gt_cfa || cf == vocab.zeq_cfa)
						compiler.fuse_prev_cmp = cf;
				}
			} else {
				execute_cfa(interp, cf);
				compiler.fuse_prev_var = 0;
				compiler.fuse_prev2_var = 0;
				compiler.fuse_prev_cmp = 0;
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

			int handle = object_new_array(interp, 0);
			if (interp->error_flag)
				return;
			gc_root_push(interp, make_array(handle));

			for (char *p = path; *p; ) {
				int run = 0;
				while (*p == '/') {
					p++;
					run++;
				}
				if (!*p)
					break;
				if (run >= 2)
					path_append(handle, make_symbol(vocab.descendant_symbol));

				char *segment = p;
				while (*p && *p != '/' && *p != '[')
					p++;
				if (p > segment) {
					char saved = *p;
					*p = '\0';
					path_append(handle, make_symbol(intern_symbol(interp, segment)));
					*p = saved;
				}

				while (*p == '[') {
					char *predicate_text = ++p;
					while (*p && *p != ']')
						p++;
					if (*p != ']') {
						fail(interp, "path literal %s: unterminated [", tok);
						gc_root_pop(interp);
						return;
					}
					*p++ = '\0';
					int predicate_handle = parse_path_predicate(interp, predicate_text);
					if (interp->error_flag) {
						gc_root_pop(interp);
						return;
					}
					path_append(handle, make_array(predicate_handle));
				}
			}

			gc_root_pop(interp);
			if (OBJECT_AT(handle)->len == 0) {
				fail(interp, "path literal %s has no segments", tok);
				return;
			}

			compile_or_push(interp, make_array(handle));
			continue;
		}

		double parsed_number;
		if (parse_float(tok, &parsed_number)) {
			compile_or_push(interp, make_float(parsed_number));
			continue;
		}

		if (tok[0] >= 'A' && tok[0] <= 'Z') {
			if (compiler.compiling) {
				fail(interp, "logic var %s is undeclared here; declare it in | | or create it at the top level first", tok);
				return;
			}
			int var_cfa = create_variable(interp, tok);
			if (interp->error_flag)
				return;
			int handle = object_new_logic_var(interp);
			if (interp->error_flag)
				return;
			vocab.dict[var_cfa + 1] = (cell)make_logic_var(handle).bits;
			execute_cfa(interp, var_cfa);
			continue;
		}

		fail(interp, "unknown word: %s", tok);
		return;
	}
}

void record_loaded_file(Interpreter *interp, const char *filename) {
	for (int i = 0; i < compiler.n_loaded_files; i++) {
		if (strcmp(compiler.loaded_files[i], filename) == 0)
			return;
	}
	if (compiler.n_loaded_files >= MAX_LOADED_FILES) {
		fail(interp, "load: %d-file history limit reached", MAX_LOADED_FILES);
		return;
	}
	compiler.loaded_files[compiler.n_loaded_files] = strdup(filename);
	compiler.n_loaded_files++;
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

	char *saved_inbuf_contents = malloc((size_t)compiler.input_buffer_len + 1);
	memcpy(saved_inbuf_contents, compiler.input_buffer, (size_t)compiler.input_buffer_len);
	int saved_inbuf_len = compiler.input_buffer_len;
	int saved_inbuf_pos = compiler.input_buffer_pos;
	int saved_need_more = compiler.need_more;

	size_t bytes_read = fread(compiler.input_buffer, 1, (size_t)file_size, file);
	fclose(file);
	compiler.input_buffer[bytes_read] = 0;
	compiler.input_buffer_len = (int)bytes_read;
	compiler.input_buffer_pos = 0;
	compiler.need_more = 0;

	compiler.load_depth++;
	run_outer(interp);
	compiler.load_depth--;

	if (!interp->error_flag && compiler.need_more) {
		fail(interp, "%s: unterminated string literal", filename);
	}
	if (!interp->error_flag && compiler.compiling) {
		fail(interp, "%s: unterminated definition", filename);
		compiler.compiling = 0;
	}

	memcpy(compiler.input_buffer, saved_inbuf_contents, (size_t)saved_inbuf_len);
	compiler.input_buffer[saved_inbuf_len] = 0;
	compiler.input_buffer_len = saved_inbuf_len;
	compiler.input_buffer_pos = saved_inbuf_pos;
	compiler.need_more = saved_need_more;
	free(saved_inbuf_contents);
}

void p_load(Interpreter *interp) {
	POP_STRING(filename_obj, "load");
	gc_root_push(interp, filename_obj_val);

	const char *filename = filename_obj->bytes;
	if (compiler.load_depth == 0)
		record_loaded_file(interp, filename);
	load_file(interp, filename);

	gc_root_pop(interp);

	DISPATCH(interp);
}

void p_reload(Interpreter *interp) {
	forget_user(interp);

	for (int i = 0; i < compiler.n_loaded_files; i++) {
		load_file(interp, compiler.loaded_files[i]);
		if (interp->error_flag)
			return;
	}

	DISPATCH(interp);
}

void mark_value(Interpreter *interp, Val value) {
	for (;;) {
		if (VAL_TAG(value) == T_LOGIC_VAR) {
			value = interp->lvar_stack[VAL_DATA(value)];
			continue;
		}

		if (VAL_TAG(value) != T_STRING &&
				VAL_TAG(value) != T_SET &&
				VAL_TAG(value) != T_ARRAY &&
				VAL_TAG(value) != T_PAIR &&
				VAL_TAG(value) != T_FRAME &&
				VAL_TAG(value) != T_MATRIX &&
				VAL_TAG(value) != T_CONT) return;

		if (VAL_TAG(value) == T_PAIR) {
			int slot = (int)VAL_DATA(value);
			if (pairs.mark[slot])
				return;
			pairs.mark[slot] = 1;
			mark_value(interp, pairs.table[slot].head);
			value = pairs.table[slot].tail;
			continue;
		}

		int handle = (int)VAL_DATA(value);
		if (handle < 0 || handle >= arena.n_objects)
			return;

		Object *obj = OBJECT_AT(handle);
		if (!obj || obj->mark_epoch == arena.current_epoch)
			return;
		obj->mark_epoch = arena.current_epoch;

		if (obj->kind == OBJECT_SET || obj->kind == OBJECT_ARRAY) {
			if (obj->len == 0)
				return;
			for (int i = 0; i < obj->len - 1; i++)
				mark_value(interp, obj->items[i]);
			value = obj->items[obj->len - 1];
			continue;
		} else if (obj->kind == OBJECT_FRAME) {
			if (obj->len == 0)
				return;
			for (int i = 0; i < obj->len - 1; i++)
				mark_value(interp, obj->frame.values[i]);
			value = obj->frame.values[obj->len - 1];
			continue;
		} else if (obj->kind == OBJECT_CONTINUATION) {
			if (obj->continuation.return_len == 0)
				return;
			for (int i = 0; i < obj->continuation.return_len - 1; i++)
				mark_value(interp, obj->continuation.return_slice[i]);
			value = obj->continuation.return_slice[obj->continuation.return_len - 1];
			continue;
		}
		return;
	}
}

static Val varmap_lookup(Interpreter *interp, VarMap *map, int slot) {
	for (int i = 0; i < map->count; i++)
		if (map->entries[i].slot == slot)
			return map->entries[i].value;

	Val fresh;
	if (map->reify) {
		char var_name[24];
		snprintf(var_name, sizeof(var_name), "_%d", map->count);
		fresh = make_symbol(intern_symbol(interp, var_name));
	} else
		fresh = make_logic_var(object_new_logic_var(interp));

	GROW_IF_FULL_SYS(map->count, map->cap, map->entries);

	map->entries[map->count].slot = slot;
	map->entries[map->count].value = fresh;
	map->count++;
	return fresh;
}



static void copy_value_inner(Interpreter *interp, VarMap *map, Val source_val, Val *copy_val, int depth) {
	int i, copy_handle;

	if (depth > MAX_NESTING_DEPTH) {
		fail(interp, "copy: structure too deeply nested (cycle?)");
		return;
	}

	source_val = deref(interp, source_val);

	switch(VAL_TAG(source_val)) {
		case T_LOGIC_VAR: {
							  *copy_val = varmap_lookup(interp, map, (int)VAL_DATA(source_val));
							  return;
						  }
		case T_STRING: {
						   Object *source = OBJECT_AT(VAL_DATA(source_val));
						   copy_handle = object_new_string(interp, source->bytes, source->len);
						   if (interp->error_flag)
						   	return;
						   *copy_val = make_string(copy_handle);
						   return;
					   }

		case T_MATRIX: {
						   Object *source = OBJECT_AT(VAL_DATA(source_val));
						   copy_handle = object_new_matrix(interp, source->matrix.rows, source->matrix.columns);
						   if (interp->error_flag)
						   	return;
						   Object *copy = OBJECT_AT(copy_handle);
						   memcpy(copy->matrix.elements, source->matrix.elements, sizeof(double) * (size_t)source->matrix.rows * source->matrix.columns);
						   *copy_val = make_matrix(copy_handle);
						   return;
					   }

		case T_ARRAY:
		case T_SET: {
						Object *source = OBJECT_AT(VAL_DATA(source_val));
						copy_handle = (VAL_TAG(source_val) == T_ARRAY) ? object_new_array(interp, source->len) : object_new_set(interp);
						if (interp->error_flag)
							return;

						Object *copy = OBJECT_AT(copy_handle);
						if (source->len > copy->capacity) {
							while (copy->capacity < source->len) 
								copy->capacity *= 2;
							copy->items = arena_realloc(copy->items, sizeof(Val) * (size_t)copy->capacity);
						}

						memset(copy->items, 0, sizeof(Val) * (size_t)source->len);
						copy->len = source->len;
						*copy_val = (VAL_TAG(source_val) == T_ARRAY) ? make_array(copy_handle) : make_set(copy_handle);
						for (i = 0; i < source->len; i++)
							copy_value_inner(interp, map, source->items[i], &copy->items[i], depth + 1);
						return;
					}
		case T_PAIR: {
						 int prev_slot = -1;
						 int spine_len = 0;
						 Val current = source_val;

						 while(VAL_TAG(current) == T_PAIR) {
							 if (spine_len++ > COPY_SPINE_MAX) {
								 fail(interp, "copy: list too long or cyclic");
								 return;
							 }

							 int source_slot = (int)VAL_DATA(current);
							 int new_slot = object_new_pair(interp);
							 if (interp->error_flag) return;

							 if (prev_slot < 0)
								 *copy_val = make_pair(new_slot);
							 else
								 pairs.table[prev_slot].tail = make_pair(new_slot);

							 Val head_copy;
							 copy_value_inner(interp, map, pairs.table[source_slot].head, &head_copy, depth + 1);
							 if (interp->error_flag) return;

							 pairs.table[new_slot].head = head_copy;
							 prev_slot = new_slot;
							 current = deref(interp, pairs.table[source_slot].tail);
						 }

						 Val tail_copy;
						 copy_value_inner(interp, map, current, &tail_copy, depth + 1);
						 if (interp->error_flag) return;

						 pairs.table[prev_slot].tail = tail_copy;
						 return;
					 }
		case T_FRAME: {
						  Object *source = OBJECT_AT(VAL_DATA(source_val));
						  copy_handle = object_new_frame(interp);
						  if (interp->error_flag)
						  	return;

						  Object *copy = OBJECT_AT(copy_handle);
						  frame_reserve(copy, source->len);

						  for (i = 0; i < source->len; i++)
							  copy->frame.keys[i] = source->frame.keys[i];
						  memset(copy->frame.values, 0, sizeof(Val) * (size_t)source->len);
						  copy->len = source->len;
						  *copy_val = make_frame(copy_handle);
						  for (i = 0; i < source->len; i++)
							  copy_value_inner(interp, map, source->frame.values[i], &copy->frame.values[i], depth + 1);
						  return;
					  }
		default:
					  *copy_val = source_val;
					  return;
	}
}

void copy_or_reify(Interpreter *interp, Val source_val, Val *copy_val, int reify) {
	VarMap map = { reify, NULL, 0, 0};
	if (arena.n_objects > arena.max_objects * 9 / 10)
		gc(interp);

	interp->gc_disabled = 1;
	copy_value_inner(interp, &map, source_val, copy_val, 0);
	interp->gc_disabled = 0;

	free(map.entries);
}


void do_copy_reify(Interpreter *interp, int reify) {
	PEEK_AT(source_val, 0, "copy");
	gc_root_push(interp, source_val);
	if (interp->error_flag)
		return;

	copy_or_reify(interp, source_val, &interp->gc_roots[interp->n_gc_roots - 1], reify);
	Val copy_val = interp->gc_roots[interp->n_gc_roots - 1];
	gc_root_pop(interp);
	if (interp->error_flag)
		return;

	interp->data_stack[interp->dsp - 1] = copy_val;
}

void p_copy(Interpreter *interp) {
	do_copy_reify(interp, 0);
	DISPATCH(interp);
}

void p_reify(Interpreter *interp) {
	do_copy_reify(interp, 1);
	DISPATCH(interp);
}

static int op_cell_count(int cursor) {
	cell *dict = vocab.dict;
	cell handler = dict[cursor];

	int superword_cells = superword_cell_count(handler);
	if (superword_cells)
		return superword_cells;

	if (handler == vocab.dict[vocab.enter_locals_mixed_cfa])
		return 3 + (int)dict[cursor + 2];

	if (handler == vocab.dict[vocab.local_fetch_cfa]
	    || handler == vocab.dict[vocab.local_store_cfa]
	    || handler == (cell)p_local_acc_add
	    || handler == (cell)p_local_acc_sub
	    || handler == (cell)p_local_acc_mul
	    || handler == (cell)p_local_acc_div)
		return 3;

	if (handler == vocab.dict[vocab.literal_cfa]
	    || handler == (cell)p_local_acc_add_0
	    || handler == (cell)p_local_acc_sub_0
	    || handler == (cell)p_local_acc_mul_0
	    || handler == (cell)p_local_acc_div_0)
		return 2;

	if (handler == vocab.dict[vocab.dostr_cfa]
	    || handler == vocab.dict[vocab.branch_cfa]
	    || handler == vocab.dict[vocab.zbranch_cfa]
	    || handler == vocab.dict[vocab.qzbranch_cfa]
	    || handler == vocab.dict[vocab.eq_zbranch_cfa]
	    || handler == vocab.dict[vocab.lt_zbranch_cfa]
	    || handler == vocab.dict[vocab.gt_zbranch_cfa]
	    || handler == vocab.dict[vocab.zeq_zbranch_cfa]
	    || handler == vocab.dict[vocab.to_var_cfa]
	    || handler == vocab.dict[vocab.enter_locals_cfa]
	    || handler == vocab.dict[vocab.enter_locals_to_cfa]
	    || handler == vocab.dict[vocab.leave_locals_cfa]
	    || handler == vocab.dict[vocab.local_fetch_0depth_cfa]
	    || handler == vocab.dict[vocab.local_store_0depth_cfa]
	    || handler == vocab.dict[vocab.local_incr_0depth_cfa]
	    || handler == vocab.dict[vocab.local_decr_0depth_cfa]
	    || handler == vocab.dict[vocab.local_finc_0depth_cfa]
	    || handler == vocab.dict[vocab.local_fdec_0depth_cfa])
		return 2;

	return 1;
}

void inline_word_body(Interpreter *interp, int target_cfa) {
	cell exit_handler = vocab.dict[vocab.exit_cfa];
	cell branch_handler = vocab.dict[vocab.branch_cfa];
	cell docol_handler = (cell)docol;

	int cursor = target_cfa + 1;
	int depth = 0;
	int expect_docol = 0;

	while (1) {
		cell handler = vocab.dict[cursor];

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

		int n = op_cell_count(cursor);
		for (int i = 0; i < n; i++)
			emit(interp, vocab.dict[cursor + i]);
		cursor += n;
		expect_docol = (handler == branch_handler);
	}
}

void mark_body(Interpreter *interp, int body_start, int body_end) {
	cell literal_ptr = vocab.dict[vocab.literal_cfa];
	cell dostr_ptr = vocab.dict[vocab.dostr_cfa];

	int cursor = body_start;
	while (cursor < body_end) {
		cell handler = vocab.dict[cursor];
		int n = op_cell_count(cursor);

		if (handler == literal_ptr) {
			Val value;
			value.bits = (uint64_t)vocab.dict[cursor + 1];
			mark_value(interp, value);
		} else if (handler == dostr_ptr) {
			Val value = make_string((int)vocab.dict[cursor + 1]);
			mark_value(interp, value);
		}

		cursor += n;
	}
}

void gc(Interpreter *interp) {
	int i;

	arena.current_epoch++;
	memset(pairs.mark, 0, sizeof(unsigned char) * (size_t)pairs.n_pairs);
	pairs.free_count = 0;

	for (i = 0; i < interp->dsp; i++)
		mark_value(interp, interp->data_stack[i]);
	for (i = 0; i < interp->rsp; i++)
		mark_value(interp, interp->return_stack[i]);
	for (i = 0; i < interp->side_dsp; i++)
		mark_value(interp, interp->side_stack[i]);
	for (i = 0; i < interp->n_gc_roots; i++)
		mark_value(interp, interp->gc_roots[i]);

	static int sorted_cfas[VOCABULARY_INIT_SIZE / 4];
	int num_cfas = 0;
	for (int cfa = vocab.latest_cfa; cfa != 0; cfa = (int)WORD_LINK(cfa)) {
		if (num_cfas >= (int)(sizeof sorted_cfas / sizeof sorted_cfas[0])) {
			fail(interp, "gc: vocabulary too large to scan safely");
			return;
		}
		sorted_cfas[num_cfas++] = cfa;
	}

	for (i = 1; i < num_cfas; i++) {
		int current = sorted_cfas[i];
		int slot = i - 1;
		while (slot >= 0 && sorted_cfas[slot] > current) {
			sorted_cfas[slot + 1] = sorted_cfas[slot];
			slot--;
		}
		sorted_cfas[slot + 1] = current;
	}

	for (i = 0; i < num_cfas; i++) {
		int cfa = sorted_cfas[i];
		int body_start = cfa + 1;
		int body_end = (i + 1 < num_cfas) ? sorted_cfas[i + 1] - 4 : vocab.here;
		
		cfa_handler handler = (cfa_handler)vocab.dict[cfa];

		if (handler == docol) {
			mark_body(interp, body_start, body_end);
		} else {
			if (handler == dovar && body_start < body_end) {
				Val value;
				value.bits = (uint64_t)vocab.dict[body_start];
				mark_value(interp, value);
			}
			mark_body(interp, body_start + 1, body_end);
		}

	}

	arena.n_free_slots = 0;
	for (int handle = 0; handle < arena.n_objects; handle++) {
		Object *obj = arena.objects[handle];
		if (obj && obj->mark_epoch == arena.current_epoch)
			continue;

		if (obj) {
			free_one_object(obj);
			arena.objects[handle] = NULL;
		}
		arena.free_slots[arena.n_free_slots++] = handle;
	}

	main_alloc.slot_next = main_alloc.slot_end = 0;

	for (int slot = 0; slot < pairs.n_pairs; slot++)
		if (!pairs.mark[slot])
			pairs.free_list[pairs.free_count++] = slot;
}

static const char *handler_word_name(cell handler) {
	for (int cfa = vocab.latest_cfa; cfa != 0; cfa = (int)WORD_LINK(cfa)) {
		if (vocab.dict[cfa] == handler)
			return &vocab.name_pool[WORD_NAME(cfa)];
	}
	return NULL;
}

static const char *var_name_from_slot(cell slot) {
	int var_cfa = (int)slot - 1;
	if (var_cfa > 0 && (cfa_handler)vocab.dict[var_cfa] == dovar)
		return &vocab.name_pool[WORD_NAME(var_cfa)];
	return NULL;
}

static void see_compiled_body(Interpreter *interp, int body_start, int body_end) {
	int cursor = body_start;

	while (cursor < body_end) {
		cell handler = vocab.dict[cursor];
		cfa_handler handler_fn = (cfa_handler)handler;
		int cell_count = op_cell_count(cursor);

		printf(" %d: ", cursor - body_start);

		if (handler_fn == docol || handler_fn == dovar) {
			int target = (int)vocab.dict[cursor + 1];
			printf("%s\n", &vocab.name_pool[WORD_NAME(target)]);
			cursor += 2;
			continue;
		}
		if (handler_fn == dosym) {
			printf(":%s\n", &vocab.symbol_pool[vocab.dict[cursor + 1]]);
			cursor += 2;
			continue;
		}

		if (superword_cell_count(handler)) {
			const char *name = handler_word_name(handler);
			printf("%s", name);
			for (int operand_index = 1; operand_index < cell_count; operand_index++) {
				const char *operand_var = var_name_from_slot(vocab.dict[cursor + operand_index]);
				printf(" %s", operand_var);
			}
		} else if (handler == vocab.dict[vocab.literal_cfa]) {
			Val value;
			value.bits = (uint64_t)vocab.dict[cursor + 1];
			fputs("(lit) ", stdout);
			print_val_compact(interp, value);
		} else {
			const char *name = handler_word_name(handler);
			printf("%s", name);
			for (int operand_index = 1; operand_index < cell_count; operand_index++)
				printf(" %lld", (long long)vocab.dict[cursor + operand_index]);
		}

		putchar('\n');
		cursor += cell_count;
	}
}

void p_see_compiled(Interpreter *interp) {
	POP_XT(target_cfa, "see-compiled");
	const char *name = &vocab.name_pool[WORD_NAME(target_cfa)];

	if ((cfa_handler)vocab.dict[target_cfa] != docol) {
		printf("%s: not a colon definition\n", name);
		DISPATCH(interp);
	}

	int body_start = target_cfa + 1;
	int body_end = vocab.here;
	for (int cfa = vocab.latest_cfa; cfa != 0; cfa = (int)WORD_LINK(cfa)) {
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
	for (int cfa = vocab.latest_cfa; cfa > vocab.lib_end_latest_cfa; cfa = (int)WORD_LINK(cfa)) {
		if (num_cfas < (int)(sizeof collected_cfas / sizeof collected_cfas[0]))
			collected_cfas[num_cfas++] = cfa;
	}

	fprintf(file, "\\ logicforth vocabulary\n\n");

	for (int i = num_cfas - 1; i >= 0; i--) {
		int cfa = collected_cfas[i];
		const char *name = &vocab.name_pool[WORD_NAME(cfa)];
		cfa_handler handler = (cfa_handler)vocab.dict[cfa];

		if (handler == docol) {
			int src_offset = (int)WORD_SOURCE(cfa);
			const char *body_source = (src_offset > 0) ? &vocab.source_pool[src_offset] : "";
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
#define IMAGE_VERSION ((uint32_t)4)

#define HANDLER_DOCOL 1
#define HANDLER_DOVAR 2
#define HANDLER_DOSYM 3

int handler_to_id(cell value) {
	for (int i = 0; i < compiler.n_handlers; i++)
		if (compiler.handler_registry[i] == (void *)value)
			return i;
	return -1;
}

/* Width of the op at `cursor`, for image translation. dovar/dosym always
   carry a trailing target-cfa operand (op_cell_count returns 1 for them, since
   mark_body absorbs that cell harmlessly; here it must be skipped explicitly).
   docol is variable width and is handled by the callers' peek logic, not here.
   The dispatch cell at `cursor` must hold a handler pointer (on load, translate
   id->pointer before calling this). */
static int image_op_cells(int cursor) {
	cell handler = vocab.dict[cursor];
	if (handler == (cell)dovar || handler == (cell)dosym)
		return 2;
	return op_cell_count(cursor);
}

static int body_end_of(const int *sorted_cfas, int count, int index) {
	return (index + 1 < count) ? sorted_cfas[index + 1] - 4 : vocab.here;
}

static void sort_cfas_ascending(int *cfas, int count) {
	for (int i = 1; i < count; i++) {
		int current = cfas[i];
		int slot = i - 1;
		while (slot >= 0 && cfas[slot] > current) {
			cfas[slot + 1] = cfas[slot];
			slot--;
		}
		cfas[slot + 1] = current;
	}
}

void w_u8 (FILE *file, uint8_t value) { fwrite(&value, 1, 1, file); }

void w_i32(FILE *file, int32_t value) { fwrite(&value, 4, 1, file); }

void w_i64(FILE *file, int64_t value) { fwrite(&value, 8, 1, file); }

void w_val(FILE *file, Val value) {
	w_i32(file, (int32_t)VAL_TAG(value));
	w_i64(file, VAL_DATA(value));
}

int r_u8 (FILE *file, uint8_t *out_value) { return fread(out_value, 1, 1, file) == 1; }

int r_u32(FILE *file, uint32_t *out_value) { return fread(out_value, 4, 1, file) == 1; }

int r_i32(FILE *file, int32_t *out_value) { return fread(out_value, 4, 1, file) == 1; }

int r_i64(FILE *file, int64_t *out_value) { return fread(out_value, 8, 1, file) == 1; }

int r_val(FILE *file, Val *out_value) {
	int32_t tag;
	int64_t data;
	if (!r_i32(file, &tag) || !r_i64(file, &data))
		return 0;
	*out_value = make_tagged((Tag)tag, data);
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
	for (int c = vocab.latest_cfa; c >= vocab.init_here; c = (int)WORD_LINK(c)) {
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

	int32_t user_dict_cells = vocab.here - vocab.init_here;
	int32_t user_namepool_bytes = vocab.names_here - vocab.init_names_here;
	int32_t user_sourcepool_bytes = vocab.source_here - vocab.init_source_here;
	int32_t user_symbolpool_bytes = vocab.symbol_pool_here - vocab.init_symbol_pool_here;
	w_i32(file, user_dict_cells);
	w_i32(file, user_namepool_bytes);
	w_i32(file, user_sourcepool_bytes);
	w_i32(file, user_symbolpool_bytes);
	w_i32(file, vocab.latest_cfa);
	w_i32(file, interp->dsp);
	w_i32(file, arena.n_objects);

	w_i32(file, vocab.init_here);
	w_i32(file, vocab.init_latest_cfa);
	w_i32(file, vocab.init_names_here);
	w_i32(file, vocab.init_source_here);
	w_i32(file, vocab.init_symbol_pool_here);

	int init_here = vocab.init_here;
	int *sorted_cfas = malloc(sizeof(int) * (size_t)MAX(user_word_count, 1));
	cell *out = malloc(sizeof(cell) * (size_t)MAX(user_dict_cells, 1));
	for (int i = 0; i < user_word_count; i++)
		sorted_cfas[i] = collected[i];
	sort_cfas_ascending(sorted_cfas, user_word_count);
	memcpy(out, &vocab.dict[init_here], sizeof(cell) * (size_t)user_dict_cells);

	for (int w = 0; w < user_word_count; w++) {
		int cfa = sorted_cfas[w];
		if (vocab.dict[cfa] != (cell)docol)
			continue;
		int end = body_end_of(sorted_cfas, user_word_count, w);
		for (int c = cfa + 1; c < end; ) {
			cell handler = vocab.dict[c];
			int id = handler_to_id(handler);
			if (id < 0) {
				fail(interp, "save-image: unrecognised handler in body of '%s' at offset %d", &vocab.name_pool[WORD_NAME(cfa)], c - cfa);
				free(out);
				free(sorted_cfas);
				fclose(file);
				gc_root_pop(interp);
				return;
			}
			out[c - init_here] = (cell)id;
			if (handler == (cell)docol)
				c += (handler_to_id(vocab.dict[c + 1]) >= 0) ? 1 : 2;
			else
				c += image_op_cells(c);
		}
	}

	for (int i = 0; i < user_dict_cells; i++)
		w_i64(file, (int64_t)out[i]);
	free(out);
	free(sorted_cfas);

	w_i32(file, user_word_count);
	for (int i = 0; i < user_word_count; i++) {
		int c = collected[i];
		cfa_handler h = (cfa_handler)vocab.dict[c];
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

	fwrite(&vocab.name_pool[vocab.init_names_here], 1, (size_t)user_namepool_bytes, file);
	fwrite(&vocab.source_pool[vocab.init_source_here], 1, (size_t)user_sourcepool_bytes, file);
	fwrite(&vocab.symbol_pool[vocab.init_symbol_pool_here], 1, (size_t)user_symbolpool_bytes, file);

	for (int slot = 0; slot < arena.n_objects; slot++) {
		Object *obj = arena.objects[slot];
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

	w_i32(file, pairs.n_pairs);
	for (int i = 0; i < pairs.n_pairs; i++) {
		w_val(file, pairs.table[i].head);
		w_val(file, pairs.table[i].tail);
	}

	w_i32(file, interp->lvar_top);
	for (int i = 0; i < interp->lvar_top; i++)
		w_val(file, interp->lvar_stack[i]);

	for (int i = 0; i < interp->dsp; i++)
		w_val(file, interp->data_stack[i]);

	fclose(file);
	gc_root_pop(interp);

	DISPATCH(interp);
}

void free_one_object(Object *obj) {
	switch (obj->kind) {
		case OBJECT_STRING: arena_free(obj->bytes); break;
		case OBJECT_SET:
		case OBJECT_ARRAY: arena_free(obj->items); break;
		case OBJECT_FRAME: arena_free(obj->frame.keys); arena_free(obj->frame.values); break;
		case OBJECT_MATRIX: free(obj->matrix.elements); break;
		case OBJECT_CONTINUATION: free(obj->continuation.return_slice); break;
	}
	arena_free_object(obj);
}

void forget_user(Interpreter *interp) {
	/* Free only user objects; objects below init_n_objects are literals baked
	   into the compiled-in vocabulary (e.g. run's " +") and must survive. */
	for (int i = arena.init_n_objects; i < arena.n_objects; i++) {
		if (arena.objects[i]) {
			free_one_object(arena.objects[i]);
			arena.objects[i] = NULL;
		}
	}
	arena.n_objects = arena.init_n_objects;
	main_alloc.slot_next = arena.n_objects;
	main_alloc.slot_end = arena.n_objects;
	arena.n_free_slots = 0;

	pairs.n_pairs = pairs.init_n_pairs;
	main_alloc.pair_next = main_alloc.pair_end = pairs.n_pairs;
	pairs.free_count = 0;

	interp->dsp = 0;
	interp->rsp = 0;
	vocab.here = vocab.init_here;
	vocab.forget_generation = 0;
	vocab.latest_cfa = vocab.init_latest_cfa;
	vocab.names_here = vocab.init_names_here;
	vocab.source_here = vocab.init_source_here;
	vocab.symbol_pool_here = vocab.init_symbol_pool_here;
	rebuild_symbol_hash();
}

static int loaded_handle_ok(Interpreter *interp, Val v) {
	int handle = (int)VAL_DATA(v);
	ObjectKind want;
	switch (VAL_TAG(v)) {
		case T_STRING: want = OBJECT_STRING; break;
		case T_SET:    want = OBJECT_SET; break;
		case T_ARRAY:  want = OBJECT_ARRAY; break;
		case T_FRAME:  want = OBJECT_FRAME; break;
		case T_MATRIX: want = OBJECT_MATRIX; break;
		case T_CONT:   want = OBJECT_CONTINUATION; break;
		case T_PAIR:      return handle >= 0 && handle < pairs.n_pairs;
		case T_LOGIC_VAR: return handle >= 0 && handle < interp->lvar_top;
		case T_SYMBOL:    return handle >= 0 && handle < vocab.symbol_pool_here;
		case T_XT:        return handle >= DICT_RESERVED && handle < vocab.here;
		default: return 1;
	}
	return handle >= 0 && handle < arena.n_objects
			&& arena.objects[handle] && arena.objects[handle]->kind == want;
}

static int validate_loaded(Interpreter *interp) {
	for (int i = 0; i < arena.n_objects; i++) {
		Object *obj = arena.objects[i];
		if (!obj) continue;
		if (obj->kind == OBJECT_SET || obj->kind == OBJECT_ARRAY) {
			for (int j = 0; j < obj->len; j++)
				if (!loaded_handle_ok(interp, obj->items[j])) return 0;
		} else if (obj->kind == OBJECT_FRAME) {
			for (int j = 0; j < obj->len; j++) {
				cell key = obj->frame.keys[j];
				if (key < 0 || key >= vocab.symbol_pool_here) return 0;
				if (!loaded_handle_ok(interp, obj->frame.values[j])) return 0;
			}
		} else if (obj->kind == OBJECT_CONTINUATION) {
			for (int j = 0; j < obj->continuation.return_len; j++)
				if (!loaded_handle_ok(interp, obj->continuation.return_slice[j])) return 0;
		}
	}
	for (int i = 0; i < pairs.n_pairs; i++)
		if (!loaded_handle_ok(interp, pairs.table[i].head) || !loaded_handle_ok(interp, pairs.table[i].tail))
			return 0;
	for (int i = 0; i < interp->lvar_top; i++)
		if (!loaded_handle_ok(interp, interp->lvar_stack[i])) return 0;
	for (int i = 0; i < interp->dsp; i++)
		if (!loaded_handle_ok(interp, interp->data_stack[i])) return 0;
	return 1;
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
	if (m_here != vocab.init_here || m_latest != vocab.init_latest_cfa
			|| m_names != vocab.init_names_here || m_sources != vocab.init_source_here
			|| m_symbols != vocab.init_symbol_pool_here)
	{
		fail(interp, "%s: interpreter bootstrap mismatch (rebuild needed)", filename);
		fclose(file);
		return;
	}

	if (user_dict_cells < 0 || vocab.init_here + user_dict_cells > VOCABULARY_INIT_SIZE
			|| user_namepool_bytes < 0 || vocab.init_names_here + user_namepool_bytes > NAME_POOL
			|| user_sourcepool_bytes < 0 || vocab.init_source_here + user_sourcepool_bytes > SOURCE_POOL
			|| user_symbolpool_bytes < 0 || vocab.init_symbol_pool_here + user_symbolpool_bytes > SYMBOL_POOL
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
		vocab.dict[vocab.init_here + i] = (cell)c;
	}
	vocab.here = vocab.init_here + user_dict_cells;

	int32_t user_word_count;
	if (!r_i32(file, &user_word_count)) {
		fail(interp, "%s: missing handler table", filename);
		goto done;
	}
	static int load_cfas[VOCABULARY_INIT_SIZE / 4];
	if (user_word_count < 0 || user_word_count > (int)(sizeof load_cfas / sizeof load_cfas[0])) {
		fail(interp, "%s: bad word count", filename);
		goto done;
	}
	for (int i = 0; i < user_word_count; i++) {
		int32_t c;
		uint8_t kind;
		if (!r_i32(file, &c) || !r_u8(file, &kind)) {
			fail(interp, "%s: truncated handler table", filename);
			goto done;
		}
		if (c < vocab.init_here || c >= vocab.here) {
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
		vocab.dict[c] = (cell)h;
		load_cfas[i] = c;
	}

	sort_cfas_ascending(load_cfas, user_word_count);
	for (int w = 0; w < user_word_count; w++) {
		int cfa = load_cfas[w];
		if (vocab.dict[cfa] != (cell)docol)
			continue;
		int end = body_end_of(load_cfas, user_word_count, w);
		for (int c = cfa + 1; c < end; ) {
			int id = (int)vocab.dict[c];
			if (id < 0 || id >= compiler.n_handlers) {
				fail(interp, "%s: handler id %d out of range", filename, id);
				goto done;
			}
			cell next_raw = vocab.dict[c + 1];
			vocab.dict[c] = (cell)compiler.handler_registry[id];
			if (vocab.dict[c] == (cell)docol)
				c += (next_raw >= 0 && next_raw < compiler.n_handlers) ? 1 : 2;
			else
				c += image_op_cells(c);
		}
	}

	if (fread(&vocab.name_pool[vocab.init_names_here], 1, (size_t)user_namepool_bytes, file) != (size_t)user_namepool_bytes
			|| fread(&vocab.source_pool[vocab.init_source_here], 1, (size_t)user_sourcepool_bytes, file) != (size_t)user_sourcepool_bytes
			|| fread(&vocab.symbol_pool[vocab.init_symbol_pool_here], 1, (size_t)user_symbolpool_bytes, file) != (size_t)user_symbolpool_bytes)
	{
		fail(interp, "%s: truncated pools", filename);
		goto done;
	}
	vocab.names_here = vocab.init_names_here + user_namepool_bytes;
	vocab.source_here = vocab.init_source_here + user_sourcepool_bytes;
	vocab.symbol_pool_here = vocab.init_symbol_pool_here + user_symbolpool_bytes;
	rebuild_symbol_hash();

	if (saved_n_objects > arena.max_objects) {
		fail(interp, "%s: image has too many objects", filename);
		goto done;
	}

	if (saved_n_objects > arena.objects_cap)
		GROW_OBJECT_TABLE(saved_n_objects);

	for (int slot = 0; slot < saved_n_objects; slot++) {
		uint8_t presence;
		if (!r_u8(file, &presence)) {
			fail(interp, "%s: truncated objects", filename);
			goto done;
		}
		if (presence == 0) {
			arena.objects[slot] = NULL;
			continue;
		}

		uint8_t kind;
		int32_t len, cap;
		if (!r_u8(file, &kind) || !r_i32(file, &len) || !r_i32(file, &cap)) {
			fail(interp, "%s: truncated object header", filename);
			goto done;
		}
		
		if (len < 0 || cap < 0 || len > cap) {
			fail(interp, "%s: object %d: invalid len/cap %d/%d", filename, slot, len, cap);
			goto done;
		}

		Object *obj = arena_alloc_object();
		obj->kind = kind;
		arena.objects[slot] = obj;
		obj->len = len;
		obj->capacity = cap;
		switch (kind) {
			case OBJECT_STRING:
				obj->bytes = arena_malloc((size_t)len + 1);
				if (len > 0 && fread(obj->bytes, 1, (size_t)len, file) != (size_t)len) {
					fail(interp, "%s: truncated string", filename);
					goto done;
				}
				obj->bytes[len] = 0;
				break;
			case OBJECT_SET:
			case OBJECT_ARRAY:
				obj->items = arena_malloc(sizeof(Val) * (size_t)MAX(cap, 1));
				for (int j = 0; j < len; j++) {
					if (!r_val(file, &obj->items[j])) {
						fail(interp, "%s: truncated items", filename);
						goto done;
					}
				}
				break;
			case OBJECT_FRAME:
				obj->frame.keys = arena_malloc(sizeof(cell) * (size_t)MAX(cap, 1));
				obj->frame.values = arena_malloc(sizeof(Val) * (size_t)MAX(cap, 1));
				for (int j = 0; j < len; j++) {
					int64_t key;
					if (!r_i64(file, &key) || !r_val(file, &obj->frame.values[j])) {
						fail(interp, "%s: truncated frame", filename);
						goto done;
					}
					obj->frame.keys[j] = (cell)key;
				}
				break;
			case OBJECT_MATRIX: {
									int32_t rows, cols;
									if (!r_i32(file, &rows) || !r_i32(file, &cols)) {
										fail(interp, "%s: truncated matrix header", filename);
										goto done;
									}
									if (rows < 0 || cols < 0 || (int64_t)rows * cols > INT_MAX) {
										fail(interp, "%s: bad matrix dims %dx%d", filename, rows, cols);
										goto done;
									}
									obj->matrix.rows = rows;
									obj->matrix.columns = cols;
									size_t n = (size_t)rows * (size_t)cols;
									obj->matrix.elements = calloc(MAX(n, 1), sizeof(double));
									if (n > 0 && fread(obj->matrix.elements, sizeof(double), n, file) != n) {
										fail(interp, "%s: truncated matrix data", filename);
										goto done;
									}
									break;
								}
			case OBJECT_CONTINUATION: {
										  int32_t return_len;
										  if (!r_i32(file, &return_len) || return_len < 0) {
											  fail(interp, "%s: bad continuation header", filename);
											  goto done;
										  }
										  obj->continuation.return_len = return_len;
										  obj->continuation.return_slice =
											  malloc(sizeof(Val) * (size_t)MAX(return_len, 1));
										  for (int j = 0; j < return_len; j++) {
											  if (!r_val(file, &obj->continuation.return_slice[j])) {
												  fail(interp, "%s: truncated continuation slice", filename);
												  goto done;
											  }
										  }
										  int32_t resume_ip;
										  if (!r_i32(file, &resume_ip)
												  || resume_ip < DICT_RESERVED || resume_ip >= vocab.here)
										  {
											  fail(interp, "%s: continuation resume_ip out of range", filename);
											  goto done;
										  }
										  obj->continuation.resume_ip = resume_ip;
										  int32_t local_base_offset;
										  if (!r_i32(file, &local_base_offset)) {
											  fail(interp, "%s: truncated continuation local_base_offset", filename);
											  goto done;
										  }
										  obj->continuation.local_base_offset = local_base_offset;
										  break;
									  }
			default:
									  fail(interp, "%s: unknown object kind %u", filename, kind);
									  goto done;
		}
	}
	arena.n_objects = saved_n_objects;
	main_alloc.slot_next = arena.n_objects;
	main_alloc.slot_end = arena.n_objects;
	arena.n_free_slots = 0;

	int32_t saved_n_pairs;
	if (!r_i32(file, &saved_n_pairs) || saved_n_pairs < 0) {
		fail(interp, "%s: bad pair count", filename);
		goto done;
	}
	while (pairs.pairs_cap < saved_n_pairs)
		GROW_PAIR_TABLE(pairs.pairs_cap * 2);
	for (int i = 0; i < saved_n_pairs; i++) {
		if (!r_val(file, &pairs.table[i].head) || !r_val(file, &pairs.table[i].tail)) {
			fail(interp, "%s: truncated pairs", filename);
			goto done;
		}
	}
	pairs.n_pairs = saved_n_pairs;
	main_alloc.pair_next = main_alloc.pair_end = pairs.n_pairs;
	pairs.free_count = 0;

	int32_t saved_lvar_top;
	if (!r_i32(file, &saved_lvar_top) || saved_lvar_top < 0) {
		fail(interp, "%s: bad lvar count", filename);
		goto done;
	}
	while (interp->lvar_cap < saved_lvar_top) {
		interp->lvar_cap *= 2;
		interp->lvar_stack = realloc(interp->lvar_stack, sizeof(Val) * (size_t)interp->lvar_cap);
	}
	for (int i = 0; i < saved_lvar_top; i++) {
		if (!r_val(file, &interp->lvar_stack[i])) {
			fail(interp, "%s: truncated lvars", filename);
			goto done;
		}
	}
	interp->lvar_top = saved_lvar_top;
	interp->bind_trail_top = 0;

	for (int i = 0; i < saved_dsp; i++) {
		if (!r_val(file, &interp->data_stack[i])) {
			fail(interp, "%s: truncated stack", filename);
			goto done;
		}
	}
	interp->dsp = saved_dsp;

	if (!validate_loaded(interp)) {
		fail(interp, "%s: image contains an invalid handle", filename);
		goto done;
	}

	vocab.latest_cfa = saved_latest_cfa;

done:
	fclose(file);

	if (interp->error_flag) {
		arena.n_objects = saved_n_objects;
		forget_user(interp);
		interp->lvar_top = 0;
		interp->bind_trail_top = 0;
	}

	DISPATCH(interp);
}


void interp_init(Interpreter *interp) {
	interp->next_mark_id = 1;
	interp->bind_trail = malloc(sizeof(int) * BIND_TRAIL_DEPTH);
	interp->bind_trail_cap = BIND_TRAIL_DEPTH;
	interp->lvar_stack = malloc(sizeof(Val) * LVAR_STACK_DEPTH);
	interp->lvar_cap = LVAR_STACK_DEPTH;
}

Interpreter *main_init(void) {
	Interpreter *interp = calloc(1, sizeof(Interpreter));
	interp_init(interp);

	arena_init();
	vocab.here = DICT_RESERVED;
	vocab.source_here = 1;

	pairs.table = malloc(sizeof(Pair) * PAIR_TABLE_DEPTH);
	pairs.pairs_cap = PAIR_TABLE_DEPTH;
	pairs.n_pairs = 0;
	main_alloc.pair_next = main_alloc.pair_end = pairs.n_pairs;
	pairs.mark = malloc(sizeof(unsigned char) * PAIR_TABLE_DEPTH);
	pairs.free_list = malloc(sizeof(int) * PAIR_TABLE_DEPTH);
	pairs.free_count = 0;

	vocab.false_symbol = intern_symbol(interp, "0");
	vocab.true_symbol = intern_symbol(interp, "1");
	vocab.wildcard_symbol = intern_symbol(interp, "*");
	vocab.descendant_symbol = intern_symbol(interp, "//");
	vocab.self_symbol = intern_symbol(interp, ".");

	return interp;
}

Interpreter *worker_init(int worker_index) {
	Interpreter *interp = calloc(1, sizeof(Interpreter));
	interp_init(interp);

	interp->trampoline_base = 3 * worker_index;
	interp->gc_disabled = 1;

	return interp;
}

int construct_vocabulary(Interpreter *interp, int load_lib) {
	compiler.handler_registry[compiler.n_handlers++] = (void *)docol;
	compiler.handler_registry[compiler.n_handlers++] = (void *)dovar;
	compiler.handler_registry[compiler.n_handlers++] = (void *)dosym;
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
	vocab.finc_cfa = define_primitive(interp, "f1+", p_inc, 0);
	vocab.fdec_cfa = define_primitive(interp, "f1-", p_dec, 0);
	define_primitive(interp, "fsq", p_sq, 0);
	define_primitive(interp, "negate", p_neg, 0);
	vocab.inc_cfa = define_primitive(interp, "1+", p_inc_poly, 0);
	vocab.dec_cfa = define_primitive(interp, "1-", p_dec_poly, 0);
	define_primitive(interp, "++", p_increment, 1);
	define_primitive(interp, "--", p_decrement, 1);
	define_primitive(interp, "f++", p_f_increment, 1);
	define_primitive(interp, "f--", p_f_decrement, 1);
	define_primitive(interp, "sq", p_sq_poly, 0);
	define_primitive(interp, "dup", p_dup, 0);
	define_primitive(interp, "drop", p_drop, 0);
	define_primitive(interp, "swap", p_swap, 0);
	define_primitive(interp, "over", p_over, 0);
	define_primitive(interp, "rot", p_rot, 0);
	define_primitive(interp, "depth", p_depth, 0);
	define_primitive(interp, "roll", p_roll, 0);
	vocab.eq_cfa = define_primitive(interp, "=", p_eq, 0);
	vocab.lt_cfa = define_primitive(interp, "lt", p_lt, 0);
	vocab.gt_cfa = define_primitive(interp, "gt", p_gt, 0);
	vocab.zeq_cfa = define_primitive(interp, "0=", p_zeq, 0);
	define_primitive(interp, "and", p_and, 0);
	define_primitive(interp, "or", p_or, 0);
	define_primitive(interp, "not", p_not, 0);
	define_primitive(interp, "null", p_null, 0);
	define_primitive(interp, "symbol?", p_symbol_q, 0);
	define_primitive(interp, "lvar", p_lvar, 0);
	define_primitive(interp, "_", p_wildcard, 0);
	define_primitive(interp, "unify", p_unify, 0);
	define_primitive(interp, "matches?", p_matches, 0);
	define_primitive(interp, "deref", p_deref, 0);
	define_primitive(interp, "amb", p_amb, 0);
	define_primitive(interp, "alloc-stats", p_alloc_stats, 0);
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
	define_primitive(interp, "match", p_match, 0);
	define_primitive(interp, "match-all", p_match_all, 0);
	define_primitive(interp, "split", p_split, 0);
	define_primitive(interp, "replace", p_replace, 0);
	define_primitive(interp, "substring", p_substring, 0);
	define_primitive(interp, "trim", p_trim, 0);
	define_primitive(interp, "join", p_join, 0);
	define_primitive(interp, "format", p_format, 0);
	define_primitive(interp, "update-at", p_update_at, 0);
	define_primitive(interp, "merge", p_merge, 0);
	define_primitive(interp, "copy", p_copy, 0);
	define_primitive(interp, "reify", p_reify, 0);

	define_primitive(interp, "reset", p_reset, 0);
	define_primitive(interp, "fail", p_fail, 0);
	define_primitive(interp, "shift", p_shift, 0);
	define_primitive(interp, "shift-with", p_shift_with, 0);
	define_primitive(interp, "resume", p_resume, 0);

	define_primitive(interp, "{", p_frameopen, 0);
	define_primitive(interp, "}", p_frameclose, 0);
	define_primitive(interp, "<", p_setopen, 0);
	define_primitive(interp, ">", p_setclose, 0);
	define_primitive(interp, "[", p_array_open, 0);
	define_primitive(interp, "]", p_array_close, 0);
	define_primitive(interp, "[(", p_list_open, 0);
	define_primitive(interp, ")]", p_list_close, 0);
	define_primitive(interp, "cons", p_cons, 0);
	define_primitive(interp, "head-tail", p_head_tail, 0);
	define_primitive(interp, "array>cons", p_array_to_cons, 0);
	define_primitive(interp, "cons>array", p_cons_to_array, 0);
	define_primitive(interp, "array>set", p_array_to_set, 0);
	define_primitive(interp, "group-by", p_group_by, 0);

	define_primitive(interp, "array", p_array, 0);
	define_primitive(interp, "array-of", p_array_of, 0);
	define_primitive(interp, "array>frame", p_array_to_frame, 0);
	define_primitive(interp, "frame>array", p_frame_to_array, 0);
	define_primitive(interp, "select-values", p_select_values, 0);
	define_primitive(interp, "select-keys", p_select_keys, 0);
	define_primitive(interp, "frame", p_frame, 0);
	define_primitive(interp, "json>frame", p_json_to_frame, 0);
	define_primitive(interp, "frame>json", p_frame_to_json, 0);
	define_primitive(interp, "take", p_take, 0);
	define_primitive(interp, "reverse", p_reverse, 0);
	define_primitive(interp, "reverse-slice!", p_reverse_slice, 0);
	define_primitive(interp, "concat", p_concat, 0);
	define_primitive(interp, "destruct", p_destruct, 0);
	define_primitive(interp, "destruct-to", p_destruct_to, 0);
	define_primitive(interp, "slice!", p_slice_store, 0);
	define_primitive(interp, "to-slice!", p_to_slice, 0);
	define_primitive(interp, "range", p_range, 0);
	define_primitive(interp, "size", p_size, 0);
	define_primitive(interp, "member?", p_member, 0);
	define_primitive(interp, "set", p_set, 0);
	define_primitive(interp, "union", p_union, 0);
	define_primitive(interp, "intersection", p_intersect, 0);
	define_primitive(interp, "difference", p_difference, 0);
	define_primitive(interp, "set-add!", p_set_add, 0);
	define_primitive(interp, "set-remove!", p_set_remove, 0);
	define_primitive(interp, "execute", p_execute, 0);
	define_primitive(interp, "map", p_map, 0);
	define_primitive(interp, "mapn", p_mapn, 0);
	define_primitive(interp, "filter", p_filter, 0);
	define_primitive(interp, "reduce", p_reduce, 0);
	define_primitive(interp, "times", p_times, 0);
	define_primitive(interp, "i-times", p_i_times, 0);
	define_primitive(interp, "pmap-ext", p_pmap, 0);
	define_primitive(interp, "pfilter-ext", p_pfilter, 0);
	define_primitive(interp, "pmap-reduce-ext", p_pmap_reduce, 0);
	define_primitive(interp, "num-cores", p_num_cores, 0);

	define_primitive(interp, "words", p_words, 0);
	define_primitive(interp, "see", p_see, 0);
	define_primitive(interp, "man", p_man, 0);
	define_primitive(interp, "see-compiled", p_see_compiled, 0);

	vocab.exit_cfa = define_primitive(interp, "exit", p_exit, 0);
	vocab.literal_cfa = define_primitive(interp, "(lit)", p_literal, 4);
	vocab.branch_cfa = define_primitive(interp, "(branch)", p_branch, 4);
	vocab.zbranch_cfa = define_primitive(interp, "(0branch)", p_0branch, 4);
	vocab.qzbranch_cfa = define_primitive(interp, "(?0branch)", p_qzbranch, 4);
	vocab.eq_zbranch_cfa = define_primitive(interp, "(=0branch)", p_eq_zbranch, 4);
	vocab.lt_zbranch_cfa = define_primitive(interp, "(lt0branch)", p_lt_zbranch, 4);
	vocab.gt_zbranch_cfa = define_primitive(interp, "(gt0branch)", p_gt_zbranch, 4);
	vocab.zeq_zbranch_cfa = define_primitive(interp, "(0=0branch)", p_zeq_zbranch, 4);
	vocab.dostr_cfa = define_primitive(interp, "(dostr)", p_dostr, 4);
	vocab.stop_cfa = define_primitive(interp, "(stop)", p_stop, 4);
	vocab.to_var_cfa = define_primitive(interp, "(to-var)", p_to_var, 4);
	vocab.enter_locals_cfa = define_primitive(interp, "(enter-locals)", p_enter_locals, 4);
	vocab.enter_locals_to_cfa = define_primitive(interp, "(enter-locals-to)", p_enter_locals_to, 4);
	vocab.enter_locals_mixed_cfa = define_primitive(interp, "(enter-locals-mixed)", p_enter_locals_mixed, 4);
	vocab.leave_locals_cfa = define_primitive(interp, "(leave-locals)", p_leave_locals, 4);
	vocab.local_fetch_cfa = define_primitive(interp, "(local@)", p_local_fetch, 4);
	vocab.local_store_cfa = define_primitive(interp, "(local!)", p_local_store, 4);
	vocab.local_fetch_0depth_cfa = define_primitive(interp, "(local@0)", p_local_fetch_0depth, 4);
	vocab.local_store_0depth_cfa = define_primitive(interp, "(local!0)", p_local_store_0depth, 4);
	vocab.local_incr_0depth_cfa  = define_primitive(interp, "(local+!0)", p_local_incr_0depth, 4);
	vocab.local_decr_0depth_cfa  = define_primitive(interp, "(local-!0)", p_local_decr_0depth, 4);
	vocab.local_finc_0depth_cfa  = define_primitive(interp, "(local f+!0)", p_local_finc_0depth, 4);
	vocab.local_fdec_0depth_cfa  = define_primitive(interp, "(local f-!0)", p_local_fdec_0depth, 4);
	local_acc_add_0_cfa = define_primitive(interp, "(acc+0)", p_local_acc_add_0, 4);
	local_acc_add_cfa   = define_primitive(interp, "(acc+)",  p_local_acc_add, 4);
	local_acc_sub_0_cfa = define_primitive(interp, "(acc-0)", p_local_acc_sub_0, 4);
	local_acc_sub_cfa   = define_primitive(interp, "(acc-)",  p_local_acc_sub, 4);
	local_acc_mul_0_cfa = define_primitive(interp, "(acc*0)", p_local_acc_mul_0, 4);
	local_acc_mul_cfa   = define_primitive(interp, "(acc*)",  p_local_acc_mul, 4);
	local_acc_div_0_cfa = define_primitive(interp, "(acc/0)", p_local_acc_div_0, 4);
	local_acc_div_cfa   = define_primitive(interp, "(acc/)",  p_local_acc_div, 4);

	define_superwords(interp);

	define_primitive(interp, ":", p_colon, 0);
	define_primitive(interp, "variable", p_variable, 0);
	define_primitive(interp, "symbol", p_symbol, 0);
	define_primitive(interp, "string>symbol", p_string_to_symbol, 0);
	define_primitive(interp, "forget", p_forget, 0);
	define_primitive(interp, "'", p_tick, 1);
	define_primitive(interp, "lookup", p_lookup, 0);
	define_primitive(interp, "to", p_to, 1);
	define_primitive(interp, ";", p_semicolon, 1);
	define_primitive(interp, "inline", p_inline, 0);
	define_primitive(interp, "if", p_if, 1);
	define_primitive(interp, "?if", p_qif, 1);
	define_primitive(interp, "then", p_then, 1);
	define_primitive(interp, "else", p_else, 1);
	define_primitive(interp, "begin", p_begin, 1);
	define_primitive(interp, "until", p_until, 1);
	define_primitive(interp, "again", p_again, 1);
	define_primitive(interp, "while", p_while, 1);
	define_primitive(interp, "repeat", p_repeat, 1);
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
	define_primitive(interp, "matrix-range", p_matrix_range, 0);
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
	define_primitive(interp, "sleep", p_sleep, 0);
	define_primitive(interp, "env", p_env, 0);
	define_primitive(interp, "env!", p_env_set, 0);
	define_primitive(interp, "cd", p_cd, 0);
	define_primitive(interp, "cwd", p_cwd, 0);
	define_primitive(interp, "read-file", p_read_file, 0);
	define_primitive(interp, "write-file", p_write_file, 0);
	define_primitive(interp, "append-file", p_append_file, 0);
	define_primitive(interp, "start-process", p_start_process, 0);
	define_primitive(interp, "write", p_write, 0);
	define_primitive(interp, "read", p_read, 0);
	define_primitive(interp, "close", p_close, 0);
	define_primitive(interp, "db-open", p_db_open, 0);
	define_primitive(interp, "db-close", p_db_close, 0);
	define_primitive(interp, "db-exec", p_db_exec, 0);
	define_primitive(interp, "db-query", p_db_query, 0);
	define_primitive(interp, "wait", p_wait, 0);
	define_primitive(interp, "stop", p_stop_process, 0);
	define_primitive(interp, "running?", p_running, 0);

	if (load_lib) {
		memcpy(compiler.input_buffer, lib_l4, lib_l4_len);
		compiler.input_buffer[lib_l4_len] = 0;
		compiler.input_buffer_len = (int)lib_l4_len;
		compiler.input_buffer_pos = 0;
		run_outer(interp);

		compiler.input_buffer_len = 0;
		compiler.input_buffer_pos = 0;
		compiler.input_buffer[0] = 0;

		if (interp->error_flag) {
			printf("lib.l4 load error\n");
			return 1;
		}
	}

	/* lib.l4 is part of the rebuilt-each-process base, not user state: the
	   init_* watermarks (the boundary an image saves above) sit after it, so
	   images carry only words defined after bootstrap. */
	vocab.init_here = vocab.here;
	vocab.init_latest_cfa = vocab.latest_cfa;
	vocab.init_names_here = vocab.names_here;
	vocab.init_source_here = vocab.source_here;
	vocab.init_symbol_pool_here = vocab.symbol_pool_here;
	arena.init_n_objects = arena.n_objects;
	pairs.init_n_pairs = pairs.n_pairs;

	vocab.lib_end_latest_cfa = vocab.latest_cfa;
	return 0;
}

static Interpreter *repl_interp;

#include "repl_highlight_groups.h"

static int lf_token_in(const char *const *set, const char *tok, long len) {
	for (int i = 0; set[i]; i++)
		if ((long)strlen(set[i]) == len && memcmp(set[i], tok, (size_t)len) == 0)
			return 1;
	return 0;
}

static int lf_is_number(const char *s, long len) {
	long i = 0;
	if (i < len && (s[i] == '-' || s[i] == '+'))
		i++;
	long digits = 0;
	while (i < len && s[i] >= '0' && s[i] <= '9') { i++; digits++; }
	if (i < len && s[i] == '.') {
		i++;
		while (i < len && s[i] >= '0' && s[i] <= '9') { i++; digits++; }
	}
	if (digits == 0)
		return 0;
	if (i < len && (s[i] == 'e' || s[i] == 'E')) {
		i++;
		if (i < len && (s[i] == '-' || s[i] == '+'))
			i++;
		long exp_digits = 0;
		while (i < len && s[i] >= '0' && s[i] <= '9') { i++; exp_digits++; }
		if (exp_digits == 0)
			return 0;
	}
	return i == len;
}

static const char *lf_token_style(const char *s, long len) {
	if (lf_is_number(s, len))
		return "ansi-teal";
	if (len == 2 && (memcmp(s, "[:", 2) == 0 || memcmp(s, ":]", 2) == 0
			|| memcmp(s, "[(", 2) == 0 || memcmp(s, ")]", 2) == 0))
		return "ansi-blue";
	if (s[0] == ':' && len > 1)
		return "ansi-olive";
	if (s[0] == '/' && len > 1 && ((s[1] >= 'a' && s[1] <= 'z') || (s[1] >= 'A' && s[1] <= 'Z')))
		return "ansi-olive";
	if (s[0] >= 'A' && s[0] <= 'Z')
		return "ansi-purple";
	if (lf_token_in(lf_control, s, len))
		return "ansi-maroon";
	if (lf_token_in(lf_defining, s, len))
		return "ansi-blue";
	if (lf_token_in(lf_logicwords, s, len))
		return "ansi-fuchsia";
	if (len > 0 && len < 128) {
		char buf[128];
		memcpy(buf, s, (size_t)len);
		buf[len] = 0;
		if (find(buf))
			return "ansi-navy";
	}
	return NULL;
}

static int lf_is_ws(char c) {
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static void repl_highlighter(ic_highlight_env_t *henv, const char *input, void *arg) {
	(void)arg;
	long n = (long)strlen(input);
	long i = 0;
	while (i < n) {
		if (lf_is_ws(input[i])) { i++; continue; }
		long start = i;
		if (input[i] == '\\' && (i + 1 >= n || lf_is_ws(input[i + 1]))) {
			while (i < n && input[i] != '\n') i++;
			ic_highlight(henv, start, i - start, "ansi-silver");
			continue;
		}
		if (input[i] == '(' && i + 1 < n && lf_is_ws(input[i + 1])) {
			while (i < n && input[i] != ')') i++;
			if (i < n) i++;
			ic_highlight(henv, start, i - start, "ansi-silver");
			continue;
		}
		if (input[i] == '"') {
			i++;
			while (i < n) {
				if (input[i] == '"') {
					if (i + 1 < n && input[i + 1] == '"') { i += 2; continue; }
					i++;
					break;
				}
				i++;
			}
			ic_highlight(henv, start, i - start, "ansi-green");
			continue;
		}
		while (i < n && !lf_is_ws(input[i])) i++;
		const char *style = lf_token_style(input + start, i - start);
		if (style)
			ic_highlight(henv, start, i - start, style);
	}
}

static void repl_complete_word(ic_completion_env_t *cenv, const char *word) {
	size_t word_len = strlen(word);
	for (int cfa = vocab.latest_cfa; cfa != 0; cfa = (int)WORD_LINK(cfa)) {
		if (WORD_IS_INTERNAL(cfa))
			continue;
		const char *name = &vocab.name_pool[WORD_NAME(cfa)];
		if (strncmp(name, word, word_len) == 0)
			if (!ic_add_completion(cenv, name))
				return;
	}
}

static void repl_completer(ic_completion_env_t *cenv, const char *prefix) {
	ic_complete_word(cenv, prefix, repl_complete_word, &ic_char_is_nonwhite);
	ic_complete_filename(cenv, prefix, '/', NULL, NULL);
}

int main(int argc, char **argv) {
	int interactive = isatty(fileno(stdin));
	int load_lib = 1;
	long max_objects_arg = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0)
			interactive = 1;
		else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--batch") == 0)
			interactive = 0;
		else if (strcmp(argv[i], "--no-lib") == 0)
			load_lib = 0;
		else if (strcmp(argv[i], "--max-objects") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "logicforth: --max-objects needs a value\n");
				return 2;
			}
			max_objects_arg = strtol(argv[++i], NULL, 10);
			if (max_objects_arg < 1) {
				fprintf(stderr, "logicforth: --max-objects must be a positive integer\n");
				return 2;
			}
		}
		else {
			fprintf(stderr, "logicforth: unknown option '%s'\n", argv[i]);
			return 2;
		}
	}

	Interpreter *interp = main_init();
	if (max_objects_arg > 0) {
		max_objects_arg = MIN(max_objects_arg, MAX_OBJECTS);
		arena.max_objects = (int)max_objects_arg;
	}
	signal(SIGPIPE, SIG_IGN);
	if (construct_vocabulary(interp, load_lib))
		return 1;

	if (interactive) {
		printf("logicforth %s\n", VERSION);
		ic_set_history(".logicforth_history", -1);
		repl_interp = interp;
		ic_set_default_completer(repl_completer, NULL);
		ic_set_default_highlighter(repl_highlighter, NULL);
		ic_enable_brace_matching(true);
		ic_set_matching_braces("[]{}");
		ic_enable_brace_insertion(false);
		ic_set_prompt_marker(NULL, "..");
		ic_enable_multiline_indent(true);
	}
	char line[1024];

	for (;;) {
		if (interactive) {
			char *entered = ic_readline("");
			if (!entered)
				break;
			int entered_len = (int)strlen(entered);
			if (compiler.input_buffer_len + entered_len + 1 < INPUT_BUFFER_SIZE - 1) {
				memcpy(compiler.input_buffer + compiler.input_buffer_len, entered, (size_t)entered_len);
				compiler.input_buffer_len += entered_len;
				compiler.input_buffer[compiler.input_buffer_len++] = '\n';
				compiler.input_buffer[compiler.input_buffer_len] = '\0';
			}
			ic_free(entered);
		} else {
			if (!fgets(line, sizeof(line), stdin))
				break;
			int line_len = (int)strlen(line);
			if (compiler.input_buffer_len + line_len < INPUT_BUFFER_SIZE - 1) {
				memcpy(compiler.input_buffer + compiler.input_buffer_len, line, (size_t)line_len + 1);
				compiler.input_buffer_len += line_len;
			}
		}

		interp->error_flag = 0;
		interp->unwinding = 0;
		int line_lvar_top = interp->lvar_top;
		int line_bind_trail_top = interp->bind_trail_top;
		compiler.need_more = 0;
		run_outer(interp);

		if (compiler.need_more)
			continue;

		if (interp->error_flag) {
			compiler.compiling = 0;
			interp->dsp = 0;
			interp->rsp = 0;
			interp->side_dsp = 0;
			interp->local_base = 0;
			interp->n_gc_roots = 0;
			trail_undo_to(interp, line_bind_trail_top);
			interp->lvar_top = line_lvar_top;
			compiler.compiling_src_start = 0;
			compiler.n_local_scopes = 0;
			compiler.n_local_names = 0;
			compiler.local_names_pool_here = 0;
		}

		if (compiler.compiling)
			continue;

		if (interactive) {
			print_prompt_state(interp);
			if (interp->error_flag)
				fputs(interp->error_message, stdout);
			else
				fputs("ok", stdout);
			putchar('\n');
			fflush(stdout);
		} else if (interp->error_flag) {
			fprintf(stdout, "error: %s\n", interp->error_message);
			fflush(stdout);
		}

		inbuf_reset();
	}
	return 0;
}

