/* Binary image serialization: save-image / load-image and the value
   readers/writers. Split out of core.c; shares the VM globals and a few
   allocator primitives (arena_alloc_object, heap_bytes_add, main_alloc,
   op_cell_count, rebuild_symbol_hash) declared in water.h. */
#include "water.h"

#define IMAGE_MAGIC "LF4I"
#define IMAGE_VERSION ((uint32_t)5)

#define HANDLER_DOCOL 1
#define HANDLER_DOVAR 2
#define HANDLER_DOSYM 3
#define HANDLER_DOUNIT 4

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
	if (handler == (cell)dovar || handler == (cell)dosym || handler == (cell)dounit)
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
	w_i32(file, arena.object_space.n);

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
			: (h == dosym) ? HANDLER_DOSYM
			: (h == dounit) ? HANDLER_DOUNIT : 0;
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

	dimension_save(file);

	for (int slot = 0; slot < arena.object_space.n; slot++) {
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
			case OBJECT_SEGMENT:
				w_i32(file, obj->segment.element_type);
				w_i32(file, obj->segment.length);
				if (obj->segment.length > 0)
					fwrite(obj->segment.data, segment_element_size(obj->segment.element_type), (size_t)obj->segment.length, file);
				break;
		}
	}

	w_i32(file, pairs.space.n);
	for (int i = 0; i < pairs.space.n; i++) {
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
		case T_PAIR:      return handle >= 0 && handle < pairs.space.n;
		case T_QUANTITY:  return handle >= 0 && handle < pairs.space.n
								 && unit_id_valid((int)pairs.table[handle].tail.bits);
		case T_LOGIC_VAR: return handle >= 0 && handle < interp->lvar_top;
		case T_SYMBOL:    return handle >= 0 && handle < vocab.symbol_pool_here;
		case T_XT:        return handle >= DICT_RESERVED && handle < vocab.here;
		default: return 1;
	}
	return handle >= 0 && handle < arena.object_space.n
			&& arena.objects[handle] && arena.objects[handle]->kind == want;
}

static int validate_loaded(Interpreter *interp) {
	for (int i = 0; i < arena.object_space.n; i++) {
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
	for (int i = 0; i < pairs.space.n; i++)
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
		fail(interp, "%s: not a water image", filename);
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
			: (kind == HANDLER_DOSYM) ? dosym
			: (kind == HANDLER_DOUNIT) ? dounit : NULL;
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

	if (!dimension_load(file)) {
		fail(interp, "%s: truncated unit tables", filename);
		goto done;
	}

	if (saved_n_objects > arena.object_space.max) {
		fail(interp, "%s: image has too many objects", filename);
		goto done;
	}

	if (saved_n_objects > arena.object_space.cap)
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
									heap_bytes_add(n * sizeof(double));
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
										  heap_bytes_add((size_t)return_len * sizeof(Val));
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
			case OBJECT_SEGMENT: {
									 int32_t element_type, length;
									 if (!r_i32(file, &element_type) || !r_i32(file, &length)) {
										 fail(interp, "%s: truncated segment header", filename);
										 goto done;
									 }
									 if (length < 0 || (element_type != SEGMENT_INT && element_type != SEGMENT_DOUBLE)) {
										 fail(interp, "%s: bad segment header", filename);
										 goto done;
									 }
									 obj->segment.element_type = element_type;
									 obj->segment.length = length;
									 size_t element_size = segment_element_size(element_type);
									 obj->segment.data = calloc((size_t)MAX(length, 1), element_size);
									 heap_bytes_add((size_t)length * element_size);
									 if (length > 0 && fread(obj->segment.data, element_size, (size_t)length, file) != (size_t)length) {
										 fail(interp, "%s: truncated segment data", filename);
										 goto done;
									 }
									 break;
								 }
			default:
									  fail(interp, "%s: unknown object kind %u", filename, kind);
									  goto done;
		}
	}
	arena.object_space.n = saved_n_objects;
	main_alloc.objects.next = arena.object_space.n;
	main_alloc.objects.end = arena.object_space.n;
	arena.object_space.n_free = 0;

	int32_t saved_n_pairs;
	if (!r_i32(file, &saved_n_pairs) || saved_n_pairs < 0) {
		fail(interp, "%s: bad pair count", filename);
		goto done;
	}
	while (pairs.space.cap < saved_n_pairs)
		GROW_PAIR_TABLE(pairs.space.cap * 2);
	for (int i = 0; i < saved_n_pairs; i++) {
		if (!r_val(file, &pairs.table[i].head) || !r_val(file, &pairs.table[i].tail)) {
			fail(interp, "%s: truncated pairs", filename);
			goto done;
		}
	}
	pairs.space.n = saved_n_pairs;
	main_alloc.pairs.next = main_alloc.pairs.end = pairs.space.n;
	pairs.space.n_free = 0;

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
		arena.object_space.n = saved_n_objects;
		forget_user(interp);
		interp->lvar_top = 0;
		interp->bind_trail_top = 0;
	}

	DISPATCH(interp);
}
