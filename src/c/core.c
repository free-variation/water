#include "telic.h"
#include "lib_embed.h"


Vocabulary vocab;
unsigned char dict_is_handler[VOCABULARY_INIT_SIZE];
Compiler compiler;
Arena arena;
PairPool pairs;
static size_t arena_reserve_request;

int in_parallel;
int parallel_region_collected;
int parallel_region_object_base;
int parallel_region_pair_base;
static _Thread_local AllocContext thread_alloc;
AllocContext main_alloc;
static platform_mutex_t intern_lock = PLATFORM_MUTEX_INIT;


void *xmalloc(size_t bytes) {
	void *block = malloc(bytes);
	if (!block) {
		fprintf(stderr, "telic: out of memory\n");
		exit(1);
	}
	return block;
}

void *xcalloc(size_t count, size_t size) {
	void *block = calloc(count, size);
	if (!block) {
		fprintf(stderr, "telic: out of memory\n");
		exit(1);
	}
	return block;
}

static void arena_init(void) {
	arena.base = platform_reserve(arena_reserve_request, &arena.reserved);
	if (!arena.base) {
		fprintf(stderr, "telic: arena reserve failed\n");
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
			fprintf(stderr, "telic: arena exhausted\n");
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
		void *next_recycled_block = *(void **)recycled_block;
		context->size_class_free[class_index] = next_recycled_block;
		if (next_recycled_block)
			__builtin_prefetch(next_recycled_block, 1);
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
		void *next_freed_struct = *(void **)fresh;
		context->freed_object_structs = next_freed_struct;
		if (next_freed_struct)
			__builtin_prefetch(next_freed_struct, 1);
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

static inline int local_claim_handle(Interpreter *interp, LocalHandles *lh, HandleSpace *space) {
	if (lh->n_free > 0)
		return lh->free[--lh->n_free];

	if (lh->next >= lh->end) {
		if (space->cap - space->n < HANDLE_PRESSURE_SLOTS)
			interp->gc_pending |= GC_PENDING;

		int claimed = atomic_fetch_add(&space->n, SLOTS_PER_CLAIM);
		if (claimed + SLOTS_PER_CLAIM > space->cap) {
			atomic_fetch_sub(&space->n, SLOTS_PER_CLAIM);
			return -1;
		}

		lh->next = claimed;
		lh->end = claimed + SLOTS_PER_CLAIM;
		GROW_IF_FULL_SYS(lh->n_chunks, lh->chunks_cap, lh->chunks);
		lh->chunks[lh->n_chunks++] = claimed;
	}

	return lh->next++;
}

int object_alloc_slot(Interpreter *interp) {
	if (in_parallel) {
		if (thread_alloc.heap_bytes_live > thread_alloc.heap_gc_threshold)
			interp->gc_pending |= GC_PENDING;
		int slot = local_claim_handle(interp, &thread_alloc.objects, &arena.object_space);
		if (slot < 0) {
			fail(interp, "object table full in parallel region");
			return -1;
		}
		return slot;
	}

	if (arena.heap_bytes_live > arena.heap_gc_threshold)
		interp->gc_pending |= GC_PENDING;

	if (main_alloc.objects.next < main_alloc.objects.end)
		return main_alloc.objects.next++;

	if (arena.object_space.n_free > 0)
		return arena.object_space.free[--arena.object_space.n_free];

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

	int object_headroom = arena.object_space.n + domain_len
			+ worker_count * SLOTS_PER_CLAIM * REGION_CLAIMS_PER_WORKER;
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

static void free_object_heap_backing(Object *obj) {
	switch (obj->kind) {
		case OBJECT_MATRIX:
			heap_bytes_sub((size_t)obj->matrix.rows * (size_t)obj->matrix.columns * sizeof(double));
			free(obj->matrix.elements);
			break;
		case OBJECT_SEGMENT:
			heap_bytes_sub((size_t)obj->segment.length * sizeof(int));
			free(obj->segment.data);
			break;
		case OBJECT_CONTINUATION:
			heap_bytes_sub((size_t)obj->continuation.return_len * sizeof(Val));
			free(obj->continuation.return_slice);
			break;
		default:
			break;
	}
}

void region_abort(ParallelRegion *region) {
	int high = arena.object_space.n;
	for (int handle = region->n_objects; handle < high; handle++) {
		Object *obj = arena.objects[handle];
		if (!obj)
			continue;
		free_object_heap_backing(obj);
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
	if (num_elements <= INLINE_ITEMS_CAPACITY) {
		obj->capacity = INLINE_ITEMS_CAPACITY;
		obj->items = obj->inline_items;
		return slot;
	}

	obj->capacity = num_elements;
	obj->items = arena_malloc(sizeof(Val) * (size_t)num_elements);
	return slot;
}

void items_reserve(Object *obj, int capacity) {
	if (capacity <= obj->capacity)
		return;

	if (obj->items == obj->inline_items) {
		Val *block = arena_malloc(sizeof(Val) * (size_t)capacity);
		memcpy(block, obj->inline_items, sizeof(Val) * (size_t)obj->len);
		obj->items = block;
	} else {
		obj->items = arena_realloc(obj->items, sizeof(Val) * (size_t)capacity);
	}
	obj->capacity = capacity;
}

int object_new_pair(Interpreter *interp) {
	int slot;

	if (in_parallel) {
		slot = local_claim_handle(interp, &thread_alloc.pairs, &pairs.space);
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

static int object_new_matrix_sized(Interpreter *interp, int num_rows, int num_columns, int zeroed) {
	NEW_OBJECT(obj, OBJECT_MATRIX);
	obj->matrix.rows = num_rows;
	obj->matrix.columns = num_columns;
	size_t num_elements = (size_t)num_rows * (size_t)num_columns;
	size_t bytes = (num_elements ? num_elements : 1) * sizeof(double);

	if (zeroed) {
		CALLOC_OR_FAIL_RETURNING_CLEANUP(interp, obj->matrix.elements, num_elements ? num_elements : 1, sizeof(double),
				{ arena_free_object(obj); arena.objects[slot] = NULL; }, -1);
	} else {
		obj->matrix.elements = malloc(bytes);
		if (!obj->matrix.elements) {
			arena_free_object(obj);
			arena.objects[slot] = NULL;
			fail(interp, "out of memory");
			return -1;
		}
#ifdef MATRIX_POISON
		memset(obj->matrix.elements, 0xff, bytes);
#endif
	}

	heap_bytes_add(num_elements * sizeof(double));

	return slot;
}

int object_new_matrix(Interpreter *interp, int num_rows, int num_columns) {
	return object_new_matrix_sized(interp, num_rows, num_columns, 1);
}

int object_new_matrix_raw(Interpreter *interp, int num_rows, int num_columns) {
	return object_new_matrix_sized(interp, num_rows, num_columns, 0);
}

int object_new_segment(Interpreter *interp, int length) {
	NEW_OBJECT(obj, OBJECT_SEGMENT);

	obj->segment.length = length;
	CALLOC_OR_FAIL_RETURNING_CLEANUP(interp, obj->segment.data, (size_t)(length > 0 ? length : 1), sizeof(int),
			{ arena_free_object(obj); arena.objects[slot] = NULL; }, -1);

	heap_bytes_add((size_t)length * sizeof(int));

	return slot;
}

int object_new_exact(Interpreter *interp, int sign, const uint32_t *numerator, int n_numerator,
		const uint32_t *denominator, int n_denominator) {
	NEW_OBJECT(obj, OBJECT_EXACT);
	obj->exact.sign = sign;
	obj->exact.n_numerator = n_numerator;
	obj->exact.n_denominator = n_denominator;
	obj->exact.limbs = arena_malloc(((size_t)n_numerator + n_denominator) * 4);
	memcpy(obj->exact.limbs, numerator, (size_t)n_numerator * 4);
	memcpy(obj->exact.limbs + n_numerator, denominator, (size_t)n_denominator * 4);
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
	MALLOC_OR_FAIL_RETURNING_CLEANUP(interp, obj->continuation.return_slice, sizeof(Val) * (size_t)MAX(return_len, 1),
			{ arena_free_object(obj); arena.objects[slot] = NULL; }, -1);
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

static int complex_cmp_against(Val complex_value, double real_part, double imaginary_part) {
	int slot = (int)VAL_DATA(complex_value);
	int real_order = compare_double(VAL_NUMBER(pairs.table[slot].head), real_part);
	if (real_order)
		return real_order;
	return compare_double(VAL_NUMBER(pairs.table[slot].tail), imaginary_part);
}

int val_cmp_depth(Interpreter *interp, Val left, Val right, int depth) {
	if (depth > MAX_NESTING_DEPTH) {
		fail(interp, "structure too deeply nested (cycle?)");
		return 0;
	}

	if (VAL_TAG(left) != VAL_TAG(right)) {
		if (VAL_TAG(left) == T_EXACT && VAL_TAG(right) == T_FLOAT)
			return exact_cmp_double(left, VAL_NUMBER(right));
		if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_EXACT)
			return -exact_cmp_double(right, VAL_NUMBER(left));
		if (VAL_TAG(left) == T_COMPLEX && VAL_TAG(right) == T_FLOAT)
			return complex_cmp_against(left, VAL_NUMBER(right), 0.0);
		if (VAL_TAG(left) == T_FLOAT && VAL_TAG(right) == T_COMPLEX)
			return -complex_cmp_against(right, VAL_NUMBER(left), 0.0);
		return (int)VAL_TAG(left) - (int)VAL_TAG(right);
	}

	switch (VAL_TAG(left)) {
		case T_FLOAT:
			return compare_double(VAL_NUMBER(left), VAL_NUMBER(right));
		case T_EXACT:
			return exact_cmp(interp, left, right);
		case T_COMPLEX: {
							int right_slot = (int)VAL_DATA(right);
							return complex_cmp_against(left,
									VAL_NUMBER(pairs.table[right_slot].head),
									VAL_NUMBER(pairs.table[right_slot].tail));
						}
		case T_QUANTITY: {
							 int left_unit  = (int)pairs.table[VAL_DATA(left)].tail.bits;
							 int right_unit = (int)pairs.table[VAL_DATA(right)].tail.bits;
							 Val left_magnitude  = pairs.table[VAL_DATA(left)].head;
							 Val right_magnitude = pairs.table[VAL_DATA(right)].head;

							 double factor;
							 if (VAL_TAG(left_magnitude) == T_FLOAT && VAL_TAG(right_magnitude) == T_FLOAT
									 && unit_conversion(right_unit, left_unit, &factor))
								 return compare_double(VAL_NUMBER(left_magnitude), VAL_NUMBER(right_magnitude) * factor);

							 if ((VAL_TAG(left_magnitude) == T_COMPLEX || VAL_TAG(right_magnitude) == T_COMPLEX)
									 && (VAL_TAG(left_magnitude) == T_COMPLEX || VAL_TAG(left_magnitude) == T_FLOAT)
									 && (VAL_TAG(right_magnitude) == T_COMPLEX || VAL_TAG(right_magnitude) == T_FLOAT)
									 && unit_conversion(right_unit, left_unit, &factor)) {
								 int left_is_complex = VAL_TAG(left_magnitude) == T_COMPLEX;
								 int right_is_complex = VAL_TAG(right_magnitude) == T_COMPLEX;
								 double left_real = left_is_complex
									 ? VAL_NUMBER(pairs.table[VAL_DATA(left_magnitude)].head) : VAL_NUMBER(left_magnitude);
								 double left_imaginary = left_is_complex
									 ? VAL_NUMBER(pairs.table[VAL_DATA(left_magnitude)].tail) : 0.0;
								 double right_real = right_is_complex
									 ? VAL_NUMBER(pairs.table[VAL_DATA(right_magnitude)].head) : VAL_NUMBER(right_magnitude);
								 double right_imaginary = right_is_complex
									 ? VAL_NUMBER(pairs.table[VAL_DATA(right_magnitude)].tail) : 0.0;

								 int real_order = compare_double(left_real, right_real * factor);
								 if (real_order)
									 return real_order;
								 return compare_double(left_imaginary, right_imaginary * factor);
							 }

							 int exact_side_is_left = VAL_TAG(left_magnitude) == T_EXACT;
							 Val exact_magnitude = exact_side_is_left ? left_magnitude : right_magnitude;
							 Val other_magnitude = exact_side_is_left ? right_magnitude : left_magnitude;
							 int exact_unit = exact_side_is_left ? left_unit : right_unit;
							 int other_unit = exact_side_is_left ? right_unit : left_unit;

							 long long ratio_numerator, ratio_denominator;
							 if (left_unit != right_unit && VAL_TAG(exact_magnitude) == T_EXACT
									 && (VAL_TAG(other_magnitude) == T_EXACT || VAL_TAG(other_magnitude) == T_FLOAT)
									 && unit_conversion_ratio(exact_unit, other_unit, &ratio_numerator, &ratio_denominator)) {
								 gc_root_push(interp, left);
								 gc_root_push(interp, right);
								 Val scaled = exact_scale_by_ratio(interp, exact_magnitude, ratio_numerator, ratio_denominator);
								 gc_root_pop(interp);
								 gc_root_pop(interp);
								 if (interp->error_flag)
									 return 0;

								 int order = VAL_TAG(other_magnitude) == T_EXACT
									 ? exact_cmp(interp, scaled, other_magnitude)
									 : exact_cmp_double(scaled, VAL_NUMBER(other_magnitude));
								 return exact_side_is_left ? order : -order;
							 }

							 if (left_unit != right_unit)
								 return left_unit - right_unit;
							 return val_cmp_depth(interp, left_magnitude, right_magnitude, depth + 1);
						 }
		case T_SYMBOL: case T_XT: case T_CURRIED: case T_ADDR: case T_LOGIC_VAR:
		case T_STREAM: case T_DB: case T_PTR: case T_CONT: case T_REST:

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


int print_truncate = 1;
int print_full_precision = 0;

void print_double(FILE *out, double number) {
	if (number == (double)(int64_t)number && number > -1e15 && number < 1e15)
		fprintf(out, "%lld", (long long)number);
	else
		fprintf(out, print_full_precision ? "%.17g" : "%g", number);
}

int stdout_is_tty(void) {
	static int cached = -1;
	if (cached < 0)
		cached = isatty(fileno(stdout));
	return cached;
}

const char *term_bold(void) { return stdout_is_tty() ? "\033[1m" : ""; }
const char *term_plain(void) { return stdout_is_tty() ? "\033[0m" : ""; }

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
	if (print_full_precision)
		fprintf(out, " %.17g", value);
	else
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

static void print_rest(FILE *out, Interpreter *interp, Val rest,
		void (*pr)(FILE *, Interpreter *, Val)) {
	fputs("..", out);
	if (rest_is_wildcard(rest))
		putc('_', out);
	else
		print_logic_var(out, interp, make_logic_var((int)VAL_DATA(rest)), pr);
}

static void print_complex_parts(FILE *out, double real_part, double imaginary_part) {
	print_double(out, real_part);
	if (imaginary_part >= 0.0)
		putc('+', out);
	print_double(out, imaginary_part);
	putc('i', out);
}

static void print_complex(FILE *out, Val value) {
	int slot = (int)VAL_DATA(value);
	print_complex_parts(out, VAL_NUMBER(pairs.table[slot].head), VAL_NUMBER(pairs.table[slot].tail));
}

static Val quantity_display_magnitude(Val magnitude, int unit) {
	if (!unit_is_named(unit) && VAL_TAG(magnitude) == T_FLOAT)
		return make_float(VAL_NUMBER(magnitude) * unit_scale_value(unit));
	return magnitude;
}

static int print_exact_magnitude_scaled(FILE *out, Val magnitude, int unit) {
	if (unit_is_named(unit))
		return 0;

	if (VAL_TAG(magnitude) == T_COMPLEX) {
		double scale = unit_scale_value(unit);
		if (scale == 1.0)
			return 0;
		int slot = (int)VAL_DATA(magnitude);
		print_complex_parts(out, VAL_NUMBER(pairs.table[slot].head) * scale,
				VAL_NUMBER(pairs.table[slot].tail) * scale);
		return 1;
	}

	if (VAL_TAG(magnitude) != T_EXACT)
		return 0;

	long long scale_numerator, scale_denominator;
	unit_scale_ratio(unit, &scale_numerator, &scale_denominator);
	if (scale_numerator == scale_denominator)
		return 0;

	exact_print_scaled(out, magnitude, scale_numerator, scale_denominator);
	return 1;
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
						   fputs("[<...>]", out);
					   } else {
						   fputs("[< ", out);
						   print_items(out, interp, OBJECT_AT(VAL_DATA(value)));
						   fputs(">]", out);
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
		case T_XT: fprintf(out, "<xt %lld>", (long long)VAL_DATA(value)); break;
	case T_CURRIED: {
						Object *curried = OBJECT_AT(VAL_DATA(value));
						int target_cfa = (int)VAL_DATA(curried->items[0]);
						const char *name = name_of(target_cfa);
						if (name)
							fprintf(out, "<xt %s +%d bound>", name, curried->len - 1);
						else
							fprintf(out, "<xt %d +%d bound>", target_cfa, curried->len - 1);
						break;
					}
		case T_ADDR: fprintf(out, "<addr %lld>", (long long)VAL_DATA(value)); break;
		case T_STREAM: fprintf(out, "<stream %d>", stream_fd(value)); break;
		case T_DB: fprintf(out, "<database %lld>", (long long)(VAL_DATA(value) & 0xFF)); break;
		case T_PTR: fprintf(out, "<ptr %lld>", (long long)VAL_DATA(value)); break;
		case T_SEGMENT: {
							Object *segment = OBJECT_AT(VAL_DATA(value));
							fprintf(out, "<int-segment %d>", segment->segment.length);
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
		case T_EXACT: exact_print(out, value); break;
		case T_COMPLEX: print_complex(out, value); break;
		case T_REST: print_rest(out, interp, value, print_val); break;
		case T_QUANTITY: {
							 int slot = (int)VAL_DATA(value);
							 int unit = (int)pairs.table[slot].tail.bits;
							 if (!print_exact_magnitude_scaled(out, pairs.table[slot].head, unit))
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

	print_depth_enter();
	if (print_depth > MAX_NESTING_DEPTH) {
		fputs("[...]", out);
		print_depth_leave();
		return;
	}

	int n = arr->len;
	int trunc = print_truncate && n > PRINT_FIRST + PRINT_LAST;
	int child_indent = indent + 2;
	fputs("[ ", out);
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

#define COMPACT_ELEMENTS 3

static void compact_elements(FILE *out, Interpreter *interp, const Val *items, int n_items, const cell *keys) {
	for (int i = 0; i < n_items && i < COMPACT_ELEMENTS; i++) {
		if (keys)
			fprintf(out, " :%s", &vocab.symbol_pool[keys[i]]);
		putc(' ', out);
		print_val_compact(out, interp, items[i]);
	}
	if (n_items > COMPACT_ELEMENTS)
		fputs(" …", out);
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
		case T_ARRAY: {
						  Object *obj = OBJECT_AT(VAL_DATA(value));
						  int is_set = VAL_TAG(value) == T_SET;
						  if (print_depth > 0) {
							  fprintf(out, is_set ? "[<%d>]" : "[%d]", obj->len);
							  break;
						  }
						  print_depth_enter();
						  fprintf(out, is_set ? "[<%d:" : "[%d:", obj->len);
						  compact_elements(out, interp, obj->items, obj->len, NULL);
						  fputs(is_set ? " >]" : " ]", out);
						  print_depth_leave();
						  break;
					  }
		case T_FRAME: {
						  Object *frame = OBJECT_AT(VAL_DATA(value));
						  if (print_depth > 0) {
							  fprintf(out, "{%d}", frame->len);
							  break;
						  }
						  print_depth_enter();
						  fprintf(out, "{%d:", frame->len);
						  compact_elements(out, interp, frame->frame.values, frame->len, frame->frame.keys);
						  fputs(" }", out);
						  print_depth_leave();
						  break;
					  }
		case T_MATRIX: {
						   Object *m = OBJECT_AT(VAL_DATA(value));
						   int n_elements = m->matrix.rows * m->matrix.columns;
						   if (print_depth > 0) {
							   fprintf(out, "[%dx%d]", m->matrix.rows, m->matrix.columns);
							   break;
						   }
						   print_depth_enter();
						   fprintf(out, "[%dx%d:", m->matrix.rows, m->matrix.columns);
						   for (int i = 0; i < n_elements && i < COMPACT_ELEMENTS; i++)
							   fprintf(out, " %.4g", m->matrix.elements[i]);
						   if (n_elements > COMPACT_ELEMENTS)
							   fputs(" …", out);
						   fputs(" ]", out);
						   print_depth_leave();
						   break;
					   }
		case T_EXACT: exact_print(out, value); break;
		case T_COMPLEX: print_complex(out, value); break;
		case T_REST: print_rest(out, interp, value, print_val_compact); break;
		case T_QUANTITY: {
							 int slot = (int)VAL_DATA(value);
							 int unit = (int)pairs.table[slot].tail.bits;
							 if (!print_exact_magnitude_scaled(out, pairs.table[slot].head, unit))
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
		case T_CURRIED: {
						Object *curried = OBJECT_AT(VAL_DATA(value));
						const char *name = name_of((int)VAL_DATA(curried->items[0]));
						fprintf(out, "'%s+%d", name ? name : "?", curried->len - 1);
						break;
					}
	case T_ADDR: fprintf(out, "@%lld", (long long)VAL_DATA(value)); break;
		case T_PTR: fprintf(out, "<ptr %lld>", (long long)VAL_DATA(value)); break;
		case T_SEGMENT: {
							Object *segment = OBJECT_AT(VAL_DATA(value));
							fprintf(out, "*I%d", segment->segment.length);
							break;
						}
		case T_CONT: fputs("k", out); break;
		case T_LOGIC_VAR: fprintf(out, "_%d", (int)VAL_DATA(value)); break;
		case T_MARK: {
						 int bracket = (int)VAL_DATA(value);
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

int current_unit = 0;
int session_unit = 0;
static int next_unit = 1;

int find(const char *name) {
	int cfa = vocab.latest_cfa;
	while (cfa != 0) {
		if (strcmp(&vocab.name_pool[WORD_NAME(cfa)], name) == 0
				&& !(WORD_IS_INTERNAL(cfa) && WORD_UNIT(cfa) != current_unit))
			return cfa;
		cfa = (int)WORD_LINK(cfa);
	}
	return 0;
}

#define NAME_CACHE_SIZE 1024

static struct { cell key; int by_handler; const char *name; } name_cache[NAME_CACHE_SIZE];
static int name_cache_latest_cfa = -1;

static const char *cached_name(cell key, int by_handler) {
	if (name_cache_latest_cfa != vocab.latest_cfa) {
		memset(name_cache, 0, sizeof(name_cache));
		name_cache_latest_cfa = vocab.latest_cfa;
	}
	unsigned slot = (unsigned)(((uint64_t)key ^ ((uint64_t)key >> 3)) & (NAME_CACHE_SIZE - 1));
	if (name_cache[slot].name && name_cache[slot].key == key && name_cache[slot].by_handler == by_handler)
		return name_cache[slot].name;

	for (int cfa = vocab.latest_cfa; cfa != 0; cfa = (int)WORD_LINK(cfa)) {
		if (by_handler ? vocab.dict[cfa] == key : (cell)cfa == key) {
			name_cache[slot].key = key;
			name_cache[slot].by_handler = by_handler;
			name_cache[slot].name = &vocab.name_pool[WORD_NAME(cfa)];
			return name_cache[slot].name;
		}
	}
	return NULL;
}

const char *name_of(int cfa) {
	return cached_name((cell)cfa, 0);
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
	REQUIRE_RETURN_ROOM(interp, chain_ip + 1, chain_sp);

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

void dodefer(DISPATCH_ARGS) {
	int deferred_cfa = (int)*chain_ip;
	int target_cfa = (int)vocab.dict[deferred_cfa + 1];

	if (!target_cfa) {
		SYNC_REGISTERS(interp, chain_ip, chain_sp);
		fail(interp, "unresolved deferred word: %s", name_of(deferred_cfa));
		return;
	}

	REQUIRE_RETURN_ROOM(interp, chain_ip, chain_sp);

	interp->return_stack[interp->rsp++] = make_addr((int)(chain_ip + 1 - vocab.dict));

	DISPATCH_REGISTERS(interp, vocab.dict + target_cfa + 1, chain_sp);
}


static void trace_step(Interpreter *interp);
static void trace_call(Interpreter *interp, int cfa);
static const char *quotation_header(int cfa);

static void unwind_locals_scopes(Interpreter *interp) {
	while (interp->local_base > interp->run_floor
			&& interp->rsp - 1 >= interp->local_base
			&& interp->rsp - 1 < interp->local_base + saved_n_locals(interp->return_stack[interp->local_base - 1])) {
		int enclosing_base = saved_local_base(interp->return_stack[interp->local_base - 1]);
		interp->rsp = interp->local_base - 1;
		interp->local_base = enclosing_base;
	}
}

static int tick_hook_cfa;
static int tick_hook_latest_cfa = -1;

void run_tick_hook(Interpreter *interp) {
	interp->gc_pending &= ~TICK_PENDING;
	if (tick_hook_latest_cfa != vocab.latest_cfa) {
		tick_hook_cfa = find("on-tick");
		tick_hook_latest_cfa = vocab.latest_cfa;
	}
	if (!tick_hook_cfa || interp->error_flag)
		return;

	execute_cfa(interp, tick_hook_cfa);
	if (interp->error_flag) {
		fprintf(stderr, "on-tick: %s\n", interp->error_message);
		interp->error_flag = 0;
		interp->unwinding = 0;
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
			if (interp->gc_pending & INTERRUPT_PENDING) {
				interp->gc_pending &= ~INTERRUPT_PENDING;
				fail(interp, "interrupted");
				break;
			}
			if (interp->gc_pending & TICK_PENDING)
				run_tick_hook(interp);
			if (interp->gc_pending & TRACE_PENDING)
				trace_step(interp);
			if (interp->gc_pending & GC_PENDING) {
				if (in_parallel) {
					interp->gc_pending &= ~GC_PENDING;
					worker_local_gc(interp);
				} else if (!interp->gc_disabled) {
					interp->gc_pending &= ~GC_PENDING;
					gc(interp);
				}
			}
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

	if (handler == dodefer) {
		int target_cfa = (int)vocab.dict[cfa + 1];
		if (!target_cfa) {
			fail(interp, "unresolved deferred word: %s", name_of(cfa));
			return;
		}

		execute_cfa(interp, target_cfa);
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

int callable_cfa(Val callable) {
	if (VAL_TAG(callable) == T_CURRIED)
		return (int)VAL_DATA(OBJECT_AT(VAL_DATA(callable))->items[0]);

	return (int)VAL_DATA(callable);
}

int curried_new(Interpreter *interp, Val target, const Val *values, int n_values) {
	Object *inherited = VAL_TAG(target) == T_CURRIED ? OBJECT_AT(VAL_DATA(target)) : NULL;
	int n_inherited = inherited ? inherited->len - 1 : 0;

	int curried_handle = object_new_array(interp, 1 + n_values + n_inherited);
	if (interp->error_flag)
		return -1;

	Object *curried = OBJECT_AT(curried_handle);
	curried->items[0] = inherited ? inherited->items[0] : target;
	for (int i = 0; i < n_values; i++)
		curried->items[1 + i] = values[i];
	for (int i = 0; i < n_inherited; i++)
		curried->items[1 + n_values + i] = inherited->items[1 + i];

	return curried_handle;
}

int curried_materialize(Interpreter *interp, Val curried_val) {
	Object *curried = OBJECT_AT(VAL_DATA(curried_val));

	int materialized_cfa = create_header(interp, "(curried)", 4);
	if (interp->error_flag)
		return -1;

	emit(interp, (cell)&docol);
	for (int i = 1; i < curried->len; i++)
		emit_val_literal(interp, curried->items[i]);
	emit_call(interp, (int)VAL_DATA(curried->items[0]));
	emit_call(interp, vocab.exit_cfa);
	if (interp->error_flag)
		return -1;

	return materialized_cfa;
}

void p_enter_curried(DISPATCH_ARGS) {
	Object *curried = OBJECT_AT((int)*chain_ip);
	int n_bound = curried->len - 1;
	int target_cfa = (int)VAL_DATA(curried->items[0]);

	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, n_bound);
	for (int i = 0; i < n_bound; i++)
		chain_sp[i] = curried->items[1 + i];

	if ((cfa_handler)vocab.dict[target_cfa] == docol) {
		REQUIRE_RETURN_ROOM(interp, chain_ip, chain_sp);
		interp->return_stack[interp->rsp++] = make_addr((int)(chain_ip + 1 - vocab.dict));

		DISPATCH_REGISTERS(interp, vocab.dict + target_cfa + 1, chain_sp + n_bound);
	}

	SYNC_REGISTERS(interp, chain_ip + 1, chain_sp + n_bound);
	execute_cfa(interp, target_cfa);

	DISPATCH(interp);
}

void call_open_callable(Interpreter *interp, Val callable, CallContext *context) {
	if (VAL_TAG(callable) != T_CURRIED) {
		call_open(interp, (int)VAL_DATA(callable), context);
		return;
	}

	context->rooted = 0;
	context->callable = callable;
	gc_root_push(interp, callable);
	if (interp->error_flag) {
		context->fast = 0;
		return;
	}
	context->rooted = 1;

	if (interp->gc_pending & TRACE_PENDING) {
		context->fast = 0;
		return;
	}

	context->reuses_locals = 0;
	context->primitive = NULL;
	context->hoisted = 0;
	context->fast = 1;
	context->leave_ip = 0;
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

	vocab.dict[interp->trampoline_base] = (cell)&p_enter_curried;
	vocab.dict[interp->trampoline_base + 1] = (cell)VAL_DATA(callable);
	vocab.dict[interp->trampoline_base + 2] = vocab.dict[vocab.stop_cfa];
}

static void trampoline_stop(DISPATCH_ARGS) {
	interp->running = 0;
	interp->ip = interp->trampoline_base + 2;
}

void call_open(Interpreter *interp, int cfa, CallContext *context) {
	cfa_handler handler = (cfa_handler)vocab.dict[cfa];

	context->reuses_locals = 0;
	context->rooted = 0;
	context->callable = make_xt(cfa);
	context->primitive = handler == docol ? NULL : handler;

	if (handler == dovar || handler == dosym || handler == dounit || handler == dodefer
			|| handler == p_execute || (interp->gc_pending & TRACE_PENDING)) {
		context->fast = 0;
		return;
	}

	context->fast = 1;
	context->leave_ip = 0;
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

	context->hoisted = 0;
	if (interp->call_depth < MAX_CALL_DEPTH) {
		interp->call_depth++;
		context->saved_run_floor = interp->run_floor;
		interp->run_floor = interp->rsp;
		context->hoisted = 1;
	}

	cell stop_handler = vocab.dict[vocab.stop_cfa];
	if (handler == docol) {
		vocab.dict[interp->trampoline_base] = (cell)docol;
		vocab.dict[interp->trampoline_base + 1] = (cell)cfa;
		vocab.dict[interp->trampoline_base + 2] = stop_handler;

		int n_locals = 0, n_received = 0, slots_ip = -1, body_start = 0;
		cfa_handler enter = (cfa_handler)vocab.dict[cfa + 1];
		n_locals = (int)vocab.dict[cfa + 2];

		if (enter == p_enter_locals_to) {
			n_received = (int)vocab.dict[cfa + 3];
			body_start = cfa + 4;
		} else if (enter == p_enter_locals_mixed) {
			n_received = (int)vocab.dict[cfa + 3];
			slots_ip = cfa + 4;
			body_start = cfa + 4 + n_received;
		} else {
			interp->loop_n = 0;
			interp->loop_body_start = cfa + 1;

			if (dict_op_is(cfa - 2, (cfa_handler)vocab.dict[vocab.branch_cfa])) {
				int exit_ip = cfa + (int)vocab.dict[cfa - 1] - 2;
				if (vocab.dict[exit_ip] == vocab.dict[vocab.exit_cfa]) {
					context->leave_ip = exit_ip;
					context->saved_leave = vocab.dict[exit_ip];
					vocab.dict[exit_ip] = stop_handler;
				}
			}
		}

		if (body_start && interp->rsp + n_locals + 1 <= RETURN_STACK_DEPTH) {
			interp->return_stack[interp->rsp++] = make_locals_header(interp->local_base, n_locals, cfa + 1);

			context->saved_loop_local_base = interp->loop_local_base;
			interp->local_base = interp->rsp;
			interp->loop_local_base = interp->rsp;
			interp->rsp += n_locals;
			for (int i = 0; i < n_locals; i++)
				interp->return_stack[interp->local_base + i] = make_tagged(T_NONE, 0);
			context->reuses_locals = 1;

			if (context->hoisted)
				interp->run_floor = interp->rsp;

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
		vocab.dict[interp->trampoline_base + 2] = (cell)&trampoline_stop;
		interp->ip = interp->trampoline_base + 2;
	}
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


	rpush(interp, make_addr(vocab.stop_cfa));
	if (interp->error_flag) {
		interp->run_floor = saved_floor;
		interp->call_depth--;
		return;
	}

	interp->running = 1;
	if (unlikely(interp->gc_pending & TRACE_PENDING)) {
		if (cfa != interp->trace_root_cfa)
			trace_call(interp, cfa);
		interp->ip = cfa + 1;
		trace_step(interp);
	}
	interp->ip = cfa + 2;
	((cfa_handler)vocab.dict[cfa + 1])(interp, vocab.dict + cfa + 2, interp->data_stack + interp->dsp);

	if (interp->running && !interp->error_flag)
		run_inner(interp, interp->run_floor);

	interp->run_floor = saved_floor;
	interp->call_depth--;
	interp->ip = saved_ip;
	interp->running = saved_running;
}

static void dispatch_body_hoisted(Interpreter *interp, int body_start) {
	interp->running = 1;
	interp->ip = body_start + 1;
	((cfa_handler)vocab.dict[body_start])(interp, vocab.dict + body_start + 1, interp->data_stack + interp->dsp);

	if (interp->running && !interp->error_flag)
		run_inner(interp, interp->run_floor);
}

void call_invoke(Interpreter *interp) {
	if (interp->loop_body_start) {
		interp->loop_local_refill = 0;

		int n = interp->loop_n;
		int base = interp->loop_local_base;
		int data_start = interp->dsp - n;

		if (unlikely(data_start < 0)) {
			fail(interp, "insufficient values on data stack; need %d", n);
			return;
		}

		if (interp->loop_slots_ip < 0) {
			for (int i = 0; i < n; i++)
				interp->return_stack[base + i] = interp->data_stack[data_start + i];
		} else {
			int slots_ip = interp->loop_slots_ip;
			for (int i = 0; i < n; i++)
				interp->return_stack[base + (int)vocab.dict[slots_ip + i]] = interp->data_stack[data_start + i];
		}

		interp->dsp -= n;
		dispatch_body_hoisted(interp, interp->loop_body_start);
		return;
	}

	dispatch_body_hoisted(interp, interp->trampoline_base);
}

void call_close(Interpreter *interp, CallContext *context) {
	if (context->rooted)
		gc_root_pop(interp);

	if (!context->fast)
		return;

	if (context->hoisted) {
		interp->run_floor = context->saved_run_floor;
		interp->call_depth--;
	}
	interp->running = context->saved_running;
	interp->ip = context->saved_ip;
	vocab.dict[interp->trampoline_base] = context->saved_slot_0;
	vocab.dict[interp->trampoline_base + 1] = context->saved_slot_1;
	vocab.dict[interp->trampoline_base + 2] = context->saved_slot_2;

	interp->loop_body_start = context->saved_loop_body_start;
	interp->loop_n = context->saved_loop_n;
	interp->loop_slots_ip = context->saved_loop_slots_ip;


	if (context->leave_ip)
		vocab.dict[context->leave_ip] = context->saved_leave;

	if (context->reuses_locals) {
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
		fprintf(stderr, "telic: dictionary full\n");
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
	WORD_SET_UNIT(vocab.latest_cfa, current_unit);
	return vocab.latest_cfa;
}

void echo_definition(const char *name, int redefined, const char *kind) {
	if (!compiler.interactive || compiler.load_depth > 1)
		return;
	printf("%s %s: %s\n", redefined ? "redefined" : "new", kind, name);
	fflush(stdout);
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
	if (compiler.current_load_file && compiler.compiling)
		record_cell_line(vocab.here, current_source_line());
	cfa_handler handler = (cfa_handler)vocab.dict[target_cfa];
	emit(interp, (cell)handler);
	dict_is_handler[vocab.here - 1] = 1;

	if (handler == docol || handler == dovar || handler == dosym || handler == dounit || handler == dodefer) {
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

int quotation_starts_at(int addr) {
	for (int i = 0; i < vocab.n_quotation_spans; i++)
		if (vocab.quotation_spans[i].start_cfa == addr)
			return 1;
	return 0;
}

const char *quotation_source(int start_cfa) {
	const QuotationSpan *span = quotation_span_containing(start_cfa);
	if (!span || span->start_cfa != start_cfa || span->source_offset == 0)
		return NULL;

	return &vocab.source_pool[span->source_offset];
}

int quotation_extent_end(int start_cfa) {
	const QuotationSpan *span = quotation_span_containing(start_cfa);
	return (span && span->start_cfa == start_cfa) ? span->end_cfa : start_cfa + 1;
}

static int location_file_index(const char *file) {
	for (int i = 0; i < vocab.n_location_files; i++)
		if (strcmp(vocab.location_files[i], file) == 0)
			return i;
	if (vocab.n_location_files >= MAX_LOCATION_FILES)
		return -1;

	vocab.location_files[vocab.n_location_files] = strdup(file);
	return vocab.n_location_files++;
}

const char *display_load_path(const char *file) {
	static char binary_dir[PATH_MAX];
	static int binary_dir_len = -1;

	if (binary_dir_len < 0) {
		binary_dir_len = 0;
		if (platform_executable_path(binary_dir, sizeof binary_dir)) {
			char *last_slash = strrchr(binary_dir, '/');
			if (last_slash) {
				*last_slash = 0;
				binary_dir_len = (int)strlen(binary_dir);
			}
		}
	}

	if (binary_dir_len > 0 && strncmp(file, binary_dir, (size_t)binary_dir_len) == 0
			&& file[binary_dir_len] == '/')
		return file + binary_dir_len + 1;
	return file;
}

void record_word_location(int cfa, const char *file, int line, int effect_offset, int summary_offset) {
	if (vocab.n_word_locations >= MAX_WORD_LOCATIONS)
		return;
	int file_index = location_file_index(display_load_path(file));
	if (file_index < 0)
		return;

	WordLocation *location = &vocab.word_locations[vocab.n_word_locations++];
	location->cfa = cfa;
	location->file = file_index;
	location->line = line;
	location->effect_offset = effect_offset;
	location->summary_offset = summary_offset;
}

const WordLocation *word_location(int cfa) {
	LOWER_BOUND(vocab.n_word_locations, mid, vocab.word_locations[mid].cfa < cfa, at);
	if (at < vocab.n_word_locations && vocab.word_locations[at].cfa == cfa)
		return &vocab.word_locations[at];
	return NULL;
}

int current_source_line(void) {
	if (compiler.input_buffer_pos < compiler.line_cursor_pos) {
		compiler.line_cursor_pos = 0;
		compiler.line_cursor_line = 1;
	}
	while (compiler.line_cursor_pos < compiler.input_buffer_pos
			&& compiler.line_cursor_pos < compiler.input_buffer_len) {
		if (compiler.input_buffer[compiler.line_cursor_pos] == '\n')
			compiler.line_cursor_line++;
		compiler.line_cursor_pos++;
	}
	return compiler.line_cursor_line;
}

void record_cell_line(int address, int line) {
	if (vocab.n_cell_lines >= MAX_CELL_LINES)
		return;
	if (vocab.n_cell_lines > 0 && vocab.cell_lines[vocab.n_cell_lines - 1].address >= address)
		return;

	CellLine *entry = &vocab.cell_lines[vocab.n_cell_lines++];
	entry->address = address;
	entry->line = line;
}

int cell_line_at(int address, int floor_address) {
	LOWER_BOUND(vocab.n_cell_lines, mid, vocab.cell_lines[mid].address < address, at);
	if (at == 0)
		return 0;
	const CellLine *entry = &vocab.cell_lines[at - 1];
	if (entry->address < floor_address)
		return 0;
	return entry->line;
}

void truncate_cell_lines(void) {
	while (vocab.n_cell_lines > 0
			&& vocab.cell_lines[vocab.n_cell_lines - 1].address >= vocab.here)
		vocab.n_cell_lines--;
}

void truncate_word_locations(void) {
	while (vocab.n_word_locations > 0
			&& vocab.word_locations[vocab.n_word_locations - 1].cfa >= vocab.here)
		vocab.n_word_locations--;
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

static void clean_snippet(char *cleaned, int cap, int source_offset) {
	int n = 0;
	int pending_space = 0;
	const char *c = &vocab.source_pool[source_offset];
	for (; *c && n < cap - 1; c++) {
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
}

static void trace_write_snippet(Interpreter *interp, int *len, int source_offset) {
	if (source_offset == 0) {
		trace_write(interp, len, "[:?]");
		return;
	}
	char cleaned[TRACE_SNIPPET_MAX + 8];
	clean_snippet(cleaned, TRACE_SNIPPET_MAX, source_offset);
	trace_write(interp, len, cleaned);
}

typedef struct {
	int addr;
	int repeats;
	const QuotationSpan *span;
	int cfa;
} TraceFrame;

static const char *handler_word_name(cell handler);

static const char *running_op_name(int fault_cell, int body_start, int body_end) {
	cell exit_handler = vocab.dict[vocab.exit_cfa];
	int cursor = body_start;

	while (cursor <= fault_cell && cursor < body_end) {
		cell handler = vocab.dict[cursor];
		cfa_handler handler_fn = (cfa_handler)handler;

		int cell_count;
		if (handler == exit_handler)
			cell_count = 1;
		else if (handler_fn == docol && quotation_starts_at(cursor))
			cell_count = 1;
		else if (handler_fn == docol || handler_fn == dovar || handler_fn == dounit || handler_fn == dosym || handler_fn == dodefer)
			cell_count = 2;
		else
			cell_count = op_cell_count(cursor);

		if (fault_cell < cursor + cell_count) {
			if (handler_fn == docol || handler_fn == dovar || handler_fn == dounit || handler_fn == dodefer) {
				int target = (int)vocab.dict[cursor + 1];
				if (cell_count == 2 && target >= DICT_RESERVED && target < vocab.here)
					return &vocab.name_pool[WORD_NAME(target)];
				return NULL;
			}
			return handler_word_name(handler);
		}

		cursor += cell_count;
	}
	return NULL;
}

static void trace_write_location(Interpreter *interp, int *len, int cfa, int addr) {
	const WordLocation *location = word_location(cfa);
	if (!location)
		return;
	int line = cell_line_at(addr, cfa);
	if (line == 0)
		line = location->line;
	char located[PATH_MAX + 16];
	snprintf(located, sizeof located, " (%s:%d)", vocab.location_files[location->file], line);
	trace_write(interp, len, located);
}

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
	int fault_cell = interp->ip - 1;
	if (interp->running && fault_cell >= interp->trampoline_base
			&& fault_cell < interp->trampoline_base + 3) {
		const char *op_name = NULL;
		cell trampoline_handler = vocab.dict[interp->trampoline_base];
		if ((cfa_handler)trampoline_handler == docol) {
			int target = (int)vocab.dict[interp->trampoline_base + 1];
			if (target >= DICT_RESERVED && target < vocab.here)
				op_name = &vocab.name_pool[WORD_NAME(target)];
		} else {
			op_name = handler_word_name(trampoline_handler);
		}
		if (op_name) {
			trace_write(interp, &len, "in ");
			trace_write(interp, &len, op_name);
		}
	} else if (n_merged > 0 && frames[0].addr == interp->ip) {
		int body_start = frames[0].span ? frames[0].span->start_cfa + 1 : frames[0].cfa + 1;
		int body_end = frames[0].span ? frames[0].span->end_cfa : vocab.here;
		const char *op_name = running_op_name(interp->ip - 1, body_start, body_end);
		if (op_name && !frames[0].span
				&& strcmp(op_name, &vocab.name_pool[WORD_NAME(frames[0].cfa)]) == 0)
			op_name = NULL;
		if (op_name) {
			trace_write(interp, &len, "in ");
			trace_write(interp, &len, op_name);
		}
	}
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
		trace_write(interp, &len, (i == 0 && len == 0) ? "in " : " ← ");
		if (frames[i].span) {
			trace_write_snippet(interp, &len, frames[i].span->source_offset);
			int owner = word_containing(frames[i].addr);
			if (owner)
				trace_write_location(interp, &len, owner, frames[i].addr);
		} else {
			trace_write(interp, &len, &vocab.name_pool[WORD_NAME(frames[i].cfa)]);
			trace_write_location(interp, &len, frames[i].cfa, frames[i].addr);
		}
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
	compiler.error_located = 0;
	capture_error_trace(interp);
}

const char *tag_name(Tag t) {
	switch (t) {
		case T_NONE:   return "null";
		case T_SYMBOL:    return "a symbol";
		case T_FLOAT:  return "a float";
		case T_STRING: return "a string";
		case T_SET:    return "a set";
		case T_ARRAY:  return "an array";
		case T_FRAME:  return "a frame";
		case T_MATRIX: return "a matrix";
		case T_XT:     return "an execution token";
		case T_CURRIED: return "an execution token";
		case T_ADDR:   return "an address";
		case T_STREAM: return "a stream";
		case T_CONT:   return "a continuation";
		case T_MARK:   return "a mark";
		case T_LOGIC_VAR: return "a logic variable";
		case T_DB: return "a database";
		case T_PTR: return "a pointer";
		case T_SEGMENT: return "a segment";
		case T_QUANTITY: return "a quantity";
		case T_EXACT:  return "an exact";
		case T_COMPLEX: return "a complex";
		case T_REST:   return "a rest pattern";
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

void p_tailcall(DISPATCH_ARGS) {
	int target_cfa = (int)*chain_ip;

	unwind_locals_scopes(interp);

	DISPATCH_REGISTERS(interp, vocab.dict + target_cfa + 1, chain_sp);
}

void p_stop(DISPATCH_ARGS) {
	interp->running = 0;
}

void p_alloc_stats(DISPATCH_ARGS) {
	printf("lvars=%ld arrays=%ld\n", alloc_count_lvar, alloc_count_array);
	alloc_count_lvar = 0;
	alloc_count_array = 0;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
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
	REQUIRE_STACK_DEPTH(interp, chain_ip + 1, chain_sp, 1);
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
	interp->return_stack[interp->rsp++] = make_locals_header(interp->local_base, n_locals,
			(int)((chain_ip - 1) - vocab.dict));
	interp->rsp += n_locals;
	interp->local_base = interp->rsp - n_locals;
	for (int i = 0; i < n_locals; i++)
		interp->return_stack[interp->local_base + i] = make_tagged(T_NONE, 0);

	DISPATCH_REGISTERS(interp, chain_ip + 1, chain_sp);
}

void p_enter_locals_to(DISPATCH_ARGS) {
	int n_locals = (int)chain_ip[0];
	int n_received = (int)chain_ip[1];

	if (interp->loop_local_refill) {
		interp->loop_local_refill = 0;
		if (unlikely(chain_sp - n_received < interp->data_stack)) {
			SYNC_REGISTERS(interp, chain_ip + 2, chain_sp);
			fail(interp, "insufficient values on data stack; need %d", n_received);
			return;
		}
		Val *incoming = chain_sp - n_received;
		for (int i = 0; i < n_received; i++)
			interp->return_stack[interp->local_base + i] = incoming[i];

		DISPATCH_REGISTERS(interp, chain_ip + 2, incoming);
	}

	if (interp->rsp + n_locals + 1 > RETURN_STACK_DEPTH) {
		SYNC_REGISTERS(interp, chain_ip + 2, chain_sp);
		fail(interp, "return stack overflow");
		return;
	}
	if (chain_sp - n_received < interp->data_stack) {
		SYNC_REGISTERS(interp, chain_ip + 2, chain_sp);
		fail(interp, "insufficient values on data stack; need %d", n_received);
		return;
	}

	interp->return_stack[interp->rsp++] = make_locals_header(interp->local_base, n_locals,
			(int)((chain_ip - 1) - vocab.dict));
	Val *incoming = chain_sp - n_received;
	for (int i = 0; i < n_received; i++)
		interp->return_stack[interp->rsp + i] = incoming[i];

	interp->local_base = interp->rsp;
	interp->rsp += n_locals;
	for (int i = n_received; i < n_locals; i++)
		interp->return_stack[interp->local_base + i] = make_tagged(T_NONE, 0);

	DISPATCH_REGISTERS(interp, chain_ip + 2, incoming);
}

void p_enter_locals_mixed(DISPATCH_ARGS) {
	int n_locals = (int)chain_ip[0];
	int n_received = (int)chain_ip[1];

	if (interp->loop_local_refill) {
		interp->loop_local_refill = 0;
		if (unlikely(chain_sp - n_received < interp->data_stack)) {
			SYNC_REGISTERS(interp, chain_ip + 2 + n_received, chain_sp);
			fail(interp, "insufficient values on data stack; need %d", n_received);
			return;
		}
		Val *incoming = chain_sp - n_received;

		for (int i = 0; i < n_received; i++)
			interp->return_stack[interp->local_base + (int)chain_ip[2 + i]] = incoming[i];

		DISPATCH_REGISTERS(interp, chain_ip + 2 + n_received, incoming);
	}

	if (interp->rsp + n_locals + 1 > RETURN_STACK_DEPTH) {
		SYNC_REGISTERS(interp, chain_ip + 2 + n_received, chain_sp);
		fail(interp, "return stack overflow");
		return;
	}
	if (chain_sp - n_received < interp->data_stack) {
		SYNC_REGISTERS(interp, chain_ip + 2 + n_received, chain_sp);
		fail(interp, "insufficient values on data stack; need %d", n_received);
		return;
	}

	interp->return_stack[interp->rsp++] = make_locals_header(interp->local_base, n_locals,
			(int)((chain_ip - 1) - vocab.dict));
	interp->local_base = interp->rsp;
	interp->rsp += n_locals;
	for (int i = 0; i < n_locals; i++)
		interp->return_stack[interp->local_base + i] = make_tagged(T_NONE, 0);

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

void p_do_enter(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip + 4, chain_sp, 3);
	Val start_val = chain_sp[-3];
	Val limit_val = chain_sp[-2];
	Val delta_val = chain_sp[-1];
	if (VAL_TAG(start_val) != T_FLOAT || VAL_TAG(limit_val) != T_FLOAT
			|| VAL_TAG(delta_val) != T_FLOAT) {
		Val offending = VAL_TAG(start_val) != T_FLOAT ? start_val
			: VAL_TAG(limit_val) != T_FLOAT ? limit_val : delta_val;
		SYNC_REGISTERS(interp, chain_ip + 4, chain_sp);
		fail(interp, "expected float start, limit, and delta; got %s", tag_name(VAL_TAG(offending)));
		return;
	}

	double delta = VAL_NUMBER(delta_val);
	if (delta == 0.0) {
		SYNC_REGISTERS(interp, chain_ip + 4, chain_sp);
		fail(interp, "expected a nonzero delta; got 0");
		return;
	}

	double trip_count = ceil((VAL_NUMBER(limit_val) - VAL_NUMBER(start_val)) / delta);
	if (trip_count >= 9007199254740992.0) {
		SYNC_REGISTERS(interp, chain_ip + 4, chain_sp);
		fail(interp, "loop would run %.3g times (max 2^53)", trip_count);
		return;
	}

	Val *locals = interp->return_stack + interp->local_base;
	locals[(int)chain_ip[0]] = start_val;
	locals[(int)chain_ip[1]] = make_float(trip_count);
	locals[(int)chain_ip[2]] = delta_val;

	cell *continue_ip = trip_count > 0.0 ? chain_ip + 4 : chain_ip + 3 + (int)chain_ip[3];

	DISPATCH_REGISTERS(interp, continue_ip, chain_sp - 3);
}

void p_do_loop(DISPATCH_ARGS) {
	Val *locals = interp->return_stack + interp->local_base;
	locals[(int)chain_ip[0]].number += locals[(int)chain_ip[2]].number;
	double remaining = locals[(int)chain_ip[1]].number - 1.0;
	locals[(int)chain_ip[1]].number = remaining;

	cell *continue_ip = remaining > 0.0 ? chain_ip + 3 + (int)chain_ip[3] : chain_ip + 4;

	DISPATCH_REGISTERS(interp, continue_ip, chain_sp);
}

#define LOCAL_ARITH_0DEPTH(name, word_name, expr) \
	void name(DISPATCH_ARGS) { \
		Val *p = &interp->return_stack[interp->local_base + (int)chain_ip[0]]; \
		if (VAL_TAG(*p) != T_FLOAT) { \
			SYNC_REGISTERS(interp, chain_ip + 1, chain_sp); \
			fail(interp, "expected a float local; got %s", tag_name(VAL_TAG(*p))); \
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

#define STACK_LOCAL_STORE_OP(suffix, op) \
	static int sl_##suffix##_store_cfa; \
	static void p_sl_##suffix##_store(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip + 2, chain_sp, 1); \
		Val *locals = interp->return_stack + interp->local_base; \
		double a = chain_sp[-1].number; \
		double b = locals[(int)chain_ip[0]].number; \
		locals[(int)chain_ip[1]] = make_float(a op b); \
		DISPATCH_REGISTERS(interp, chain_ip + 2, chain_sp - 1); \
	}
STACK_LOCAL_STORE_OP(add, +)
STACK_LOCAL_STORE_OP(sub, -)
STACK_LOCAL_STORE_OP(mul, *)
STACK_LOCAL_STORE_OP(div, /)

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
	static int ll_##suffix##_0_store_cfa; \
	static void p_ll_##suffix##_0(DISPATCH_ARGS) { \
		REQUIRE_STACK_ROOM(interp, chain_ip + 2, chain_sp, 1); \
		Val *locals = interp->return_stack + interp->local_base; \
		double a = locals[(int)chain_ip[0]].number; \
		double b = locals[(int)chain_ip[1]].number; \
		*chain_sp = make_float(a op b); \
		DISPATCH_REGISTERS(interp, chain_ip + 2, chain_sp + 1); \
	} \
	static void p_ll_##suffix##_0_store(DISPATCH_ARGS) { \
		Val *locals = interp->return_stack + interp->local_base; \
		double a = locals[(int)chain_ip[0]].number; \
		double b = locals[(int)chain_ip[1]].number; \
		locals[(int)chain_ip[2]] = make_float(a op b); \
		DISPATCH_REGISTERS(interp, chain_ip + 3, chain_sp); \
	}
LOCAL_LOCAL_OP(add, +)
LOCAL_LOCAL_OP(sub, -)
LOCAL_LOCAL_OP(mul, *)

#define LOCAL_LIT_OP(suffix, op) \
	static int ll_lit_##suffix##_0_cfa; \
	static int ll_lit_##suffix##_0_store_cfa; \
	static void p_ll_lit_##suffix##_0(DISPATCH_ARGS) { \
		REQUIRE_STACK_ROOM(interp, chain_ip + 2, chain_sp, 1); \
		Val lit; \
		lit.bits = (uint64_t)chain_ip[1]; \
		double a = interp->return_stack[interp->local_base + (int)chain_ip[0]].number; \
		*chain_sp = make_float(a op lit.number); \
		DISPATCH_REGISTERS(interp, chain_ip + 2, chain_sp + 1); \
	} \
	static void p_ll_lit_##suffix##_0_store(DISPATCH_ARGS) { \
		Val lit; \
		lit.bits = (uint64_t)chain_ip[1]; \
		Val *locals = interp->return_stack + interp->local_base; \
		double a = locals[(int)chain_ip[0]].number; \
		locals[(int)chain_ip[2]] = make_float(a op lit.number); \
		DISPATCH_REGISTERS(interp, chain_ip + 3, chain_sp); \
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

static int ll_litrev_sub_0_store_cfa;
static void p_ll_litrev_sub_0_store(DISPATCH_ARGS) {
	Val lit;
	lit.bits = (uint64_t)chain_ip[1];
	Val *locals = interp->return_stack + interp->local_base;
	double a = locals[(int)chain_ip[0]].number;
	locals[(int)chain_ip[2]] = make_float(lit.number - a);
	DISPATCH_REGISTERS(interp, chain_ip + 3, chain_sp);
}

static int array_lit_cfa;
static int at_i_local0_cfa;
static int at_i_lit_cfa;
static int at_i_lit_local0_cfa;
static int gather_local0_cfa;
static int at_i_ll0_cfa;
static int at_e_lit_cfa;
static int at_e_local0_cfa;
static int at_e_ll0_cfa;
static int at_e_lit_local0_cfa;
static int gather_e_local0_cfa;
static int at_e_swap_l0_cfa;
static int at_e_depth_cfa;
static int at_e_depth_top_cfa;
static int at_i_swap_l0_cfa;
static int at_i_depth_cfa;
static int at_i_depth_top_cfa;
static int add_f_depth_cfa;
static int sub_f_depth_cfa;
static int mul_f_depth_cfa;
static int div_f_depth_cfa;
static int pick_n_cfa;
static int load2_cfa, load3_cfa;

int try_fuse_stack_local_store(Interpreter *interp, int depth, int slot) {
	if (depth != 0)
		return 0;

	cell *dict = vocab.dict;
	int here = vocab.here;
	if (here < 3 || here - 3 < compiler.fuse_floor)
		return 0;
	if (!dict_is_handler[here - 1] || !dict_is_handler[here - 3])
		return 0;
	if (!dict_op_is(here - 3, p_local_fetch_0depth))
		return 0;

	cfa_handler binop = (cfa_handler)dict[here - 1];
	int store_cfa;
	if (binop == p_add_f) store_cfa = sl_add_store_cfa;
	else if (binop == p_sub_f) store_cfa = sl_sub_store_cfa;
	else if (binop == p_mul_f) store_cfa = sl_mul_store_cfa;
	else if (binop == p_div_f) store_cfa = sl_div_store_cfa;
	else return 0;

	cell source_slot = dict[here - 2];

	vocab.here -= 3;
	emit_call(interp, store_cfa);
	emit(interp, source_slot);
	emit(interp, (cell)slot);

	return 1;
}

int try_fuse_local_arith_store(Interpreter *interp, int depth, int slot) {
	if (depth != 0)
		return 0;

	cell *dict = vocab.dict;
	int here = vocab.here;
	if (here < 3 || here - 3 < compiler.fuse_floor)
		return 0;
	if (!dict_is_handler[here - 3])
		return 0;

	cfa_handler fused = (cfa_handler)dict[here - 3];
	int store_cfa;
	if (fused == p_ll_add_0) store_cfa = ll_add_0_store_cfa;
	else if (fused == p_ll_sub_0) store_cfa = ll_sub_0_store_cfa;
	else if (fused == p_ll_mul_0) store_cfa = ll_mul_0_store_cfa;
	else if (fused == p_ll_lit_add_0) store_cfa = ll_lit_add_0_store_cfa;
	else if (fused == p_ll_lit_sub_0) store_cfa = ll_lit_sub_0_store_cfa;
	else if (fused == p_ll_lit_mul_0) store_cfa = ll_lit_mul_0_store_cfa;
	else if (fused == p_ll_litrev_sub_0) store_cfa = ll_litrev_sub_0_store_cfa;
	else return 0;

	cell first_operand = dict[here - 2];
	cell second_operand = dict[here - 1];

	vocab.here -= 3;
	emit_call(interp, store_cfa);
	emit(interp, first_operand);
	emit(interp, second_operand);
	emit(interp, (cell)slot);

	return 1;
}

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

int fuse_rewrite(Interpreter *interp, int n_replaced_cells, int fused_cfa, cell operand) {
	vocab.here -= n_replaced_cells;
	emit_call(interp, fused_cfa);
	emit(interp, operand);
	return 1;
}

int fuse_rewrite_pair(Interpreter *interp, int n_replaced_cells, int fused_cfa, cell operand_a, cell operand_b) {
	vocab.here -= n_replaced_cells;
	emit_call(interp, fused_cfa);
	emit(interp, operand_a);
	emit(interp, operand_b);
	return 1;
}

static int try_fuse_operand_op(Interpreter *interp, cfa_handler producer, int fused_cfa) {
	if (!compiler.compiling)
		return 0;

	cell *dict = vocab.dict;
	int here = vocab.here;
	if (here < 2)
		return 0;
	if (here - 2 < compiler.fuse_floor)
		return 0;
	if (!dict_op_is(here - 2, producer))
		return 0;

	return fuse_rewrite(interp, 2, fused_cfa, dict[here - 1]);
}

static int try_fuse_literal_index_op(Interpreter *interp, int fused_cfa) {
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

	return fuse_rewrite(interp, 2, fused_cfa, (cell)(int)VAL_NUMBER(literal));
}

int try_fuse_at_i_local(Interpreter *interp) {
	return try_fuse_operand_op(interp, p_local_fetch_0depth, at_i_local0_cfa);
}

int try_fuse_array_literal(Interpreter *interp) {
	if (!compiler.compiling)
		return 0;

	cell *dict = vocab.dict;
	int here = vocab.here;
	if (here < 2 || here - 2 < compiler.fuse_floor)
		return 0;
	if (!dict_op_is(here - 2, p_literal))
		return 0;

	Val count_val;
	count_val.bits = (uint64_t)dict[here - 1];
	if (VAL_TAG(count_val) != T_FLOAT)
		return 0;

	double count = VAL_NUMBER(count_val);
	if (count < 1 || count > (double)DATA_STACK_DEPTH || count != (double)(int)count)
		return 0;

	return fuse_rewrite(interp, 2, array_lit_cfa, (cell)(int)count);
}

int try_fuse_gather_local(Interpreter *interp) {
	return try_fuse_operand_op(interp, p_at_i_local0, gather_local0_cfa);
}

int try_fuse_at_e_local(Interpreter *interp) {
	return try_fuse_operand_op(interp, p_local_fetch_0depth, at_e_local0_cfa);
}

int try_fuse_gather_e_local(Interpreter *interp) {
	return try_fuse_operand_op(interp, p_at_e_local0, gather_e_local0_cfa);
}

static int try_fuse_two_local_op(Interpreter *interp, int ll0_cfa) {
	if (!compiler.compiling)
		return 0;

	cell *dict = vocab.dict;
	int here = vocab.here;

	if (here >= 3 && here - 3 >= compiler.fuse_floor
	    && here - 3 == compiler.loadn_at
	    && dict_op_is(here - 3, p_load2))
		return fuse_rewrite_pair(interp, 3, ll0_cfa, dict[here - 2], dict[here - 1]);

	if (here >= 4 && here - 4 >= compiler.fuse_floor
	    && here - 4 == compiler.loadn_at
	    && dict_op_is(here - 4, p_load3)) {
		cell lead_slot = dict[here - 3];
		cell base_slot = dict[here - 2];
		cell idx_slot = dict[here - 1];
		vocab.here -= 4;
		emit_call(interp, vocab.local_fetch_0depth_cfa);
		emit(interp, lead_slot);
		return fuse_rewrite_pair(interp, 0, ll0_cfa, base_slot, idx_slot);
	}

	if (here >= 4 && here - 4 >= compiler.fuse_floor
	    && dict_op_is(here - 4, p_local_fetch_0depth)
	    && dict_op_is(here - 2, p_local_fetch_0depth))
		return fuse_rewrite_pair(interp, 4, ll0_cfa, dict[here - 3], dict[here - 1]);

	return 0;
}

int try_fuse_at_i_ll(Interpreter *interp) {
	return try_fuse_two_local_op(interp, at_i_ll0_cfa);
}

static int try_fuse_swap_local_index(Interpreter *interp, int fused_cfa) {
	if (!compiler.compiling)
		return 0;

	cell *dict = vocab.dict;
	int here = vocab.here;
	if (here < 3 || here - 3 < compiler.fuse_floor)
		return 0;

	if (!dict_op_is(here - 1, p_swap))
		return 0;

	if (dict_op_is(here - 3, p_local_fetch_0depth))
		return fuse_rewrite(interp, 3, fused_cfa, dict[here - 2]);

	return 0;
}

int try_fuse_at_i_swap_local(Interpreter *interp) {
	return try_fuse_swap_local_index(interp, at_i_swap_l0_cfa);
}

int try_fuse_at_e_swap_local(Interpreter *interp) {
	return try_fuse_swap_local_index(interp, at_e_swap_l0_cfa);
}

int try_fuse_pick_literal(Interpreter *interp) {
	if (!compiler.compiling)
		return 0;

	cell *dict = vocab.dict;
	int here = vocab.here;
	if (here < 2 || here - 2 < compiler.fuse_floor)
		return 0;

	if (!dict_op_is(here - 2, p_literal))
		return 0;

	Val depth_val;
	depth_val.bits = (uint64_t)dict[here - 1];
	if (VAL_TAG(depth_val) != T_FLOAT)
		return 0;

	int depth = (int)VAL_NUMBER(depth_val);
	if (depth < 0)
		return 0;

	return fuse_rewrite(interp, 2, pick_n_cfa, depth);
}

static int last_stack_read(int here, int *depth_out, int *cells_out) {
	if (dict_op_is(here - 1, p_dup)) {
		*depth_out = 0;
		*cells_out = 1;
		return 1;
	}
	if (dict_op_is(here - 1, p_over)) {
		*depth_out = 1;
		*cells_out = 1;
		return 1;
	}
	if (here >= 2 && dict_op_is(here - 2, p_pick_n)) {
		*depth_out = (int)vocab.dict[here - 1];
		*cells_out = 2;
		return 1;
	}

	return 0;
}

static int try_fuse_depth_op(Interpreter *interp, int fused_cfa) {
	if (!compiler.compiling)
		return 0;

	int depth, cells;
	if (!last_stack_read(vocab.here, &depth, &cells))
		return 0;
	if (vocab.here - cells < compiler.fuse_floor)
		return 0;

	return fuse_rewrite(interp, cells, fused_cfa, depth);
}

int try_fuse_float_depth(Interpreter *interp, int op_cfa) {
	int fused_cfa = 0;
	if (op_cfa == vocab.add_f_cfa)
		fused_cfa = add_f_depth_cfa;
	else if (op_cfa == vocab.sub_f_cfa)
		fused_cfa = sub_f_depth_cfa;
	else if (op_cfa == vocab.mul_f_cfa)
		fused_cfa = mul_f_depth_cfa;
	else if (op_cfa == vocab.div_f_cfa)
		fused_cfa = div_f_depth_cfa;

	if (!fused_cfa)
		return 0;

	return try_fuse_depth_op(interp, fused_cfa);
}

static int try_fuse_depth_index(Interpreter *interp, int depth_top_cfa, int depth_cfa) {
	if (!compiler.compiling)
		return 0;

	int array_depth, array_cells;

	if (dict_op_is(vocab.here - 1, p_swap)) {
		if (!last_stack_read(vocab.here - 1, &array_depth, &array_cells))
			return 0;
		if (vocab.here - 1 - array_cells < compiler.fuse_floor)
			return 0;

		return fuse_rewrite(interp, array_cells + 1, depth_top_cfa, array_depth);
	}

	int index_depth, index_cells;
	if (!last_stack_read(vocab.here, &index_depth, &index_cells))
		return 0;

	if (!last_stack_read(vocab.here - index_cells, &array_depth, &array_cells))
		return 0;

	if (vocab.here - index_cells - array_cells < compiler.fuse_floor)
		return 0;

	if (index_depth < 1)
		return 0;

	return fuse_rewrite_pair(interp, index_cells + array_cells, depth_cfa,
			array_depth, index_depth - 1);
}

int try_fuse_at_i_depth(Interpreter *interp) {
	return try_fuse_depth_index(interp, at_i_depth_top_cfa, at_i_depth_cfa);
}

int try_fuse_at_e_depth(Interpreter *interp) {
	return try_fuse_depth_index(interp, at_e_depth_top_cfa, at_e_depth_cfa);
}

int try_fuse_at_e_ll(Interpreter *interp) {
	return try_fuse_two_local_op(interp, at_e_ll0_cfa);
}

typedef struct {
	cfa_handler arith_handler;
	int *local_cfa;
	int *lit_cfa;
	int *litrev_cfa;
} LocalArithFusion;

static const LocalArithFusion local_arith_fusions[] = {
	{ p_add_f, &ll_add_0_cfa, &ll_lit_add_0_cfa, &ll_lit_add_0_cfa },
	{ p_sub_f, &ll_sub_0_cfa, &ll_lit_sub_0_cfa, &ll_litrev_sub_0_cfa },
	{ p_mul_f, &ll_mul_0_cfa, &ll_lit_mul_0_cfa, &ll_lit_mul_0_cfa },
};

int try_fuse_local_arith(Interpreter *interp, cfa_handler op_handler) {
	if (!compiler.compiling)
		return 0;

	const LocalArithFusion *fusion = NULL;
	for (size_t i = 0; i < sizeof local_arith_fusions / sizeof local_arith_fusions[0]; i++)
		if (local_arith_fusions[i].arith_handler == op_handler) {
			fusion = &local_arith_fusions[i];
			break;
		}
	if (!fusion)
		return 0;

	cell *dict = vocab.dict;
	int here = vocab.here;

	if (here >= 3 && here - 3 >= compiler.fuse_floor
	    && here - 3 == compiler.loadn_at
	    && dict_op_is(here - 3, p_load2))
		return fuse_rewrite_pair(interp, 3, *fusion->local_cfa, dict[here - 2], dict[here - 1]);

	if (here < 4)
		return 0;
	if (here - 4 < compiler.fuse_floor)
		return 0;

	if (!dict_is_handler[here - 4] || !dict_is_handler[here - 2])
		return 0;
	cfa_handler deep = (cfa_handler)dict[here - 4];
	cfa_handler top = (cfa_handler)dict[here - 2];

	if (deep == p_local_fetch_0depth && top == p_local_fetch_0depth)
		return fuse_rewrite_pair(interp, 4, *fusion->local_cfa, dict[here - 3], dict[here - 1]);

	if (deep == p_local_fetch_0depth && top == p_literal) {
		Val lit;
		lit.bits = (uint64_t)dict[here - 1];
		if (VAL_TAG(lit) != T_FLOAT)
			return 0;
		return fuse_rewrite_pair(interp, 4, *fusion->lit_cfa, dict[here - 3], (cell)lit.bits);
	}

	if (deep == p_literal && top == p_local_fetch_0depth) {
		Val lit;
		lit.bits = (uint64_t)dict[here - 3];
		if (VAL_TAG(lit) != T_FLOAT)
			return 0;
		return fuse_rewrite_pair(interp, 4, *fusion->litrev_cfa, dict[here - 1], (cell)lit.bits);
	}

	return 0;
}


static int try_fuse_index_lit(Interpreter *interp, int lit_local0_cfa, int lit_cfa) {
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
	    && dict_op_is(here - 4, p_local_fetch_0depth))
		return fuse_rewrite_pair(interp, 4, lit_local0_cfa, dict[here - 3], (cell)index);

	return try_fuse_literal_index_op(interp, lit_cfa);
}

int try_fuse_at_i_lit(Interpreter *interp) {
	return try_fuse_index_lit(interp, at_i_lit_local0_cfa, at_i_lit_cfa);
}

int try_fuse_at_e_lit(Interpreter *interp) {
	return try_fuse_index_lit(interp, at_e_lit_local0_cfa, at_e_lit_cfa);
}

void inbuf_reset(void) {
	compiler.input_buffer_len = 0;
	compiler.input_buffer_pos = 0;
	compiler.input_buffer[0] = 0;
	compiler.need_more = 0;
}

int refill_input(void) {
	if (compiler.load_depth > 0 || compiler.nested_input_depth > 0)
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
	       && isspace((unsigned char)compiler.input_buffer[compiler.input_buffer_pos])) {
		if (compiler.input_buffer[compiler.input_buffer_pos] == '\n')
			compiler.input_line++;
		compiler.input_buffer_pos++;
	}

	if (compiler.input_buffer_pos >= compiler.input_buffer_len)
		return NULL;

	int start = compiler.input_buffer_pos;
	const char *buffer = compiler.input_buffer;
	char lead = buffer[start];
	char after_lead = start + 1 < compiler.input_buffer_len ? buffer[start + 1] : 0;

	if (lead == ';' || lead == ']' || lead == '}') {
		compiler.input_buffer_pos++;
	} else if ((lead == ':' || lead == '>') && after_lead == ']') {
		compiler.input_buffer_pos += 2;
	} else if (lead == '[') {
		int two_char_opener = after_lead == ':' || after_lead == '<';
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
				if ((preceding == ':' || preceding == '>')
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

	const char *digits = (text[0] == '-' || text[0] == '+') ? text + 1 : text;
	if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
		if (!isxdigit((unsigned char)digits[2]))
			return 0;

		char *end_of_hex;
		errno = 0;
		unsigned long long magnitude = strtoull(digits + 2, &end_of_hex, 16);
		if (*end_of_hex != 0 || errno == ERANGE || magnitude > (1ULL << 53))
			return 0;

		*out = text[0] == '-' ? -(double)magnitude : (double)magnitude;
		return 1;
	}

	char *end_of_number;
	double value = strtod(text, &end_of_number);
	if (*end_of_number != 0)
		return 0;
	*out = value;
	return 1;
}

static void skip_whitespace(void) {
	while (compiler.input_buffer_pos < compiler.input_buffer_len
			&& isspace((unsigned char)compiler.input_buffer[compiler.input_buffer_pos])) {
		if (compiler.input_buffer[compiler.input_buffer_pos] == '\n')
			compiler.input_line++;
		compiler.input_buffer_pos++;
	}
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
	ITEMS_GROW_IF_FULL(path);
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
	*depth_out = 0;
	*slot_out = 0;

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
			compiler.found_local_name_idx = name_idx;
			compiler.found_local_scope = scope;
			return 1;
		}
	}
	return 0;
}

void emit_local_fetch(Interpreter *interp, int local_depth, int local_slot_idx) {
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
	} else {
		emit_call(interp, vocab.local_fetch_cfa);
		emit(interp, (cell)local_depth);
		emit(interp, (cell)local_slot_idx);
	}
	compiler.fuse_prev_var = 0;
	compiler.fuse_prev2_var = 0;
}

typedef struct {
	const char *token;
	int token_len;
	int allowed_distance;
	const char *name;
	int distance;
	cell uses;
	int prefix_len;
} SuggestionSearch;

static int common_prefix_length(const char *token, const char *name) {
	int length = 0;
	while (token[length] && token[length] == name[length])
		length++;
	return length;
}

static void suggestion_consider(Interpreter *interp, SuggestionSearch *search, const char *name, cell uses) {
	int name_len = (int)strlen(name);
	int length_gap = name_len - search->token_len;
	if (length_gap > search->allowed_distance || -length_gap > search->allowed_distance)
		return;

	int distance = string_edit_distance(interp, search->token, search->token_len, name, name_len);
	if (distance > search->allowed_distance)
		return;

	int prefix_len = common_prefix_length(search->token, name);
	if (search->name) {
		if (distance > search->distance)
			return;
		if (distance == search->distance) {
			if (uses < search->uses)
				return;
			if (uses == search->uses && prefix_len <= search->prefix_len)
				return;
		}
	}

	search->name = name;
	search->distance = distance;
	search->uses = uses;
	search->prefix_len = prefix_len;
}

static const char *nearest_word_name(Interpreter *interp, const char *token) {
	int token_len = (int)strlen(token);
	if (token_len <= 1)
		return NULL;

	SuggestionSearch search = {
		.token = token,
		.token_len = token_len,
		.allowed_distance = token_len == 2 ? 1 : 2,
		.name = NULL,
		.distance = 0,
		.uses = 0,
		.prefix_len = 0,
	};

	for (int cfa = vocab.latest_cfa; cfa != 0; cfa = (int)WORD_LINK(cfa)) {
		if (WORD_IS_INTERNAL(cfa))
			continue;
		suggestion_consider(interp, &search, &vocab.name_pool[WORD_NAME(cfa)], WORD_USE_COUNT(cfa));
		if (interp->error_flag)
			return NULL;
	}
	if (compiler.compiling)
		for (int i = 0; i < compiler.n_local_names; i++) {
			suggestion_consider(interp, &search, &compiler.local_names_pool[compiler.local_name_offsets[i]], 0);
			if (interp->error_flag)
				return NULL;
		}

	return search.name;
}

#define FRAME_KEY_STEPS_MAX 8

static int try_frame_key_token(Interpreter *interp, const char *token) {
	const char *first_operator = strpbrk(token, "@!");
	if (!first_operator || first_operator[1] == '\0')
		return 0;

	char text[INPUT_BUFFER_SIZE];
	strncpy(text, token, sizeof(text) - 1);
	text[sizeof(text) - 1] = '\0';

	char *cursor = text + (first_operator - token);
	char *keys[FRAME_KEY_STEPS_MAX];
	char operators[FRAME_KEY_STEPS_MAX];
	int n_steps = 0;

	while (*cursor) {
		if (n_steps == FRAME_KEY_STEPS_MAX)
			return 0;
		operators[n_steps] = *cursor;
		*cursor++ = '\0';

		char *key = cursor;
		while (*cursor && *cursor != '@' && *cursor != '!')
			cursor++;
		if (key == cursor)
			return 0;
		keys[n_steps++] = key;
	}

	for (int step = 0; step < n_steps - 1; step++)
		if (operators[step] == '!')
			return 0;

	int left_depth;
	int left_slot;
	int left_is_local = text[0] && compiler.compiling && find_local(text, &left_depth, &left_slot);
	int left_cfa = (text[0] && !left_is_local) ? find(text) : 0;
	if (text[0] && !left_is_local && !left_cfa)
		return 0;

	if (left_is_local) {
		if (reject_outer_local(interp, text))
			return 0;
		compiler.local_fetched[compiler.found_local_name_idx] = 1;
		emit_local_fetch(interp, left_depth, left_slot);
	} else if (left_cfa) {
		WORD_USE_INCREMENT(left_cfa);
		if (compiler.compiling)
			emit_call(interp, (cell)left_cfa);
		else
			execute_cfa(interp, left_cfa);
		if (interp->error_flag)
			return 1;
	}

	for (int step = 0; step < n_steps; step++) {
		int key = intern_symbol(interp, keys[step]);
		if (interp->error_flag)
			return 1;

		if (compiler.compiling) {
			emit_call(interp, operators[step] == '@'
					? vocab.frame_get_inline_key_cfa : vocab.frame_set_inline_key_cfa);
			emit(interp, (cell)key);
			continue;
		}

		if (operators[step] == '@') {
			push(interp, make_symbol(key));
			execute_cfa(interp, find("@"));
		} else {
			Val target = pop(interp);
			if (interp->error_flag)
				return 1;
			Val stored = pop(interp);
			if (interp->error_flag)
				return 1;
			push(interp, target);
			push(interp, stored);
			push(interp, make_symbol(key));
			execute_cfa(interp, find("!"));
			if (interp->error_flag)
				return 1;
			pop(interp);
		}
		if (interp->error_flag)
			return 1;
	}

	compiler.fuse_prev_var = 0;
	compiler.fuse_prev2_var = 0;
	compiler.fuse_prev_cmp = 0;
	return 1;
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
				if (reject_outer_local(interp, tok))
					return;
				compiler.local_fetched[compiler.found_local_name_idx] = 1;
				emit_local_fetch(interp, local_depth, local_slot_idx);
				continue;
			}
		}

		int cf = find(tok);
		if (cf) {
			WORD_USE_INCREMENT(cf);
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
							|| cf == vocab.gt_f_cfa
							|| cf == vocab.lte_cfa || cf == vocab.gte_cfa
							|| cf == vocab.lte_f_cfa || cf == vocab.gte_f_cfa)
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

		Val exact_literal;
		if (parse_exact_literal(interp, tok, &exact_literal)) {
			if (interp->error_flag)
				return;
			compile_or_push(interp, exact_literal);
			continue;
		}

		double parsed_number;
		if (parse_float(tok, &parsed_number)) {
			compile_or_push(interp, make_float(parsed_number));
			continue;
		}

		Val complex_literal;
		if (parse_complex_literal(interp, tok, &complex_literal)) {
			if (interp->error_flag)
				return;
			compile_or_push(interp, complex_literal);
			continue;
		}

		if (try_frame_key_token(interp, tok))
			continue;

		const char *nearest_name = nearest_word_name(interp, tok);
		if (interp->error_flag)
			return;
		if (nearest_name)
			fail(interp, "unknown word: %s (did you mean %s?)", tok, nearest_name);
		else
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
		fail(interp, "%d-file history limit reached", MAX_LOADED_FILES);
		return;
	}
	compiler.loaded_files[compiler.n_loaded_files] = strdup(filename);
	compiler.n_loaded_files++;
}

static void run_input_text(Interpreter *interp, const char *text, int length, const char *origin) {
	char *saved_contents = xmalloc((size_t)compiler.input_buffer_len + 1);
	memcpy(saved_contents, compiler.input_buffer, (size_t)compiler.input_buffer_len);
	int saved_len = compiler.input_buffer_len;
	int saved_pos = compiler.input_buffer_pos;
	int saved_need_more = compiler.need_more;

	memcpy(compiler.input_buffer, text, (size_t)length);
	compiler.input_buffer[length] = 0;
	compiler.input_buffer_len = length;
	compiler.input_buffer_pos = 0;
	compiler.need_more = 0;

	const char *saved_load_file = compiler.current_load_file;
	int saved_cursor_pos = compiler.line_cursor_pos;
	int saved_cursor_line = compiler.line_cursor_line;
	compiler.current_load_file = origin;
	compiler.line_cursor_pos = 0;
	compiler.line_cursor_line = 1;
	compiler.nested_input_depth++;
	run_outer(interp);
	compiler.nested_input_depth--;
	compiler.current_load_file = saved_load_file;
	compiler.line_cursor_pos = saved_cursor_pos;
	compiler.line_cursor_line = saved_cursor_line;

	if (!interp->error_flag && compiler.need_more)
		fail(interp, "unterminated string literal");

	if (!interp->error_flag && compiler.compiling) {
		fail(interp, "unterminated definition");
		compiler.compiling = 0;
	}

	if (interp->error_flag) {
		rollback_partial_definition();
		if (origin && !compiler.error_located) {
			int line = 1;
			for (int i = 0; i < compiler.input_buffer_pos && i < compiler.input_buffer_len; i++)
				if (compiler.input_buffer[i] == '\n')
					line++;
			char located[sizeof interp->error_message];
			snprintf(located, sizeof located, "%s:%d: %s", origin, line, interp->error_message);
			memcpy(interp->error_message, located, sizeof interp->error_message);
			compiler.error_located = 1;
		}
	}

	memcpy(compiler.input_buffer, saved_contents, (size_t)saved_len);
	compiler.input_buffer[saved_len] = 0;
	compiler.input_buffer_len = saved_len;
	compiler.input_buffer_pos = saved_pos;
	compiler.need_more = saved_need_more;
	free(saved_contents);
}

void p_evaluate(DISPATCH_ARGS) {
	POP_STRING(source, "evaluate");

	if (source->len >= INPUT_BUFFER_SIZE) {
		fail(interp, "source too large (%d bytes, max %d)", source->len, INPUT_BUFFER_SIZE - 1);
		return;
	}

	run_input_text(interp, source->bytes, source->len, NULL);

	DISPATCH(interp);
}

void load_file(Interpreter *interp, const char *filename) {
	char resolved_path[PATH_MAX];
	const char *resolved = filename;

	FILE *file = fopen(filename, "r");
	if (!file && filename[0] != '/' && compiler.current_load_dir) {
		snprintf(resolved_path, sizeof resolved_path, "%s/%s", compiler.current_load_dir, filename);
		file = fopen(resolved_path, "r");
		if (file)
			resolved = resolved_path;
	}
	if (!file) {
		fail(interp, "cannot open %s", filename);
		return;
	}

	fseek(file, 0, SEEK_END);
	long file_size = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (file_size < 0 || file_size >= INPUT_BUFFER_SIZE) {
		fail(interp, "%s too large or invalid (%ld bytes, max %d)",
				resolved, file_size, INPUT_BUFFER_SIZE - 1);
		fclose(file);
		return;
	}

	char *contents = xmalloc((size_t)file_size + 1);
	size_t bytes_read = fread(contents, 1, (size_t)file_size, file);
	fclose(file);
	contents[bytes_read] = 0;

	if (bytes_read >= 2 && contents[0] == '#' && contents[1] == '!')
		for (size_t i = 0; i < bytes_read && contents[i] != '\n'; i++)
			contents[i] = ' ';

	char resolved_dir[PATH_MAX];
	const char *last_slash = strrchr(resolved, '/');
	if (last_slash) {
		size_t dir_len = (size_t)(last_slash - resolved);
		memcpy(resolved_dir, resolved, dir_len);
		resolved_dir[dir_len] = 0;
	} else {
		resolved_dir[0] = '.';
		resolved_dir[1] = 0;
	}

	int saved_unit = current_unit;
	current_unit = next_unit++;
	const char *saved_load_dir = compiler.current_load_dir;
	compiler.current_load_dir = resolved_dir;
	compiler.load_depth++;

	run_input_text(interp, contents, (int)bytes_read, resolved);

	compiler.load_depth--;
	compiler.current_load_dir = saved_load_dir;
	current_unit = saved_unit;
	free(contents);
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

#define MARK_MAX_C_DEPTH 512

static void mark_value_at(Interpreter *interp, Val value, int depth);

static inline void mark_child(Interpreter *interp, Val child, int depth) {
	if (depth < MARK_MAX_C_DEPTH) {
		mark_value_at(interp, child, depth + 1);
		return;
	}
	GROW_IF_FULL_SYS(interp->mark_worklist_count, interp->mark_worklist_cap, interp->mark_worklist);
	interp->mark_worklist[interp->mark_worklist_count++] = child;
}

void mark_value(Interpreter *interp, Val value) {
	mark_value_at(interp, value, 0);
	while (interp->mark_worklist_count > 0)
		mark_value_at(interp, interp->mark_worklist[--interp->mark_worklist_count], 0);
}

static void mark_value_at(Interpreter *interp, Val value, int depth) {
	for (;;) {
		if (VAL_TAG(value) == T_LOGIC_VAR) {
			value = interp->lvar_stack[VAL_DATA(value)];
			continue;
		}

		if (VAL_TAG(value) == T_REST) {
			if (rest_is_wildcard(value))
				return;
			value = interp->lvar_stack[VAL_DATA(value)];
			continue;
		}

		if (VAL_TAG(value) == T_PTR) {
			value = ffi_pointer_owner_of((int)VAL_DATA(value));
			continue;
		}

		if (VAL_TAG(value) != T_STRING &&
				VAL_TAG(value) != T_SET &&
				VAL_TAG(value) != T_ARRAY &&
				VAL_TAG(value) != T_CURRIED &&
				VAL_TAG(value) != T_FRAME &&
				VAL_TAG(value) != T_MATRIX &&
				VAL_TAG(value) != T_SEGMENT &&
				VAL_TAG(value) != T_CONT &&
				VAL_TAG(value) != T_EXACT &&
				VAL_TAG(value) != T_QUANTITY &&
				VAL_TAG(value) != T_COMPLEX) return;

		if (VAL_TAG(value) == T_QUANTITY || VAL_TAG(value) == T_COMPLEX) {
			int slot = (int)VAL_DATA(value);
			if (slot < interp->gc_pair_base)
				return;
			GC_ASSERT(!in_parallel || handle_in_chunks(slot, thread_alloc.pairs.chunks, thread_alloc.pairs.n_chunks, thread_alloc.pairs.next), "worker marked a quantity outside its own chunks");
			if (pairs.mark_epoch[slot] == interp->gc_epoch)
				return;
			pairs.mark_epoch[slot] = interp->gc_epoch;
			mark_child(interp, pairs.table[slot].head, depth);
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
				mark_child(interp, obj->items[i], depth);
			value = obj->items[obj->len - 1];
			continue;
		} else if (obj->kind == OBJECT_FRAME) {
			if (obj->len == 0)
				return;
			for (int i = 0; i < obj->len - 1; i++)
				mark_child(interp, obj->frame.values[i], depth);
			value = obj->frame.values[obj->len - 1];
			continue;
		} else if (obj->kind == OBJECT_CONTINUATION) {
			if (obj->continuation.return_len == 0)
				return;
			for (int i = 0; i < obj->continuation.return_len - 1; i++)
				mark_child(interp, obj->continuation.return_slice[i], depth);
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
		fail(interp, "structure too deeply nested (cycle?)");
		return;
	}

	source_val = deref(interp, source_val);

	switch(VAL_TAG(source_val)) {
		case T_LOGIC_VAR: {
							  *copy_val = varmap_lookup(interp, map, (int)VAL_DATA(source_val));
							  return;
						  }
		case T_REST: {
						 if (rest_is_wildcard(source_val) || map->reify) {
							 *copy_val = source_val;
							 return;
						 }
						 Val fresh = varmap_lookup(interp, map, (int)VAL_DATA(source_val));
						 *copy_val = make_rest(VAL_DATA(fresh));
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
						items_reserve(copy, source->len);

						memset(copy->items, 0, sizeof(Val) * (size_t)source->len);
						copy->len = source->len;
						*copy_val = (VAL_TAG(source_val) == T_ARRAY) ? make_array(copy_handle) : make_set(copy_handle);
						for (i = 0; i < source->len; i++)
							copy_value_inner(interp, map, source->items[i], &copy->items[i], depth + 1);
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

	if (handler == (cell)p_do_enter || handler == (cell)p_do_loop)
		return 5;

	if (handler == vocab.dict[vocab.enter_locals_to_cfa])
		return 3;

	if (handler == (cell)p_load2)
		return 3;
	if (handler == (cell)p_load3)
		return 4;
	if (handler == (cell)p_store_e_lll0)
		return 4;
	if (handler == (cell)p_ll_add_0_store
	    || handler == (cell)p_ll_sub_0_store
	    || handler == (cell)p_ll_mul_0_store
	    || handler == (cell)p_ll_lit_add_0_store
	    || handler == (cell)p_ll_lit_sub_0_store
	    || handler == (cell)p_ll_lit_mul_0_store
	    || handler == (cell)p_ll_litrev_sub_0_store)
		return 4;

	if (handler == (cell)p_inc_store_i
	    || handler == (cell)p_dec_store_i
	    || handler == (cell)p_add_store_i
	    || handler == (cell)p_sub_store_i
	    || handler == (cell)p_mul_store_i
	    || handler == (cell)p_div_store_i)
		return 2;

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
	    || handler == (cell)p_sl_add_store
	    || handler == (cell)p_sl_sub_store
	    || handler == (cell)p_sl_mul_store
	    || handler == (cell)p_sl_div_store
	    || handler == (cell)p_at_i_lit_local0
	    || handler == (cell)p_at_i_ll0
	    || handler == (cell)p_at_e_ll0
	    || handler == (cell)p_at_e_lit_local0
	    || handler == (cell)p_at_e_depth
	    || handler == (cell)p_at_i_depth)
		return 3;

	if (handler == vocab.dict[vocab.literal_cfa]
	    || handler == (cell)p_local_acc_add_0
	    || handler == (cell)p_local_acc_sub_0
	    || handler == (cell)p_local_acc_mul_0
	    || handler == (cell)p_local_acc_div_0
	    || handler == (cell)p_at_i_local0
	    || handler == (cell)p_at_i_swap_local0
	    || handler == (cell)p_at_i_depth_top
	    || handler == (cell)p_add_f_depth
	    || handler == (cell)p_sub_f_depth
	    || handler == (cell)p_mul_f_depth
	    || handler == (cell)p_div_f_depth
	    || handler == (cell)p_pick_n
	    || handler == (cell)p_array_lit
	    || handler == (cell)p_at_i_lit
	    || handler == (cell)p_at_e_local0
	    || handler == (cell)p_at_e_lit
	    || handler == (cell)p_at_e_swap_local0
	    || handler == (cell)p_at_e_depth_top
	    || handler == (cell)p_gather_local0
	    || handler == (cell)p_gather_e_local0)
		return 2;

	if (handler == vocab.dict[vocab.dostr_cfa]
	    || handler == vocab.dict[vocab.tailcall_cfa]
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
	    || handler == vocab.dict[vocab.lte_zbranch_cfa]
	    || handler == vocab.dict[vocab.gte_zbranch_cfa]
	    || handler == vocab.dict[vocab.lte_f_zbranch_cfa]
	    || handler == vocab.dict[vocab.gte_f_zbranch_cfa]
	    || handler == vocab.dict[vocab.to_var_cfa]
	    || handler == vocab.dict[vocab.enter_locals_cfa]
	    || handler == vocab.dict[vocab.enter_locals_to_cfa]
	    || handler == vocab.dict[vocab.leave_locals_cfa]
	    || handler == vocab.dict[vocab.local_fetch_0depth_cfa]
	    || handler == vocab.dict[vocab.local_store_0depth_cfa]
	    || handler == vocab.dict[vocab.local_incr_0depth_cfa]
	    || handler == vocab.dict[vocab.local_decr_0depth_cfa]
	    || handler == vocab.dict[vocab.local_finc_0depth_cfa]
	    || handler == vocab.dict[vocab.local_fdec_0depth_cfa]
	    || handler == vocab.dict[vocab.frame_get_inline_key_cfa]
	    || handler == vocab.dict[vocab.frame_set_inline_key_cfa])
		return 2;

	return 1;
}

void inline_word_body(Interpreter *interp, int target_cfa) {
	cell exit_handler = vocab.dict[vocab.exit_cfa];

	int splice_start = vocab.here;
	int cursor = target_cfa + 1;

	while (1) {
		cell handler = vocab.dict[cursor];
		cfa_handler handler_fn = (cfa_handler)handler;

		if (handler == exit_handler)
			break;

		if (handler_fn == docol && quotation_starts_at(cursor)) {
			vocab.here = splice_start;
			emit_call(interp, target_cfa);
			return;
		}

		if (handler == vocab.dict[vocab.tailcall_cfa]) {
			emit_call(interp, (int)vocab.dict[cursor + 1]);
			cursor += 2;
			continue;
		}

		if (handler_fn == docol || handler_fn == dovar || handler_fn == dounit || handler_fn == dosym || handler_fn == dodefer) {
			emit(interp, handler);
			emit(interp, vocab.dict[cursor + 1]);
			cursor += 2;
			continue;
		}

		int n = op_cell_count(cursor);
		for (int i = 0; i < n; i++)
			emit(interp, vocab.dict[cursor + i]);
		cursor += n;
	}
}

void mark_body(Interpreter *interp, int body_start, int body_end) {
	cell literal_ptr = vocab.dict[vocab.literal_cfa];
	cell dostr_ptr = vocab.dict[vocab.dostr_cfa];

	int cursor = body_start;
	while (cursor < body_end) {
		cell handler = vocab.dict[cursor];
		cfa_handler handler_fn = (cfa_handler)handler;

		if (handler == literal_ptr) {
			Val value;
			value.bits = (uint64_t)vocab.dict[cursor + 1];
			mark_value(interp, value);
			cursor += 2;
			continue;
		}
		if (handler == dostr_ptr) {
			Val value = make_string((int)vocab.dict[cursor + 1]);
			mark_value(interp, value);
			cursor += 2;
			continue;
		}
		if (handler_fn == docol) {
			cursor += quotation_starts_at(cursor) ? 1 : 2;
			continue;
		}
		if (handler_fn == dovar || handler_fn == dounit || handler_fn == dosym || handler_fn == dodefer) {
			cursor += 2;
			continue;
		}

		cursor += op_cell_count(cursor);
	}
}

static void mark_roots(Interpreter *interp) {
	int i;

	for (i = 0; i < interp->dsp; i++)
		mark_value(interp, interp->data_stack[i]);
	for (i = 0; i < interp->rsp; i++)
		mark_value(interp, interp->return_stack[i]);
	for (i = 0; i < interp->side_dsp; i++)
		mark_value(interp, interp->side_stack[i]);
	for (i = 0; i < interp->n_gc_roots; i++)
		mark_value(interp, interp->gc_roots[i]);
	for (i = 0; i < interp->entry_snapshot_depth; i++)
		mark_value(interp, interp->entry_snapshot[i]);
}

#define SWEEP_PREFETCH_DISTANCE 16

void gc(Interpreter *interp) {
	int i;

	if (in_parallel) {
		fail(interp, "cannot collect inside a parallel region");
		return;
	}

	interp->gc_epoch = atomic_fetch_add(&arena.current_epoch, 1) + 1;
	interp->gc_object_base = 0;
	interp->gc_pair_base = 0;
	pairs.space.n_free = 0;

	mark_roots(interp);

	static int sorted_cfas[VOCABULARY_INIT_SIZE / 4];
	int num_cfas = 0;
	for (int cfa = vocab.latest_cfa; cfa != 0; cfa = (int)WORD_LINK(cfa)) {
		if (num_cfas >= (int)(sizeof sorted_cfas / sizeof sorted_cfas[0])) {
			fail(interp, "vocabulary too large to scan safely");
			return;
		}
		sorted_cfas[num_cfas++] = cfa;
	}

	for (i = 0; i < num_cfas / 2; i++) {
		int swap = sorted_cfas[i];
		sorted_cfas[i] = sorted_cfas[num_cfas - 1 - i];
		sorted_cfas[num_cfas - 1 - i] = swap;
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
	int n_handles = arena.object_space.n;
	for (int handle = 0; handle < n_handles; handle++) {
		if (handle + SWEEP_PREFETCH_DISTANCE < n_handles && arena.objects[handle + SWEEP_PREFETCH_DISTANCE])
			__builtin_prefetch(arena.objects[handle + SWEEP_PREFETCH_DISTANCE]);
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
	return cached_name(handler, 1);
}

static const char *var_name_from_slot(cell slot) {
	int var_cfa = (int)slot - 1;
	if (var_cfa > 0 && (cfa_handler)vocab.dict[var_cfa] == dovar)
		return &vocab.name_pool[WORD_NAME(var_cfa)];
	return NULL;
}

#define SEE_TREE_MAX_DEPTH 32


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

static int see_print_cell(FILE *out, Interpreter *interp, int cursor) {
	cell handler = vocab.dict[cursor];
	cfa_handler handler_fn = (cfa_handler)handler;

	int is_tailcall = handler == vocab.dict[vocab.tailcall_cfa];
	if (is_tailcall || handler_fn == docol || handler_fn == dovar || handler_fn == dounit || handler_fn == dodefer) {
		int target = (int)vocab.dict[cursor + 1];
		if (is_tailcall)
			fputs("(tailcall) ", out);
		const QuotationSpan *quotation = (handler_fn == docol || is_tailcall)
			? quotation_span_containing(target) : NULL;
		if (quotation && quotation->start_cfa == target) {
			if (quotation->source_offset == 0) {
				fputs("[:?]", out);
			} else {
				char cleaned[TRACE_SNIPPET_MAX + 8];
				clean_snippet(cleaned, TRACE_SNIPPET_MAX, quotation->source_offset);
				fputs(cleaned, out);
			}
		} else if (target >= 4 && target < vocab.here) {
			fputs(&vocab.name_pool[WORD_NAME(target)], out);
		} else {
			fputs("?", out);
		}
		return 2;
	}
	if (handler_fn == dosym) {
		fprintf(out, ":%s", &vocab.symbol_pool[vocab.dict[cursor + 1]]);
		return 2;
	}

	int cell_count = op_cell_count(cursor);
	see_print_op(out, interp, cursor, cell_count);
	return cell_count;
}

static Object *trace_patterns_array(Interpreter *interp) {
	Val patterns_val = interp->trace_patterns;
	if (VAL_TAG(patterns_val) != T_ARRAY)
		return NULL;
	Object *patterns = OBJECT_AT(VAL_DATA(patterns_val));
	return patterns->len > 0 ? patterns : NULL;
}

static int trace_word_selected(Interpreter *interp, const char *op_text, int length, int *start, int *end) {
	Object *patterns = trace_patterns_array(interp);
	if (!patterns)
		return 1;
	return bytes_match_span(interp, op_text, length, OBJECT_AT(VAL_DATA(patterns->items[0])), start, end);
}

static int trace_line_selected(Interpreter *interp, const char *line, int length, int *start, int *end) {
	Object *patterns = trace_patterns_array(interp);
	if (!patterns || patterns->len == 1)
		return 1;

	int n_patterns = patterns->len;
	for (int i = 1; i < n_patterns; i++) {
		Object *pattern = OBJECT_AT(VAL_DATA(patterns->items[i]));
		if (bytes_match_span(interp, line, length, pattern, start, end))
			return 1;
		if (interp->error_flag)
			return 0;
	}
	return 0;
}

#define TRACE_MARK "> "
#define TRACE_MARK_WIDTH 2
#define TRACE_MARK_ON "\033[35m"
#define TRACE_HIGHLIGHT_ON "\033[1;33m"
#define TRACE_HIGHLIGHT_OFF "\033[0m"

static int trace_print_span(const char *line, int length, int from, int start, int end) {
	if (start < from || start >= length)
		return from;
	end = MIN(end, length);
	fwrite(line + from, 1, (size_t)(start - from), stderr);
	fputs(TRACE_HIGHLIGHT_ON, stderr);
	fwrite(line + start, 1, (size_t)(end - start), stderr);
	fputs(TRACE_HIGHLIGHT_OFF, stderr);
	return end;
}

static void trace_print_highlighted(const char *line, int length, int word_start, int word_end, int line_start, int line_end) {
	int from = 0;
	if (word_start >= 0)
		from = trace_print_span(line, length, from, word_start, word_end);
	if (line_start >= 0)
		from = trace_print_span(line, length, from, line_start, line_end);
	fwrite(line + from, 1, (size_t)(length - from), stderr);
}

static void trace_emit(Interpreter *interp, const char *op_text, int op_len) {
	int word_start = -1;
	int word_end = -1;
	if (!trace_word_selected(interp, op_text, op_len, &word_start, &word_end))
		return;

	int shown_from = interp->dsp > TRACE_STACK_SHOWN ? interp->dsp - TRACE_STACK_SHOWN : 0;
	int n_shown = interp->dsp - shown_from;
	int terminal = isatty(fileno(stderr));
	if (!trace_patterns_array(interp)) {
		fputs(terminal ? TRACE_MARK_ON TRACE_MARK TRACE_HIGHLIGHT_OFF : TRACE_MARK, stderr);
		fprintf(stderr, "%-24.*s|", op_len, op_text);
		if (shown_from > 0)
			fputs(" …", stderr);
		for (int i = 0; i < n_shown; i++) {
			putc(' ', stderr);
			print_val_compact(stderr, interp, interp->data_stack[shown_from + i]);
		}
		putc('\n', stderr);
		fflush(stderr);
		return;
	}

	char *value_text[TRACE_STACK_SHOWN];
	size_t value_len[TRACE_STACK_SHOWN];
	int value_offset[TRACE_STACK_SHOWN];

	char *rendered = NULL;
	size_t rendered_len = 0;
	FILE *out = open_memstream(&rendered, &rendered_len);
	if (!out)
		return;
	fprintf(out, TRACE_MARK "%-24.*s|", op_len, op_text);
	if (shown_from > 0)
		fputs(" …", out);
	for (int i = 0; i < n_shown; i++) {
		value_text[i] = NULL;
		value_len[i] = 0;
		FILE *value_out = open_memstream(&value_text[i], &value_len[i]);
		if (value_out) {
			print_val(value_out, interp, interp->data_stack[shown_from + i]);
			fclose(value_out);
		}
		fflush(out);
		putc(' ', out);
		value_offset[i] = (int)ftell(out);
		if (value_text[i])
			fwrite(value_text[i], 1, value_len[i], out);
	}
	fclose(out);

	int line_start = -1;
	int line_end = -1;
	if (rendered && trace_line_selected(interp, rendered, (int)rendered_len, &line_start, &line_end)) {
		int filtering = trace_patterns_array(interp) != NULL;
		int highlight = filtering && isatty(fileno(stderr));
		if (filtering)
			putc('\n', stderr);

		int head_len = n_shown > 0 ? value_offset[0] : (int)rendered_len;
		if (terminal)
			fputs(TRACE_MARK_ON TRACE_MARK TRACE_HIGHLIGHT_OFF, stderr);
		else
			fputs(TRACE_MARK, stderr);
		if (highlight)
			trace_print_highlighted(rendered + TRACE_MARK_WIDTH, head_len - TRACE_MARK_WIDTH, word_start, word_end, line_start - TRACE_MARK_WIDTH, line_end - TRACE_MARK_WIDTH);
		else
			fwrite(rendered + TRACE_MARK_WIDTH, 1, (size_t)(head_len - TRACE_MARK_WIDTH), stderr);
		for (int i = 0; i < n_shown; i++) {
			int start = value_offset[i];
			int end = start + (int)value_len[i];
			int matched_here = line_start >= start && line_start < end;
			if (matched_here || (int)value_len[i] <= TRACE_VALUE_WIDTH) {
				if (highlight && matched_here)
					trace_print_highlighted(rendered + start, (int)value_len[i], -1, -1, line_start - start, MIN(line_end, end) - start);
				else
					fwrite(rendered + start, 1, value_len[i], stderr);
			} else {
				fwrite(rendered + start, 1, (size_t)TRACE_VALUE_WIDTH, stderr);
				fputs("…", stderr);
			}
			if (i + 1 < n_shown)
				putc(' ', stderr);
		}
		putc('\n', stderr);
		if (filtering)
			putc('\n', stderr);
		fflush(stderr);
	}
	for (int i = 0; i < n_shown; i++)
		free(value_text[i]);
	free(rendered);
}

static int trace_op_text(Interpreter *interp, int cursor, char *buffer, int capacity) {
	(void)interp;
	cell handler = vocab.dict[cursor];
	cfa_handler handler_fn = (cfa_handler)handler;
	if (handler == vocab.dict[vocab.exit_cfa])
		return snprintf(buffer, (size_t)capacity, "exit");

	int is_tailcall = handler == vocab.dict[vocab.tailcall_cfa];
	if (is_tailcall || handler_fn == docol || handler_fn == dovar || handler_fn == dounit || handler_fn == dodefer) {
		const char *name = name_of((int)vocab.dict[cursor + 1]);
		if (!name)
			return -1;
		return snprintf(buffer, (size_t)capacity, "%s%s", is_tailcall ? "(tailcall) " : "", name);
	}
	if (handler_fn == dosym)
		return snprintf(buffer, (size_t)capacity, ":%s", &vocab.symbol_pool[vocab.dict[cursor + 1]]);
	if (op_cell_count(cursor) != 1)
		return -1;

	const char *name = handler_word_name(handler);
	if (!name)
		return -1;
	return snprintf(buffer, (size_t)capacity, "%s", name);
}

static void trace_step(Interpreter *interp) {
	int cursor = interp->ip;
	if (cursor == interp->trampoline_base && (cfa_handler)vocab.dict[cursor] == docol) {
		trace_call(interp, (int)vocab.dict[cursor + 1]);
		return;
	}
	if (cursor >= interp->trampoline_base && cursor < interp->trampoline_base + 3)
		return;
	if (cursor < DICT_RESERVED || cursor >= vocab.here)
		return;
	if (vocab.dict[cursor] == vocab.dict[vocab.stop_cfa])
		return;

	char op_buffer[TRACE_SNIPPET_MAX + 64];
	int op_len = trace_op_text(interp, cursor, op_buffer, (int)sizeof(op_buffer));
	if (op_len >= 0) {
		trace_emit(interp, op_buffer, op_len);
		return;
	}

	FILE *op_out = interp->trace_op_out;
	if (!op_out)
		return;
	rewind(op_out);
	see_print_cell(op_out, interp, cursor);
	fflush(op_out);
	trace_emit(interp, interp->trace_op_text, (int)ftell(op_out));
}

static void trace_call(Interpreter *interp, int cfa) {
	FILE *op_out = interp->trace_op_out;
	if (!op_out)
		return;
	rewind(op_out);
	const QuotationSpan *quotation = quotation_span_containing(cfa);
	if (quotation && quotation->start_cfa == cfa) {
		fputs(quotation_header(cfa), op_out);
	} else {
		const char *name = name_of(cfa);
		fputs(name ? name : "?", op_out);
	}
	fflush(op_out);
	trace_emit(interp, interp->trace_op_text, (int)ftell(op_out));
}

static void see_compiled_body(FILE *out, Interpreter *interp, int body_start, int body_end) {
	cell exit_handler = vocab.dict[vocab.exit_cfa];
	cell docol_handler = (cell)docol;
	int cursor = body_start;
	int depth = 0;

	while (cursor < body_end) {
		cell handler = vocab.dict[cursor];

		fprintf(out, " %d: ", cursor - body_start);

		if (handler == exit_handler) {
			fputs("exit\n", out);
			cursor++;
			if (depth == 0)
				break;
			depth--;
			continue;
		}

		if (handler == docol_handler && quotation_starts_at(cursor)) {
			fputs("[:\n", out);
			cursor++;
			depth++;
			continue;
		}

		cursor += see_print_cell(out, interp, cursor);
		putc('\n', out);
	}
}


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
			if (quotation_starts_at(cursor) || target < 4 || target >= vocab.here) {
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
		if (handler_fn == dovar || handler_fn == dounit || handler_fn == dodefer) {
			int target = (int)vocab.dict[cursor + 1];
			if (target >= 4 && target < vocab.here)
				fprintf(out, "%s\n", &vocab.name_pool[WORD_NAME(target)]);
			else
				fputs("?\n", out);
			cursor += 2;
			continue;
		}
		if (handler == vocab.dict[vocab.tailcall_cfa]) {
			int target = (int)vocab.dict[cursor + 1];
			if (quotation_starts_at(target))
				fputs("(tailcall) [:]\n", out);
			else if (target >= 4 && target < vocab.here)
				fprintf(out, "(tailcall) %s\n", &vocab.name_pool[WORD_NAME(target)]);
			else
				fputs("(tailcall) ?\n", out);
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

void render_curried_bindings(FILE *out, Interpreter *interp, Val target) {
	if (VAL_TAG(target) != T_CURRIED)
		return;

	Object *curried = OBJECT_AT(VAL_DATA(target));
	fputs("\\ curried, binds", out);
	for (int i = 1; i < curried->len; i++) {
		putc(' ', out);
		print_val_compact(out, interp, curried->items[i]);
	}
	putc('\n', out);
}

int capture_render(Interpreter *interp, void (*render)(FILE *, Interpreter *, Val), Val target) {
	char *buffer = NULL;
	size_t size = 0;
	FILE *out = open_memstream(&buffer, &size);
	if (!out) {
		fail(interp, "out of memory");
		return -1;
	}

	render(out, interp, target);
	fclose(out);

	int length = (int)size;
	if (length > 0 && buffer[length - 1] == '\n')
		length--;

	int handle = object_new_string(interp, buffer ? buffer : "", length);
	free(buffer);
	return handle;
}

static const char *quotation_header(int cfa) {
	const char *source = quotation_source(cfa);

	return source ? source : "[: ... :]";
}

static void see_compiled_render(FILE *out, Interpreter *interp, Val target) {
	render_curried_bindings(out, interp, target);

	int target_cfa = callable_cfa(target);
	const char *name = name_of(target_cfa);

	if ((cfa_handler)vocab.dict[target_cfa] != docol) {
		fprintf(out, "%s: not a colon definition\n", name ? name : "?");
		return;
	}

	int body_start = target_cfa + 1;
	int body_end = vocab.here;
	for (int cfa = vocab.latest_cfa; cfa != 0; cfa = (int)WORD_LINK(cfa)) {
		if (cfa > target_cfa && cfa - 4 < body_end)
			body_end = cfa - 4;
	}

	if (name)
		fprintf(out, ": %s   \\ %d cells\n", name, body_end - body_start);
	else
		fprintf(out, "%s   \\ %d cells\n", quotation_header(target_cfa), body_end - body_start);
	see_compiled_body(out, interp, body_start, body_end);
	fputs(name ? ";\n" : ":]\n", out);
}

#define SEE_WORD_PAIR(print_name, string_name, print_word, string_word, render) \
	void print_name(DISPATCH_ARGS) { \
		POP_CALLABLE(target_cfa, print_word); \
		(void)target_cfa; \
		render(stdout, interp, target_cfa##_val); \
		fflush(stdout); \
		\
		DISPATCH(interp); \
	} \
	\
	void string_name(DISPATCH_ARGS) { \
		POP_CALLABLE(target_cfa, string_word); \
		(void)target_cfa; \
		int handle = capture_render(interp, render, target_cfa##_val); \
		if (interp->error_flag) \
			return; \
		push(interp, make_string(handle)); \
		\
		DISPATCH(interp); \
	}

SEE_WORD_PAIR(p_see_compiled, p_see_compiled_to_string, "see-compiled", "see-compiled>string", see_compiled_render)

static void gauge_put(Interpreter *interp, int frame_handle, const char *key, double value) {
	frame_put(OBJECT_AT(frame_handle), intern_symbol(interp, key), make_float(value));
}

static void gauge_put_pair(Interpreter *interp, int frame_handle, const char *key, double used, double capacity) {
	int pair_handle = object_new_array(interp, 2);
	if (interp->error_flag)
		return;
	Object *pair = OBJECT_AT(pair_handle);
	pair->items[0] = make_float(used);
	pair->items[1] = make_float(capacity);
	frame_put(OBJECT_AT(frame_handle), intern_symbol(interp, key), make_array(pair_handle));
}

static int gauge_group(Interpreter *interp, int top_handle, const char *key) {
	int group_handle = object_new_frame(interp);
	if (interp->error_flag)
		return -1;
	frame_put(OBJECT_AT(top_handle), intern_symbol(interp, key), make_frame(group_handle));
	return group_handle;
}

static int dictionary_word_count(int stop_cfa) {
	int n_words = 0;
	for (int cfa = vocab.latest_cfa; cfa != 0 && cfa != stop_cfa; cfa = (int)WORD_LINK(cfa))
		n_words++;
	return n_words;
}

static int symbol_count(void) {
	int n_symbols = 0;
	for (int i = 0; i < SYMBOL_HASH_SIZE; i++)
		if (vocab.symbol_hash[i])
			n_symbols++;
	return n_symbols;
}

void p_gauges(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	SYNC_REGISTERS(interp, chain_ip, chain_sp);
	int roots_before = interp->n_gc_roots;
	int top = object_new_frame(interp);
	if (interp->error_flag)
		return;
	gc_root_push(interp, make_frame(top));

	int dictionary = gauge_group(interp, top, "dictionary");
	if (dictionary >= 0) {
		gauge_put_pair(interp, dictionary, "cells", vocab.here, VOCABULARY_INIT_SIZE);
		gauge_put(interp, dictionary, "words", dictionary_word_count(0));
		gauge_put(interp, dictionary, "session-words", dictionary_word_count(vocab.lib_end_latest_cfa));
		gauge_put_pair(interp, dictionary, "name-pool", vocab.names_here, NAME_POOL);
		gauge_put_pair(interp, dictionary, "source-pool", vocab.source_here, SOURCE_POOL);
		gauge_put(interp, dictionary, "symbols", symbol_count());
		gauge_put_pair(interp, dictionary, "symbol-pool", vocab.symbol_pool_here, SYMBOL_POOL);
		gauge_put_pair(interp, dictionary, "quotations", vocab.n_quotation_spans, MAX_QUOTATION_SPANS);
		gauge_put_pair(interp, dictionary, "word-locations", vocab.n_word_locations, MAX_WORD_LOCATIONS);
		gauge_put_pair(interp, dictionary, "cell-lines", vocab.n_cell_lines, MAX_CELL_LINES);
		gauge_put_pair(interp, dictionary, "loaded-files", compiler.n_loaded_files, MAX_LOADED_FILES);
	}

	int heap = gauge_group(interp, top, "heap");
	if (heap >= 0) {
		int free_handles = arena.object_space.cap - arena.object_space.n;
		gauge_put_pair(interp, heap, "arena", (double)arena.used, (double)arena.reserved);
		gauge_put(interp, heap, "live", (double)arena.heap_bytes_live);
		gauge_put(interp, heap, "gc-threshold", (double)arena.heap_gc_threshold);
		gauge_put(interp, heap, "memory-headroom", (double)arena.heap_gc_threshold - (double)arena.heap_bytes_live);
		gauge_put_pair(interp, heap, "objects", arena.object_space.n - arena.object_space.n_free, arena.object_space.cap);
		gauge_put_pair(interp, heap, "handles-claimed", arena.object_space.n, arena.object_space.cap);
		gauge_put(interp, heap, "max-objects", MAX_OBJECTS);
		gauge_put(interp, heap, "free-handles", arena.object_space.n_free);
		gauge_put_pair(interp, heap, "handle-headroom", free_handles, HANDLE_PRESSURE_SLOTS);
		gauge_put_pair(interp, heap, "pairs", pairs.space.n - pairs.space.n_free, pairs.space.cap);
		gauge_put(interp, heap, "collections", (double)arena.current_epoch);
	}

	int stacks = gauge_group(interp, top, "stacks");
	if (stacks >= 0) {
		gauge_put_pair(interp, stacks, "data", interp->dsp, DATA_STACK_DEPTH);
		gauge_put_pair(interp, stacks, "return", interp->rsp, RETURN_STACK_DEPTH);
		gauge_put_pair(interp, stacks, "side", interp->side_dsp, SIDESTACK_DEPTH);
		gauge_put_pair(interp, stacks, "calls", interp->call_depth, MAX_CALL_DEPTH);
		gauge_put_pair(interp, stacks, "trail", interp->bind_trail_top, interp->bind_trail_cap);
		gauge_put_pair(interp, stacks, "logic-vars", interp->lvar_top, interp->lvar_cap);
		gauge_put_pair(interp, stacks, "roots", roots_before, MAX_GC_ROOTS);
	}

	int resources = gauge_group(interp, top, "resources");
	if (resources >= 0) {
		int regex_in_use = 0;
		for (int i = 0; i < REGEX_CACHE_SIZE; i++)
			regex_in_use += interp->regex_cache[i].in_use ? 1 : 0;
		gauge_put_pair(interp, resources, "databases", interp->n_databases, MAX_DATABASES);
		gauge_put_pair(interp, resources, "regex-cache", regex_in_use, REGEX_CACHE_SIZE);
		gauge_put_pair(interp, resources, "workers", worker_pool_count(), MAX_WORKER_THREADS);
	}

	int computer = gauge_group(interp, top, "computer");
	if (computer >= 0) {
		ComputerGauges readings = { 0 };
		int available = platform_computer_gauges(&readings);
		const char *keys[] = { "cpu-count", "physical-memory", "load-1", "load-5", "load-15", "user-time", "system-time",
			"max-rss", "minor-faults", "major-faults", "voluntary-switches", "involuntary-switches" };
		const double values[] = { readings.cpu_count, readings.physical_bytes, readings.load_1, readings.load_5, readings.load_15,
			readings.user_seconds, readings.system_seconds, readings.max_rss_bytes, readings.minor_faults, readings.major_faults,
			readings.voluntary_switches, readings.involuntary_switches };
		int n_keys = (int)(sizeof(keys) / sizeof(keys[0]));
		for (int i = 0; i < n_keys; i++) {
			Val reading = available ? make_float(values[i]) : make_tagged(T_NONE, 0);
			frame_put(OBJECT_AT(computer), intern_symbol(interp, keys[i]), reading);
		}
	}

	int session = gauge_group(interp, top, "session");
	if (session >= 0) {
		gauge_put(interp, session, "line", compiler.input_line);
		gauge_put(interp, session, "interactive", compiler.interactive ? 1 : 0);
		gauge_put(interp, session, "load-depth", compiler.load_depth);
		gauge_put(interp, session, "gc-disabled", interp->gc_disabled ? 1 : 0);
		gauge_put(interp, session, "tracing", (interp->gc_pending & TRACE_PENDING) ? 1 : 0);
	}

	gc_root_pop(interp);
	if (interp->error_flag)
		return;
	*chain_sp = make_frame(top);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

static int body_end_of(int target_cfa) {
	int body_end = vocab.here;
	for (int cfa = vocab.latest_cfa; cfa != 0; cfa = (int)WORD_LINK(cfa))
		if (cfa > target_cfa && cfa - 4 < body_end)
			body_end = cfa - 4;
	return body_end;
}

static int body_references(int body_start, int body_end, int target_cfa) {
	cell target_handler = vocab.dict[target_cfa];
	int target_is_primitive = (cfa_handler)target_handler != docol && (cfa_handler)target_handler != dovar
		&& (cfa_handler)target_handler != dounit && (cfa_handler)target_handler != dodefer;
	cell store_handler = vocab.dict[vocab.to_var_cfa];
	cell branch_handler = vocab.dict[vocab.branch_cfa];
	cell previous_handler = 0;
	int cursor = body_start;
	while (cursor < body_end) {
		cell handler = vocab.dict[cursor];
		cfa_handler handler_fn = (cfa_handler)handler;
		if (handler_fn == docol && quotation_starts_at(cursor)) {
			if (previous_handler != branch_handler)
				return 0;
			previous_handler = handler;
			cursor++;
			continue;
		}
		previous_handler = handler;
		int is_tailcall = handler == vocab.dict[vocab.tailcall_cfa];
		if (is_tailcall || handler == store_handler || handler_fn == docol || handler_fn == dovar
				|| handler_fn == dounit || handler_fn == dodefer) {
			if ((int)vocab.dict[cursor + 1] == target_cfa)
				return 1;
			cursor += 2;
			continue;
		}
		if (handler_fn == dosym) {
			cursor += 2;
			continue;
		}
		if (handler == vocab.dict[vocab.literal_cfa]) {
			Val literal;
			literal.bits = (uint64_t)vocab.dict[cursor + 1];
			if (VAL_TAG(literal) == T_XT && (int)VAL_DATA(literal) == target_cfa)
				return 1;
		} else if (target_is_primitive && handler == target_handler) {
			return 1;
		} else if (superword_cell_count(handler) && !superword_is_lit_fold(handler)) {
			int n_cells = op_cell_count(cursor);
			for (int operand = 1; operand < n_cells; operand++)
				if ((int)vocab.dict[cursor + operand] - 1 == target_cfa)
					return 1;
		}
		cursor += op_cell_count(cursor);
	}
	return 0;
}

void p_callers(DISPATCH_ARGS) {
	POP_CALLABLE(target_cfa, "callers");
	if (VAL_TAG(target_cfa_val) == T_CURRIED || !name_of(target_cfa)) {
		fail(interp, "expected a named word; got %s", VAL_TAG(target_cfa_val) == T_CURRIED ? "a curried token" : "a quotation");
		return;
	}

	int n_callers = 0;
	int caller_cfas[MAX_CALLERS];
	for (int cfa = vocab.latest_cfa; cfa != 0 && n_callers < MAX_CALLERS; cfa = (int)WORD_LINK(cfa)) {
		if ((cfa_handler)vocab.dict[cfa] != docol)
			continue;
		if (body_references(cfa + 1, body_end_of(cfa), target_cfa))
			caller_cfas[n_callers++] = cfa;
	}

	int result_handle = object_new_array(interp, n_callers);
	if (interp->error_flag)
		return;
	memset(OBJECT_AT(result_handle)->items, 0, sizeof(Val) * (size_t)(n_callers > 0 ? n_callers : 1));
	gc_root_push(interp, make_array(result_handle));
	for (int i = 0; i < n_callers; i++) {
		const char *caller_name = name_of(caller_cfas[n_callers - 1 - i]);
		int name_handle = object_new_string(interp, caller_name, (int)strlen(caller_name));
		if (interp->error_flag) {
			gc_root_pop(interp);
			return;
		}
		OBJECT_AT(result_handle)->items[i] = make_string(name_handle);
	}
	gc_root_pop(interp);
	push(interp, make_array(result_handle));

	DISPATCH(interp);
}

void p_trace(DISPATCH_ARGS) {
	POP_ARRAY(patterns, "trace");
	POP_CALLABLE(xt, "trace");
	int n_patterns = patterns->len;
	for (int i = 0; i < n_patterns; i++) {
		Val pattern_val = patterns->items[i];
		if (VAL_TAG(pattern_val) != T_STRING) {
			fail(interp, "expected an array of pattern strings; element %d is %s", i, tag_name(VAL_TAG(pattern_val)));
			return;
		}
		bytes_match(interp, "", 0, OBJECT_AT(VAL_DATA(pattern_val)));
		if (interp->error_flag)
			return;
	}
	push_curried_bindings(interp, xt_val);
	if (interp->error_flag)
		return;

	Val outer_patterns = interp->trace_patterns;
	int outer_root_cfa = interp->trace_root_cfa;
	FILE *outer_op_out = interp->trace_op_out;
	char *outer_op_text = interp->trace_op_text;
	size_t outer_op_capacity = interp->trace_op_capacity;
	int already_tracing = interp->gc_pending & TRACE_PENDING;
	gc_root_push(interp, patterns_val);
	interp->trace_patterns = patterns_val;
	interp->trace_root_cfa = xt;
	interp->trace_op_text = NULL;
	interp->trace_op_capacity = 0;
	interp->trace_op_out = open_memstream(&interp->trace_op_text, &interp->trace_op_capacity);
	interp->gc_pending |= TRACE_PENDING;
	execute_xt(interp, xt);
	if (!already_tracing)
		interp->gc_pending &= ~TRACE_PENDING;
	if (interp->trace_op_out)
		fclose(interp->trace_op_out);
	free(interp->trace_op_text);
	interp->trace_patterns = outer_patterns;
	interp->trace_root_cfa = outer_root_cfa;
	interp->trace_op_out = outer_op_out;
	interp->trace_op_text = outer_op_text;
	interp->trace_op_capacity = outer_op_capacity;
	gc_root_pop(interp);

	DISPATCH(interp);
}

static void see_tree_render(FILE *out, Interpreter *interp, Val target) {
	render_curried_bindings(out, interp, target);

	int target_cfa = callable_cfa(target);
	const char *name = name_of(target_cfa);

	if ((cfa_handler)vocab.dict[target_cfa] != docol) {
		fprintf(out, "%s: not a colon definition\n", name ? name : "?");
		return;
	}

	int stack[SEE_TREE_MAX_DEPTH + 1];
	stack[0] = target_cfa;

	if (name)
		fprintf(out, ": %s\n", name);
	else
		fprintf(out, "%s\n", quotation_header(target_cfa));
	see_tree_body(out, interp, target_cfa + 1, 2, stack, 1);
	fputs(name ? ";\n" : ":]\n", out);
}

SEE_WORD_PAIR(p_see_tree, p_see_tree_to_string, "see-tree", "see-tree>string", see_tree_render)
void p_save(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val filename_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(filename_val, T_STRING, "save", "a string");
	const char *filename = OBJECT_AT(VAL_DATA(filename_val))->bytes;

	FILE *file = fopen(filename, "w");
	if (!file) {
		fail(interp, "cannot create %s", filename);
		return;
	}

	static int collected_cfas[VOCABULARY_INIT_SIZE / 4];
	int num_cfas = 0;
	for (int cfa = vocab.latest_cfa; cfa > vocab.lib_end_latest_cfa; cfa = (int)WORD_LINK(cfa)) {
		if (num_cfas < (int)(sizeof collected_cfas / sizeof collected_cfas[0]))
			collected_cfas[num_cfas++] = cfa;
	}

	fprintf(file, "\\ telic vocabulary\n\n");

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

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}


void free_one_object(Object *obj) {
	switch (obj->kind) {
		case OBJECT_STRING: arena_free(obj->bytes); break;
		case OBJECT_EXACT: arena_free(obj->exact.limbs); break;
		case OBJECT_SET:
		case OBJECT_ARRAY:
			if (obj->items != obj->inline_items)
				arena_free(obj->items);
			break;
		case OBJECT_FRAME: arena_free(obj->frame.keys); arena_free(obj->frame.values); break;
		default: free_object_heap_backing(obj); break;
	}
	arena_free_object(obj);
}

void worker_local_gc(Interpreter *interp) {
	int last_pair_chunk = thread_alloc.pairs.n_chunks - 1;

	parallel_region_collected = 1;
	interp->gc_epoch = atomic_fetch_add(&arena.current_epoch, 1) + 1;
	interp->gc_object_base = parallel_region_object_base;
	interp->gc_pair_base = parallel_region_pair_base;

	mark_roots(interp);

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
	truncate_word_locations();
	truncate_cell_lines();
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
	compiler.handler_registry[compiler.n_handlers++] = (void *)dodefer;
	define_primitive(interp, "+", p_add, 0);
	define_primitive(interp, "-", p_sub, 0);
	define_primitive(interp, "*", p_mul, 0);
	define_primitive(interp, "/", p_div, 0);
	define_primitive(interp, "+!", p_add_inplace, 0);
	define_primitive(interp, "-!", p_sub_inplace, 0);
	define_primitive(interp, "*!", p_mul_inplace, 0);
	define_primitive(interp, "/!", p_div_inplace, 0);
	vocab.add_f_cfa = define_primitive(interp, "f+", p_add_f, 0);
	vocab.sub_f_cfa = define_primitive(interp, "f-", p_sub_f, 0);
	vocab.mul_f_cfa = define_primitive(interp, "f*", p_mul_f, 0);
	vocab.eq_f_cfa = define_primitive(interp, "f=", p_eq_f, 0);
	vocab.lt_f_cfa = define_primitive(interp, "f<", p_lt_f, 0);
	vocab.gt_f_cfa = define_primitive(interp, "f>", p_gt_f, 0);
	vocab.lte_f_cfa = define_primitive(interp, "f<=", p_lte_f, 0);
	vocab.gte_f_cfa = define_primitive(interp, "f>=", p_gte_f, 0);
	define_primitive(interp, "bit-and", p_bit_and, 0);
	define_primitive(interp, "bit-or", p_bit_or, 0);
	define_primitive(interp, "bit-xor", p_bit_xor, 0);
	define_primitive(interp, "lshift", p_lshift, 0);
	define_primitive(interp, "rshift", p_rshift, 0);
	define_primitive(interp, "bit-not", p_bit_not, 0);
	define_primitive(interp, "lowest-bit", p_lowest_bit, 0);
	vocab.div_f_cfa = define_primitive(interp, "f/", p_div_f, 0);
	define_primitive(interp, "f^", p_fpow, 0);
	define_primitive(interp, "fmod", p_fmodop, 0);
	define_primitive(interp, "fabs", p_fabs, 0);
	define_primitive(interp, "fsqrt", p_fsqrt, 0);
	define_primitive(interp, "fexp", p_fexp, 0);
	define_primitive(interp, "flog", p_flog, 0);
	define_primitive(interp, "fln", p_fln, 0);
	define_primitive(interp, "fln1+", p_fln1p, 0);
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
	define_primitive(interp, "nip", p_nip, 0);
	define_primitive(interp, "rot", p_rot, 0);
	define_primitive(interp, "depth", p_depth, 0);
	vocab.pick_cfa = define_primitive(interp, "pick", p_pick, 0);
	define_primitive(interp, "roll", p_roll, 0);
	vocab.eq_cfa = define_primitive(interp, "=", p_eq, 0);
	vocab.lt_cfa = define_primitive(interp, "<", p_lt, 0);
	vocab.gt_cfa = define_primitive(interp, ">", p_gt, 0);
	vocab.lte_cfa = define_primitive(interp, "<=", p_lte, 0);
	vocab.gte_cfa = define_primitive(interp, ">=", p_gte, 0);
	define_primitive(interp, "eq", p_eq_elements, 0);
	define_primitive(interp, "neq", p_neq_elements, 0);
	define_primitive(interp, "nan?", p_nan, 0);
	vocab.zeq_cfa = define_primitive(interp, "0=", p_zeq, 0);
	define_primitive(interp, "and", p_and, 0);
	define_primitive(interp, "or", p_or, 0);
	define_primitive(interp, "not", p_not, 0);
	define_primitive(interp, "null", p_null, 0);
	define_primitive(interp, "null?", p_null_, 0);
	define_primitive(interp, "type-of", p_type_of, 0);

	type_of_intern_names(interp);

	define_primitive(interp, "lvar", p_lvar, 0);
	define_primitive(interp, "_", p_wildcard, 0);
	define_primitive(interp, "unify", p_unify, 0);
	define_primitive(interp, "~", p_unify, 0);
	define_primitive(interp, "matches?", p_matches, 0);
	define_primitive(interp, "unify?", p_unify_keep, 0);
	define_primitive(interp, "deref", p_deref, 0);
	define_primitive(interp, "rest", p_rest, 0);
	define_primitive(interp, "amb", p_amb, 0);
	define_primitive(interp, "alloc-stats", p_alloc_stats, 0);
	define_primitive(interp, ".", p_dot, 0);
	define_primitive(interp, ".a", p_dot_all, 0);
	define_primitive(interp, "render", p_render, 0);
	define_primitive(interp, "cr", p_cr, 0);
	define_primitive(interp, "emit", p_emit_, 0);
	define_primitive(interp, ".s", p_dots, 0);
	define_primitive(interp, "bye", p_bye, 0);
	define_primitive(interp, "halt", p_halt, 0);
	define_primitive(interp, "clear", p_clear, 0);
	define_primitive(interp, "gc", p_gc, 0);
	define_primitive(interp, "(gauges)", p_gauges, 4);
	define_primitive(interp, "evaluate", p_evaluate, 0);
	define_primitive(interp, "load", p_load, 0);
	define_primitive(interp, "save", p_save, 0);
	define_primitive(interp, "reload", p_reload, 0);
	define_primitive(interp, ">r", p_tor, 0);
	define_primitive(interp, "r>", p_rfrom, 0);
	define_primitive(interp, "r@", p_rfetch, 0);
	define_primitive(interp, ">side", p_to_side, 0);
	define_primitive(interp, "side>", p_side_to, 0);
	define_primitive(interp, "side-drop", p_side_drop, 0);
	define_primitive(interp, "side-peek", p_side_peek, 0);
	define_primitive(interp, "side-depth", p_side_depth, 0);
	define_primitive(interp, "@", p_frame_get, 0);
	define_primitive(interp, "@or", p_frame_get_or, 0);
	define_primitive(interp, "!", p_frame_set, 0);
	define_primitive(interp, "keys", p_frame_keys, 0);
	define_primitive(interp, "key-set", p_frame_key_set, 0);
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
	define_primitive(interp, "upper-case", p_upper_case, 0);
	define_primitive(interp, "lower-case", p_lower_case, 0);
	define_primitive(interp, "join", p_join, 0);
	define_primitive(interp, "string>number", p_string_to_number, 0);
	define_primitive(interp, "edit-distance", p_edit_distance, 0);
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
	define_primitive(interp, "throw", p_throw, 0);

	define_primitive(interp, "{", p_frameopen, 0);
	define_primitive(interp, "}", p_frameclose, 0);
	define_primitive(interp, "[<", p_setopen, 0);
	define_primitive(interp, ">]", p_setclose, 0);
	define_primitive(interp, "[", p_array_open, 0);
	define_primitive(interp, "]", p_array_close, 0);
	define_primitive(interp, "array>set", p_array_to_set, 0);
	define_primitive(interp, "group-by", p_group_by, 0);

	vocab.array_cfa = define_primitive(interp, "array", p_array, 0);
	define_primitive(interp, "array-of", p_array_of, 0);
	define_primitive(interp, "int-segment", p_int_segment, 0);
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
	define_primitive(interp, "argsort", p_argsort, 0);
	define_primitive(interp, "sample", p_sample, 0);
	define_primitive(interp, "spread", p_spread, 0);
	define_primitive(interp, "slice!", p_slice_store, 0);
	define_primitive(interp, "to-slice!", p_to_slice, 0);
	define_primitive(interp, "range", p_range, 0);
	define_primitive(interp, "size", p_size, 0);
	define_primitive(interp, "byte-size", p_byte_size, 0);
	define_primitive(interp, "(in?)", p_in, 4);
	define_primitive(interp, "set", p_set, 0);
	define_primitive(interp, "union", p_union, 0);
	define_primitive(interp, "intersection", p_intersect, 0);
	define_primitive(interp, "difference", p_difference, 0);
	define_primitive(interp, "set-add!", p_set_add, 0);
	define_primitive(interp, "set-remove!", p_set_remove, 0);
	define_primitive(interp, "add-last!", p_add_last, 0);
	define_primitive(interp, "remove-last!", p_remove_last, 0);
	define_primitive(interp, "execute", p_execute, 0);
	define_primitive(interp, "curry", p_curry, 0);
	define_primitive(interp, "2curry", p_2curry, 0);
	define_primitive(interp, "ncurry", p_ncurry, 0);
	define_primitive(interp, "(execute-catching)", p_execute_catching, 4);
	define_primitive(interp, "map", p_map, 0);
	define_primitive(interp, "each", p_each, 0);
	define_primitive(interp, "nmap", p_nmap, 0);
	define_primitive(interp, "filter", p_filter, 0);
	define_primitive(interp, "find-first", p_find_first, 0);
	define_primitive(interp, "reduce", p_reduce, 0);
	define_primitive(interp, "times", p_times, 0);
	define_primitive(interp, "i-times", p_i_times, 0);
	define_primitive(interp, "fold-times", p_fold_times, 0);
	define_primitive(interp, "pmap-ext", p_pmap, 0);
	define_primitive(interp, "pfilter-ext", p_pfilter, 0);
	define_primitive(interp, "pmap-reduce-ext", p_pmap_reduce, 0);
	define_primitive(interp, "num-cores", p_num_cores, 0);

	define_primitive(interp, "words", p_words, 0);
	define_primitive(interp, "(globals)", p_globals, 4);
	define_primitive(interp, "apropos", p_apropos, 0);
	define_primitive(interp, "telic", p_telic, 0);
	define_primitive(interp, "telic-version", p_telic_version, 0);
	define_primitive(interp, "see", p_see, 0);
	define_primitive(interp, "see>string", p_see_to_string, 0);
	define_primitive(interp, "edit", p_edit, 0);
	define_primitive(interp, "callers", p_callers, 0);
	define_primitive(interp, "man", p_man, 0);
	define_primitive(interp, "see-compiled", p_see_compiled, 0);
	define_primitive(interp, "trace", p_trace, 0);
	define_primitive(interp, "see-compiled>string", p_see_compiled_to_string, 0);
	define_primitive(interp, "see-tree", p_see_tree, 0);
	define_primitive(interp, "see-tree>string", p_see_tree_to_string, 0);

	vocab.exit_cfa = define_primitive(interp, "exit", p_exit, 0);
	vocab.literal_cfa = define_primitive(interp, "(lit)", p_literal, 4);
	vocab.branch_cfa = define_primitive(interp, "(branch)", p_branch, 4);
	vocab.tailcall_cfa = define_primitive(interp, "(tailcall)", p_tailcall, 4);
	vocab.zbranch_cfa = define_primitive(interp, "(0branch)", p_0branch, 4);
	vocab.qzbranch_cfa = define_primitive(interp, "(?0branch)", p_qzbranch, 4);
	vocab.eq_zbranch_cfa = define_primitive(interp, "(=0branch)", p_eq_zbranch, 4);
	vocab.lt_zbranch_cfa = define_primitive(interp, "(<0branch)", p_lt_zbranch, 4);
	vocab.gt_zbranch_cfa = define_primitive(interp, "(>0branch)", p_gt_zbranch, 4);
	vocab.zeq_zbranch_cfa = define_primitive(interp, "(0=0branch)", p_zeq_zbranch, 4);
	vocab.eq_f_zbranch_cfa = define_primitive(interp, "(f=0branch)", p_eq_f_zbranch, 4);
	vocab.lt_f_zbranch_cfa = define_primitive(interp, "(f<0branch)", p_lt_f_zbranch, 4);
	vocab.gt_f_zbranch_cfa = define_primitive(interp, "(f>0branch)", p_gt_f_zbranch, 4);
	vocab.lte_zbranch_cfa = define_primitive(interp, "(<=0branch)", p_lte_zbranch, 4);
	vocab.gte_zbranch_cfa = define_primitive(interp, "(>=0branch)", p_gte_zbranch, 4);
	vocab.lte_f_zbranch_cfa = define_primitive(interp, "(f<=0branch)", p_lte_f_zbranch, 4);
	vocab.gte_f_zbranch_cfa = define_primitive(interp, "(f>=0branch)", p_gte_f_zbranch, 4);
	vocab.dostr_cfa = define_primitive(interp, "(dostr)", p_dostr, 4);
	vocab.stop_cfa = define_primitive(interp, "(stop)", p_stop, 4);
	vocab.to_var_cfa = define_primitive(interp, "(to-var)", p_to_var, 4);
	vocab.enter_locals_cfa = define_primitive(interp, "(enter-locals)", p_enter_locals, 4);
	vocab.enter_locals_to_cfa = define_primitive(interp, "(enter-locals-to)", p_enter_locals_to, 4);
	vocab.enter_locals_mixed_cfa = define_primitive(interp, "(enter-locals-mixed)", p_enter_locals_mixed, 4);
	vocab.leave_locals_cfa = define_primitive(interp, "(leave-locals)", p_leave_locals, 4);
	vocab.do_enter_cfa = define_primitive(interp, "(do)", p_do_enter, 4);
	vocab.do_loop_cfa = define_primitive(interp, "(loop)", p_do_loop, 4);
	vocab.local_fetch_cfa = define_primitive(interp, "(local@)", p_local_fetch, 4);
	vocab.local_store_cfa = define_primitive(interp, "(local!)", p_local_store, 4);
	vocab.local_fetch_0depth_cfa = define_primitive(interp, "(local@0)", p_local_fetch_0depth, 4);
	load2_cfa = define_primitive(interp, "(load2)", p_load2, 4);
	load3_cfa = define_primitive(interp, "(load3)", p_load3, 4);
	at_i_local0_cfa = define_primitive(interp, "(@i.l0)", p_at_i_local0, 4);
	at_i_lit_cfa = define_primitive(interp, "(@i.lit)", p_at_i_lit, 4);
	array_lit_cfa = define_primitive(interp, "(array.lit)", p_array_lit, 4);
	at_i_lit_local0_cfa = define_primitive(interp, "(@i.lit.l0)", p_at_i_lit_local0, 4);
	gather_local0_cfa = define_primitive(interp, "(gather.l0)", p_gather_local0, 4);
	at_i_ll0_cfa = define_primitive(interp, "(@i.ll0)", p_at_i_ll0, 4);
	at_i_swap_l0_cfa = define_primitive(interp, "(@i.swap.l0)", p_at_i_swap_local0, 4);
	at_i_depth_cfa = define_primitive(interp, "(@i.dd)", p_at_i_depth, 4);
	at_i_depth_top_cfa = define_primitive(interp, "(@i.d)", p_at_i_depth_top, 4);
	add_f_depth_cfa = define_primitive(interp, "(f+.d)", p_add_f_depth, 4);
	sub_f_depth_cfa = define_primitive(interp, "(f-.d)", p_sub_f_depth, 4);
	mul_f_depth_cfa = define_primitive(interp, "(f*.d)", p_mul_f_depth, 4);
	div_f_depth_cfa = define_primitive(interp, "(f/.d)", p_div_f_depth, 4);
	define_primitive(interp, "(enter-curried)", p_enter_curried, 4);
	pick_n_cfa = define_primitive(interp, "(pick.n)", p_pick_n, 4);
	define_primitive(interp, "(@i.array)", p_at_i_array, 4);
	define_primitive(interp, "(@i.segment)", p_at_i_segment, 4);
	at_e_lit_cfa = define_primitive(interp, "(@e.lit)", p_at_e_lit, 4);
	at_e_local0_cfa = define_primitive(interp, "(@e.l0)", p_at_e_local0, 4);
	at_e_ll0_cfa = define_primitive(interp, "(@e.ll0)", p_at_e_ll0, 4);
	at_e_lit_local0_cfa = define_primitive(interp, "(@e.lit.l0)", p_at_e_lit_local0, 4);
	gather_e_local0_cfa = define_primitive(interp, "(gather.e.l0)", p_gather_e_local0, 4);
	at_e_swap_l0_cfa = define_primitive(interp, "(@e.swap.l0)", p_at_e_swap_local0, 4);
	at_e_depth_cfa = define_primitive(interp, "(@e.dd)", p_at_e_depth, 4);
	at_e_depth_top_cfa = define_primitive(interp, "(@e.d)", p_at_e_depth_top, 4);
	define_primitive(interp, "(!i.array)", p_store_i_array, 4);
	define_primitive(interp, "(!i-drop.array)", p_store_i_drop_array, 4);
	define_primitive(interp, "(size.len)", p_size_len, 4);
	define_primitive(interp, "(=.symbol)", p_eq_symbol, 4);
	define_primitive(interp, "(=.string)", p_eq_string, 4);
	define_primitive(interp, "(@.symbol)", p_frame_get_symbol, 4);
	define_primitive(interp, "(!.symbol)", p_frame_set_symbol, 4);
	vocab.frame_get_inline_key_cfa = define_primitive(interp, "(@key)", p_frame_get_inline_key, 4);
	vocab.frame_set_inline_key_cfa = define_primitive(interp, "(!key)", p_frame_set_inline_key, 4);
	ll_add_0_cfa = define_primitive(interp, "(ll+0)", p_ll_add_0, 4);
	ll_sub_0_cfa = define_primitive(interp, "(ll-0)", p_ll_sub_0, 4);
	ll_mul_0_cfa = define_primitive(interp, "(ll*0)", p_ll_mul_0, 4);
	ll_lit_add_0_cfa = define_primitive(interp, "(ll.lit+0)", p_ll_lit_add_0, 4);
	ll_lit_sub_0_cfa = define_primitive(interp, "(ll.lit-0)", p_ll_lit_sub_0, 4);
	ll_lit_mul_0_cfa = define_primitive(interp, "(ll.lit*0)", p_ll_lit_mul_0, 4);
	ll_litrev_sub_0_cfa = define_primitive(interp, "(ll.litrev-0)", p_ll_litrev_sub_0, 4);
	sl_add_store_cfa = define_primitive(interp, "(sl+!0)", p_sl_add_store, 4);
	sl_sub_store_cfa = define_primitive(interp, "(sl-!0)", p_sl_sub_store, 4);
	sl_mul_store_cfa = define_primitive(interp, "(sl*!0)", p_sl_mul_store, 4);
	sl_div_store_cfa = define_primitive(interp, "(sl/!0)", p_sl_div_store, 4);
	ll_add_0_store_cfa = define_primitive(interp, "(ll+0!)", p_ll_add_0_store, 4);
	ll_sub_0_store_cfa = define_primitive(interp, "(ll-0!)", p_ll_sub_0_store, 4);
	ll_mul_0_store_cfa = define_primitive(interp, "(ll*0!)", p_ll_mul_0_store, 4);
	ll_lit_add_0_store_cfa = define_primitive(interp, "(ll.lit+0!)", p_ll_lit_add_0_store, 4);
	ll_lit_sub_0_store_cfa = define_primitive(interp, "(ll.lit-0!)", p_ll_lit_sub_0_store, 4);
	ll_lit_mul_0_store_cfa = define_primitive(interp, "(ll.lit*0!)", p_ll_lit_mul_0_store, 4);
	ll_litrev_sub_0_store_cfa = define_primitive(interp, "(ll.litrev-0!)", p_ll_litrev_sub_0_store, 4);
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
	define_primitive(interp, "defer", p_defer, 0);
	define_primitive(interp, "embodies", p_embodies, 1);
	define_primitive(interp, "embodies!", p_embodies_final, 1);
	define_primitive(interp, "constant", p_constant, 0);
	define_primitive(interp, "symbol", p_symbol, 0);
	define_primitive(interp, "base", p_base, 0);
	define_primitive(interp, "unit", p_unit, 0);
	define_primitive(interp, "magnitude", p_magnitude, 0);
	define_primitive(interp, "unit-of", p_unit_of, 0);
	define_primitive(interp, "float>exact", p_float_to_exact, 0);
	define_primitive(interp, "exact>float", p_exact_to_float, 0);
	define_primitive(interp, "rationalize", p_rationalize, 0);
	define_primitive(interp, "value>bytes", p_value_to_bytes, 0);
	define_primitive(interp, "bytes>value", p_bytes_to_value, 0);
	define_primitive(interp, "complex", p_complex, 0);
	define_primitive(interp, "real-part", p_real_part, 0);
	define_primitive(interp, "imaginary-part", p_imaginary_part, 0);
	define_primitive(interp, "conjugate", p_conjugate, 0);
	define_primitive(interp, "arg", p_arg, 0);
	define_primitive(interp, "numerator", p_numerator, 0);
	define_primitive(interp, "denominator", p_denominator, 0);
	define_primitive(interp, "string>symbol", p_string_to_symbol, 0);
	define_primitive(interp, "forget", p_forget, 0);
	define_primitive(interp, "'", p_tick, 1);
	define_primitive(interp, "lookup", p_lookup, 0);
	define_primitive(interp, "to", p_to, 1);
	define_primitive(interp, ";", p_semicolon, 1);
	define_primitive(interp, "recurse", p_recurse, 1);
	define_primitive(interp, "inline", p_inline, 0);
	define_primitive(interp, "internal", p_internal, 0);
	define_primitive(interp, "if", p_if, 1);
	define_primitive(interp, "?if", p_qif, 1);
	define_primitive(interp, "then", p_then, 1);
	define_primitive(interp, "else", p_else, 1);
	define_primitive(interp, "begin", p_begin, 1);
	define_primitive(interp, "until", p_until, 1);
	define_primitive(interp, "again", p_again, 1);
	define_primitive(interp, "while", p_while, 1);
	define_primitive(interp, "repeat", p_repeat, 1);
	define_primitive(interp, "do", p_do, 1);
	define_primitive(interp, "loop", p_loop, 1);
	define_primitive(interp, "leave", p_leave, 1);
	define_primitive(interp, "continue", p_continue, 1);
	define_primitive(interp, "case", p_case, 1);
	define_primitive(interp, "of", p_of, 1);
	define_primitive(interp, "endof", p_endof, 1);
	define_primitive(interp, "endcase", p_endcase, 1);
	define_primitive(interp, "[:", p_qcolon, 1);
	define_primitive(interp, ":]", p_qsemi, 1);
	define_primitive(interp, "|", p_bar, 1);

	define_primitive(interp, "0-matrix", p_0_matrix, 0);
	define_primitive(interp, "matrix", p_matrix, 0);
	define_primitive(interp, "dim", p_dim, 0);
	define_primitive(interp, "transpose", p_transpose, 0);
	define_primitive(interp, "submatrix", p_submatrix, 0);
	define_primitive(interp, "select-rows", p_select_rows, 0);
	define_primitive(interp, "mesh", p_mesh, 0);
	define_primitive(interp, "augment", p_augment, 0);
	define_primitive(interp, "vstack", p_vstack, 0);
	define_primitive(interp, "diagonal-matrix", p_diagonal_matrix, 0);
	vocab.at_i_cfa = define_primitive(interp, "@i", p_at_i, 0);
	define_primitive(interp, "!i", p_store_i, 0);
	define_primitive(interp, "@j", p_at_j, 0);
	define_primitive(interp, "@i,j", p_at_ij, 0);
	vocab.at_e_cfa = define_primitive(interp, "@e", p_at_e, 0);
	define_primitive(interp, "!e", p_store_e, 0);
	define_primitive(interp, "!i,j", p_store_ij, 0);
	define_primitive(interp, "diagonal", p_diagonal, 0);
	define_primitive(interp, "reshape", p_reshape, 0);
	define_primitive(interp, "matrix-range", p_matrix_range, 0);
	define_primitive(interp, "matrix>array", p_matrix_to_array, 0);
	define_primitive(interp, "matmul", p_matmul, 0);
	define_primitive(interp, "sum", p_sum, 0);
	define_primitive(interp, "var", p_variance, 0);
	define_primitive(interp, "quantile", p_quantile, 0);
	define_primitive(interp, "frobenius-norm", p_frobenius_norm, 0);
	define_primitive(interp, "row-sums", p_row_sums, 0);
	define_primitive(interp, "column-sums", p_column_sums, 0);
	define_primitive(interp, "cumulative-sum", p_cumulative_sum, 0);
	define_primitive(interp, "max", p_max, 0);
	define_primitive(interp, "min", p_min, 0);
	define_primitive(interp, "argmax", p_argmax, 0);
	define_primitive(interp, "argmin", p_argmin, 0);
	define_primitive(interp, "nonmissing-count", p_nonmissing_count, 0);
	define_primitive(interp, "where", p_where, 0);
	define_primitive(interp, "row-maxes", p_row_maxes, 0);
	define_primitive(interp, "row-mins", p_row_mins, 0);
	define_primitive(interp, "column-maxes", p_column_maxes, 0);
	define_primitive(interp, "column-mins", p_column_mins, 0);

	define_primitive(interp, "correlation-kendall", p_correlation_kendall, 0);
	define_primitive(interp, "ks-distance", p_ks_distance, 0);


	define_primitive(interp, "abs", p_abs, 0);
	define_primitive(interp, "sqrt", p_sqrt, 0);
	define_primitive(interp, "exp", p_exp, 0);
	define_primitive(interp, "log10", p_log, 0);
	define_primitive(interp, "ln", p_ln, 0);
	define_primitive(interp, "ln1+", p_ln1p, 0);
	define_primitive(interp, "log2", p_log2, 0);
	define_primitive(interp, "lgamma", p_lgamma, 0);
	define_primitive(interp, "erf", p_erf, 0);
	define_primitive(interp, "erfc", p_erfc, 0);
	define_primitive(interp, "^", p_power, 0);
	define_primitive(interp, "%", p_divmod, 0);
	define_primitive(interp, "mod", p_mod, 0);
	define_primitive(interp, "min2", p_min2, 0);
	define_primitive(interp, "max2", p_max2, 0);
	define_primitive(interp, "sin", p_sin, 0);
	define_primitive(interp, "cos", p_cos, 0);
	define_primitive(interp, "tan", p_tan, 0);
	define_primitive(interp, "tanh", p_tanh, 0);
	define_primitive(interp, "sinh", p_sinh, 0);
	define_primitive(interp, "cosh", p_cosh, 0);
	define_primitive(interp, "asin", p_asin, 0);
	define_primitive(interp, "acos", p_acos, 0);
	define_primitive(interp, "atan", p_atan, 0);
	define_primitive(interp, "atan2", p_atan2, 0);
	define_primitive(interp, "round", p_round, 0);
	define_primitive(interp, "round-up", p_round_up, 0);
	define_primitive(interp, "round-down", p_round_down, 0);
	define_primitive(interp, "truncate", p_truncate, 0);

	define_primitive(interp, "seed", p_seed, 0);
	define_primitive(interp, "random", p_random, 0);
	define_primitive(interp, "random-int", p_random_int, 0);
	define_primitive(interp, "resample-indices-ext", p_resample_indices_ext, 0);
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
	define_primitive(interp, "(tick-every)", p_tick_every, 4);
	define_primitive(interp, "args", p_args, 0);
	define_primitive(interp, "env", p_env, 0);
	define_primitive(interp, "env!", p_env_set, 0);
	define_primitive(interp, "cd", p_cd, 0);
	define_primitive(interp, "cwd", p_cwd, 0);
	define_primitive(interp, "binary-dir", p_binary_dir, 0);
	define_primitive(interp, "file-exists?", p_file_exists, 0);
	define_primitive(interp, "read-file", p_read_file, 0);
	define_primitive(interp, "write-file", p_write_file, 0);
	define_primitive(interp, "append-file", p_append_file, 0);
	define_primitive(interp, "list-directory", p_list_directory, 0);
	define_primitive(interp, "(file-info)", p_file_info, 4);
	define_primitive(interp, "make-directory", p_make_directory, 0);
	define_primitive(interp, "delete-file", p_delete_file, 0);
	define_primitive(interp, "delete-directory", p_delete_directory, 0);
	define_primitive(interp, "rename-file", p_rename_file, 0);
	define_primitive(interp, "touch-file", p_touch_file, 0);
	define_primitive(interp, "load-tsv", p_load_tsv, 0);
	define_primitive(interp, "save-tsv", p_save_tsv, 0);
	define_primitive(interp, "start-process", p_start_process, 0);
	define_primitive(interp, "open-file", p_open_file, 0);
	define_primitive(interp, "write", p_write, 0);
	define_primitive(interp, "read", p_read, 0);
	define_primitive(interp, "read-available", p_read_available, 0);
	define_primitive(interp, "read-line", p_read_line, 0);
	define_primitive(interp, "wait-readable", p_wait_readable, 0);
	define_primitive(interp, "close", p_close, 0);
	define_primitive(interp, "stdin", p_stdin, 0);
	define_primitive(interp, "stdout", p_stdout, 0);
	define_primitive(interp, "stderr", p_stderr, 0);
	define_primitive(interp, "stdout>string", p_stdout_to_string, 0);
	define_primitive(interp, "tty?", p_tty, 0);
	define_primitive(interp, "db-open", p_db_open, 0);
	define_primitive(interp, "ffi-open", p_ffi_open, 0);
	define_primitive(interp, "ffi-function", p_ffi_function, 0);
	define_primitive(interp, "ffi-variadic", p_ffi_variadic, 0);
	ffi_register_call_cfa(define_primitive(interp, "(ffi-call)", p_ffi_call, 4));
	define_primitive(interp, "ffi-free", p_ffi_free, 0);
	define_primitive(interp, "matrix>pointer", p_matrix_to_pointer, 0);
	define_primitive(interp, "floats>matrix", p_floats_to_matrix, 0);
	define_primitive(interp, "segment>pointer", p_segment_to_pointer, 0);
	define_primitive(interp, "pointer-cell", p_pointer_cell, 0);
	define_primitive(interp, "pointer-deref", p_pointer_deref, 0);
	define_primitive(interp, "pointer-long", p_pointer_long, 0);
	define_primitive(interp, "pointer-string-at", p_pointer_string_at, 0);
	define_primitive(interp, "pointer>address", p_pointer_to_address, 0);
	define_primitive(interp, "db-close", p_db_close, 0);
	define_primitive(interp, "db-exec", p_db_exec, 0);
	define_primitive(interp, "(db-query)", p_db_query, 4);
	define_primitive(interp, "wait", p_wait, 0);
	define_primitive(interp, "stop", p_stop_process, 0);
	define_primitive(interp, "running?", p_running, 0);

	if (load_lib) {
		memcpy(compiler.input_buffer, lib_telic, lib_telic_len);
		compiler.input_buffer[lib_telic_len] = 0;
		compiler.input_buffer_len = (int)lib_telic_len;
		compiler.input_buffer_pos = 0;
		run_outer(interp);

		compiler.input_buffer_len = 0;
		compiler.input_buffer_pos = 0;
		compiler.input_buffer[0] = 0;

		if (interp->error_flag) {
			if (interp->error_message[0])
				fprintf(stderr, "telic: lib.telic load error: %s\n", interp->error_message);
			else
				fprintf(stderr, "telic: lib.telic load error\n");
			if (interp->error_trace[0])
				fprintf(stderr, "%s\n", interp->error_trace);
			return 1;
		}
	}


	vocab.init_here = vocab.here;
	vocab.init_latest_cfa = vocab.latest_cfa;
	vocab.init_names_here = vocab.names_here;
	vocab.init_source_here = vocab.source_here;
	vocab.init_symbol_pool_here = vocab.symbol_pool_here;
	arena.object_space.init = arena.object_space.n;
	pairs.space.init = pairs.space.n;
	alloc_count_lvar = 0;
	alloc_count_array = 0;
	dimension_freeze();

	vocab.lib_end_latest_cfa = vocab.latest_cfa;
	return 0;
}

static void run_program_text(Interpreter *interp, const char *text) {
	int text_len = (int)strlen(text);
	if (text_len >= INPUT_BUFFER_SIZE) {
		fail(interp, "-e: program too large (%d bytes, max %d)", text_len, INPUT_BUFFER_SIZE - 1);
		return;
	}

	memcpy(compiler.input_buffer, text, (size_t)text_len + 1);
	compiler.input_buffer_len = text_len;
	compiler.input_buffer_pos = 0;
	compiler.need_more = 0;

	run_outer(interp);

	if (!interp->error_flag && compiler.need_more)
		fail(interp, "-e: unterminated string literal");
	if (!interp->error_flag && compiler.compiling) {
		fail(interp, "-e: unterminated definition");
		compiler.compiling = 0;
	}
	if (interp->error_flag)
		rollback_partial_definition();

	inbuf_reset();
}

static void print_usage(void) {
	printf("usage: telic [options] [file.telic [args ...]]\n"
		"\n"
		"Runs the program file and exits; everything after it is the program's,\n"
		"answered as a string array by the word `args`. With no file, starts\n"
		"the REPL (interactive when stdin is a terminal).\n"
		"\n"
		"  -i, --interactive   drop into the REPL after running the program\n"
		"  -b, --batch         no banner or prompts, even on a terminal\n"
		"  -e CODE             run CODE as a program, in argument order before the file (implies -b)\n"
		"      --no-lib        skip loading the embedded library\n"
		"      --arena SIZE    reserve SIZE gigabytes of heap (e.g. 32g)\n"
		"      --max-objects N cap the object table at N entries\n"
		"  -v, --version       print the logo and version, then exit\n"
		"  -w, --words         print the word listing, then exit\n"
		"  -h, --help          print this help, then exit\n");
}

int main(int argc, char **argv) {
	int interactive = isatty(fileno(stdin));
	int interactive_set = 0;
	int load_lib = 1;
	int show_version = 0;
	int show_words = 0;
	long max_objects_arg = 0;
	const char *program_items[argc];
	unsigned char program_item_is_code[argc];
	int n_program_items = 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			print_usage();
			return 0;
		}
		else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0)
			show_version = 1;
		else if (strcmp(argv[i], "-w") == 0 || strcmp(argv[i], "--words") == 0)
			show_words = 1;
		else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--interactive") == 0) {
			interactive = 1;
			interactive_set = 1;
		}
		else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--batch") == 0) {
			interactive = 0;
			interactive_set = 1;
		}
		else if (strcmp(argv[i], "-e") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "telic: -e needs a code string\n");
				return 2;
			}
			program_items[n_program_items] = argv[++i];
			program_item_is_code[n_program_items] = 1;
			n_program_items++;
			interactive = 0;
			interactive_set = 1;
		}
		else if (strcmp(argv[i], "--no-lib") == 0)
			load_lib = 0;
		else if (strcmp(argv[i], "--arena") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "telic: --arena needs a size in gigabytes (e.g. 32g)\n");
				return 2;
			}
			char *suffix;
			double gigabytes = strtod(argv[++i], &suffix);
			int suffix_ok = *suffix == 0 || ((*suffix == 'g' || *suffix == 'G') && suffix[1] == 0);
			if (!suffix_ok || !(gigabytes >= 1) || gigabytes > 8e9) {
				fprintf(stderr, "telic: --arena takes gigabytes from 1g up (e.g. 32g)\n");
				return 2;
			}
			arena_reserve_request = (size_t)(gigabytes * (double)((size_t)1 << 30));
		}
		else if (strcmp(argv[i], "--max-objects") == 0) {
			if (i + 1 >= argc) {
				fprintf(stderr, "telic: --max-objects needs a value\n");
				return 2;
			}
			max_objects_arg = strtol(argv[++i], NULL, 10);
			if (max_objects_arg < 1) {
				fprintf(stderr, "telic: --max-objects must be a positive integer\n");
				return 2;
			}
		}
		else if (argv[i][0] == '-') {
			fprintf(stderr, "telic: unknown option '%s' (see telic --help)\n", argv[i]);
			return 2;
		}
		else {
			program_items[n_program_items] = argv[i];
			program_item_is_code[n_program_items] = 0;
			n_program_items++;
			set_script_args(argv + i + 1, argc - i - 1);
			break;
		}
	}

	if (n_program_items > 0 && !interactive_set)
		interactive = 0;

	Interpreter *interp = main_init();
	if (max_objects_arg > 0) {
		max_objects_arg = MIN(max_objects_arg, MAX_OBJECTS);
		arena.object_space.max = (int)max_objects_arg;
	}
	platform_init();
	if (construct_vocabulary(interp, load_lib))
		return 1;

	session_unit = current_unit = next_unit++;

	if (show_version) {
		execute_cfa(interp, find("telic"));
		return 0;
	}

	if (show_words) {
		execute_cfa(interp, find("words"));
		return 0;
	}

	compiler.interactive = interactive;

	for (int i = 0; i < n_program_items; i++) {
		if (program_item_is_code[i])
			run_program_text(interp, program_items[i]);
		else
			load_file(interp, program_items[i]);
		if (interp->error_flag) {
			fprintf(stderr, "error: %s\n", interp->error_message);
			if (interp->error_trace[0])
				fprintf(stderr, "%s\n", interp->error_trace);
			return 1;
		}
		if (!program_item_is_code[i])
			record_loaded_file(interp, program_items[i]);
	}

	if (n_program_items > 0 && !interactive)
		return 0;

	interactive = platform_repl_begin(interp, interactive);
	compiler.interactive = interactive;

	for (;;) {
		int fresh_entry = compiler.input_buffer_len == 0;
		int chunk = platform_read_chunk(compiler.input_buffer + compiler.input_buffer_len,
				INPUT_BUFFER_SIZE - compiler.input_buffer_len, interactive);
		if (chunk == 0)
			break;
		if (chunk > 0)
			compiler.input_buffer_len += chunk;

		if (fresh_entry) {
			interp->gc_pending &= ~INTERRUPT_PENDING;
			compiler.input_line++;
			if (interp->dsp > interp->entry_snapshot_cap) {
				interp->entry_snapshot = realloc(interp->entry_snapshot,
						sizeof(Val) * (size_t)interp->dsp);
				interp->entry_snapshot_cap = interp->dsp;
			}
			if (interp->dsp > 0)
				memcpy(interp->entry_snapshot, interp->data_stack, sizeof(Val) * (size_t)interp->dsp);
			interp->entry_snapshot_depth = interp->dsp;
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
			rollback_partial_definition();
			compiler.loop_begin = 0;
			compiler.leave_chain = 0;
			compiler.do_continue_chain = 0;
			compiler.case_chain = 0;
			compiler.n_active_do_loops = 0;
			compiler.compiling = 0;
			if (interp->entry_snapshot_depth > 0)
				memcpy(interp->data_stack, interp->entry_snapshot,
						sizeof(Val) * (size_t)interp->entry_snapshot_depth);
			interp->dsp = interp->entry_snapshot_depth;
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
			if (interp->error_flag) {
				fputs(interp->error_message, stdout);
				if (interp->error_trace[0]) {
					putchar('\n');
					fputs(interp->error_trace, stdout);
				}
			} else {
				fputs(term_bold(), stdout);
				fputs("ok", stdout);
				if (interp->dsp > 0) {
					printf(" %d", interp->dsp);
					fputs(term_plain(), stdout);
					putchar('|');
					fputs(term_bold(), stdout);
					print_val_compact(stdout, interp, interp->data_stack[interp->dsp - 1]);
				}
				fputs(term_plain(), stdout);
			}
			putchar('\n');
			fflush(stdout);

			int after_entry_cfa = find("after-entry");
			if (after_entry_cfa) {
				interp->error_flag = 0;
				execute_cfa(interp, after_entry_cfa);
				if (interp->error_flag) {
					printf("after-entry: %s\n", interp->error_message);
					fflush(stdout);
				}
			}
		} else if (interp->error_flag) {
			fprintf(stdout, "error: %s\n", interp->error_message);
			if (interp->error_trace[0])
				fprintf(stdout, "%s\n", interp->error_trace);
			fflush(stdout);
		}

		interp->entry_snapshot_depth = 0;
		inbuf_reset();
	}
	return 0;
}

