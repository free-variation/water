
#include "water.h"

void p_env(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val name_val = chain_sp[-1];
	if (VAL_TAG(name_val) != T_STRING) {
		fail(interp, "expected %s; got %s", tag_name(T_STRING), tag_name(VAL_TAG(name_val)));
		return;
	}

	const char *value = getenv(OBJECT_AT(VAL_DATA(name_val))->bytes);
	if (value == NULL)
		chain_sp[-1] = make_tagged(T_NONE, 0);
	else {
		int handle = object_new_string(interp, value, (int)strlen(value));
		if (interp->error_flag)
			return;
		chain_sp[-1] = make_string(handle);
	}

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_env_set(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val value_val = chain_sp[-1];
	if (VAL_TAG(value_val) != T_STRING) {
		fail(interp, "expected %s; got %s", tag_name(T_STRING), tag_name(VAL_TAG(value_val)));
		return;
	}
	Val name_val = chain_sp[-2];
	if (VAL_TAG(name_val) != T_STRING) {
		fail(interp, "expected %s; got %s", tag_name(T_STRING), tag_name(VAL_TAG(name_val)));
		return;
	}
	Object *name = OBJECT_AT(VAL_DATA(name_val));

	if (setenv(name->bytes, OBJECT_AT(VAL_DATA(value_val))->bytes, 1) != 0) {
		fail(interp, "could not set %s", name->bytes);
		return;
	}

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}

void p_cd(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val path_val = chain_sp[-1];
	if (VAL_TAG(path_val) != T_STRING) {
		fail(interp, "expected %s; got %s", tag_name(T_STRING), tag_name(VAL_TAG(path_val)));
		return;
	}
	Object *path = OBJECT_AT(VAL_DATA(path_val));

	if (chdir(path->bytes) != 0) {
		fail(interp, "cannot change to %s", path->bytes);
		return;
	}

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_cwd(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);
	char buffer[PATH_MAX];
	if (getcwd(buffer, sizeof buffer) == NULL) {
		fail(interp, "cannot read working directory");
		return;
	}

	int handle = object_new_string(interp, buffer, (int)strlen(buffer));
	if (interp->error_flag)
		return;
	*chain_sp = make_string(handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

static FILE *open_sized_read(Interpreter *interp, const char *path, long *size_out) {
	FILE *file = fopen(path, "rb");
	if (file == NULL) {
		fail(interp, "cannot open %s", path);
		return NULL;
	}

	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	rewind(file);
	if (size < 0 || size > INT_MAX) {
		fclose(file);
		fail(interp, "cannot size %s", path);
		return NULL;
	}

	*size_out = size;
	return file;
}

void p_read_file(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val path_val = chain_sp[-1];
	if (VAL_TAG(path_val) != T_STRING) {
		fail(interp, "expected %s; got %s", tag_name(T_STRING), tag_name(VAL_TAG(path_val)));
		return;
	}

	long size;
	FILE *file = open_sized_read(interp, OBJECT_AT(VAL_DATA(path_val))->bytes, &size);
	if (!file)
		return;

	int handle = object_new_string_uninit(interp, (int)size);
	if (interp->error_flag) {
		fclose(file);
		return;
	}

	Object *string = OBJECT_AT(handle);
	size_t got = fread(string->bytes, 1, (size_t)size, file);
	fclose(file);

	string->len = (int)got;
	string->bytes[got] = 0;
	chain_sp[-1] = make_string(handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

static void write_file(Interpreter *interp, Object *content, Object *path, const char *mode) {
	FILE *file = fopen(path->bytes, mode);
	if (file == NULL) {
		fail(interp, "cannot open %s", path->bytes);
		return;
	}

	size_t written = fwrite(content->bytes, 1, (size_t)content->len, file);
	fclose(file);

	if (written != (size_t)content->len)
		fail(interp, "short write to %s", path->bytes);
}

#define WRITE_FILE_OP(c_name, mode) \
	void c_name(DISPATCH_ARGS) { \
		REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2); \
		Val path_val = chain_sp[-1]; \
		if (VAL_TAG(path_val) != T_STRING) { \
			fail(interp, "expected %s; got %s", tag_name(T_STRING), tag_name(VAL_TAG(path_val))); \
			return; \
		} \
		Val content_val = chain_sp[-2]; \
		if (VAL_TAG(content_val) != T_STRING) { \
			fail(interp, "expected %s; got %s", tag_name(T_STRING), tag_name(VAL_TAG(content_val))); \
			return; \
		} \
		\
		write_file(interp, OBJECT_AT(VAL_DATA(content_val)), OBJECT_AT(VAL_DATA(path_val)), mode); \
		if (interp->error_flag) \
			return; \
		\
		DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2); \
	}

WRITE_FILE_OP(p_write_file, "wb")
WRITE_FILE_OP(p_append_file, "ab")

static int tsv_row_to_array(Interpreter *interp, char *row, int row_length) {
	int cell_count = 1;
	for (int i = 0; i < row_length; i++)
		if (row[i] == '\t')
			cell_count++;

	int array_handle = object_new_array(interp, cell_count);
	if (interp->error_flag) return -1;

	Object *array = OBJECT_AT(array_handle);
	memset(array->items, 0, sizeof(Val) * (size_t)cell_count);
	gc_root_push(interp, make_array(array_handle));

	int cell_index = 0;
	int cell_start = 0;
	for (int i = 0; i <= row_length; i++) {
			if (i < row_length && row[i] != '\t')
				continue;

			row[i] = 0;
			char *cell = row + cell_start;
			int cell_length = i - cell_start;
			double number;

			if (cell_length == 0)
				array->items[cell_index] = make_tagged(T_NONE, 0);
			else if (parse_float(cell, &number))
				array->items[cell_index] = make_float(number);
			else
				array->items[cell_index] = make_string(object_new_string(interp, cell, cell_length));

			if (interp->error_flag) {
				gc_root_pop(interp);
				return -1;
			}

			cell_index++;
			cell_start = i + 1;
	}

	gc_root_pop(interp);
	return array_handle;
}

void p_read_tsv(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val path_val = chain_sp[-1];
	if (VAL_TAG(path_val) != T_STRING) {
		fail(interp, "expected %s; got %s", tag_name(T_STRING), tag_name(VAL_TAG(path_val)));
		return;
	}

	long size;
	FILE *file = open_sized_read(interp, OBJECT_AT(VAL_DATA(path_val))->bytes, &size);
	if (!file)
		return;

	char *buffer = malloc((size_t)size + 1);
	if (buffer == NULL) {
		fclose(file);
		fail(interp, "out of memory");
		return;
	}
	int length = (int)fread(buffer, 1, (size_t)size, file);
	fclose(file);
	buffer[length] = 0;

	int row_count = 0;
	if (length > 0) {
		row_count = 1;
		for (int i = 0; i < length; i++)
			if (buffer[i] == '\n')
				row_count++;
		if (buffer[length - 1] == '\n')
			row_count--;
	}

	int outer_handle = object_new_array(interp, row_count);
	if (interp->error_flag) {
		free(buffer);
		return;
	}
	Object *outer = OBJECT_AT(outer_handle);
	memset(outer->items, 0, sizeof(Val) * (size_t)row_count);
	gc_root_push(interp, make_array(outer_handle));

	int row_index = 0;
	int row_start = 0;
	for (int i = 0; i <= length; i++) {
		if (i < length && buffer[i] != '\n')
			continue;
		if (i == length && i == row_start)
			break;
		int row_length = i - row_start;
		if (row_length > 0 && buffer[row_start + row_length - 1] == '\r')
			row_length--;
		buffer[row_start + row_length] = 0;
		int row_handle = tsv_row_to_array(interp, buffer + row_start, row_length);
		if (interp->error_flag) {
			gc_root_pop(interp);
			free(buffer);
			return;
		}
		outer->items[row_index++] = make_array(row_handle);
		row_start = i + 1;
	}

	gc_root_pop(interp);
	free(buffer);
	chain_sp[-1] = make_array(outer_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_write_tsv(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val path_val = chain_sp[-1];
	if (VAL_TAG(path_val) != T_STRING) {
		fail(interp, "expected %s; got %s", tag_name(T_STRING), tag_name(VAL_TAG(path_val)));
		return;
	}
	Val rows_val = chain_sp[-2];
	if (VAL_TAG(rows_val) != T_ARRAY) {
		fail(interp, "expected %s; got %s", tag_name(T_ARRAY), tag_name(VAL_TAG(rows_val)));
		return;
	}
	Object *rows = OBJECT_AT(VAL_DATA(rows_val));

	FILE *file = fopen(OBJECT_AT(VAL_DATA(path_val))->bytes, "wb");
	if (file == NULL) {
		fail(interp, "cannot open %s", OBJECT_AT(VAL_DATA(path_val))->bytes);
		return;
	}

	for (int r = 0; r < rows->len; r++) {
		Val row_val = rows->items[r];
		if (VAL_TAG(row_val) != T_ARRAY) {
			fclose(file);
			fail(interp, "row %d is %s, expected an array", r, tag_name(VAL_TAG(row_val)));
			return;
		}
		Object *row = OBJECT_AT(VAL_DATA(row_val));
		for (int c = 0; c < row->len; c++) {
			if (c > 0)
				fputc('\t', file);
			Val cell = row->items[c];
			switch (VAL_TAG(cell)) {
				case T_NONE:
					break;
				case T_FLOAT: {
					double number = VAL_NUMBER(cell);
					if (number == (double)(int64_t)number && number > -1e15 && number < 1e15)
						fprintf(file, "%lld", (long long)number);
					else
						fprintf(file, "%g", number);
					break;
				}
				case T_STRING: {
					Object *string = OBJECT_AT(VAL_DATA(cell));
					for (int b = 0; b < string->len; b++)
						if (string->bytes[b] == '\t' || string->bytes[b] == '\n') {
							fclose(file);
							fail(interp, "row %d cell %d contains a tab or newline", r, c);
							return;
						}
					fwrite(string->bytes, 1, (size_t)string->len, file);
					break;
				}
				default:
					fclose(file);
					fail(interp, "row %d cell %d is %s, cannot represent in TSV", r, c, tag_name(VAL_TAG(cell)));
					return;
			}
		}
		fputc('\n', file);
	}

	fclose(file);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}

void p_write(DISPATCH_ARGS) {
	PEEK_AT(stream_val, 0, "write");
	if (VAL_TAG(stream_val) != T_STREAM) {
		fail(interp, "expected a stream; got %s", tag_name(VAL_TAG(stream_val)));
		return;
	}
	PEEK_TYPE_AT(string_val, 1, "write", T_STRING);
	int file_descriptor = (int)VAL_DATA(stream_val);
	Object *string = OBJECT_AT(VAL_DATA(string_val));

	int total_written = 0;
	while (total_written < string->len) {
		ssize_t bytes_written = write(file_descriptor, string->bytes + total_written, (size_t)(string->len - total_written));
		if (bytes_written < 0) {
			if (errno == EINTR)
				continue;
			fail(interp, "%s", strerror(errno));
			return;
		}
		total_written += (int)bytes_written;
	}

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 2);
}

void p_read(DISPATCH_ARGS) {
	PEEK_AT(stream_val, 0, "read");
	if (VAL_TAG(stream_val) != T_STREAM) {
		fail(interp, "expected a stream; got %s", tag_name(VAL_TAG(stream_val)));
		return;
	}
	int file_descriptor = (int)VAL_DATA(stream_val);

	int length = 0;
	int capacity = 1 << 16;
	char *buffer = malloc((size_t)capacity);
	if (!buffer) {
		fail(interp, "out of memory");
		return;
	}
	while (1) {
		if (length == capacity) {
			if (capacity > INT_MAX / 2) {
				free(buffer);
				fail(interp, "stream exceeds %d bytes", INT_MAX);
				return;
			}
			capacity *= 2;
			char *grown = realloc(buffer, (size_t)capacity);
			if (!grown) {
				free(buffer);
				fail(interp, "out of memory");
				return;
			}
			buffer = grown;
		}

		ssize_t bytes_read = read(file_descriptor, buffer + length, (size_t)(capacity - length));
		if (bytes_read < 0) {
			if (errno == EINTR)
				continue;
			free(buffer);
			fail(interp, "%s", strerror(errno));
			return;
		}

		if (bytes_read == 0)
			break;
		length += (int)bytes_read;
	}

	int handle = object_new_string(interp, buffer ? buffer : "", length);
	free(buffer);
	if (interp->error_flag) return;

	chain_sp[-1] = make_string(handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_close(DISPATCH_ARGS) {
	PEEK_AT(stream_val, 0, "close");
	if (VAL_TAG(stream_val) != T_STREAM) {
		fail(interp, "expected a stream; got %s", tag_name(VAL_TAG(stream_val)));
		return;
	}
	close((int)VAL_DATA(stream_val));

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}


void p_stdin(DISPATCH_ARGS)  { push(interp, make_stream(0)); DISPATCH(interp); }
void p_stdout(DISPATCH_ARGS) { push(interp, make_stream(1)); DISPATCH(interp); }
void p_stderr(DISPATCH_ARGS) { push(interp, make_stream(2)); DISPATCH(interp); }

