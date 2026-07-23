#include "water.h"
#include <ffi/ffi.h>
#include <dlfcn.h>

#define FFI_MAX_ARGS 16

typedef enum {
	FFI_VOID = 0,
	FFI_INT,
	FFI_LONG,
	FFI_DOUBLE,
	FFI_PTR,
	FFI_STRING
} FFITypeTag;

typedef struct {
	void *function;
	ffi_cif cif;
	ffi_type *arg_ffi[FFI_MAX_ARGS];
	FFITypeTag arg_tags[FFI_MAX_ARGS];

	int argc;
	FFITypeTag return_tag;
} FFIBinding;

static void **ffi_pointers;
static int ffi_pointers_count;
static int ffi_pointers_cap;
static pthread_mutex_t ffi_pointers_lock = PTHREAD_MUTEX_INITIALIZER;

static FFIBinding **ffi_bindings;
static int ffi_bindings_count;
static int ffi_bindings_cap;

static int ffi_call_cfa;

static int ffi_pointer_intern(void *pointer) {
	pthread_mutex_lock(&ffi_pointers_lock);
	int index = -1;
	for (int i = 0; i < ffi_pointers_count; i++)
		if (!ffi_pointers[i]) {
			ffi_pointers[i] = pointer;
			index = i;
			break;
		}
	if (index < 0) {
		GROW_IF_FULL_SYS(ffi_pointers_count, ffi_pointers_cap, ffi_pointers);
		ffi_pointers[ffi_pointers_count] = pointer;
		index = ffi_pointers_count++;
	}
	pthread_mutex_unlock(&ffi_pointers_lock);
	return index;
}

static int ffi_type_of(Interpreter *interp, Val symbol, FFITypeTag *tag, ffi_type **type) {
	if (VAL_TAG(symbol) != T_SYMBOL) {
		fail(interp, "type must be a symbol; got %s", tag_name(VAL_TAG(symbol)));
		return 0;
	}

	const char *name = &vocab.symbol_pool[VAL_DATA(symbol)];
	if (!strcmp(name, "void")) {
		*tag = FFI_VOID;
		*type = &ffi_type_void;
	} else if (!strcmp(name, "int")) {
		*tag = FFI_INT;
		*type = &ffi_type_sint;
	} else if (!strcmp(name, "long")) {
		*tag = FFI_LONG;
		*type = &ffi_type_slong;
	} else if (!strcmp(name, "double")) {
		*tag = FFI_DOUBLE;
		*type = &ffi_type_double;
	} else if (!strcmp(name, "ptr")) {
		*tag = FFI_PTR;
		*type = &ffi_type_pointer;
	} else if (!strcmp(name, "string")) {
		*tag = FFI_STRING;
		*type = &ffi_type_pointer;
	} else {
		fail(interp, "unknown FFI type :%s", name);
		return 0;
	}
	return 1;
}

void p_ffi_open(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val path_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(path_val, T_STRING, "ffi-open", "a string");

	void *library = dlopen(OBJECT_AT(VAL_DATA(path_val))->bytes, RTLD_NOW | RTLD_GLOBAL);
	if (!library) {
		fail(interp, "%s", dlerror());
		return;
	}

	chain_sp[-1] = make_pointer(ffi_pointer_intern(library));

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

static void make_ffi_binding(Interpreter *interp, Val library_val, Object *function_name, Object *arg_types, Val return_symbol, int fixed_args, const char *name) {
	if (VAL_TAG(library_val) != T_PTR) {
		fail(interp, "library must be a pointer handle; got %s", tag_name(VAL_TAG(library_val)));
		return;
	}

	void *library = ffi_pointers[VAL_DATA(library_val)];
	void *function = dlsym(library, function_name->bytes);
	if (!function) {
		fail(interp, "symbol %s not found", function_name->bytes);
		return;
	}

	int argc = arg_types->len;
	if (argc > FFI_MAX_ARGS) {
		fail(interp, "too many arguments (max %d)", FFI_MAX_ARGS);
		return;
	}

	FFIBinding *binding = calloc(1, sizeof(FFIBinding));
	binding->function = function;
	binding->argc = argc;

	ffi_type *return_ffi;
	if (!ffi_type_of(interp, return_symbol, &binding->return_tag, &return_ffi)) {
		free(binding);
		return;
	}

	for (int i = 0; i < argc; i++)
		if (!ffi_type_of(interp, arg_types->items[i], &binding->arg_tags[i], &binding->arg_ffi[i])) {
			free(binding);
			return;
		}

	int prepared;
	if (fixed_args < 0)
		prepared = ffi_prep_cif(&binding->cif, FFI_DEFAULT_ABI, (unsigned)argc, return_ffi, binding->arg_ffi) == FFI_OK;
	else
		prepared = ffi_prep_cif_var(&binding->cif, FFI_DEFAULT_ABI, (unsigned)fixed_args, (unsigned)argc, return_ffi, binding->arg_ffi) == FFI_OK;
	if (!prepared) {
		free(binding);
		fail(interp, "ffi_prep_cif failed");
		return;
	}

	GROW_IF_FULL_SYS(ffi_bindings_count, ffi_bindings_cap, ffi_bindings);
	int index = ffi_bindings_count++;
	ffi_bindings[index] = binding;

	create_header(interp, name, 0);
	emit(interp, (cell)docol);
	emit_val_literal(interp, make_float((double)index));
	emit_call(interp, ffi_call_cfa);
	emit_call(interp, vocab.exit_cfa);
}

void p_ffi_function(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 4);
	Val return_symbol = chain_sp[-1];
	Val arg_types_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(arg_types_val, T_ARRAY, "ffi-function", "an array");
	Object *arg_types = OBJECT_AT(VAL_DATA(arg_types_val));
	Val function_name_val = chain_sp[-3];
	REQUIRE_CHAIN_TAG(function_name_val, T_STRING, "ffi-function", "a string");
	Object *function_name = OBJECT_AT(VAL_DATA(function_name_val));
	Val library_val = chain_sp[-4];

	char *name = next_token();
	if (!name) {
		fail(interp, "expected a name");
		return;
	}

	make_ffi_binding(interp, library_val, function_name, arg_types, return_symbol, -1, name);
	if (interp->error_flag)
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 4);
}

void p_ffi_variadic(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 5);
	Val fixed_args_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(fixed_args_val, T_FLOAT, "ffi-variadic", "a float fixed-argument count");
	int fixed_args = (int)VAL_NUMBER(fixed_args_val);
	Val return_symbol = chain_sp[-2];
	Val arg_types_val = chain_sp[-3];
	REQUIRE_CHAIN_TAG(arg_types_val, T_ARRAY, "ffi-variadic", "an array");
	Object *arg_types = OBJECT_AT(VAL_DATA(arg_types_val));
	Val function_name_val = chain_sp[-4];
	REQUIRE_CHAIN_TAG(function_name_val, T_STRING, "ffi-variadic", "a string");
	Object *function_name = OBJECT_AT(VAL_DATA(function_name_val));
	Val library_val = chain_sp[-5];

	if (fixed_args < 0 || fixed_args > arg_types->len) {
		fail(interp, "fixed count %d out of range for %d arguments", fixed_args, arg_types->len);
		return;
	}

	char *name = next_token();
	if (!name) {
		fail(interp, "expected a name");
		return;
	}

	make_ffi_binding(interp, library_val, function_name, arg_types, return_symbol, fixed_args, name);
	if (interp->error_flag)
		return;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 5);
}

void p_ffi_call(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val index_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(index_val, T_FLOAT, "(ffi-call)", "a float binding");
	int index = (int)VAL_NUMBER(index_val);
	FFIBinding *binding = ffi_bindings[index];

	int argc = binding->argc;
	Val *call_args = chain_sp - 1 - argc;
	if (call_args < interp->data_stack) {
		fail(interp, "stack underflow; need %d arguments", argc);
		return;
	}

	union {
		int as_int;
		long as_long;
		double as_double;
		void *as_pointer;
	} arg_cells[FFI_MAX_ARGS];

	void *arg_pointers[FFI_MAX_ARGS];

	for (int i = 0; i < argc; i++) {
		Val argument = call_args[i];
		switch (binding->arg_tags[i]) {
			case FFI_INT:
				arg_cells[i].as_int = (int)VAL_NUMBER(argument);
				arg_pointers[i] = &arg_cells[i].as_int;
				break;
			case FFI_LONG:
				arg_cells[i].as_long = (long)VAL_NUMBER(argument);
				arg_pointers[i] = &arg_cells[i].as_long;
				break;
			case FFI_DOUBLE:
				arg_cells[i].as_double = VAL_NUMBER(argument);
				arg_pointers[i] = &arg_cells[i].as_double;
				break;
			case FFI_PTR:
				if (VAL_TAG(argument) != T_PTR) {
					fail(interp, "argument %d expected a pointer; got %s", i, tag_name(VAL_TAG(argument)));
					return;
				}
				pthread_mutex_lock(&ffi_pointers_lock);
				arg_cells[i].as_pointer = ffi_pointers[VAL_DATA(argument)];
				pthread_mutex_unlock(&ffi_pointers_lock);
				arg_pointers[i] = &arg_cells[i].as_pointer;
				break;
			case FFI_STRING:
				if (VAL_TAG(argument) != T_STRING) {
					fail(interp, "argument %d expected a string; got %s", i, tag_name(VAL_TAG(argument)));
					return;
				}
				arg_cells[i].as_pointer = OBJECT_AT(VAL_DATA(argument))->bytes;
				arg_pointers[i] = &arg_cells[i].as_pointer;
				break;
			default:
				fail(interp, "argument %d has return-only type :void", i);
				return;
		}
	}

	union {
		ffi_arg as_int;
		double as_double;
		void *as_pointer;
	} result;
	ffi_call(&binding->cif, FFI_FN(binding->function), &result, arg_pointers);

	Val *returned_top = call_args;
	switch (binding->return_tag) {
		case FFI_VOID:
			break;
		case FFI_INT:
			*returned_top++ = make_float((double)(int)result.as_int);
			break;
		case FFI_LONG:
			*returned_top++ = make_float((double)(long)result.as_int);
			break;
		case FFI_DOUBLE:
			*returned_top++ = make_float(result.as_double);
			break;
		case FFI_PTR:
			*returned_top++ = make_pointer(ffi_pointer_intern(result.as_pointer));
			break;
		case FFI_STRING:
			if (result.as_pointer) {
				SYNC_REGISTERS(interp, chain_ip, call_args);
				int handle = object_new_string(interp, result.as_pointer, (int)strlen(result.as_pointer));
				if (interp->error_flag)
					return;
				*returned_top++ = make_string(handle);
			} else
				*returned_top++ = make_tagged(T_NONE, 0);
			break;
	}

	DISPATCH_REGISTERS(interp, chain_ip, returned_top);
}

void p_ffi_free(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val pointer_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(pointer_val, T_PTR, "ffi-free", "a pointer");
	int index = (int)VAL_DATA(pointer_val);
	free(ffi_pointers[index]);
	ffi_pointers[index] = NULL;

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

int ffi_register_call_cfa(int cfa) {
	ffi_call_cfa = cfa;
	return cfa;
}

void p_matrix_to_pointer(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val matrix_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(matrix_val, T_MATRIX, "matrix>pointer", "a matrix");

	chain_sp[-1] = make_pointer(ffi_pointer_intern(OBJECT_AT(VAL_DATA(matrix_val))->matrix.elements));

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_segment_to_pointer(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val segment_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(segment_val, T_SEGMENT, "segment>pointer", "a segment");

	chain_sp[-1] = make_pointer(ffi_pointer_intern(OBJECT_AT(VAL_DATA(segment_val))->segment.data));

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_pointer_cell(DISPATCH_ARGS) {
	REQUIRE_STACK_ROOM(interp, chain_ip, chain_sp, 1);

	void *cell;
	CALLOC_OR_FAIL(interp, cell, 1, sizeof(void *));

	*chain_sp = make_pointer(ffi_pointer_intern(cell));

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp + 1);
}

void p_pointer_deref(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val cell_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(cell_val, T_PTR, "pointer-deref", "a pointer");

	void *cell = ffi_pointers[VAL_DATA(cell_val)];
	void *loaded = *(void **)cell;

	chain_sp[-1] = make_pointer(ffi_pointer_intern(loaded));

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_pointer_to_address(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val pointer_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(pointer_val, T_PTR, "pointer>address", "a pointer");

	int64_t numeric_address = (int64_t)(intptr_t)ffi_pointers[VAL_DATA(pointer_val)];
	if (numeric_address >= ((int64_t)1 << 53)) {
		fail(interp, "pointer address exceeds 2^53; not float-exact");
		return;
	}

	chain_sp[-1] = make_float((double)numeric_address);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_pointer_long(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 1);
	Val cell_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(cell_val, T_PTR, "pointer-long", "a pointer");

	int64_t value = *(int64_t *)ffi_pointers[VAL_DATA(cell_val)];
	if (value >= ((int64_t)1 << 53)) {
		fail(interp, "value exceeds 2^53; not float-exact");
		return;
	}

	chain_sp[-1] = make_float((double)value);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp);
}

void p_floats_to_matrix(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val count_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(count_val, T_FLOAT, "floats>matrix", "a float count");
	int n_elements = (int)VAL_NUMBER(count_val);
	Val pointer_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(pointer_val, T_PTR, "floats>matrix", "a pointer");
	if (n_elements < 1) {
		fail(interp, "count must be positive; got %d", n_elements);
		return;
	}

	const float *source = ffi_pointers[VAL_DATA(pointer_val)];

	int matrix_handle = object_new_matrix(interp, n_elements, 1);
	if (interp->error_flag)
		return;

	double *elements = OBJECT_AT(matrix_handle)->matrix.elements;
	for (int i = 0; i < n_elements; i++)
		elements[i] = (double)source[i];

	chain_sp[-2] = make_matrix(matrix_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}

void p_pointer_string_at(DISPATCH_ARGS) {
	REQUIRE_STACK_DEPTH(interp, chain_ip, chain_sp, 2);
	Val index_val = chain_sp[-1];
	REQUIRE_CHAIN_TAG(index_val, T_FLOAT, "pointer-string-at", "an index");
	int index = (int)VAL_NUMBER(index_val);
	Val pointer_val = chain_sp[-2];
	REQUIRE_CHAIN_TAG(pointer_val, T_PTR, "pointer-string-at", "a pointer");
	if (index < 0) {
		fail(interp, "index must be nonnegative; got %d", index);
		return;
	}

	const char *const *string_table = ffi_pointers[VAL_DATA(pointer_val)];
	const char *item = string_table[index];
	int string_handle = object_new_string(interp, item, (int)strlen(item));
	if (interp->error_flag)
		return;

	chain_sp[-2] = make_string(string_handle);

	DISPATCH_REGISTERS(interp, chain_ip, chain_sp - 1);
}
