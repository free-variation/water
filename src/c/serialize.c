#include "telic.h"

#define SERIAL_MAGIC_0 'T'
#define SERIAL_MAGIC_1 'E'
#define SERIAL_MAGIC_2 'L'
#define SERIAL_MAGIC_3 'C'
#define SERIAL_VERSION 1
#define SERIAL_SEGMENT_INT 0
#define SERIAL_SEGMENT_DOUBLE 1

enum {
	SERIAL_NONE = 1,
	SERIAL_WILDCARD,
	SERIAL_FLOAT,
	SERIAL_SYMBOL,
	SERIAL_STRING,
	SERIAL_ARRAY,
	SERIAL_SET,
	SERIAL_FRAME,
	SERIAL_PAIR,
	SERIAL_MATRIX,
	SERIAL_SEGMENT,
	SERIAL_EXACT,
	SERIAL_COMPLEX,
	SERIAL_QUANTITY,
	SERIAL_BACKREF
};

#if defined(SERIAL_PRETEND_BIG_ENDIAN) || (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define SERIAL_HOST_IS_LITTLE_ENDIAN 0
#else
#define SERIAL_HOST_IS_LITTLE_ENDIAN 1
#endif

static uint32_t little_endian_32(uint32_t value) {
	return SERIAL_HOST_IS_LITTLE_ENDIAN ? value : __builtin_bswap32(value);
}

static uint64_t little_endian_64(uint64_t value) {
	return SERIAL_HOST_IS_LITTLE_ENDIAN ? value : __builtin_bswap64(value);
}

static uint64_t double_bits(double value) {
	uint64_t bits;
	memcpy(&bits, &value, 8);
	return bits;
}

static double bits_double(uint64_t bits) {
	double value;
	memcpy(&value, &bits, 8);
	return value;
}

typedef struct {
	uint64_t *keys;
	int *indices;
	int n_slots;
	int n_entries;
} SeenTable;

typedef struct {
	unsigned char *bytes;
	size_t length;
	size_t capacity;
	SeenTable seen;
	int n_written;
	int failed;
} Writer;

typedef struct {
	const unsigned char *bytes;
	size_t length;
	size_t position;
	Val *values;
	int n_values;
	int values_capacity;
} Reader;

static int seen_init(Writer *writer) {
	writer->seen.n_slots = 256;
	writer->seen.n_entries = 0;
	writer->seen.keys = calloc((size_t)writer->seen.n_slots, sizeof(uint64_t));
	writer->seen.indices = calloc((size_t)writer->seen.n_slots, sizeof(int));
	return writer->seen.keys && writer->seen.indices;
}

static void seen_free(Writer *writer) {
	free(writer->seen.keys);
	free(writer->seen.indices);
}

static int seen_slot(const SeenTable *seen, uint64_t key) {
	uint64_t hash = key * 0x9E3779B97F4A7C15ull;
	int slot = (int)(hash >> 40) & (seen->n_slots - 1);

	while (seen->keys[slot] && seen->keys[slot] != key)
		slot = (slot + 1) & (seen->n_slots - 1);
	return slot;
}

static int seen_grow(SeenTable *seen) {
	int n_slots = seen->n_slots * 2;
	uint64_t *keys = calloc((size_t)n_slots, sizeof(uint64_t));
	int *indices = calloc((size_t)n_slots, sizeof(int));
	if (!keys || !indices) {
		free(keys);
		free(indices);
		return 0;
	}

	SeenTable grown = { keys, indices, n_slots, seen->n_entries };
	for (int i = 0; i < seen->n_slots; i++)
		if (seen->keys[i]) {
			int slot = seen_slot(&grown, seen->keys[i]);
			keys[slot] = seen->keys[i];
			indices[slot] = seen->indices[i];
		}

	free(seen->keys);
	free(seen->indices);
	*seen = grown;
	return 1;
}

static int seen_lookup(Writer *writer, Val value, int *index) {
	int slot = seen_slot(&writer->seen, value.bits);
	if (writer->seen.keys[slot]) {
		*index = writer->seen.indices[slot];
		return 1;
	}
	return 0;
}

static void seen_record(Writer *writer, Val value, int index) {
	if (writer->seen.n_entries * 2 >= writer->seen.n_slots && !seen_grow(&writer->seen)) {
		writer->failed = 1;
		return;
	}

	int slot = seen_slot(&writer->seen, value.bits);
	writer->seen.keys[slot] = value.bits;
	writer->seen.indices[slot] = index;
	writer->seen.n_entries++;
}

static void write_bytes(Writer *writer, const void *source, size_t n_bytes) {
	if (writer->failed)
		return;

	if (writer->length + n_bytes > writer->capacity) {
		size_t capacity = writer->capacity ? writer->capacity : 256;
		while (capacity < writer->length + n_bytes)
			capacity *= 2;

		unsigned char *grown = realloc(writer->bytes, capacity);
		if (!grown) {
			writer->failed = 1;
			return;
		}
		writer->bytes = grown;
		writer->capacity = capacity;
	}

	memcpy(writer->bytes + writer->length, source, n_bytes);
	writer->length += n_bytes;
}

static void write_tag(Writer *writer, int tag) {
	unsigned char byte = (unsigned char)tag;
	write_bytes(writer, &byte, 1);
}

static void write_u32(Writer *writer, int value) {
	uint32_t stored = little_endian_32((uint32_t)value);
	write_bytes(writer, &stored, 4);
}

static void write_i32(Writer *writer, int value) {
	uint32_t stored = little_endian_32((uint32_t)(int32_t)value);
	write_bytes(writer, &stored, 4);
}

static void write_double(Writer *writer, double value) {
	uint64_t stored = little_endian_64(double_bits(value));
	write_bytes(writer, &stored, 8);
}

static void write_doubles(Writer *writer, const double *values, int n_values) {
	if (SERIAL_HOST_IS_LITTLE_ENDIAN) {
		write_bytes(writer, values, (size_t)n_values * sizeof(double));
		return;
	}

	for (int i = 0; i < n_values; i++)
		write_double(writer, values[i]);
}

static void write_limbs(Writer *writer, const uint32_t *limbs, int n_limbs) {
	if (SERIAL_HOST_IS_LITTLE_ENDIAN) {
		write_bytes(writer, limbs, (size_t)n_limbs * 4);
		return;
	}

	for (int i = 0; i < n_limbs; i++) {
		uint32_t stored = little_endian_32(limbs[i]);
		write_bytes(writer, &stored, 4);
	}
}

static void write_text(Writer *writer, const char *text, int length) {
	write_u32(writer, length);
	write_bytes(writer, text, (size_t)length);
}

static void write_value(Interpreter *interp, Writer *writer, Val value);

static void write_unit(Interpreter *interp, Writer *writer, int unit) {
	const char *unit_name;
	const char *term_dimensions[MAX_UNIT_TERMS];
	int power_numerators[MAX_UNIT_TERMS];
	int power_denominators[MAX_UNIT_TERMS];
	int scale_numerator;
	int scale_denominator;

	int n_terms = unit_description(unit, &unit_name, term_dimensions,
			power_numerators, power_denominators, &scale_numerator, &scale_denominator);

	for (int i = 0; i < n_terms; i++)
		if (term_dimensions[i][0] == 0) {
			fail(interp, "cannot serialize a quantity in an unnamed dimension");
			writer->failed = 1;
			return;
		}

	write_text(writer, unit_name, (int)strlen(unit_name));
	write_u32(writer, n_terms);
	for (int i = 0; i < n_terms; i++) {
		write_text(writer, term_dimensions[i], (int)strlen(term_dimensions[i]));
		write_i32(writer, power_numerators[i]);
		write_i32(writer, power_denominators[i]);
	}
	write_i32(writer, scale_numerator);
	write_i32(writer, scale_denominator);
}

static int write_shared_header(Writer *writer, Val value, int tag) {
	int index;
	if (seen_lookup(writer, value, &index)) {
		write_tag(writer, SERIAL_BACKREF);
		write_u32(writer, index);
		return 1;
	}

	seen_record(writer, value, writer->n_written++);
	write_tag(writer, tag);
	return 0;
}

static void write_value(Interpreter *interp, Writer *writer, Val value) {
	if (writer->failed || interp->error_flag)
		return;

	value = deref(interp, value);

	switch (VAL_TAG(value)) {
		case T_NONE:
			write_tag(writer, SERIAL_NONE);
			return;
		case T_UNBOUND:
			write_tag(writer, SERIAL_WILDCARD);
			return;
		case T_FLOAT:
			write_tag(writer, SERIAL_FLOAT);
			write_double(writer, VAL_NUMBER(value));
			return;
		case T_SYMBOL: {
			const char *name = &vocab.symbol_pool[VAL_DATA(value)];
			write_tag(writer, SERIAL_SYMBOL);
			write_text(writer, name, (int)strlen(name));
			return;
		}
		case T_STRING: {
			if (write_shared_header(writer, value, SERIAL_STRING))
				return;
			Object *string_object = OBJECT_AT(VAL_DATA(value));
			write_text(writer, string_object->bytes, string_object->len);
			return;
		}
		case T_ARRAY:
		case T_SET: {
			int tag = VAL_TAG(value) == T_ARRAY ? SERIAL_ARRAY : SERIAL_SET;
			if (write_shared_header(writer, value, tag))
				return;
			Object *collection = OBJECT_AT(VAL_DATA(value));
			write_u32(writer, collection->len);
			for (int i = 0; i < collection->len; i++)
				write_value(interp, writer, collection->items[i]);
			return;
		}
		case T_FRAME: {
			if (write_shared_header(writer, value, SERIAL_FRAME))
				return;
			Object *frame = OBJECT_AT(VAL_DATA(value));
			write_u32(writer, frame->len);
			for (int i = 0; i < frame->len; i++) {
				const char *key = &vocab.symbol_pool[frame->frame.keys[i]];
				write_text(writer, key, (int)strlen(key));
				write_value(interp, writer, frame->frame.values[i]);
			}
			return;
		}
		case T_MATRIX: {
			if (write_shared_header(writer, value, SERIAL_MATRIX))
				return;
			Object *matrix = OBJECT_AT(VAL_DATA(value));
			write_u32(writer, matrix->matrix.rows);
			write_u32(writer, matrix->matrix.columns);
			write_doubles(writer, matrix->matrix.elements,
					matrix->matrix.rows * matrix->matrix.columns);
			return;
		}
		case T_SEGMENT: {
			if (write_shared_header(writer, value, SERIAL_SEGMENT))
				return;
			Object *segment = OBJECT_AT(VAL_DATA(value));
			write_u32(writer, SERIAL_SEGMENT_INT);
			write_u32(writer, segment->segment.length);
			for (int i = 0; i < segment->segment.length; i++)
				write_double(writer, segment_get(segment, i));
			return;
		}
		case T_EXACT: {
			if (write_shared_header(writer, value, SERIAL_EXACT))
				return;
			Object *exact = OBJECT_AT(VAL_DATA(value));
			write_i32(writer, exact->exact.sign);
			write_u32(writer, exact->exact.n_numerator);
			write_u32(writer, exact->exact.n_denominator);
			write_limbs(writer, exact->exact.limbs,
					exact->exact.n_numerator + exact->exact.n_denominator);
			return;
		}
		case T_COMPLEX: {
			if (write_shared_header(writer, value, SERIAL_COMPLEX))
				return;
			int slot = (int)VAL_DATA(value);
			write_double(writer, VAL_NUMBER(pairs.table[slot].head));
			write_double(writer, VAL_NUMBER(pairs.table[slot].tail));
			return;
		}
		case T_QUANTITY: {
			if (write_shared_header(writer, value, SERIAL_QUANTITY))
				return;
			int slot = (int)VAL_DATA(value);
			write_unit(interp, writer, (int)pairs.table[slot].tail.bits);
			write_value(interp, writer, pairs.table[slot].head);
			return;
		}
		default:
			fail(interp, "cannot serialize %s", tag_name(VAL_TAG(value)));
			writer->failed = 1;
			return;
	}
}

static int read_bytes(Interpreter *interp, Reader *reader, void *target, size_t n_bytes) {
	if (reader->position + n_bytes > reader->length) {
		fail(interp, "truncated value data");
		return 0;
	}

	memcpy(target, reader->bytes + reader->position, n_bytes);
	reader->position += n_bytes;
	return 1;
}

static int read_u32(Interpreter *interp, Reader *reader, int *value) {
	uint32_t stored;
	if (!read_bytes(interp, reader, &stored, 4))
		return 0;

	stored = little_endian_32(stored);
	if (stored > (uint32_t)1 << 30) {
		fail(interp, "value data declares an impossible length");
		return 0;
	}

	*value = (int)stored;
	return 1;
}

static int read_i32(Interpreter *interp, Reader *reader, int *value) {
	uint32_t stored;
	if (!read_bytes(interp, reader, &stored, 4))
		return 0;

	*value = (int32_t)little_endian_32(stored);
	return 1;
}

static int read_double(Interpreter *interp, Reader *reader, double *value) {
	uint64_t stored;
	if (!read_bytes(interp, reader, &stored, 8))
		return 0;

	*value = bits_double(little_endian_64(stored));
	return 1;
}

static void doubles_from_little_endian(double *values, int n_values) {
	if (SERIAL_HOST_IS_LITTLE_ENDIAN)
		return;

	for (int i = 0; i < n_values; i++)
		values[i] = bits_double(little_endian_64(double_bits(values[i])));
}

static int read_text(Interpreter *interp, Reader *reader, char *target, int capacity) {
	int length;
	if (!read_u32(interp, reader, &length))
		return -1;
	if (length >= capacity) {
		fail(interp, "name in value data is longer than %d bytes", capacity - 1);
		return -1;
	}
	if (!read_bytes(interp, reader, target, (size_t)length))
		return -1;

	target[length] = 0;
	return length;
}

static int reader_remember(Interpreter *interp, Reader *reader, Val value) {
	if (reader->n_values == reader->values_capacity) {
		int capacity = reader->values_capacity ? reader->values_capacity * 2 : 64;
		Val *grown = realloc(reader->values, (size_t)capacity * sizeof(Val));
		if (!grown) {
			fail(interp, "out of memory");
			return -1;
		}
		reader->values = grown;
		reader->values_capacity = capacity;
	}

	reader->values[reader->n_values] = value;
	return reader->n_values++;
}

static int read_value(Interpreter *interp, Reader *reader, Val *out);

static int read_unit(Interpreter *interp, Reader *reader, int *unit) {
	char unit_name[NAME_MAX_LENGTH];
	char dimension_names_read[MAX_UNIT_TERMS][NAME_MAX_LENGTH];
	const char *term_dimensions[MAX_UNIT_TERMS];
	int power_numerators[MAX_UNIT_TERMS];
	int power_denominators[MAX_UNIT_TERMS];

	if (read_text(interp, reader, unit_name, sizeof(unit_name)) < 0)
		return 0;

	int n_terms;
	if (!read_u32(interp, reader, &n_terms))
		return 0;
	if (n_terms > MAX_UNIT_TERMS) {
		fail(interp, "unit in value data has %d dimensions (max %d)", n_terms, MAX_UNIT_TERMS);
		return 0;
	}

	for (int i = 0; i < n_terms; i++) {
		if (read_text(interp, reader, dimension_names_read[i], NAME_MAX_LENGTH) < 0)
			return 0;
		term_dimensions[i] = dimension_names_read[i];
		if (!read_i32(interp, reader, &power_numerators[i]))
			return 0;
		if (!read_i32(interp, reader, &power_denominators[i]))
			return 0;
	}

	int scale_numerator;
	int scale_denominator;
	if (!read_i32(interp, reader, &scale_numerator))
		return 0;
	if (!read_i32(interp, reader, &scale_denominator))
		return 0;

	*unit = unit_declare(interp, unit_name, term_dimensions, power_numerators, power_denominators,
			n_terms, scale_numerator, scale_denominator);
	return *unit >= 0 && !interp->error_flag;
}

static int read_collection(Interpreter *interp, Reader *reader, int is_set, Val *out) {
	int n_elements;
	if (!read_u32(interp, reader, &n_elements))
		return 0;

	int handle = is_set ? object_new_set(interp) : object_new_array(interp, n_elements);
	if (interp->error_flag)
		return 0;

	Val collection = is_set ? make_set(handle) : make_array(handle);
	if (!is_set)
		for (int i = 0; i < n_elements; i++)
			OBJECT_AT(handle)->items[i] = make_tagged(T_NONE, 0);

	int index = reader_remember(interp, reader, collection);
	if (index < 0)
		return 0;

	gc_root_push(interp, collection);
	for (int i = 0; i < n_elements; i++) {
		Val element;
		if (!read_value(interp, reader, &element)) {
			gc_root_pop(interp);
			return 0;
		}

		if (is_set)
			set_add(interp, handle, element);
		else
			OBJECT_AT(handle)->items[i] = element;

		if (interp->error_flag) {
			gc_root_pop(interp);
			return 0;
		}
	}
	gc_root_pop(interp);

	reader->values[index] = collection;
	*out = collection;
	return 1;
}

static int read_frame(Interpreter *interp, Reader *reader, Val *out) {
	int n_pairs;
	if (!read_u32(interp, reader, &n_pairs))
		return 0;

	int handle = object_new_frame(interp);
	if (interp->error_flag)
		return 0;

	Val frame = make_frame(handle);
	int index = reader_remember(interp, reader, frame);
	if (index < 0)
		return 0;

	gc_root_push(interp, frame);
	for (int i = 0; i < n_pairs; i++) {
		char key[NAME_MAX_LENGTH];
		if (read_text(interp, reader, key, sizeof(key)) < 0) {
			gc_root_pop(interp);
			return 0;
		}

		int symbol = intern_symbol(interp, key);
		Val value;
		if (interp->error_flag || !read_value(interp, reader, &value)) {
			gc_root_pop(interp);
			return 0;
		}

		frame_put(OBJECT_AT(handle), (cell)symbol, value);
	}
	gc_root_pop(interp);

	*out = frame;
	return 1;
}

static int read_value(Interpreter *interp, Reader *reader, Val *out) {
	unsigned char tag;
	if (!read_bytes(interp, reader, &tag, 1))
		return 0;

	switch (tag) {
		case SERIAL_NONE:
			*out = make_tagged(T_NONE, 0);
			return 1;
		case SERIAL_WILDCARD:
			*out = make_tagged(T_UNBOUND, 0);
			return 1;
		case SERIAL_FLOAT: {
			double number;
			if (!read_double(interp, reader, &number))
				return 0;
			*out = make_float(number);
			return 1;
		}
		case SERIAL_SYMBOL: {
			char name[NAME_MAX_LENGTH];
			if (read_text(interp, reader, name, sizeof(name)) < 0)
				return 0;
			int symbol = intern_symbol(interp, name);
			if (interp->error_flag)
				return 0;
			*out = make_symbol(symbol);
			return 1;
		}
		case SERIAL_STRING: {
			int length;
			if (!read_u32(interp, reader, &length))
				return 0;
			if (reader->position + (size_t)length > reader->length) {
				fail(interp, "truncated value data");
				return 0;
			}

			int handle = object_new_string(interp, (const char *)reader->bytes + reader->position, length);
			if (interp->error_flag)
				return 0;
			reader->position += (size_t)length;

			*out = make_string(handle);
			return reader_remember(interp, reader, *out) >= 0;
		}
		case SERIAL_ARRAY:
			return read_collection(interp, reader, 0, out);
		case SERIAL_SET:
			return read_collection(interp, reader, 1, out);
		case SERIAL_FRAME:
			return read_frame(interp, reader, out);
		case SERIAL_PAIR:
			fail(interp, "value data holds a cons pair, which this version no longer has");
			return 0;
		case SERIAL_MATRIX: {
			int rows;
			int columns;
			if (!read_u32(interp, reader, &rows) || !read_u32(interp, reader, &columns))
				return 0;

			size_t n_bytes = (size_t)rows * (size_t)columns * sizeof(double);
			if (reader->position + n_bytes > reader->length) {
				fail(interp, "truncated value data");
				return 0;
			}

			int handle = object_new_matrix_raw(interp, rows, columns);
			if (interp->error_flag)
				return 0;

			memcpy(OBJECT_AT(handle)->matrix.elements, reader->bytes + reader->position, n_bytes);
			doubles_from_little_endian(OBJECT_AT(handle)->matrix.elements, rows * columns);
			reader->position += n_bytes;

			*out = make_matrix(handle);
			return reader_remember(interp, reader, *out) >= 0;
		}
		case SERIAL_SEGMENT: {
			int element_kind;
			int length;
			if (!read_u32(interp, reader, &element_kind) || !read_u32(interp, reader, &length))
				return 0;

			if (element_kind == SERIAL_SEGMENT_DOUBLE) {
				int handle = object_new_matrix_raw(interp, length, 1);
				if (interp->error_flag)
					return 0;

				double *elements = OBJECT_AT(handle)->matrix.elements;
				for (int i = 0; i < length; i++)
					if (!read_double(interp, reader, &elements[i]))
						return 0;

				*out = make_matrix(handle);
				return reader_remember(interp, reader, *out) >= 0;
			}
			if (element_kind != SERIAL_SEGMENT_INT) {
				fail(interp, "value data has an unknown segment kind %d", element_kind);
				return 0;
			}

			int handle = object_new_segment(interp, length);
			if (interp->error_flag)
				return 0;

			for (int i = 0; i < length; i++) {
				double element;
				if (!read_double(interp, reader, &element))
					return 0;
				segment_set(OBJECT_AT(handle), i, element);
			}

			*out = make_segment(handle);
			return reader_remember(interp, reader, *out) >= 0;
		}
		case SERIAL_EXACT: {
			int sign;
			int n_numerator;
			int n_denominator;
			if (!read_i32(interp, reader, &sign) || !read_u32(interp, reader, &n_numerator)
					|| !read_u32(interp, reader, &n_denominator))
				return 0;
			if (n_numerator < 1 || n_denominator < 1) {
				fail(interp, "exact in value data has no digits");
				return 0;
			}

			size_t n_bytes = ((size_t)n_numerator + n_denominator) * 4;
			if (reader->position + n_bytes > reader->length) {
				fail(interp, "truncated value data");
				return 0;
			}

			int n_limbs = n_numerator + n_denominator;
			uint32_t *limbs = malloc(n_bytes);
			if (!limbs) {
				fail(interp, "out of memory");
				return 0;
			}

			memcpy(limbs, reader->bytes + reader->position, n_bytes);
			for (int i = 0; i < n_limbs; i++)
				limbs[i] = little_endian_32(limbs[i]);

			int handle = object_new_exact(interp, sign, limbs, n_numerator,
					limbs + n_numerator, n_denominator);
			free(limbs);
			if (interp->error_flag)
				return 0;
			reader->position += n_bytes;

			*out = make_exact(handle);
			return reader_remember(interp, reader, *out) >= 0;
		}
		case SERIAL_COMPLEX: {
			double real_part;
			double imaginary_part;
			if (!read_double(interp, reader, &real_part) || !read_double(interp, reader, &imaginary_part))
				return 0;

			*out = complex_from_parts(interp, real_part, imaginary_part);
			if (interp->error_flag)
				return 0;
			return reader_remember(interp, reader, *out) >= 0;
		}
		case SERIAL_QUANTITY: {
			int unit;
			if (!read_unit(interp, reader, &unit))
				return 0;

			Val magnitude;
			if (!read_value(interp, reader, &magnitude))
				return 0;

			gc_root_push(interp, magnitude);
			*out = quantity_of(interp, magnitude, unit);
			gc_root_pop(interp);
			if (interp->error_flag)
				return 0;
			return reader_remember(interp, reader, *out) >= 0;
		}
		case SERIAL_BACKREF: {
			int index;
			if (!read_u32(interp, reader, &index))
				return 0;
			if (index >= reader->n_values) {
				fail(interp, "value data references an object that is not there");
				return 0;
			}

			*out = reader->values[index];
			return 1;
		}
		default:
			fail(interp, "value data has an unknown tag %d", tag);
			return 0;
	}
}

void p_value_to_bytes(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val value = chain_sp[-1];

	SYNC_REGISTERS(interp, chain_ip, chain_sp);
	Writer writer = { NULL, 0, 0, { NULL, NULL, 0, 0 }, 0, 0 };
	if (!seen_init(&writer)) {
		seen_free(&writer);
		fail(interp, "out of memory");
		return;
	}

	unsigned char header[5] = { SERIAL_MAGIC_0, SERIAL_MAGIC_1, SERIAL_MAGIC_2, SERIAL_MAGIC_3, SERIAL_VERSION };
	write_bytes(&writer, header, sizeof(header));
	write_value(interp, &writer, value);

	if (writer.failed && !interp->error_flag)
		fail(interp, "out of memory");

	if (interp->error_flag) {
		seen_free(&writer);
		free(writer.bytes);
		return;
	}

	int handle = object_new_string(interp, (const char *)writer.bytes, (int)writer.length);
	seen_free(&writer);
	free(writer.bytes);
	if (interp->error_flag)
		return;

	interp->data_stack[interp->dsp - 1] = make_string(handle);
	DISPATCH(interp);
}

void p_bytes_to_value(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val value = chain_sp[-1];
	REQUIRE_CHAIN_TAG(value, T_STRING, "bytes>value", "a string");

	SYNC_REGISTERS(interp, chain_ip, chain_sp);
	Object *source = OBJECT_AT(VAL_DATA(value));
	Reader reader = { (const unsigned char *)source->bytes, (size_t)source->len, 0, NULL, 0, 0 };

	unsigned char header[5];
	if (!read_bytes(interp, &reader, header, sizeof(header)))
		return;
	if (header[0] != SERIAL_MAGIC_0 || header[1] != SERIAL_MAGIC_1
			|| header[2] != SERIAL_MAGIC_2 || header[3] != SERIAL_MAGIC_3) {
		fail(interp, "not Telic value data");
		return;
	}
	if (header[4] != SERIAL_VERSION) {
		fail(interp, "unsupported value-data version %d", header[4]);
		return;
	}

	Val restored;
	int ok = read_value(interp, &reader, &restored);
	free(reader.values);
	if (!ok)
		return;

	interp->data_stack[interp->dsp - 1] = restored;
	DISPATCH(interp);
}
