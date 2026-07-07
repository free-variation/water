/* OS / I/O primitives: files, TSV, stream fds, environment, working
   directory. Split out of words.c; all dependencies are declared in water.h. */
#include "water.h"

void p_env(Interpreter *interp) {
	POP_STRING(name, "env");
	const char *value = getenv(name->bytes);
	if (value == NULL)
		push(interp, make_tagged(T_NONE, 0));
	else
		push(interp, make_string(object_new_string(interp, value, (int)strlen(value))));

	DISPATCH(interp);
}

void p_env_set(Interpreter *interp) {
	POP_STRING(value, "env!");
	POP_STRING(name, "env!");
	if (setenv(name->bytes, value->bytes, 1) != 0)
		fail(interp, "env!: could not set %s", name->bytes);

	DISPATCH(interp);
}

void p_cd(Interpreter *interp) {
	POP_STRING(path, "cd");
	if (chdir(path->bytes) != 0)
		fail(interp, "cd: cannot change to %s", path->bytes);

	DISPATCH(interp);
}

void p_cwd(Interpreter *interp) {
	char buffer[PATH_MAX];
	if (getcwd(buffer, sizeof buffer) == NULL) {
		fail(interp, "cwd: cannot read working directory");
		return;
	}
	push(interp, make_string(object_new_string(interp, buffer, (int)strlen(buffer))));

	DISPATCH(interp);
}

void p_read_file(Interpreter *interp) {
	POP_STRING(path, "read-file");

	FILE *file = fopen(path->bytes, "rb");
	if (file == NULL) {
		fail(interp, "read-file: cannot open %s", path->bytes);
		return;
	}

	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	rewind(file);
	if (size < 0 || size > INT_MAX) {
		fclose(file);
		fail(interp, "read-file: cannot size %s", path->bytes);
		return;
	}

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
	push(interp, make_string(handle));

	DISPATCH(interp);
}

static void write_file(Interpreter *interp, const char *mode, const char *op) {
	POP_STRING(path, op);
	POP_STRING(content, op);

	FILE *file = fopen(path->bytes, mode);
	if (file == NULL) {
		fail(interp, "%s: cannot open %s", op, path->bytes);
		return;
	}

	size_t written = fwrite(content->bytes, 1, (size_t)content->len, file);
	fclose(file);

	if (written != (size_t)content->len)
		fail(interp, "%s: short write to %s", op, path->bytes);
}

void p_write_file(Interpreter *interp) {
	write_file(interp, "wb", "write-file");

	DISPATCH(interp);
}

void p_append_file(Interpreter *interp) {
	write_file(interp, "ab", "append-file");

	DISPATCH(interp);
}

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

void p_read_tsv(Interpreter *interp) {
	POP_STRING(path, "read-tsv");

	FILE *file = fopen(path->bytes, "rb");
	if (file == NULL) {
		fail(interp, "read-tsv: cannot open %s", path->bytes);
		return;
	}

	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	rewind(file);
	if (size < 0 || size > INT_MAX) {
		fclose(file);
		fail(interp, "read-tsv: cannot size %s", path->bytes);
		return;
	}

	char *buffer = malloc((size_t)size + 1);
	if (buffer == NULL) {
		fclose(file);
		fail(interp, "read-tsv: out of memory");
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
	push(interp, make_array(outer_handle));

	DISPATCH(interp);
}

void p_write_tsv(Interpreter *interp) {
	POP_STRING(path, "write-tsv");
	POP_ARRAY(rows, "write-tsv");

	FILE *file = fopen(path->bytes, "wb");
	if (file == NULL) {
		fail(interp, "write-tsv: cannot open %s", path->bytes);
		return;
	}

	for (int r = 0; r < rows->len; r++) {
		Val row_val = rows->items[r];
		if (VAL_TAG(row_val) != T_ARRAY) {
			fclose(file);
			fail(interp, "write-tsv: row %d is %s, expected an array", r, tag_name(VAL_TAG(row_val)));
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
							fail(interp, "write-tsv: row %d cell %d contains a tab or newline", r, c);
							return;
						}
					fwrite(string->bytes, 1, (size_t)string->len, file);
					break;
				}
				default:
					fclose(file);
					fail(interp, "write-tsv: row %d cell %d is %s, cannot represent in TSV", r, c, tag_name(VAL_TAG(cell)));
					return;
			}
		}
		fputc('\n', file);
	}

	fclose(file);

	DISPATCH(interp);
}

void p_write(Interpreter *interp) {
	PEEK_AT(stream_val, 0, "write");
	if (VAL_TAG(stream_val) != T_STREAM) {
		fail(interp, "write: expected a stream; got %s", tag_name(VAL_TAG(stream_val)));
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
			fail(interp, "write: %s", strerror(errno));
			return;
		}
		total_written += (int)bytes_written;
	}

	interp->dsp -= 2;

	DISPATCH(interp);
}

void p_read(Interpreter *interp) {
	PEEK_AT(stream_val, 0, "read");
	if (VAL_TAG(stream_val) != T_STREAM) {
		fail(interp, "read: expected a stream; got %s", tag_name(VAL_TAG(stream_val)));
		return;
	}
	int file_descriptor = (int)VAL_DATA(stream_val);

	int length = 0;
	int capacity = 1 << 16;
	char *buffer = malloc((size_t)capacity);
	if (!buffer) {
		fail(interp, "read: out of memory");
		return;
	}
	while (1) {
		if (length == capacity) {
			if (capacity > INT_MAX / 2) {
				free(buffer);
				fail(interp, "read: stream exceeds %d bytes", INT_MAX);
				return;
			}
			capacity *= 2;
			char *grown = realloc(buffer, (size_t)capacity);
			if (!grown) {
				free(buffer);
				fail(interp, "read: out of memory");
				return;
			}
			buffer = grown;
		}

		ssize_t bytes_read = read(file_descriptor, buffer + length, (size_t)(capacity - length));
		if (bytes_read < 0) {
			if (errno == EINTR)
				continue;
			free(buffer);
			fail(interp, "read: %s", strerror(errno));
			return;
		}

		if (bytes_read == 0)
			break;
		length += (int)bytes_read;
	}

	int handle = object_new_string(interp, buffer ? buffer : "", length);
	free(buffer);
	if (interp->error_flag) return;

	interp->data_stack[interp->dsp - 1] = make_string(handle);

	DISPATCH(interp);
}

void p_close(Interpreter *interp) {
	PEEK_AT(stream_val, 0, "close");
	if (VAL_TAG(stream_val) != T_STREAM) {
		fail(interp, "close: expected a stream; got %s", tag_name(VAL_TAG(stream_val)));
		return;
	}
	close((int)VAL_DATA(stream_val));
	interp->dsp -= 1;

	DISPATCH(interp);
}

/* The three standard streams as T_STREAM values over fds 0/1/2, so they compose
   with read/write/close. stdin conflicts with the REPL's own stdin reading;
   it's for programs loaded from a file, not piped in. */
void p_stdin(Interpreter *interp)  { push(interp, make_stream(0)); DISPATCH(interp); }
void p_stdout(Interpreter *interp) { push(interp, make_stream(1)); DISPATCH(interp); }
void p_stderr(Interpreter *interp) { push(interp, make_stream(2)); DISPATCH(interp); }

