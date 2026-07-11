#include "water.h"
#include "lib_embed.h"


Vocabulary vocab;
unsigned char dict_is_handler[VOCABULARY_INIT_SIZE];
Compiler compiler;
Arena arena;
PairPool pairs;

int in_parallel;
int parallel_region_collected;
int parallel_region_object_base;
int parallel_region_pair_base;
static _Thread_local AllocContext thread_alloc;
AllocContext main_alloc;
static platform_mutex_t intern_lock = PLATFORM_MUTEX_INIT;


static void *xmalloc(size_t bytes) {
	void *block = malloc(bytes);
	if (!block) {
		fprintf(stderr, "water: out of memory\n");
		exit(1);
	}
	return block;
}

static void *xcalloc(size_t count, size_t size) {
	void *block = calloc(count, size);
	if (!block) {
		fprintf(stderr, "water: out of memory\n");
		exit(1);
	}
	return block;
}

static void arena_init(void) {
	arena.base = platform_reserve(&arena.reserved);
	if (!arena.base) {
		fprintf(stderr, "water: arena reserve failed\n");
		exit(1);
	}

	arena.used = 0;
	arena.heap_bytes_live = 0;
	arena.heap_gc_threshold = HEAP_GC_FLOOR;
	main_alloc.slab_next = main_alloc.slab_end = arena.base;

	arena.object_space.max = (int)(arena.reserved / ARENA_BYTES_PER_HANDLE);
	arena.object_space.cap = OBJECTS_INIT_CAP;
	arena.objects = xcalloc(arena.object_space.cap, sizeof(Object *));
	arena.object_space.n = 0;
	main_alloc.objects.next = arena.object_space.n;
	main_alloc.objects.end = arena.object_space.n;
	arena.object_space.free = xmalloc(sizeof(int) * (size_t)arena.object_space.cap);
	arena.object_space.n_free = 0;
}

static inline void *arena_bump(AllocContext *ctx, size_t advance_bytes) {
	if ((size_t)(ctx->slab_end - ctx->slab_next) < advance_bytes) {
		size_t slab_claim_bytes = advance_bytes > SLAB_BYTES ? advance_bytes : SLAB_BYTES;
		size_t claimed = atomic_fetch_add(&arena.used, slab_claim_bytes);
		if (claimed + slab_claim_bytes > arena.reserved) {
			fprintf(stderr, "water: arena exhausted\n");
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

Object *arena_alloc_object(void) {
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

static inline int local_claim_handle(LocalHandles *lh, HandleSpace *space) {
	if (lh->n_free > 0)
		return lh->free[--lh->n_free];

	if (lh->next >= lh->end) {
		int claimed = atomic_fetch_add(&space->n, SLOTS_PER_CLAIM);
		if (claimed + SLOTS_PER_CLAIM > space->cap)
			return -1;

		lh->next = claimed;
		lh->end = claimed + SLOTS_PER_CLAIM;
		GROW_IF_FULL_SYS(lh->n_chunks, lh->chunks_cap, lh->chunks);
		lh->chunks[lh->n_chunks++] = claimed;
	}

	return lh->next++;
}

int object_alloc_slot(Interpreter *interp) {
	if (in_parallel) {
		int slot = local_claim_handle(&thread_alloc.objects, &arena.object_space);
		if (slot < 0) {
			fail(interp, "object table full in parallel region");
			return -1;
		}
		return slot;
	}

	if (main_alloc.objects.next < main_alloc.objects.end)
		return main_alloc.objects.next++;

	if (arena.object_space.n < arena.object_space.max) {
		int claim = arena.object_space.max - arena.object_space.n;
		if (claim > SLOTS_PER_CLAIM) 
			claim = SLOTS_PER_CLAIM;
		if (arena.object_space.n + claim > arena.object_space.cap) {
			int new_cap = arena.object_space.cap * 2;
			if (new_cap < arena.object_space.n + claim)
				new_cap = arena.object_space.n + claim;
			if (new_cap > arena.object_space.max)
				new_cap = arena.object_space.max;
			GROW_OBJECT_TABLE(new_cap);
		}
		
		main_alloc.objects.next = arena.object_space.n;
		arena.object_space.n += claim;
		main_alloc.objects.end = arena.object_space.n;

		return main_alloc.objects.next++;
	}

	if (arena.object_space.n_free > 0) {
		return arena.object_space.free[--arena.object_space.n_free];
	}

	if (interp->gc_disabled)
		return -1;

	gc(interp);

	if (arena.object_space.n_free > 0) {
		return arena.object_space.free[--arena.object_space.n_free];
	}

	return -1;
}

void reset_thread_alloc(void) {
	memset(&thread_alloc, 0, sizeof thread_alloc);
}

static void free_thread_alloc_lists(void) {
	free(thread_alloc.objects.free);
	free(thread_alloc.pairs.free);
	free(thread_alloc.objects.chunks);
	free(thread_alloc.pairs.chunks);
	memset(&thread_alloc, 0, sizeof thread_alloc);
}

void heap_bytes_add(size_t bytes) {
	atomic_fetch_add(&arena.heap_bytes_live, bytes);
	if (in_parallel)
		thread_alloc.heap_bytes_live += bytes;
}

static inline void heap_bytes_sub(size_t bytes) {
	atomic_fetch_sub(&arena.heap_bytes_live, bytes);
	if (in_parallel)
		thread_alloc.heap_bytes_live -= bytes;
}

void region_begin(ParallelRegion *region, int domain_len, int worker_count) {
	CLAMP(worker_count, 1, MAX_WORKER_THREADS);

	int object_headroom = arena.object_space.n + domain_len + worker_count * SLOTS_PER_CLAIM;
	object_headroom = MIN(object_headroom, arena.object_space.max);
	if (object_headroom > arena.object_space.cap)
		GROW_OBJECT_TABLE(object_headroom);

	int pair_headroom = pairs.space.n + domain_len + worker_count * SLOTS_PER_CLAIM;
	if (pair_headroom > pairs.space.cap)
		GROW_PAIR_TABLE(pair_headroom);

	region->used = arena.used;
	region->n_objects = arena.object_space.n;
	region->n_pairs = pairs.space.n;

	parallel_region_object_base = arena.object_space.n;
	parallel_region_pair_base = pairs.space.n;
	parallel_region_collected = 0;

	in_parallel = 1;
	reset_thread_alloc();
}

void region_commit(ParallelRegion *region) {
	(void)region;
	memset(&thread_alloc, 0, sizeof thread_alloc);
}

void region_abort(ParallelRegion *region) {
	int high = arena.object_space.n;
	for (int handle = region->n_objects; handle < high; handle++) {
		Object *obj = arena.objects[handle];
		if (!obj)
			continue;
		switch (obj->kind) {
			case OBJECT_MATRIX:
				heap_bytes_sub((size_t)obj->matrix.rows * (size_t)obj->matrix.columns * sizeof(double));
				free(obj->matrix.elements);
				break;
			case OBJECT_SEGMENT:
				heap_bytes_sub((size_t)obj->segment.length * segment_element_size(obj->segment.element_type));
				free(obj->segment.data);
				break;
			case OBJECT_CONTINUATION:
				heap_bytes_sub((size_t)obj->continuation.return_len * sizeof(Val));
				free(obj->continuation.return_slice);
				break;
			default:
				break;
		}
		arena.objects[handle] = NULL;
	}

	arena.used = region->used;
	arena.object_space.n = region->n_objects;
	pairs.space.n = region->n_pairs;
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
		slot = local_claim_handle(&thread_alloc.pairs, &pairs.space);
		if (slot < 0) {
			fail(interp, "pair table full in parallel region");
			return -1;
		}
		INIT_PAIR(slot);
		return slot;
	}

	if (pairs.space.n_free > 0) {
		slot = pairs.space.free[--pairs.space.n_free];
		INIT_PAIR(slot);
		return slot;
	}

	if (pairs.space.n == pairs.space.cap) {
		if (!interp->gc_disabled)
			gc(interp);
		if (pairs.space.n_free > 0) {
			slot = pairs.space.free[--pairs.space.n_free];
			INIT_PAIR(slot);
			return slot;
		}

		GROW_PAIR_TABLE(pairs.space.cap * 2);
	}

	slot = pairs.space.n++;
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
	if (in_parallel) {
		if (thread_alloc.heap_bytes_live > thread_alloc.heap_gc_threshold)
			interp->gc_pending = 1;
	} else if (!interp->gc_disabled && arena.heap_bytes_live > arena.heap_gc_threshold) {
		interp->gc_pending = 1;
	}

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

	heap_bytes_add(num_elements * sizeof(double));

	return slot;
}

int object_new_segment(Interpreter *interp, int length, SegmentType element_type) {
	if (in_parallel) {
		if (thread_alloc.heap_bytes_live > thread_alloc.heap_gc_threshold)
			interp->gc_pending = 1;
	} else if (!interp->gc_disabled && arena.heap_bytes_live > arena.heap_gc_threshold) {
		interp->gc_pending = 1;
	}
	
	NEW_OBJECT(obj, OBJECT_SEGMENT);

	obj->segment.element_type = element_type;
	obj->segment.length = length;
	size_t element_size = segment_element_size(element_type);
	obj->segment.data = calloc((size_t)(length > 0 ? length : 1), element_size);

	if (!obj->segment.data) {
		arena_free_object(obj);
		arena.objects[slot] = NULL;
		fail(interp, "segment: out of memory (%lld bytes)", (long long)length * (long long)element_size);
		return -1;
	}

	heap_bytes_add((size_t)length * element_size);

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
	
	heap_bytes_add((size_t)return_len * sizeof(Val));
	
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

	free_thread_alloc_lists();
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

	platform_thread_t threads[MAX_WORKER_THREADS];
	int created = 0;

	for (int worker = 1; worker < n_threads; worker++)
		if (platform_thread_create(&threads[created], worker_entry, &task) == 0)
			created++;

	worker_entry(&task);
	for (int worker = 0; worker < created; worker++)
		platform_thread_join(threads[worker]);
}

static int compare_double(double a, double b) {
	return a < b ? -1 : a > b ? 1 : 0;
}

int val_cmp_depth(Interpreter *interp, Val left, Val right, int depth) {
	if (depth > MAX_NESTING_DEPTH) {
		fail(interp, "compare: structure too deeply nested (cycle?)");
		return 0;
	}

	if (VAL_TAG(left) != VAL_TAG(right))
		return (int)VAL_TAG(left) - (int)VAL_TAG(right);

	switch (VAL_TAG(left)) {
		case T_FLOAT:
			return compare_double(VAL_NUMBER(left), VAL_NUMBER(right));
		case T_QUANTITY: {
							 int left_unit  = (int)pairs.table[VAL_DATA(left)].tail.bits;
							 int right_unit = (int)pairs.table[VAL_DATA(right)].tail.bits;
							 Val left_magnitude  = pairs.table[VAL_DATA(left)].head;
							 Val right_magnitude = pairs.table[VAL_DATA(right)].head;

							 double factor;
							 if (VAL_TAG(left_magnitude) == T_FLOAT && VAL_TAG(right_magnitude) == T_FLOAT
									 && unit_conversion(right_unit, left_unit, &factor))
								 return compare_double(VAL_NUMBER(left_magnitude), VAL_NUMBER(right_magnitude) * factor);

							 if (left_unit != right_unit)
								 return left_unit - right_unit;
							 return val_cmp_depth(interp, left_magnitude, right_magnitude, depth + 1);
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
		case T_SEGMENT: {
							Object *left_segment = OBJECT_AT(VAL_DATA(left));
							Object *right_segment = OBJECT_AT(VAL_DATA(right));

							
							if (left_segment->segment.element_type != right_segment->segment.element_type)
								return left_segment->segment.element_type - right_segment->segment.element_type;
							if (left_segment->segment.length != right_segment->segment.length)
								return left_segment->segment.length - right_segment->segment.length;

							for (int i = 0; i < left_segment->segment.length; i++) {
								double a = segment_get(left_segment, i);
								double b = segment_get(right_segment, i);
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

int double_cmp(const void *left, const void *right) {
	double a = *(const double *)left;
	double b = *(const double *)right;

	if (a < b) return -1;
	if (a > b) return 1;
	return 0;
}
	

void print_double(FILE *out, double number) {
	if (number == (double)(int64_t)number && number > -1e15 && number < 1e15)
		fprintf(out, "%lld", (long long)number);
	else
		fprintf(out, "%g", number);
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

void print_items(FILE *out, Interpreter *interp, Object *collection) {
	int length = collection->len;

	if (!print_truncate || length <= PRINT_FIRST + PRINT_LAST) {
		for (int i = 0; i < length; i++) {
			print_val(out, interp, collection->items[i]);
			putc(' ', out);
		}
	} else {
		for (int i = 0; i < PRINT_FIRST; i++) {
			print_val(out, interp, collection->items[i]);
			putc(' ', out);
		}
		fputs("... ", out);
		for (int i = length - PRINT_LAST; i < length; i++) {
			print_val(out, interp, collection->items[i]);
			putc(' ', out);
		}
	}
}

void print_corners(FILE *out, Object *matrix) {
	double *elements = matrix->matrix.elements;
	int n = matrix->matrix.rows * matrix->matrix.columns;

	if (!print_truncate || n <= PRINT_FIRST + PRINT_LAST) {
		for (int i = 0; i < n; i++) {
			putc(' ', out);
			print_double(out, elements[i]);
		}
	} else {
		for (int i = 0; i < PRINT_FIRST; i++) {
			putc(' ', out);
			print_double(out, elements[i]);
		}
		fputs(" ...", out);
		for (int i = n - PRINT_LAST; i < n; i++) {
			putc(' ', out);
			print_double(out, elements[i]);
		}
	}
}

#define MATRIX_DISP_FIRST_ROWS 5
#define MATRIX_DISP_LAST_ROWS 3
#define MATRIX_DISP_FIRST_COLS 5
#define MATRIX_DISP_LAST_COLS 3

void print_matrix_cell(FILE *out, double value) {
	fprintf(out, " %10.4g", value);
}

void print_matrix_grid(FILE *out, Object *m, int unit) {
	int rows = m->matrix.rows;
	int cols = m->matrix.columns;
	int rows_trunc = print_truncate
		&& rows > MATRIX_DISP_FIRST_ROWS + MATRIX_DISP_LAST_ROWS;
	int cols_trunc = print_truncate
		&& cols > MATRIX_DISP_FIRST_COLS + MATRIX_DISP_LAST_COLS;

	fprintf(out, "<matrix %dx%d>", rows, cols);
	if (unit > 0) {
		putc(' ', out);
		render_unit(out, unit);
	}
	putc('\n', out);

	for (int i = 0; i < rows; i++) {
		if (rows_trunc) {
			if (i == MATRIX_DISP_FIRST_ROWS)
				fputs(" ...\n", out);
			if (i >= MATRIX_DISP_FIRST_ROWS && i < rows - MATRIX_DISP_LAST_ROWS)
				continue;
		}

		for (int j = 0; j < cols; j++) {
			if (cols_trunc) {
				if (j == MATRIX_DISP_FIRST_COLS)
					fprintf(out, " %10s", "...");
				if (j >= MATRIX_DISP_FIRST_COLS && j < cols - MATRIX_DISP_LAST_COLS)
					continue;
			}
			print_matrix_cell(out, MAT(m, i, j));
		}
		putc('\n', out);
	}
}

static const char *logic_var_name(int id) {
	for (int cfa = vocab.latest_cfa; cfa != 0; cfa = (int)WORD_LINK(cfa)) {
		if ((cfa_handler)vocab.dict[cfa] != dovar)
			continue;
		Val stored;
		stored.bits = (uint64_t)vocab.dict[cfa + 1];
		if (VAL_TAG(stored) == T_LOGIC_VAR && (int)VAL_DATA(stored) == id)
			return &vocab.name_pool[WORD_NAME(cfa)];
	}
	return NULL;
}

static void print_logic_var(FILE *out, Interpreter *interp, Val var,
		void (*pr)(FILE *, Interpreter *, Val)) {
	Val resolved = deref(interp, var);
	if (VAL_TAG(resolved) == T_LOGIC_VAR) {
		int id = (int)VAL_DATA(resolved);
		const char *name = logic_var_name(id);
		if (name) fprintf(out, "?%s", name); else fprintf(out, "_%d", id);
		return;
	}
	const char *name = logic_var_name((int)VAL_DATA(var));
	if (name) fprintf(out, "%s=", name);
	pr(out, interp, resolved);
}

static Val quantity_display_magnitude(Val magnitude, int unit) {
	if (!unit_is_named(unit) && VAL_TAG(magnitude) == T_FLOAT)
		return make_float(VAL_NUMBER(magnitude) * unit_scale_value(unit));
	return magnitude;
}

void print_val(FILE *out, Interpreter *interp, Val value) {
	if (VAL_TAG(value) == T_LOGIC_VAR) { print_logic_var(out, interp, value, print_val); return; }
	value = deref(interp, value);
	switch (VAL_TAG(value)) {
		case T_NONE: fputs("null", out); break;
		case T_UNBOUND: fputs("_", out); break;
		case T_FLOAT: print_double(out, VAL_NUMBER(value)); break;
		case T_SYMBOL: fprintf(out, ":%s", &vocab.symbol_pool[VAL_DATA(value)]); break;
		case T_STRING: {
			Object *str = OBJECT_AT(VAL_DATA(value));
			if (print_depth > 0)
				fprintf(out, "\"%s\"", str->bytes);
			else
				fputs(str->bytes, out);
			break;
		}
		case T_SET:
					   print_depth_enter();
					   if (print_depth > MAX_NESTING_DEPTH) {
						   fputs("<...>", out);
					   } else {
						   fputs("< ", out);
						   print_items(out, interp, OBJECT_AT(VAL_DATA(value)));
						   putc('>', out);
					   }
					   print_depth_leave();
					   break;
		case T_ARRAY:
					   print_depth_enter();
					   if (print_depth > MAX_NESTING_DEPTH) {
						   fputs("[...]", out);
					   } else {
						   fputs("[ ", out);
						   print_items(out, interp, OBJECT_AT(VAL_DATA(value)));
						   putc(']', out);
					   }
					   print_depth_leave();
					   break;
		case T_PAIR: {
			print_depth_enter();
			if (print_depth > MAX_NESTING_DEPTH) {
				fputs("[(...)]", out);
			} else {
				fputs("[( ", out);
				Val cur = value;
				int count = 0;
				while (VAL_TAG(cur) == T_PAIR && count < LIST_PRINT_MAX) {
					Pair *pair = &pairs.table[VAL_DATA(cur)];
					print_val(out, interp, pair->head);
					putc(' ', out);
					cur = deref(interp, pair->tail);
					count++;
				}
				if (count == LIST_PRINT_MAX)
					fputs("... ", out);
				else {
					print_val(out, interp, cur);
					putc(' ', out);
				}
				fputs(")]", out);
			}
			print_depth_leave();
			break;
		}
		case T_XT: fprintf(out, "<xt %lld>", (long long)VAL_DATA(value)); break;
		case T_ADDR: fprintf(out, "<addr %lld>", (long long)VAL_DATA(value)); break;
		case T_STREAM: fprintf(out, "<stream %lld>", (long long)VAL_DATA(value)); break;
		case T_DB: fprintf(out, "<database %lld>", (long long)VAL_DATA(value)); break;
		case T_PTR: fprintf(out, "<ptr %lld>", (long long)VAL_DATA(value)); break;
		case T_SEGMENT: {
							Object *segment = OBJECT_AT(VAL_DATA(value));
							const char *name = "?";
							switch (segment->segment.element_type) {
								case SEGMENT_INT:    name = "int"; break;
								case SEGMENT_DOUBLE: name = "double"; break;
							}
							fprintf(out, "<%s-segment %d>", name, segment->segment.length);
							break;
						}
		case T_LOGIC_VAR: fprintf(out, "_%d", (int)VAL_DATA(value)); break;
		case T_MATRIX: {
						   Object *matrix = OBJECT_AT(VAL_DATA(value));
						   print_depth_enter();
						   fprintf(out, "<matrix %dx%d: ", matrix->matrix.rows, matrix->matrix.columns);
						   print_corners(out, matrix);
						   putc('>', out);
						   print_depth_leave();
						   break;
					   }
		case T_QUANTITY: {
							 int slot = (int)VAL_DATA(value);
							 int unit = (int)pairs.table[slot].tail.bits;
							 print_val(out, interp, quantity_display_magnitude(pairs.table[slot].head, unit));
							 putc(' ', out);
							 render_unit(out, unit);
							 break;
						 }
		case T_FRAME: {
						  Object *frame = OBJECT_AT(VAL_DATA(value));
						  print_depth_enter();
						  if (print_depth > MAX_NESTING_DEPTH) {
							  fputs("{...}", out);
						  } else {
							  fputs("{ ", out);
							  for (int i = 0; i < frame->len; i++) {
								  fprintf(out, ":%s ", &vocab.symbol_pool[frame->frame.keys[i]]);
								  print_val(out, interp, frame->frame.values[i]);
								  putc(' ', out);
							  }
							  putc('}', out);
						  }
						  print_depth_leave();
						  break;
					  }
		case T_MARK: {
						 int bracket = (int)VAL_DATA(value);
						 if (bracket == '(')
							 fputs("[(", out);
						 else
							 putc(bracket == '{' || bracket == '[' || bracket == '<' ? bracket : '?', out);
						 break;
					 }
		default: fputs("<?>", out); break;
	}
}

static int array_has_nested(Object *arr) {
	for (int i = 0; i < arr->len; i++)
		if (VAL_TAG(arr->items[i]) == T_ARRAY)
			return 1;
	return 0;
}

static void pp_value(FILE *out, Interpreter *interp, Val value, int indent) {
	if (VAL_TAG(value) != T_ARRAY) {
		print_val(out, interp, value);
		return;
	}
	Object *arr = OBJECT_AT(VAL_DATA(value));
	if (!array_has_nested(arr)) {
		print_val(out, interp, value);
		return;
	}

	int n = arr->len;
	int trunc = print_truncate && n > PRINT_FIRST + PRINT_LAST;
	int child_indent = indent + 2;
	fputs("[ ", out);
	print_depth_enter();
	int first = 1;
	for (int i = 0; i < n; i++) {
		if (trunc && i == PRINT_FIRST) {
			putc('\n', out);
			for (int s = 0; s < child_indent; s++)
				putc(' ', out);
			fputs("...", out);
		}
		if (trunc && i >= PRINT_FIRST && i < n - PRINT_LAST)
			continue;
		if (!first) {
			putc('\n', out);
			for (int s = 0; s < child_indent; s++)
				putc(' ', out);
		}
		first = 0;
		pp_value(out, interp, arr->items[i], child_indent);
	}
	print_depth_leave();
	fputs(" ]", out);
}

void pretty_print_array(FILE *out, Interpreter *interp, Val value) {
	Object *arr = OBJECT_AT(VAL_DATA(value));
	if (!array_has_nested(arr)) {
		print_val(out, interp, value);
		return;
	}
	pp_value(out, interp, value, 0);
}

void print_val_inspect(FILE *out, Interpreter *interp, Val value) {
	print_depth_enter();
	print_val(out, interp, value);
	print_depth_leave();
}

void print_val_compact(FILE *out, Interpreter *interp, Val value) {
	if (VAL_TAG(value) == T_LOGIC_VAR) { print_logic_var(out, interp, value, print_val_compact); return; }
	value = deref(interp, value);
	switch (VAL_TAG(value)) {
		case T_NONE: fputs("null", out); break;
		case T_UNBOUND: fputs("_", out); break;
		case T_FLOAT: {
						  double number = VAL_NUMBER(value);
						  if (number == (double)(int64_t)number && number > -1e12 && number < 1e12)
							  fprintf(out, "%lld", (long long)number);
						  else
							  fprintf(out, "%.4g", number);
						  break;
					  }
		case T_STRING: {
						   Object *obj = OBJECT_AT(VAL_DATA(value));
						   if (obj->len <= 10)
						   	fprintf(out, "\"%.*s\"", obj->len, obj->bytes);
						   else
						   	fprintf(out, "\"%.9s…\"", obj->bytes);
						   break;
					   }
		case T_SYMBOL: {
						   const char *name = &vocab.symbol_pool[VAL_DATA(value)];
						   int len = (int)strlen(name);
						   if (len <= 10)
						   	fprintf(out, ":%s", name);
						   else
						   	fprintf(out, ":%.9s…", name);
						   break;
					   }
		case T_SET:
					   print_depth_enter();
					   fprintf(out, "<%d>", OBJECT_AT(VAL_DATA(value))->len);
					   print_depth_leave();
					   break;
		case T_ARRAY:
					   print_depth_enter();
					   fprintf(out, "[%d]", OBJECT_AT(VAL_DATA(value))->len);
					   print_depth_leave();
					   break;
		case T_PAIR:
					   fputs("[(…)]", out);
					   break;
		case T_FRAME:
					   print_depth_enter();
					   fprintf(out, "{%d}", OBJECT_AT(VAL_DATA(value))->len);
					   print_depth_leave();
					   break;
		case T_MATRIX: {
						   Object *m = OBJECT_AT(VAL_DATA(value));
						   print_depth_enter();
						   fprintf(out, "M%dx%d", m->matrix.rows, m->matrix.columns);
						   print_depth_leave();
						   break;
					   }
		case T_QUANTITY: {
							 int slot = (int)VAL_DATA(value);
							 int unit = (int)pairs.table[slot].tail.bits;
							 print_val_compact(out, interp, quantity_display_magnitude(pairs.table[slot].head, unit));
							 putc(' ', out);
							 render_unit(out, unit);
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
						   	fprintf(out, "'%s", name);
						   else
						   	fprintf(out, "'%.8s…", name);
					   } else {
						   fputs("'?", out);
					   }
					   break;
				   }
		case T_ADDR: fprintf(out, "@%lld", (long long)VAL_DATA(value)); break;
		case T_PTR: fprintf(out, "<ptr %lld>", (long long)VAL_DATA(value)); break;
		case T_SEGMENT: {
							Object *segment = OBJECT_AT(VAL_DATA(value));
							char element = '?';
							switch (segment->segment.element_type) {
								case SEGMENT_INT:    element = 'I'; break;
								case SEGMENT_DOUBLE: element = 'D'; break;
							}
							fprintf(out, "*%c%d", element, segment->segment.length);
							break;
						}
		case T_CONT: fputs("k", out); break;
		case T_LOGIC_VAR: fprintf(out, "_%d", (int)VAL_DATA(value)); break;
		case T_MARK: {
						 int bracket = (int)VAL_DATA(value);
						 if (bracket == '(')
							 fputs("[(", out);
						 else
							 putc(bracket == '{' || bracket == '[' || bracket == '<' ? bracket : '?', out);
						 break;
					 }
		default: fputs("?", out); break;
	}
}

void print_frame_pretty(FILE *out, Interpreter *interp, Object *frame, int indent) {
	if (indent > 2 * MAX_NESTING_DEPTH) {
		fputs("{...}", out);
		return;
	}
	fputs("{\n", out);
	for (int i = 0; i < frame->len; i++) {
		for (int s = 0; s < indent + 2; s++)
			putc(' ', out);
		fprintf(out, ":%s ", &vocab.symbol_pool[frame->frame.keys[i]]);
		Val value = frame->frame.values[i];
		if (VAL_TAG(value) == T_FRAME)
			print_frame_pretty(out, interp, OBJECT_AT(VAL_DATA(value)), indent + 2);
		else
			print_val(out, interp, value);
		putc('\n', out);
	}
	for (int s = 0; s < indent; s++)
		putc(' ', out);
	putc('}', out);
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
		print_val_compact(stdout, interp, interp->data_stack[interp->dsp - 1]);
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

void docol(DISPATCH_ARGS) {
	if (interp->rsp >= RETURN_STACK_DEPTH) {
		SYNC_REGISTERS(interp, chain_ip + 1, chain_sp);
		fail(interp, "return stack overflow");
		return;
	}

	interp->return_stack[interp->rsp++] = make_addr((int)(chain_ip + 1 - vocab.dict));
	
	DISPATCH_REGISTERS(interp, vocab.dict + (int)*chain_ip + 1, chain_sp);
}

void dosym(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip + 1, chain_sp, 1);
	*chain_sp = make_symbol((int)vocab.dict[(int)*chain_ip + 1]);

	DISPATCH_REGISTERS(interp, chain_ip + 1, chain_sp + 1);
}

void dovar(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip + 1, chain_sp, 1);
	chain_sp->bits = (uint64_t)vocab.dict[(int)*chain_ip + 1];

	DISPATCH_REGISTERS(interp, chain_ip + 1, chain_sp + 1);
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

		if (interp->gc_pending) {
			interp->gc_pending = 0;
			if (in_parallel)
				worker_local_gc(interp);
			else if (!interp->gc_disabled)
				gc(interp);
		}

		cfa_handler handler = (cfa_handler)vocab.dict[interp->ip++];
		handler(interp, vocab.dict + interp->ip, interp->data_stack + interp->dsp);
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

	if (handler == dounit) {
		apply_unit(interp, cfa);
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

void call_open(Interpreter *interp, int cfa, CallContext *context) {
	cfa_handler handler = (cfa_handler)vocab.dict[cfa];

	context->reuses_locals = 0;

	if (handler == dovar || handler == dosym || handler == dounit) {
		context->fast = 0;
		return;
	}

	context->fast = 1;
	context->saved_ip = interp->ip;
	context->saved_running = interp->running;
	context->saved_slot_0 = vocab.dict[interp->trampoline_base];
	context->saved_slot_1 = vocab.dict[interp->trampoline_base + 1];
	context->saved_slot_2 = vocab.dict[interp->trampoline_base + 2];

	context->saved_loop_body_start = interp->loop_body_start;
	context->saved_loop_n = interp->loop_n;
	context->saved_loop_slots_ip = interp->loop_slots_ip;
	interp->loop_body_start = 0;
	interp->loop_slots_ip = -1;

	cell stop_handler = vocab.dict[vocab.stop_cfa];
	if (handler == docol) {
		vocab.dict[interp->trampoline_base] = (cell)docol;
		vocab.dict[interp->trampoline_base + 1] = (cell)cfa;
		vocab.dict[interp->trampoline_base + 2] = stop_handler;

		int n_locals = 0, n_received = 0, slots_ip = -1, body_start = 0;
		cfa_handler enter = (cfa_handler)vocab.dict[cfa + 1];
		n_locals = (int)vocab.dict[cfa + 2];
		
		if (enter == p_enter_locals_to) {
			n_received = n_locals;
			body_start = cfa + 3;
		} else if (enter == p_enter_locals_mixed) {
			n_received = (int)vocab.dict[cfa + 3];
			slots_ip = cfa + 4;
			body_start = cfa + 4 + n_received;
		}

		if (body_start && interp->rsp + n_locals + 1 <= RETURN_STACK_DEPTH) {
			interp->return_stack[interp->rsp++] = make_locals_header(interp->local_base, n_locals);

			context->saved_loop_local_base = interp->loop_local_base;
			interp->local_base = interp->rsp;
			interp->loop_local_base = interp->rsp;
			interp->rsp += n_locals;
			context->reuses_locals = 1;

			interp->loop_n = n_received;
			interp->loop_slots_ip = slots_ip;
			context->leave_ip = 0;

			if (dict_op_is(cfa - 2, (cfa_handler)vocab.dict[vocab.branch_cfa])) {
				int leave_ip = cfa + (int)vocab.dict[cfa - 1] - 4;
				if (dict_op_is(leave_ip, p_leave_locals)) {
					context->leave_ip = leave_ip;
					context->saved_leave = vocab.dict[leave_ip];
					vocab.dict[leave_ip] = stop_handler;
					interp->loop_body_start = body_start;
				}
			}
		}

	} else {
		vocab.dict[interp->trampoline_base] = (cell)handler;
		vocab.dict[interp->trampoline_base + 1] = stop_handler;
		vocab.dict[interp->trampoline_base + 2] = stop_handler;
	}
}

static void dispatch_body(Interpreter *interp, int body_start) {
	if (interp->call_depth >= MAX_CALL_DEPTH) {
		fail(interp, "call stack too deep (runaway recursion via execute/resume/amb?)");
		return;
	}
	interp->call_depth++;
	int saved_floor = interp->run_floor;
	interp->run_floor = interp->rsp;

	interp->running = 1;
	interp->ip = body_start + 1;
	((cfa_handler)vocab.dict[body_start])(interp, vocab.dict + body_start + 1, interp->data_stack + interp->dsp);

	if (interp->running && !interp->error_flag)
		run_inner(interp, interp->run_floor);

	interp->run_floor = saved_floor;
	interp->call_depth--;
}

void execute_xt(Interpreter *interp, int cfa) {
	if ((cfa_handler)vocab.dict[cfa] != docol) {
		execute_cfa(interp, cfa);
		return;
	}
	if (interp->call_depth >= MAX_CALL_DEPTH) {
		fail(interp, "call stack too deep (runaway recursion via execute/resume/amb?)");
		return;
	}
	interp->call_depth++;

	int saved_ip = interp->ip;
	int saved_running = interp->running;
	int saved_floor = interp->run_floor;
	interp->run_floor = interp->rsp;

	/* The frame the trampoline's docol would push, but aimed at the immortal
	   stop cell — continuations captured inside the body must see the same
	   return-stack shape either way. */
	rpush(interp, make_addr(vocab.stop_cfa));
	if (interp->error_flag) {
		interp->run_floor = saved_floor;
		interp->call_depth--;
		return;
	}

	interp->running = 1;
	interp->ip = cfa + 2;
	((cfa_handler)vocab.dict[cfa + 1])(interp, vocab.dict + cfa + 2, interp->data_stack + interp->dsp);

	if (interp->running && !interp->error_flag)
		run_inner(interp, interp->run_floor);

	interp->run_floor = saved_floor;
	interp->call_depth--;
	interp->ip = saved_ip;
	interp->running = saved_running;
}

void call_invoke(Interpreter *interp) {
	if (interp->loop_body_start) {
		interp->loop_local_refill = 0;

		int n = interp->loop_n;
		int base = interp->loop_local_base;
		int data_start = interp->dsp - n;

		if (interp->loop_slots_ip < 0) {
			for (int i = 0; i < n; i++)
				interp->return_stack[base + i] = interp->data_stack[data_start + i];
		} else {
			int slots_ip = interp->loop_slots_ip;
			for (int i = 0; i < n; i++)
				interp->return_stack[base + (int)vocab.dict[slots_ip + i]] = interp->data_stack[data_start + i];
		}

		interp->dsp -= n;
		dispatch_body(interp, interp->loop_body_start);
		return;
	}

	dispatch_body(interp, interp->trampoline_base);
}

void call_close(Interpreter *interp, CallContext *context) {
	if (!context->fast)
		return;
	interp->running = context->saved_running;
	interp->ip = context->saved_ip;
	vocab.dict[interp->trampoline_base] = context->saved_slot_0;
	vocab.dict[interp->trampoline_base + 1] = context->saved_slot_1;
	vocab.dict[interp->trampoline_base + 2] = context->saved_slot_2;

	interp->loop_body_start = context->saved_loop_body_start;
	interp->loop_n = context->saved_loop_n;
	interp->loop_slots_ip = context->saved_loop_slots_ip;


	if (context->reuses_locals) {
		if (context->leave_ip)
			vocab.dict[context->leave_ip] = context->saved_leave;

		Val locals_header = interp->return_stack[interp->loop_local_base - 1];
		interp->rsp = interp->loop_local_base - 1;
		interp->local_base = saved_local_base(locals_header);
		interp->loop_local_base = context->saved_loop_local_base;
	}
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
		platform_mutex_lock(&intern_lock);

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
		platform_mutex_unlock(&intern_lock);
	return symbol_offset;
}

void dict_ensure(Interpreter *interp, int extra) {
	(void)interp;
	if (vocab.here + extra > VOCABULARY_INIT_SIZE) {
		fprintf(stderr, "water: dictionary full\n");
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
	dict_is_handler[vocab.here] = 0;
	vocab.dict[vocab.here++] = value;
	compiler.fuse_prev_cmp = 0;
}

void emit_call(Interpreter *interp, int target_cfa) {
	cfa_handler handler = (cfa_handler)vocab.dict[target_cfa];
	emit(interp, (cell)handler);
	dict_is_handler[vocab.here - 1] = 1;

	if (handler == docol || handler == dovar || handler == dosym || handler == dounit) {
		emit(interp, (cell)target_cfa);
	}
}

void emit_val_literal(Interpreter *interp, Val value) {
	emit_call(interp, vocab.literal_cfa);
	emit(interp, (cell)value.bits);
}

static const QuotationSpan *quotation_span_containing(int addr) {
	const QuotationSpan *innermost = NULL;
	for (int i = 0; i < vocab.n_quotation_spans; i++) {
		const QuotationSpan *span = &vocab.quotation_spans[i];
		if (addr >= span->start_cfa && addr < span->end_cfa
				&& (!innermost || span->start_cfa > innermost->start_cfa))
			innermost = span;
	}
	return innermost;
}

static int word_containing(int addr) {
	for (int cfa = vocab.latest_cfa; cfa != 0; cfa = (int)WORD_LINK(cfa))
		if (cfa <= addr)
			return cfa;
	return 0;
}

static void trace_write(Interpreter *interp, int *len, const char *text) {
	while (*text && *len < ERROR_TRACE_SIZE - 1)
		interp->error_trace[(*len)++] = *text++;
	interp->error_trace[*len] = 0;
}

static void trace_write_snippet(Interpreter *interp, int *len, int source_offset) {
	if (source_offset == 0) {
		trace_write(interp, len, "[:?]");
		return;
	}
	char cleaned[TRACE_SNIPPET_MAX + 8];
	int n = 0;
	int pending_space = 0;
	const char *c = &vocab.source_pool[source_offset];
	for (; *c && n < TRACE_SNIPPET_MAX - 1; c++) {
		if (isspace((unsigned char)*c)) {
			pending_space = 1;
			continue;
		}
		if (pending_space && n > 0)
			cleaned[n++] = ' ';
		pending_space = 0;
		cleaned[n++] = *c;
	}
	if (*c) {
		while (n > 0 && ((unsigned char)cleaned[n - 1] & 0xC0) == 0x80)
			n--;
		if (n > 0 && (unsigned char)cleaned[n - 1] >= 0xC0)
			n--;
		memcpy(&cleaned[n], "… :]", 6);
		n += 6;
	}
	cleaned[n] = 0;
	trace_write(interp, len, cleaned);
}

typedef struct {
	int addr;
	int repeats;
	const QuotationSpan *span;
	int cfa;
} TraceFrame;

static void capture_error_trace(Interpreter *interp) {
	interp->error_trace[0] = 0;

	TraceFrame frames[TRACE_FRAMES_MAX];
	int n_frames = 0;
	int dropped = 0;
	int scope_base = interp->local_base;
	int scope_top = scope_base > 0
		? scope_base + saved_n_locals(interp->return_stack[scope_base - 1]) : -1;

	if (interp->running && interp->ip >= DICT_RESERVED) {
		frames[n_frames].addr = interp->ip;
		frames[n_frames++].repeats = 1;
	}
	for (int i = interp->rsp - 1; i >= 0; i--) {
		if (scope_base > 0 && i >= scope_base && i < scope_top)
			continue;
		if (scope_base > 0 && i == scope_base - 1) {
			scope_base = saved_local_base(interp->return_stack[i]);
			scope_top = scope_base > 0
				? scope_base + saved_n_locals(interp->return_stack[scope_base - 1]) : -1;
			continue;
		}
		Val entry = interp->return_stack[i];
		if (VAL_TAG(entry) != T_ADDR)
			continue;
		int addr = (int)VAL_DATA(entry);
		if (addr < DICT_RESERVED || addr == vocab.stop_cfa)
			continue;
		if (n_frames > 0 && frames[n_frames - 1].addr == addr) {
			frames[n_frames - 1].repeats++;
			continue;
		}
		if (n_frames >= TRACE_FRAMES_MAX) {
			dropped++;
			continue;
		}
		frames[n_frames].addr = addr;
		frames[n_frames++].repeats = 1;
	}

	int n_merged = 0;
	for (int i = 0; i < n_frames; i++) {
		const QuotationSpan *span = quotation_span_containing(frames[i].addr);
		int cfa = span ? 0 : word_containing(frames[i].addr);
		if (!span && !cfa)
			continue;
		if (n_merged > 0 && frames[n_merged - 1].span == span
				&& frames[n_merged - 1].cfa == cfa) {
			frames[n_merged - 1].repeats += frames[i].repeats;
			continue;
		}
		frames[n_merged].addr = frames[i].addr;
		frames[n_merged].repeats = frames[i].repeats;
		frames[n_merged].span = span;
		frames[n_merged++].cfa = cfa;
	}

	int len = 0;
	for (int i = 0; i < n_merged; i++) {
		if (n_merged > TRACE_FRAMES_FIRST + TRACE_FRAMES_LAST + 1
				&& i == TRACE_FRAMES_FIRST) {
			char skipped[32];
			snprintf(skipped, sizeof(skipped), " ← …+%d",
					n_merged - TRACE_FRAMES_FIRST - TRACE_FRAMES_LAST + dropped);
			trace_write(interp, &len, skipped);
			i = n_merged - TRACE_FRAMES_LAST - 1;
			continue;
		}
		trace_write(interp, &len, i == 0 ? "in " : " ← ");
		if (frames[i].span)
			trace_write_snippet(interp, &len, frames[i].span->source_offset);
		else
			trace_write(interp, &len, &vocab.name_pool[WORD_NAME(frames[i].cfa)]);
		if (frames[i].repeats > 1) {
			char multiple[16];
			snprintf(multiple, sizeof(multiple), " ×%d", frames[i].repeats);
			trace_write(interp, &len, multiple);
		}
	}
}

void fail(Interpreter *interp, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vsnprintf(interp->error_message, sizeof(interp->error_message), fmt, args);
	va_end(args);
	interp->error_flag = 1;
	capture_error_trace(interp);
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
		case T_PTR: return "a pointer";
		case T_SEGMENT: return "a segment";
		case T_QUANTITY: return "a quantity";
		default:       return "an unknown value";
	}
}

void p_exit(DISPATCH_ARGS) {
	while (interp->rsp > 0 && VAL_TAG(interp->return_stack[interp->rsp - 1]) == T_MARK) 
		interp->rsp--;


	unwind_locals_scopes(interp);

	if (interp->rsp <= interp->run_floor) {
		interp->running = 0;
		return;
	}

	Val saved_ip = interp->return_stack[--interp->rsp];

	DISPATCH_REGISTERS(interp, vocab.dict + (int)VAL_DATA(saved_ip), chain_sp);
}

void p_stop(DISPATCH_ARGS) {
	interp->running = 0;
}

void p_alloc_stats(DISPATCH_ARGS) {
	printf("lvars=%ld arrays=%ld\n", alloc_count_lvar, alloc_count_array);
	alloc_count_lvar = 0;
	alloc_count_array = 0;

	DISPATCH(interp);
}

void p_literal(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip + 1, chain_sp, 1);
	chain_sp->bits = (uint64_t)*chain_ip;
	DISPATCH_REGISTERS(interp, chain_ip + 1, chain_sp + 1);
}

void p_branch(DISPATCH_ARGS) {
	cell *branch_target = chain_ip + (int)*chain_ip;

	DISPATCH_REGISTERS(interp, branch_target, chain_sp);
}

static inline int zbranch_falsy(Val condition) {
	return (VAL_TAG(condition) == T_FLOAT) ? (VAL_NUMBER(condition) == 0.0)
		: (VAL_DATA(condition) == 0);
}

void p_0branch(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip + 1, chain_sp, 1);
	cell *continue_ip = zbranch_falsy(chain_sp[-1]) ? chain_ip + (int)*chain_ip : chain_ip + 1;

	DISPATCH_REGISTERS(interp, continue_ip, chain_sp - 1);
}

void p_qzbranch(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH_MSG(interp, chain_ip + 1, chain_sp, 1, "?if: stack too shallow");
	cell *continue_ip = zbranch_falsy(chain_sp[-1]) ? chain_ip + (int)*chain_ip : chain_ip + 1;

	DISPATCH_REGISTERS(interp, continue_ip, chain_sp);
}

void p_dostr(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip + 1, chain_sp, 1);
	SYNC_REGISTERS(interp, chain_ip + 1, chain_sp);
	int interpolated = interpolate(interp, (int)*chain_ip);
	if (interp->error_flag)
		return;
	*chain_sp = make_string(interpolated);

	DISPATCH_REGISTERS(interp, chain_ip + 1, chain_sp + 1);
}

void p_enter_locals(DISPATCH_ARGS) {
	int n_locals = (int)chain_ip[0];
	if (interp->rsp + n_locals + 1 > RETURN_STACK_DEPTH) {
		SYNC_REGISTERS(interp, chain_ip + 1, chain_sp);
		fail(interp, "return stack overflow");
		return;
	}
	interp->return_stack[interp->rsp++] = make_locals_header(interp->local_base, n_locals);
	interp->rsp += n_locals;
	interp->local_base = interp->rsp - n_locals;

	DISPATCH_REGISTERS(interp, chain_ip + 1, chain_sp);
}

void p_enter_locals_to(DISPATCH_ARGS) {
	int n_locals = (int)chain_ip[0];

	if (interp->loop_local_refill) {
		interp->loop_local_refill = 0;
		Val *incoming = chain_sp - n_locals;
		for (int i = 0; i < n_locals; i++)
			interp->return_stack[interp->local_base + i] = incoming[i];

		DISPATCH_REGISTERS(interp, chain_ip + 1, incoming);
	}

	if (interp->rsp + n_locals + 1 > RETURN_STACK_DEPTH) {
		SYNC_REGISTERS(interp, chain_ip + 1, chain_sp);
		fail(interp, "(enter-locals-to): return stack overflow");
		return;
	}
	if (chain_sp - n_locals < interp->data_stack) {
		SYNC_REGISTERS(interp, chain_ip + 1, chain_sp);
		fail(interp, "(enter-locals-to): insufficient values on data stack; need %d", n_locals);
		return;
	}

	interp->return_stack[interp->rsp++] = make_locals_header(interp->local_base, n_locals);
	Val *incoming = chain_sp - n_locals;
	for (int i = 0; i < n_locals; i++)
		interp->return_stack[interp->rsp + i] = incoming[i];

	interp->local_base = interp->rsp;
	interp->rsp += n_locals;

	DISPATCH_REGISTERS(interp, chain_ip + 1, incoming);
}

void p_enter_locals_mixed(DISPATCH_ARGS) {
	int n_locals = (int)chain_ip[0];
	int n_received = (int)chain_ip[1];

	if (interp->loop_local_refill) {
		interp->loop_local_refill = 0;
		Val *incoming = chain_sp - n_received;

		for (int i = 0; i < n_received; i++)
			interp->return_stack[interp->local_base + (int)chain_ip[2 + i]] = incoming[i];

		DISPATCH_REGISTERS(interp, chain_ip + 2 + n_received, incoming);
	}

	if (interp->rsp + n_locals + 1 > RETURN_STACK_DEPTH) {
		SYNC_REGISTERS(interp, chain_ip + 2 + n_received, chain_sp);
		fail(interp, "(enter-locals-mixed): return stack overflow");
		return;
	}
	if (chain_sp - n_received < interp->data_stack) {
		SYNC_REGISTERS(interp, chain_ip + 2 + n_received, chain_sp);
		fail(interp, "(enter-locals-mixed): insufficient values on data stack; need %d", n_received);
		return;
	}

	interp->return_stack[interp->rsp++] = make_locals_header(interp->local_base, n_locals);
	interp->local_base = interp->rsp;
	interp->rsp += n_locals;

	Val *incoming = chain_sp - n_received;
	for (int i = 0; i < n_received; i++)
		interp->return_stack[interp->local_base + (int)chain_ip[2 + i]] = incoming[i];

	DISPATCH_REGISTERS(interp, chain_ip + 2 + n_received, incoming);
}

void p_leave_locals(DISPATCH_ARGS) {
	int n_locals = (int)chain_ip[0];

	if (interp->local_base == interp->loop_local_base) {
		DISPATCH_REGISTERS(interp, chain_ip + 1, chain_sp);
	}

	interp->rsp -= n_locals;
	if (interp->rsp <= 0) {
		SYNC_REGISTERS(interp, chain_ip + 1, chain_sp);
		fail(interp, "return stack underflow");
		return;
	}
	Val locals_header = interp->return_stack[--interp->rsp];
	interp->local_base = saved_local_base(locals_header);

	DISPATCH_REGISTERS(interp, chain_ip + 1, chain_sp);
}

static Val *local_slot(Interpreter *interp, cell *chain_ip) {
	int depth = (int)chain_ip[0];
	int slot  = (int)chain_ip[1];

	int base = interp->local_base;
	for (int i = 0; i < depth; i++)
		base = saved_local_base(interp->return_stack[base - 1]);

	return &interp->return_stack[base + slot];
}

void p_local_fetch(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip + 2, chain_sp, 1);
	*chain_sp = *local_slot(interp, chain_ip);

	DISPATCH_REGISTERS(interp, chain_ip + 2, chain_sp + 1);
}

void p_local_fetch_1depth(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip + 1, chain_sp, 1);
	int base = saved_local_base(interp->return_stack[interp->local_base - 1]);
	*chain_sp = interp->return_stack[base + (int)chain_ip[0]];

	DISPATCH_REGISTERS(interp, chain_ip + 1, chain_sp + 1);
}

void p_local_store(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip + 2, chain_sp, 1);
	*local_slot(interp, chain_ip) = chain_sp[-1];

	DISPATCH_REGISTERS(interp, chain_ip + 2, chain_sp - 1);
}

void p_local_fetch_0depth(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip + 1, chain_sp, 1);
	*chain_sp = interp->return_stack[interp->local_base + (int)chain_ip[0]];

	DISPATCH_REGISTERS(interp, chain_ip + 1, chain_sp + 1);
}

void p_load2(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip + 2, chain_sp, 2);
	
	Val *locals = interp->return_stack + interp->local_base;
	chain_sp[0] = locals[(int)chain_ip[0]];
	chain_sp[1] = locals[(int)chain_ip[1]];

	DISPATCH_REGISTERS(interp, chain_ip + 2, chain_sp + 2);
}


void p_load3(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip + 3, chain_sp, 3);
	Val *locals = interp->return_stack + interp->local_base;
	chain_sp[0] = locals[(int)chain_ip[0]];
	chain_sp[1] = locals[(int)chain_ip[1]];
	chain_sp[2] = locals[(int)chain_ip[2]];

	DISPATCH_REGISTERS(interp, chain_ip + 3, chain_sp + 3);
}

void p_local_store_0depth(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip + 1, chain_sp, 1);
	interp->return_stack[interp->local_base + (int)chain_ip[0]] = chain_sp[-1];

	DISPATCH_REGISTERS(interp, chain_ip + 1, chain_sp - 1);
}

#define LOCAL_ARITH_0DEPTH(name, word_name, expr) \
	void name(DISPATCH_ARGS) { \
		Val *p = &interp->return_stack[interp->local_base + (int)chain_ip[0]]; \
		if (VAL_TAG(*p) != T_FLOAT) { \
			SYNC_REGISTERS(interp, chain_ip + 1, chain_sp); \
			fail(interp, word_name ": expected a float local; got %s", tag_name(VAL_TAG(*p))); \
			return; \
		} \
		double n = VAL_NUMBER(*p); \
		*p = make_float(expr); \
		DISPATCH_REGISTERS(interp, chain_ip + 1, chain_sp); \
	}
LOCAL_ARITH_0DEPTH(p_local_incr_0depth, "(local+!)", n + 1.0)
LOCAL_ARITH_0DEPTH(p_local_decr_0depth, "(local-!)", n - 1.0)

#define UNSAFE_LOCAL_ARITH_0DEPTH(name, expr) \
	void name(DISPATCH_ARGS) { \
		Val *p = &interp->return_stack[interp->local_base + (int)chain_ip[0]]; \
		double n = p->number; \
		p->number = (expr); \
		DISPATCH_REGISTERS(interp, chain_ip + 1, chain_sp); \
	}
UNSAFE_LOCAL_ARITH_0DEPTH(p_local_finc_0depth, n + 1.0)
UNSAFE_LOCAL_ARITH_0DEPTH(p_local_fdec_0depth, n - 1.0)

#define LOCAL_ACC_OP(suffix, op) \
	static int local_acc_##suffix##_0_cfa; \
	static int local_acc_##suffix##_cfa; \
	static void p_local_acc_##suffix##_0(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip + 1, chain_sp, 1); \
		Val *p = &interp->return_stack[interp->local_base + (int)chain_ip[0]]; \
		p->number = chain_sp[-1].number op p->number; \
		DISPATCH_REGISTERS(interp, chain_ip + 1, chain_sp - 1); \
	} \
	static void p_local_acc_##suffix(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip + 2, chain_sp, 1); \
		int base = interp->local_base; \
		for (int i = 0; i < (int)chain_ip[0]; i++) \
			base = saved_local_base(interp->return_stack[base - 1]); \
		Val *p = &interp->return_stack[base + (int)chain_ip[1]]; \
		p->number = chain_sp[-1].number op p->number; \
		DISPATCH_REGISTERS(interp, chain_ip + 2, chain_sp - 1); \
	}
LOCAL_ACC_OP(add, +)
LOCAL_ACC_OP(sub, -)
LOCAL_ACC_OP(mul, *)
LOCAL_ACC_OP(div, /)

#define LOCAL_LOCAL_OP(suffix, op) \
	static int ll_##suffix##_0_cfa; \
	static void p_ll_##suffix##_0(DISPATCH_ARGS) { \
		REQUIRE_STACK_ROOM(interp, chain_ip + 2, chain_sp, 1); \
		Val *locals = interp->return_stack + interp->local_base; \
		double a = locals[(int)chain_ip[0]].number; \
		double b = locals[(int)chain_ip[1]].number; \
		*chain_sp = make_float(a op b); \
		DISPATCH_REGISTERS(interp, chain_ip + 2, chain_sp + 1); \
	}
LOCAL_LOCAL_OP(add, +)
LOCAL_LOCAL_OP(sub, -)
LOCAL_LOCAL_OP(mul, *)

#define LOCAL_LIT_OP(suffix, op) \
	static int ll_lit_##suffix##_0_cfa; \
	static void p_ll_lit_##suffix##_0(DISPATCH_ARGS) { \
		REQUIRE_STACK_ROOM(interp, chain_ip + 2, chain_sp, 1); \
		Val lit; \
		lit.bits = (uint64_t)chain_ip[1]; \
		double a = interp->return_stack[interp->local_base + (int)chain_ip[0]].number; \
		*chain_sp = make_float(a op lit.number); \
		DISPATCH_REGISTERS(interp, chain_ip + 2, chain_sp + 1); \
	}
LOCAL_LIT_OP(add, +)
LOCAL_LIT_OP(sub, -)
LOCAL_LIT_OP(mul, *)

static int ll_litrev_sub_0_cfa;
static void p_ll_litrev_sub_0(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip + 2, chain_sp, 1);
	Val lit;
	lit.bits = (uint64_t)chain_ip[1];
	double a = interp->return_stack[interp->local_base + (int)chain_ip[0]].number;
	*chain_sp = make_float(lit.number - a);
	DISPATCH_REGISTERS(interp, chain_ip + 2, chain_sp + 1);
}

static int at_i_local0_cfa;
static int at_i_lit_cfa;
static int at_i_lit_local0_cfa;
static int gather_local0_cfa;
static int at_i_ll0_cfa;
static int at_i_l1l0_cfa;
static int load2_cfa, load3_cfa;

int try_fuse_local_acc(Interpreter *interp, int depth, int slot) {
	cell *dict = vocab.dict;
	int here = vocab.here;
	if (here < 1)
		return 0;

	if (!dict_is_handler[here - 1])
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
		if (here - 3 < compiler.fuse_floor)
			return 0;
		if (!dict_op_is(here - 3, p_local_fetch_0depth))
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
	if (here - 4 < compiler.fuse_floor)
		return 0;
	if (!dict_op_is(here - 4, p_local_fetch))
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

int try_fuse_at_i_local(Interpreter *interp) {
	if (!compiler.compiling)
		return 0;

	cell *dict = vocab.dict;
	int here = vocab.here;
	if (here < 2)
		return 0;
	if (here - 2 < compiler.fuse_floor)
		return 0;
	if (!dict_op_is(here - 2, p_local_fetch_0depth))
		return 0;

	int slot = (int)dict[here - 1];
	vocab.here -= 2;
	emit_call(interp, at_i_local0_cfa);
	emit(interp, (cell)slot);

	return 1;
}

int try_fuse_gather_local(Interpreter *interp) {
	if (!compiler.compiling)
		return 0;

	cell *dict = vocab.dict;
	int here = vocab.here;
	if (here < 2)
		return 0;
	if (here - 2 < compiler.fuse_floor)
		return 0;
	if (!dict_op_is(here - 2, p_at_i_local0))
		return 0;

	int slot = (int)dict[here - 1];
	vocab.here -= 2;
	emit_call(interp, gather_local0_cfa);
	emit(interp, (cell)slot);

	return 1;
}

int try_fuse_at_i_ll(Interpreter *interp) {
	if (!compiler.compiling)
		return 0;

	cell *dict = vocab.dict;
	int here = vocab.here;

	if (here >= 3 && here - 3 >= compiler.fuse_floor
	    && here - 3 == compiler.loadn_at
	    && dict_op_is(here - 3, p_load2)) {
		int arr_slot = (int)dict[here - 2];
		int idx_slot = (int)dict[here - 1];
		vocab.here -= 3;
		emit_call(interp, at_i_ll0_cfa);
		emit(interp, (cell)arr_slot);
		emit(interp, (cell)idx_slot);
		return 1;
	}

	if (here >= 4 && here - 4 >= compiler.fuse_floor
	    && dict_op_is(here - 4, p_local_fetch_0depth)
	    && dict_op_is(here - 2, p_local_fetch_0depth)) {
		int arr_slot = (int)dict[here - 3];
		int idx_slot = (int)dict[here - 1];
		vocab.here -= 4;
		emit_call(interp, at_i_ll0_cfa);
		emit(interp, (cell)arr_slot);
		emit(interp, (cell)idx_slot);
		return 1;
	}

	if (here >= 4 && here - 4 >= compiler.fuse_floor
	    && dict_op_is(here - 4, p_local_fetch_1depth)
	    && dict_op_is(here - 2, p_local_fetch_0depth)) {
		int arr_slot = (int)dict[here - 3];
		int idx_slot = (int)dict[here - 1];
		vocab.here -= 4;
		emit_call(interp, at_i_l1l0_cfa);
		emit(interp, (cell)arr_slot);
		emit(interp, (cell)idx_slot);
		return 1;
	}

	return 0;
}

int try_fuse_local_arith(Interpreter *interp, cfa_handler op_handler) {
	if (!compiler.compiling)
		return 0;

	int local_cfa, lit_cfa, litrev_cfa;
	if (op_handler == p_add_f) {
		local_cfa = ll_add_0_cfa;
		lit_cfa = ll_lit_add_0_cfa;
		litrev_cfa = ll_lit_add_0_cfa;
	} else if (op_handler == p_sub_f) {
		local_cfa = ll_sub_0_cfa;
		lit_cfa = ll_lit_sub_0_cfa;
		litrev_cfa = ll_litrev_sub_0_cfa;
	} else if (op_handler == p_mul_f) {
		local_cfa = ll_mul_0_cfa;
		lit_cfa = ll_lit_mul_0_cfa;
		litrev_cfa = ll_lit_mul_0_cfa;
	} else
		return 0;

	cell *dict = vocab.dict;
	int here = vocab.here;

	if (here >= 3 && here - 3 >= compiler.fuse_floor
	    && here - 3 == compiler.loadn_at
	    && dict_op_is(here - 3, p_load2)) {
		int slot_a = (int)dict[here - 2];
		int slot_b = (int)dict[here - 1];
		vocab.here -= 3;
		emit_call(interp, local_cfa);
		emit(interp, (cell)slot_a);
		emit(interp, (cell)slot_b);
		return 1;
	}

	if (here < 4)
		return 0;
	if (here - 4 < compiler.fuse_floor)
		return 0;

	if (!dict_is_handler[here - 4] || !dict_is_handler[here - 2])
		return 0;
	cfa_handler deep = (cfa_handler)dict[here - 4];
	cfa_handler top = (cfa_handler)dict[here - 2];

	if (deep == p_local_fetch_0depth && top == p_local_fetch_0depth) {
		int slot_a = (int)dict[here - 3];
		int slot_b = (int)dict[here - 1];
		vocab.here -= 4;
		emit_call(interp, local_cfa);
		emit(interp, (cell)slot_a);
		emit(interp, (cell)slot_b);
		return 1;
	}

	if (deep == p_local_fetch_0depth && top == p_literal) {
		Val lit;
		lit.bits = (uint64_t)dict[here - 1];
		if (VAL_TAG(lit) != T_FLOAT)
			return 0;
		int slot_a = (int)dict[here - 3];
		vocab.here -= 4;
		emit_call(interp, lit_cfa);
		emit(interp, (cell)slot_a);
		emit(interp, (cell)lit.bits);
		return 1;
	}

	if (deep == p_literal && top == p_local_fetch_0depth) {
		Val lit;
		lit.bits = (uint64_t)dict[here - 3];
		if (VAL_TAG(lit) != T_FLOAT)
			return 0;
		int slot_a = (int)dict[here - 1];
		vocab.here -= 4;
		emit_call(interp, litrev_cfa);
		emit(interp, (cell)slot_a);
		emit(interp, (cell)lit.bits);
		return 1;
	}

	return 0;
}



int try_fuse_at_i_lit(Interpreter *interp) {
	if (!compiler.compiling)
		return 0;

	cell *dict = vocab.dict;
	int here = vocab.here;
	if (here < 2)
		return 0;
	if (here - 2 < compiler.fuse_floor)
		return 0;
	if (!dict_op_is(here - 2, p_literal))
		return 0;

	Val literal;
	literal.bits = (uint64_t)dict[here - 1];
	if (VAL_TAG(literal) != T_FLOAT)
		return 0;

	int index = (int)VAL_NUMBER(literal);

	if (here >= 4 && here - 4 >= compiler.fuse_floor
	    && dict_op_is(here - 4, p_local_fetch_0depth)) {
		int slot = (int)dict[here - 3];
		vocab.here -= 4;
		emit_call(interp, at_i_lit_local0_cfa);
		emit(interp, (cell)slot);
		emit(interp, (cell)index);
		return 1;
	}

	vocab.here -= 2;
	emit_call(interp, at_i_lit_cfa);
	emit(interp, (cell)index);

	return 1;
}

void p_set(DISPATCH_ARGS) {
	POP_INT(count, "set", "count");
	if (count < 0 || count > interp->dsp) {
		fail(interp, "set: count %d out of range (stack has %d available)", count, interp->dsp);
		return;
	}

	int first_item = interp->dsp - count;
	int set_handle = build_set_from_values(interp, &interp->data_stack[first_item], count);
	if (interp->error_flag)
		return;

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

int refill_input(void) {
	if (compiler.load_depth > 0)
		return 0;

	int chunk = platform_read_chunk(compiler.input_buffer + compiler.input_buffer_len,
			INPUT_BUFFER_SIZE - compiler.input_buffer_len, compiler.interactive);
	if (chunk <= 0)
		return 0;
	compiler.input_buffer_len += chunk;
	return 1;
}

char *next_token(void) {
	while (compiler.input_buffer_pos < compiler.input_buffer_len
	       && isspace((unsigned char)compiler.input_buffer[compiler.input_buffer_pos]))
		compiler.input_buffer_pos++;

	if (compiler.input_buffer_pos >= compiler.input_buffer_len)
		return NULL;

	int start = compiler.input_buffer_pos;
	const char *buffer = compiler.input_buffer;
	char lead = buffer[start];
	char after_lead = start + 1 < compiler.input_buffer_len ? buffer[start + 1] : 0;

	if (lead == ';' || lead == ']' || lead == '}') {
		compiler.input_buffer_pos++;
	} else if ((lead == ':' || lead == ')') && after_lead == ']') {
		compiler.input_buffer_pos += 2;
	} else if (lead == '[') {
		int two_char_opener = after_lead == ':' || after_lead == '('
			|| after_lead == '|' || after_lead == '>';
		compiler.input_buffer_pos += two_char_opener ? 2 : 1;
	} else if (lead == '{') {
		compiler.input_buffer_pos++;
	} else {
		int bracket_depth = 0;
		int brace_depth = 0;
		while (compiler.input_buffer_pos < compiler.input_buffer_len) {
			char c = buffer[compiler.input_buffer_pos];
			if (isspace((unsigned char)c) || c == ';')
				break;
			if (c == ']' && bracket_depth == 0) {
				char preceding = buffer[compiler.input_buffer_pos - 1];
				if ((preceding == ':' || preceding == ')')
						&& compiler.input_buffer_pos - 1 > start)
					compiler.input_buffer_pos--;
				break;
			}
			if (c == '}' && brace_depth == 0)
				break;
			if (c == '[')
				bracket_depth++;
			if (c == ']')
				bracket_depth--;
			if (c == '{')
				brace_depth++;
			if (c == '}')
				brace_depth--;
			compiler.input_buffer_pos++;
		}
	}

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

void skip_whitespace_and_comments(void) {
	for (;;) {
		skip_whitespace();
		if (compiler.input_buffer_pos >= compiler.input_buffer_len)
			return;
		char lead_char = compiler.input_buffer[compiler.input_buffer_pos];
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
		return;
	}
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
					int la = compiler.loadn_at;
					if (la >= compiler.fuse_floor
					    && dict_op_is(la, p_load2)
					    && la + 3 == vocab.here) {
						vocab.dict[la] = (cell)p_load3;
						emit(interp, (cell)local_slot_idx);
					} else if (vocab.here - 2 >= compiler.fuse_floor
					           && dict_op_is(vocab.here - 2, p_local_fetch_0depth)) {
						int prev_slot = (int)vocab.dict[vocab.here - 1];
						vocab.here -= 2;
						compiler.loadn_at = vocab.here;
						emit_call(interp, load2_cfa);
						emit(interp, (cell)prev_slot);
						emit(interp, (cell)local_slot_idx);
					} else {
						emit_call(interp, vocab.local_fetch_0depth_cfa);
						emit(interp, (cell)local_slot_idx);
					}
				} else if (local_depth == 1) {
					emit_call(interp, vocab.local_fetch_1depth_cfa);
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
							|| cf == vocab.gt_cfa || cf == vocab.zeq_cfa
							|| cf == vocab.eq_f_cfa || cf == vocab.lt_f_cfa
							|| cf == vocab.gt_f_cfa)
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
	if (interp->error_flag)
		rollback_partial_definition();

	memcpy(compiler.input_buffer, saved_inbuf_contents, (size_t)saved_inbuf_len);
	compiler.input_buffer[saved_inbuf_len] = 0;
	compiler.input_buffer_len = saved_inbuf_len;
	compiler.input_buffer_pos = saved_inbuf_pos;
	compiler.need_more = saved_need_more;
	free(saved_inbuf_contents);
}

void p_load(DISPATCH_ARGS) {
	POP_STRING(filename_obj, "load");
	gc_root_push(interp, filename_obj_val);

	const char *filename = filename_obj->bytes;
	load_file(interp, filename);
	if (compiler.load_depth == 0 && !interp->error_flag)
		record_loaded_file(interp, filename);

	gc_root_pop(interp);

	DISPATCH(interp);
}

void p_reload(DISPATCH_ARGS) {
	forget_user(interp);

	for (int i = 0; i < compiler.n_loaded_files; i++) {
		load_file(interp, compiler.loaded_files[i]);
		if (interp->error_flag)
			return;
	}

	DISPATCH(interp);
}

#ifdef GC_DEBUG
static int handle_in_chunks(int handle, int *chunks, int n_chunks, int last_next) {
	for (int c = 0; c < n_chunks; c++) {
		int start = chunks[c];
		int end = (c == n_chunks - 1) ? last_next : start + SLOTS_PER_CLAIM;
		if (handle >= start && handle < end)
			return 1;
	}
	return 0;
}
#endif

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
				VAL_TAG(value) != T_SEGMENT &&
				VAL_TAG(value) != T_CONT &&
				VAL_TAG(value) != T_QUANTITY) return;

		if (VAL_TAG(value) == T_PAIR) {
			int slot = (int)VAL_DATA(value);
			if (slot < interp->gc_pair_base)
				return;
			GC_ASSERT(!in_parallel || handle_in_chunks(slot, thread_alloc.pairs.chunks, thread_alloc.pairs.n_chunks, thread_alloc.pairs.next), "worker marked a pair outside its own chunks");
			if (pairs.mark_epoch[slot] == interp->gc_epoch)
				return;
			pairs.mark_epoch[slot] = interp->gc_epoch;
			mark_value(interp, pairs.table[slot].head);
			value = pairs.table[slot].tail;
			continue;
		}

		if (VAL_TAG(value) == T_QUANTITY) {
			int slot = (int)VAL_DATA(value);
			if (slot < interp->gc_pair_base)
				return;
			GC_ASSERT(!in_parallel || handle_in_chunks(slot, thread_alloc.pairs.chunks, thread_alloc.pairs.n_chunks, thread_alloc.pairs.next), "worker marked a quantity outside its own chunks");
			if (pairs.mark_epoch[slot] == interp->gc_epoch)
				return;
			pairs.mark_epoch[slot] = interp->gc_epoch;
			mark_value(interp, pairs.table[slot].head);
			return;
		}

		int handle = (int)VAL_DATA(value);
		if (handle < interp->gc_object_base || handle >= arena.object_space.n)
			return;

		GC_ASSERT(!in_parallel || handle_in_chunks(handle, thread_alloc.objects.chunks, thread_alloc.objects.n_chunks, thread_alloc.objects.next), "worker marked an object outside its own chunks");
		Object *obj = OBJECT_AT(handle);
		if (!obj || obj->mark_epoch == interp->gc_epoch)
			return;
		obj->mark_epoch = interp->gc_epoch;

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
	if (arena.object_space.n >= arena.object_space.max
			&& arena.object_space.n_free < arena.object_space.max / 10)
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

void p_copy(DISPATCH_ARGS) {
	do_copy_reify(interp, 0);
	DISPATCH(interp);
}

void p_reify(DISPATCH_ARGS) {
	do_copy_reify(interp, 1);
	DISPATCH(interp);
}

int op_cell_count(int cursor) {
	cell *dict = vocab.dict;
	cell handler = dict[cursor];

	int superword_cells = superword_cell_count(handler);
	if (superword_cells)
		return superword_cells;

	if (handler == vocab.dict[vocab.enter_locals_mixed_cfa])
		return 3 + (int)dict[cursor + 2];

	if (handler == (cell)p_load2)
		return 3;
	if (handler == (cell)p_load3)
		return 4;

	if (handler == vocab.dict[vocab.local_fetch_cfa]
	    || handler == vocab.dict[vocab.local_store_cfa]
	    || handler == (cell)p_local_acc_add
	    || handler == (cell)p_local_acc_sub
	    || handler == (cell)p_local_acc_mul
	    || handler == (cell)p_local_acc_div
	    || handler == (cell)p_ll_add_0
	    || handler == (cell)p_ll_sub_0
	    || handler == (cell)p_ll_mul_0
	    || handler == (cell)p_ll_lit_add_0
	    || handler == (cell)p_ll_lit_sub_0
	    || handler == (cell)p_ll_lit_mul_0
	    || handler == (cell)p_ll_litrev_sub_0
	    || handler == (cell)p_at_i_lit_local0
	    || handler == (cell)p_at_i_ll0
	    || handler == (cell)p_at_i_l1l0)
		return 3;

	if (handler == vocab.dict[vocab.literal_cfa]
	    || handler == (cell)p_local_acc_add_0
	    || handler == (cell)p_local_acc_sub_0
	    || handler == (cell)p_local_acc_mul_0
	    || handler == (cell)p_local_acc_div_0
	    || handler == (cell)p_at_i_local0
	    || handler == (cell)p_at_i_lit
	    || handler == (cell)p_gather_local0)
		return 2;

	if (handler == vocab.dict[vocab.dostr_cfa]
	    || handler == vocab.dict[vocab.branch_cfa]
	    || handler == vocab.dict[vocab.zbranch_cfa]
	    || handler == vocab.dict[vocab.qzbranch_cfa]
	    || handler == vocab.dict[vocab.eq_zbranch_cfa]
	    || handler == vocab.dict[vocab.lt_zbranch_cfa]
	    || handler == vocab.dict[vocab.gt_zbranch_cfa]
	    || handler == vocab.dict[vocab.zeq_zbranch_cfa]
	    || handler == vocab.dict[vocab.eq_f_zbranch_cfa]
	    || handler == vocab.dict[vocab.lt_f_zbranch_cfa]
	    || handler == vocab.dict[vocab.gt_f_zbranch_cfa]
	    || handler == vocab.dict[vocab.to_var_cfa]
	    || handler == vocab.dict[vocab.enter_locals_cfa]
	    || handler == vocab.dict[vocab.enter_locals_to_cfa]
	    || handler == vocab.dict[vocab.leave_locals_cfa]
	    || handler == vocab.dict[vocab.local_fetch_0depth_cfa]
	    || handler == vocab.dict[vocab.local_fetch_1depth_cfa]
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

	if (in_parallel) {
		fail(interp, "gc: cannot collect inside a parallel region");
		return;
	}

	interp->gc_epoch = atomic_fetch_add(&arena.current_epoch, 1) + 1;
	interp->gc_object_base = 0;
	interp->gc_pair_base = 0;
	pairs.space.n_free = 0;

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

	arena.object_space.n_free = 0;
	for (int handle = 0; handle < arena.object_space.n; handle++) {
		Object *obj = arena.objects[handle];
		if (obj && obj->mark_epoch == interp->gc_epoch)
			continue;

		if (obj) {
			free_one_object(obj);
			arena.objects[handle] = NULL;
		}
		arena.object_space.free[arena.object_space.n_free++] = handle;
	}

	main_alloc.objects.next = main_alloc.objects.end = 0;

	for (int slot = 0; slot < pairs.space.n; slot++)
		if (pairs.mark_epoch[slot] != interp->gc_epoch)
			pairs.space.free[pairs.space.n_free++] = slot;

	size_t survived = arena.heap_bytes_live;
	arena.heap_gc_threshold = MAX(survived * 2, HEAP_GC_FLOOR);
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

#define SEE_TREE_MAX_DEPTH 32

/* Print one non-control op (name + operands) at cursor; shared by see-compiled
 * and see-tree. cell_count is op_cell_count(cursor). */
static void see_print_op(FILE *out, Interpreter *interp, int cursor, int cell_count) {
	cell handler = vocab.dict[cursor];
	if (superword_is_lit_fold(handler)) {
		Val immediate;
		immediate.bits = (uint64_t)vocab.dict[cursor + 1];
		fprintf(out, "%s ", handler_word_name(handler));
		print_val_compact(out, interp, immediate);
	} else if (superword_cell_count(handler)) {
		fprintf(out, "%s", handler_word_name(handler));
		for (int operand_index = 1; operand_index < cell_count; operand_index++)
			fprintf(out, " %s", var_name_from_slot(vocab.dict[cursor + operand_index]));
	} else if (handler == vocab.dict[vocab.literal_cfa]) {
		Val value;
		value.bits = (uint64_t)vocab.dict[cursor + 1];
		fputs("(lit) ", out);
		print_val_compact(out, interp, value);
	} else if (handler == (cell)p_ll_lit_add_0
	           || handler == (cell)p_ll_lit_sub_0
	           || handler == (cell)p_ll_lit_mul_0
	           || handler == (cell)p_ll_litrev_sub_0) {
		Val lit;
		lit.bits = (uint64_t)vocab.dict[cursor + 2];
		fprintf(out, "%s %lld ", handler_word_name(handler), (long long)vocab.dict[cursor + 1]);
		print_val_compact(out, interp, lit);
	} else {
		fprintf(out, "%s", handler_word_name(handler));
		for (int operand_index = 1; operand_index < cell_count; operand_index++)
			fprintf(out, " %lld", (long long)vocab.dict[cursor + operand_index]);
	}
}

static void see_compiled_body(FILE *out, Interpreter *interp, int body_start, int body_end) {
	cell exit_handler = vocab.dict[vocab.exit_cfa];
	cell branch_handler = vocab.dict[vocab.branch_cfa];
	cell docol_handler = (cell)docol;
	int cursor = body_start;
	int depth = 0;
	int expect_docol = 0;

	while (cursor < body_end) {
		cell handler = vocab.dict[cursor];
		cfa_handler handler_fn = (cfa_handler)handler;

		fprintf(out, " %d: ", cursor - body_start);

		if (handler == exit_handler) {
			fputs("exit\n", out);
			cursor++;
			if (depth == 0)
				break;
			depth--;
			expect_docol = 0;
			continue;
		}

		if (expect_docol && handler == docol_handler) {
			fputs("[:\n", out);
			cursor++;
			depth++;
			expect_docol = 0;
			continue;
		}

		if (handler_fn == docol || handler_fn == dovar || handler_fn == dounit) {
			int target = (int)vocab.dict[cursor + 1];
			if (target >= 4 && target < vocab.here)
				fprintf(out, "%s\n", &vocab.name_pool[WORD_NAME(target)]);
			else
				fputs("?\n", out);
			cursor += 2;
			expect_docol = 0;
			continue;
		}
		if (handler_fn == dosym) {
			fprintf(out, ":%s\n", &vocab.symbol_pool[vocab.dict[cursor + 1]]);
			cursor += 2;
			expect_docol = 0;
			continue;
		}

		int cell_count = op_cell_count(cursor);
		see_print_op(out, interp, cursor, cell_count);
		putc('\n', out);
		cursor += cell_count;
		expect_docol = (handler == branch_handler);
	}
}

/* Like see_compiled_body, but a call to a colon word is expanded inline:
 * its name, then its body indented two more spaces, recursively, down to
 * primitives. `stack` holds the cfas on the current expansion path so direct
 * or mutual recursion prints as a leaf instead of looping forever. */
static void see_tree_body(FILE *out, Interpreter *interp, int body_start, int indent, int *stack, int sp) {
	cell exit_handler = vocab.dict[vocab.exit_cfa];
	int cursor = body_start;
	int depth = 0;

	while (cursor < vocab.here) {
		cell handler = vocab.dict[cursor];
		cfa_handler handler_fn = (cfa_handler)handler;

		for (int s = 0; s < indent; s++)
			putc(' ', out);
		fprintf(out, "%d: ", cursor - body_start);

		if (handler == exit_handler) {
			fputs("exit\n", out);
			cursor++;
			if (depth == 0)
				break;
			depth--;
			continue;
		}

		if (handler_fn == docol) {
			int target = (int)vocab.dict[cursor + 1];
			if (target < 4 || target >= vocab.here) {
				/* the cell after docol is an inline op, not a word cfa, so this
				 * docol opens a quotation ([branch][off][docol][body][exit])
				 * rather than calling a colon word */
				fputs("[:\n", out);
				cursor++;
				depth++;
				continue;
			}
			cursor += 2;
			const char *name = &vocab.name_pool[WORD_NAME(target)];
			int seen = 0;
			for (int i = 0; i < sp; i++)
				if (stack[i] == target) {
					seen = 1;
					break;
				}
			if (seen || sp >= SEE_TREE_MAX_DEPTH) {
				fprintf(out, "%s ...\n", name);
			} else {
				fprintf(out, "%s:\n", name);
				stack[sp] = target;
				see_tree_body(out, interp, target + 1, indent + 2, stack, sp + 1);
			}
			continue;
		}
		if (handler_fn == dovar || handler_fn == dounit) {
			int target = (int)vocab.dict[cursor + 1];
			if (target >= 4 && target < vocab.here)
				fprintf(out, "%s\n", &vocab.name_pool[WORD_NAME(target)]);
			else
				fputs("?\n", out);
			cursor += 2;
			continue;
		}
		if (handler_fn == dosym) {
			fprintf(out, ":%s\n", &vocab.symbol_pool[vocab.dict[cursor + 1]]);
			cursor += 2;
			continue;
		}

		int cell_count = op_cell_count(cursor);
		see_print_op(out, interp, cursor, cell_count);
		putc('\n', out);
		cursor += cell_count;
	}
}

int capture_render(Interpreter *interp, void (*render)(FILE *, Interpreter *, int), int target_cfa) {
	char *buffer = NULL;
	size_t size = 0;
	FILE *out = open_memstream(&buffer, &size);
	if (!out) {
		fail(interp, "see>string: out of memory");
		return -1;
	}

	render(out, interp, target_cfa);
	fclose(out);

	int length = (int)size;
	if (length > 0 && buffer[length - 1] == '\n')
		length--;

	int handle = object_new_string(interp, buffer ? buffer : "", length);
	free(buffer);
	return handle;
}

static void see_compiled_render(FILE *out, Interpreter *interp, int target_cfa) {
	const char *name = &vocab.name_pool[WORD_NAME(target_cfa)];

	if ((cfa_handler)vocab.dict[target_cfa] != docol) {
		fprintf(out, "%s: not a colon definition\n", name);
		return;
	}

	int body_start = target_cfa + 1;
	int body_end = vocab.here;
	for (int cfa = vocab.latest_cfa; cfa != 0; cfa = (int)WORD_LINK(cfa)) {
		if (cfa > target_cfa && cfa - 4 < body_end)
			body_end = cfa - 4;
	}

	fprintf(out, ": %s   \\ %d cells\n", name, body_end - body_start);
	see_compiled_body(out, interp, body_start, body_end);
	fputs(";\n", out);
}

void p_see_compiled(DISPATCH_ARGS) {
	POP_XT(target_cfa, "see-compiled");
	see_compiled_render(stdout, interp, target_cfa);
	fflush(stdout);

	DISPATCH(interp);
}

void p_see_compiled_to_string(DISPATCH_ARGS) {
	POP_XT(target_cfa, "see-compiled>string");
	int handle = capture_render(interp, see_compiled_render, target_cfa);
	if (interp->error_flag)
		return;
	push(interp, make_string(handle));

	DISPATCH(interp);
}

static void see_tree_render(FILE *out, Interpreter *interp, int target_cfa) {
	const char *name = &vocab.name_pool[WORD_NAME(target_cfa)];

	if ((cfa_handler)vocab.dict[target_cfa] != docol) {
		fprintf(out, "%s: not a colon definition\n", name);
		return;
	}

	int stack[SEE_TREE_MAX_DEPTH + 1];
	stack[0] = target_cfa;

	fprintf(out, ": %s\n", name);
	see_tree_body(out, interp, target_cfa + 1, 2, stack, 1);
	fputs(";\n", out);
}

void p_see_tree(DISPATCH_ARGS) {
	POP_XT(target_cfa, "see-tree");
	see_tree_render(stdout, interp, target_cfa);
	fflush(stdout);

	DISPATCH(interp);
}

void p_see_tree_to_string(DISPATCH_ARGS) {
	POP_XT(target_cfa, "see-tree>string");
	int handle = capture_render(interp, see_tree_render, target_cfa);
	if (interp->error_flag)
		return;
	push(interp, make_string(handle));

	DISPATCH(interp);
}
void p_save(DISPATCH_ARGS) {
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

	fprintf(file, "\\ water vocabulary\n\n");

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


void free_one_object(Object *obj) {
	switch (obj->kind) {
		case OBJECT_STRING: arena_free(obj->bytes); break;
		case OBJECT_SET:
		case OBJECT_ARRAY: arena_free(obj->items); break;
		case OBJECT_FRAME: arena_free(obj->frame.keys); arena_free(obj->frame.values); break;
		case OBJECT_MATRIX: {
								heap_bytes_sub((size_t)obj->matrix.rows * (size_t)obj->matrix.columns * sizeof(double));
								free(obj->matrix.elements);
							   	break;
							}
		case OBJECT_CONTINUATION: {
								heap_bytes_sub((size_t)obj->continuation.return_len * sizeof(Val));
									  free(obj->continuation.return_slice); 
									  break;
								  }
		case OBJECT_SEGMENT: {
								heap_bytes_sub((size_t)obj->segment.length * segment_element_size(obj->segment.element_type));
								 free(obj->segment.data); 
								 break;
							 }
	}
	arena_free_object(obj);
}

void worker_local_gc(Interpreter *interp) {
	int last_pair_chunk = thread_alloc.pairs.n_chunks - 1;

	parallel_region_collected = 1;
	interp->gc_epoch = atomic_fetch_add(&arena.current_epoch, 1) + 1;
	interp->gc_object_base = parallel_region_object_base;
	interp->gc_pair_base = parallel_region_pair_base;

	int i;
	for (i = 0; i < interp->dsp; i++)
		mark_value(interp, interp->data_stack[i]);
	for (i = 0; i < interp->rsp; i++)
		mark_value(interp, interp->return_stack[i]);
	for (i = 0; i < interp->side_dsp; i++)
		mark_value(interp, interp->side_stack[i]);
	for (i = 0; i < interp->n_gc_roots; i++)
		mark_value(interp, interp->gc_roots[i]);

	thread_alloc.objects.n_free = 0;
	int last_slot_chunk = thread_alloc.objects.n_chunks - 1;
	for (int c = 0; c < thread_alloc.objects.n_chunks; c++) {
		int start = thread_alloc.objects.chunks[c];
		int end = (c == last_slot_chunk) ? thread_alloc.objects.next : start + SLOTS_PER_CLAIM;
		for (int handle = start; handle < end; handle++) {
			Object *obj = arena.objects[handle];
			if (obj && obj->mark_epoch == interp->gc_epoch)
				continue;
			if (obj) {
				free_one_object(obj);
				arena.objects[handle] = NULL;
			}
			GROW_IF_FULL_SYS(thread_alloc.objects.n_free, thread_alloc.objects.free_cap, thread_alloc.objects.free);
			thread_alloc.objects.free[thread_alloc.objects.n_free++] = handle;
		}
	}

	thread_alloc.pairs.n_free = 0;
	for (int c = 0; c < thread_alloc.pairs.n_chunks; c++) {
		int start = thread_alloc.pairs.chunks[c];
		int end = (c == last_pair_chunk) ? thread_alloc.pairs.next : start + SLOTS_PER_CLAIM;
		for (int slot = start; slot < end; slot++) {
			if (pairs.mark_epoch[slot] == interp->gc_epoch)
				continue;
			GROW_IF_FULL_SYS(thread_alloc.pairs.n_free, thread_alloc.pairs.free_cap, thread_alloc.pairs.free);
			thread_alloc.pairs.free[thread_alloc.pairs.n_free++] = slot;
		}
	}

	size_t live = thread_alloc.heap_bytes_live;
	thread_alloc.heap_gc_threshold = MAX(live * 2, HEAP_GC_FLOOR);
}

void forget_user(Interpreter *interp) {
	/* Free only user objects; objects below init_n_objects are literals baked
	   into the compiled-in vocabulary (e.g. run's " +") and must survive. */
	for (int i = arena.object_space.init; i < arena.object_space.n; i++) {
		if (arena.objects[i]) {
			free_one_object(arena.objects[i]);
			arena.objects[i] = NULL;
		}
	}
	arena.object_space.n = arena.object_space.init;
	main_alloc.objects.next = arena.object_space.n;
	main_alloc.objects.end = arena.object_space.n;
	arena.object_space.n_free = 0;

	pairs.space.n = pairs.space.init;
	main_alloc.pairs.next = main_alloc.pairs.end = pairs.space.n;
	pairs.space.n_free = 0;

	interp->dsp = 0;
	interp->rsp = 0;
	vocab.here = vocab.init_here;
	vocab.forget_generation = 0;
	vocab.latest_cfa = vocab.init_latest_cfa;
	vocab.names_here = vocab.init_names_here;
	vocab.source_here = vocab.init_source_here;
	vocab.symbol_pool_here = vocab.init_symbol_pool_here;
	truncate_quotation_spans();
	rebuild_symbol_hash();
}



void interp_init(Interpreter *interp) {
	interp->next_mark_id = 1;
	interp->bind_trail = xmalloc(sizeof(int) * BIND_TRAIL_DEPTH);
	interp->bind_trail_cap = BIND_TRAIL_DEPTH;
	interp->lvar_stack = xmalloc(sizeof(Val) * LVAR_STACK_DEPTH);
	interp->lvar_cap = LVAR_STACK_DEPTH;
	interp->loop_local_base = -1;
}

Interpreter *main_init(void) {
	Interpreter *interp = xcalloc(1, sizeof(Interpreter));
	interp_init(interp);

	arena_init();
	vocab.here = DICT_RESERVED;
	vocab.source_here = 1;

	pairs.table = xmalloc(sizeof(Pair) * PAIR_TABLE_DEPTH);
	pairs.space.cap = PAIR_TABLE_DEPTH;
	pairs.space.n = 0;
	main_alloc.pairs.next = main_alloc.pairs.end = pairs.space.n;
	pairs.mark_epoch = xcalloc(PAIR_TABLE_DEPTH, sizeof(cell));
	pairs.space.free = xmalloc(sizeof(int) * PAIR_TABLE_DEPTH);
	pairs.space.n_free = 0;

	dimension_init();

	vocab.false_symbol = intern_symbol(interp, "0");
	vocab.true_symbol = intern_symbol(interp, "1");
	vocab.wildcard_symbol = intern_symbol(interp, "*");
	vocab.descendant_symbol = intern_symbol(interp, "//");
	vocab.self_symbol = intern_symbol(interp, ".");

	return interp;
}

Interpreter *worker_init(int worker_index) {
	Interpreter *interp = xcalloc(1, sizeof(Interpreter));
	interp_init(interp);

	interp->trampoline_base = 3 * worker_index;
	interp->gc_disabled = 1;

	return interp;
}

int construct_vocabulary(Interpreter *interp, int load_lib) {
	compiler.handler_registry[compiler.n_handlers++] = (void *)docol;
	compiler.handler_registry[compiler.n_handlers++] = (void *)dovar;
	compiler.handler_registry[compiler.n_handlers++] = (void *)dosym;
	compiler.handler_registry[compiler.n_handlers++] = (void *)dounit;
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
	vocab.eq_f_cfa = define_primitive(interp, "feq", p_eq_f, 0);
	vocab.lt_f_cfa = define_primitive(interp, "flt", p_lt_f, 0);
	vocab.gt_f_cfa = define_primitive(interp, "fgt", p_gt_f, 0);
	define_primitive(interp, "bit-and", p_bit_and, 0);
	define_primitive(interp, "bit-or", p_bit_or, 0);
	define_primitive(interp, "bit-xor", p_bit_xor, 0);
	define_primitive(interp, "lshift", p_lshift, 0);
	define_primitive(interp, "rshift", p_rshift, 0);
	define_primitive(interp, "bit-not", p_bit_not, 0);
	define_primitive(interp, "lowest-bit", p_lowest_bit, 0);
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
	define_primitive(interp, "type-of", p_type_of, 0);

	type_of_intern_names(interp);
	
	define_primitive(interp, "lvar", p_lvar, 0);
	define_primitive(interp, "_", p_wildcard, 0);
	define_primitive(interp, "unify", p_unify, 0);
	define_primitive(interp, "~", p_unify, 0);
	define_primitive(interp, "matches?", p_matches, 0);
	define_primitive(interp, "deref", p_deref, 0);
	define_primitive(interp, "amb", p_amb, 0);
	define_primitive(interp, "alloc-stats", p_alloc_stats, 0);
	define_primitive(interp, ".", p_dot, 0);
	define_primitive(interp, ".a", p_dot_all, 0);
	define_primitive(interp, "render", p_render, 0);
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
	define_primitive(interp, "byte-substring", p_byte_substring, 0);
	define_primitive(interp, "char-at", p_char_at, 0);
	define_primitive(interp, "codepoint-at", p_codepoint_at, 0);
	define_primitive(interp, "string>chars", p_string_to_chars, 0);
	define_primitive(interp, "string>codepoints", p_string_to_codepoints, 0);
	define_primitive(interp, "codepoint>char", p_codepoint_to_char, 0);
	define_primitive(interp, "codepoints>string", p_codepoints_to_string, 0);
	define_primitive(interp, "trim", p_trim, 0);
	define_primitive(interp, "join", p_join, 0);
	define_primitive(interp, "string>number", p_string_to_number, 0);
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
	define_primitive(interp, "int-segment", p_int_segment, 0);
	define_primitive(interp, "double-segment", p_double_segment, 0);
	define_primitive(interp, "array>frame", p_array_to_frame, 0);
	define_primitive(interp, "frame>array", p_frame_to_array, 0);
	define_primitive(interp, "select-values", p_select_values, 0);
	define_primitive(interp, "select-keys", p_select_keys, 0);
	define_primitive(interp, "frame", p_frame, 0);
	define_primitive(interp, "json>frame", p_json_to_frame, 0);
	define_primitive(interp, "frame>json", p_frame_to_json, 0);
	define_primitive(interp, "take", p_take, 0);
	define_primitive(interp, "reverse", p_reverse, 0);
	define_primitive(interp, "concat", p_concat, 0);
	define_primitive(interp, "flatten-array", p_flatten_array, 0);
	define_primitive(interp, "sort", p_sort, 0);
	define_primitive(interp, "sample", p_sample, 0);
	define_primitive(interp, "destruct", p_destruct, 0);
	define_primitive(interp, "destruct-to", p_destruct_to, 0);
	define_primitive(interp, "slice!", p_slice_store, 0);
	define_primitive(interp, "to-slice!", p_to_slice, 0);
	define_primitive(interp, "range", p_range, 0);
	define_primitive(interp, "size", p_size, 0);
	define_primitive(interp, "byte-size", p_byte_size, 0);
	define_primitive(interp, "member?", p_member, 0);
	define_primitive(interp, "set", p_set, 0);
	define_primitive(interp, "union", p_union, 0);
	define_primitive(interp, "intersection", p_intersect, 0);
	define_primitive(interp, "difference", p_difference, 0);
	define_primitive(interp, "set-add!", p_set_add, 0);
	define_primitive(interp, "set-remove!", p_set_remove, 0);
	define_primitive(interp, "add-last!", p_add_last, 0);
	define_primitive(interp, "remove-last!", p_remove_last, 0);
	define_primitive(interp, "execute", p_execute, 0);
	define_primitive(interp, "(execute-catching)", p_execute_catching, 4);
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
	define_primitive(interp, "see>string", p_see_to_string, 0);
	define_primitive(interp, "man", p_man, 0);
	define_primitive(interp, "see-compiled", p_see_compiled, 0);
	define_primitive(interp, "see-compiled>string", p_see_compiled_to_string, 0);
	define_primitive(interp, "see-tree", p_see_tree, 0);
	define_primitive(interp, "see-tree>string", p_see_tree_to_string, 0);

	vocab.exit_cfa = define_primitive(interp, "exit", p_exit, 0);
	vocab.literal_cfa = define_primitive(interp, "(lit)", p_literal, 4);
	vocab.branch_cfa = define_primitive(interp, "(branch)", p_branch, 4);
	vocab.zbranch_cfa = define_primitive(interp, "(0branch)", p_0branch, 4);
	vocab.qzbranch_cfa = define_primitive(interp, "(?0branch)", p_qzbranch, 4);
	vocab.eq_zbranch_cfa = define_primitive(interp, "(=0branch)", p_eq_zbranch, 4);
	vocab.lt_zbranch_cfa = define_primitive(interp, "(lt0branch)", p_lt_zbranch, 4);
	vocab.gt_zbranch_cfa = define_primitive(interp, "(gt0branch)", p_gt_zbranch, 4);
	vocab.zeq_zbranch_cfa = define_primitive(interp, "(0=0branch)", p_zeq_zbranch, 4);
	vocab.eq_f_zbranch_cfa = define_primitive(interp, "(feq0branch)", p_eq_f_zbranch, 4);
	vocab.lt_f_zbranch_cfa = define_primitive(interp, "(flt0branch)", p_lt_f_zbranch, 4);
	vocab.gt_f_zbranch_cfa = define_primitive(interp, "(fgt0branch)", p_gt_f_zbranch, 4);
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
	vocab.local_fetch_1depth_cfa = define_primitive(interp, "(local@1)", p_local_fetch_1depth, 4);
	load2_cfa = define_primitive(interp, "(load2)", p_load2, 4);
	load3_cfa = define_primitive(interp, "(load3)", p_load3, 4);
	at_i_local0_cfa = define_primitive(interp, "(@i.l0)", p_at_i_local0, 4);
	at_i_lit_cfa = define_primitive(interp, "(@i.lit)", p_at_i_lit, 4);
	at_i_lit_local0_cfa = define_primitive(interp, "(@i.lit.l0)", p_at_i_lit_local0, 4);
	gather_local0_cfa = define_primitive(interp, "(gather.l0)", p_gather_local0, 4);
	at_i_ll0_cfa = define_primitive(interp, "(@i.ll0)", p_at_i_ll0, 4);
	at_i_l1l0_cfa = define_primitive(interp, "(@i.l1l0)", p_at_i_l1l0, 4);
	define_primitive(interp, "(@i.array)", p_at_i_array, 4);
	define_primitive(interp, "(@i.segment)", p_at_i_segment, 4);
	define_primitive(interp, "(!i.array)", p_store_i_array, 4);
	define_primitive(interp, "(!i-drop.array)", p_store_i_drop_array, 4);
	ll_add_0_cfa = define_primitive(interp, "(ll+0)", p_ll_add_0, 4);
	ll_sub_0_cfa = define_primitive(interp, "(ll-0)", p_ll_sub_0, 4);
	ll_mul_0_cfa = define_primitive(interp, "(ll*0)", p_ll_mul_0, 4);
	ll_lit_add_0_cfa = define_primitive(interp, "(ll.lit+0)", p_ll_lit_add_0, 4);
	ll_lit_sub_0_cfa = define_primitive(interp, "(ll.lit-0)", p_ll_lit_sub_0, 4);
	ll_lit_mul_0_cfa = define_primitive(interp, "(ll.lit*0)", p_ll_lit_mul_0, 4);
	ll_litrev_sub_0_cfa = define_primitive(interp, "(ll.litrev-0)", p_ll_litrev_sub_0, 4);
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
	define_primitive(interp, "constant", p_constant, 0);
	define_primitive(interp, "symbol", p_symbol, 0);
	define_primitive(interp, "base", p_base, 0);
	define_primitive(interp, "unit", p_unit, 0);
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
	define_primitive(interp, "leave", p_leave, 1);
	define_primitive(interp, "continue", p_continue, 1);
	define_primitive(interp, "[:", p_qcolon, 1);
	define_primitive(interp, ":]", p_qsemi, 1);
	define_primitive(interp, "|", p_bar, 1);
	define_primitive(interp, "|>", p_bar_to, 1);
	define_primitive(interp, "[|", p_bracket_bar, 1);
	define_primitive(interp, "[>", p_bracket_bar_to, 1);

	define_primitive(interp, "0-matrix", p_0_matrix, 0);
	define_primitive(interp, "matrix", p_matrix, 0);
	define_primitive(interp, "dim", p_dim, 0);
	define_primitive(interp, "transpose", p_transpose, 0);
	define_primitive(interp, "submatrix", p_submatrix, 0);
	define_primitive(interp, "select-rows", p_select_rows, 0);
	define_primitive(interp, "augment", p_augment, 0);
	define_primitive(interp, "vstack", p_vstack, 0);
	define_primitive(interp, "diagonal-matrix", p_diagonal_matrix, 0);
	vocab.at_i_cfa = define_primitive(interp, "@i", p_at_i, 0);
	define_primitive(interp, "!i", p_store_i, 0);
	define_primitive(interp, "@j", p_at_j, 0);
	define_primitive(interp, "@i,j", p_at_ij, 0);
	define_primitive(interp, "diagonal", p_diagonal, 0);
	define_primitive(interp, "reshape", p_reshape, 0);
	define_primitive(interp, "matrix-range", p_matrix_range, 0);
	define_primitive(interp, "sum", p_sum, 0);
	define_primitive(interp, "var", p_variance, 0);
	define_primitive(interp, "quantile", p_quantile, 0);
	define_primitive(interp, "norm", p_norm, 0);
	define_primitive(interp, "frobenius-norm", p_frobenius_norm, 0);
	define_primitive(interp, "row-sums", p_row_sums, 0);
	define_primitive(interp, "column-sums", p_column_sums, 0);
	define_primitive(interp, "max", p_max, 0);
	define_primitive(interp, "min", p_min, 0);
	define_primitive(interp, "argmax", p_argmax, 0);
	define_primitive(interp, "argmin", p_argmin, 0);
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

	define_primitive(interp, "seed", p_seed, 0);
	define_primitive(interp, "random", p_random, 0);
	define_primitive(interp, "random-int", p_random_int, 0);
	define_primitive(interp, "now", p_now, 0);
	define_primitive(interp, "(wall-now)", p_wall_now, 4);
	define_primitive(interp, "(epoch>date)", p_epoch_to_date, 4);
	define_primitive(interp, "(epoch>date-local)", p_epoch_to_date_local, 4);
	define_primitive(interp, "(date>epoch)", p_date_to_epoch, 4);
	define_primitive(interp, "(date>epoch-local)", p_date_to_epoch_local, 4);
	define_primitive(interp, "(format-time)", p_format_time, 4);
	define_primitive(interp, "(format-time-local)", p_format_time_local, 4);
	define_primitive(interp, "(parse-time)", p_parse_time, 4);
	define_primitive(interp, "sleep", p_sleep, 0);
	define_primitive(interp, "env", p_env, 0);
	define_primitive(interp, "env!", p_env_set, 0);
	define_primitive(interp, "cd", p_cd, 0);
	define_primitive(interp, "cwd", p_cwd, 0);
	define_primitive(interp, "read-file", p_read_file, 0);
	define_primitive(interp, "write-file", p_write_file, 0);
	define_primitive(interp, "append-file", p_append_file, 0);
	define_primitive(interp, "read-tsv", p_read_tsv, 0);
	define_primitive(interp, "write-tsv", p_write_tsv, 0);
	define_primitive(interp, "start-process", p_start_process, 0);
	define_primitive(interp, "write", p_write, 0);
	define_primitive(interp, "read", p_read, 0);
	define_primitive(interp, "close", p_close, 0);
	define_primitive(interp, "stdin", p_stdin, 0);
	define_primitive(interp, "stdout", p_stdout, 0);
	define_primitive(interp, "stderr", p_stderr, 0);
	define_primitive(interp, "db-open", p_db_open, 0);
	define_primitive(interp, "ffi-open", p_ffi_open, 0);
	define_primitive(interp, "ffi-function", p_ffi_function, 0);
	define_primitive(interp, "ffi-variadic", p_ffi_variadic, 0);
	ffi_register_call_cfa(define_primitive(interp, "(ffi-call)", p_ffi_call, 4));
	define_primitive(interp, "ffi-free", p_ffi_free, 0);
	define_primitive(interp, "matrix>pointer", p_matrix_to_pointer, 0);
	define_primitive(interp, "segment>pointer", p_segment_to_pointer, 0);
	define_primitive(interp, "db-close", p_db_close, 0);
	define_primitive(interp, "db-exec", p_db_exec, 0);
	define_primitive(interp, "db-query", p_db_query, 0);
	define_primitive(interp, "wait", p_wait, 0);
	define_primitive(interp, "stop", p_stop_process, 0);
	define_primitive(interp, "running?", p_running, 0);

	if (load_lib) {
		memcpy(compiler.input_buffer, lib_h2o, lib_h2o_len);
		compiler.input_buffer[lib_h2o_len] = 0;
		compiler.input_buffer_len = (int)lib_h2o_len;
		compiler.input_buffer_pos = 0;
		run_outer(interp);

		compiler.input_buffer_len = 0;
		compiler.input_buffer_pos = 0;
		compiler.input_buffer[0] = 0;

		if (interp->error_flag) {
			printf("lib.h2o load error\n");
			return 1;
		}
	}

	/* lib.h2o is part of the rebuilt-each-process base, not user state: the
	   init_* watermarks (the boundary an image saves above) sit after it, so
	   images carry only words defined after bootstrap. */
	vocab.init_here = vocab.here;
	vocab.init_latest_cfa = vocab.latest_cfa;
	vocab.init_names_here = vocab.names_here;
	vocab.init_source_here = vocab.source_here;
	vocab.init_symbol_pool_here = vocab.symbol_pool_here;
	arena.object_space.init = arena.object_space.n;
	pairs.space.init = pairs.space.n;
	dimension_freeze();

	vocab.lib_end_latest_cfa = vocab.latest_cfa;
	return 0;
}

int main(int argc, char **argv) {
	int interactive = isatty(fileno(stdin));
	int interactive_set = 0;
	int load_lib = 1;
	long max_objects_arg = 0;
	const char *program_files[64];
	int n_program_files = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0) {
			interactive = 1;
			interactive_set = 1;
		}
		else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--batch") == 0) {
			interactive = 0;
			interactive_set = 1;
		}
		else if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--file") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "water: %s needs a file\n", argv[i]);
				return 2;
			}
			if (n_program_files >= 64) {
				fprintf(stderr, "water: too many -f files (max 64)\n");
				return 2;
			}
			program_files[n_program_files++] = argv[++i];
		}
		else if (strcmp(argv[i], "--no-lib") == 0)
			load_lib = 0;
		else if (strcmp(argv[i], "--max-objects") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "water: --max-objects needs a value\n");
				return 2;
			}
			max_objects_arg = strtol(argv[++i], NULL, 10);
			if (max_objects_arg < 1) {
				fprintf(stderr, "water: --max-objects must be a positive integer\n");
				return 2;
			}
		}
		else {
			fprintf(stderr, "water: unknown option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (n_program_files > 0 && !interactive_set)
		interactive = 0;

	Interpreter *interp = main_init();
	if (max_objects_arg > 0) {
		max_objects_arg = MIN(max_objects_arg, MAX_OBJECTS);
		arena.object_space.max = (int)max_objects_arg;
	}
	platform_init();
	if (construct_vocabulary(interp, load_lib))
		return 1;

	for (int i = 0; i < n_program_files; i++) {
		load_file(interp, program_files[i]);
		if (interp->error_flag) {
			fprintf(stderr, "error: %s\n", interp->error_message);
			if (interp->error_trace[0])
				fprintf(stderr, "%s\n", interp->error_trace);
			return 1;
		}
		record_loaded_file(interp, program_files[i]);
	}

	if (n_program_files > 0 && !interactive)
		return 0;

	interactive = platform_repl_begin(interp, interactive);
	compiler.interactive = interactive;

	for (;;) {
		int chunk = platform_read_chunk(compiler.input_buffer + compiler.input_buffer_len,
				INPUT_BUFFER_SIZE - compiler.input_buffer_len, interactive);
		if (chunk == 0)
			break;
		if (chunk > 0)
			compiler.input_buffer_len += chunk;

		interp->error_flag = 0;
		interp->unwinding = 0;
		int line_lvar_top = interp->lvar_top;
		int line_bind_trail_top = interp->bind_trail_top;
		compiler.need_more = 0;
		run_outer(interp);

		if (compiler.need_more)
			continue;

		if (interp->error_flag) {
			rollback_partial_definition();
			compiler.loop_begin = 0;
			compiler.leave_chain = 0;
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
			if (interp->error_flag) {
				fputs(interp->error_message, stdout);
				if (interp->error_trace[0]) {
					putchar('\n');
					fputs(interp->error_trace, stdout);
				}
			} else
				fputs("ok", stdout);
			putchar('\n');
			fflush(stdout);
		} else if (interp->error_flag) {
			fprintf(stdout, "error: %s\n", interp->error_message);
			if (interp->error_trace[0])
				fprintf(stdout, "%s\n", interp->error_trace);
			fflush(stdout);
		}

		inbuf_reset();
	}
	return 0;
}

