#include "logicforth.h"
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

static int ffi_type_of(Interpreter *interp, Val symbol, FFITypeTag *tag, ffi_type **type, const char *op) {
	if (VAL_TAG(symbol) != T_SYMBOL) {
		fail(interp, "%s: type must be a symbol; got %s", op, tag_name(VAL_TAG(symbol)));
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
		fail(interp, "%s: unknown FFI type :%s", op, name);
		return 0;
	}
	return 1;
}

void p_ffi_open(Interpreter *interp) {
	POP_STRING(library_path, "ffi-open");

	void *library = dlopen(library_path->bytes, RTLD_NOW | RTLD_GLOBAL);
	if (!library) {
		fail(interp, "ffi-open: %s", dlerror());
		return;
	}

	push(interp, make_pointer(ffi_pointer_intern(library)));

	DISPATCH(interp);
}

static void make_ffi_binding(Interpreter *interp, Val library_val, Object *function_name, Object *arg_types, Val return_symbol, int fixed_args, const char *name, const char *op) {
	if (VAL_TAG(library_val) != T_PTR) {
		fail(interp, "%s: library must be a pointer handle; got %s", op, tag_name(VAL_TAG(library_val)));
		return;
	}

	void *library = ffi_pointers[VAL_DATA(library_val)];
	void *function = dlsym(library, function_name->bytes);
	if (!function) {
		fail(interp, "%s: symbol %s not found", op, function_name->bytes);
		return;
	}

	int argc = arg_types->len;
	if (argc > FFI_MAX_ARGS) {
		fail(interp, "%s: too many arguments (max %d)", op, FFI_MAX_ARGS);
		return;
	}

	FFIBinding *binding = calloc(1, sizeof(FFIBinding));
	binding->function = function;
	binding->argc = argc;

	ffi_type *return_ffi;
	if (!ffi_type_of(interp, return_symbol, &binding->return_tag, &return_ffi, op)) {
		free(binding);
		return;
	}

	for (int i = 0; i < argc; i++)
		if (!ffi_type_of(interp, arg_types->items[i], &binding->arg_tags[i], &binding->arg_ffi[i], op)) {
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
		fail(interp, "%s: ffi_prep_cif failed", op);
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

void p_ffi_function(Interpreter *interp) {
	POP(return_symbol);
	POP_ARRAY(arg_types, "ffi-function");
	POP_STRING(function_name, "ffi-function");
	POP(library_val);

	if (interp->error_flag) return;

	char *name = next_token();
	if (!name) {
		fail(interp, "ffi-function: expected a name");
		return;
	}

	make_ffi_binding(interp, library_val, function_name, arg_types, return_symbol, -1, name, "ffi-function");

	DISPATCH(interp);
}

void p_ffi_variadic(Interpreter *interp) {
	POP_INT(fixed_args, "ffi-variadic", "fixed-argument count");
	POP(return_symbol);
	POP_ARRAY(arg_types, "ffi-variadic");
	POP_STRING(function_name, "ffi-variadic");
	POP(library_val);

	if (interp->error_flag) return;

	if (fixed_args < 0 || fixed_args > arg_types->len) {
		fail(interp, "ffi-variadic: fixed count %d out of range for %d arguments", fixed_args, arg_types->len);
		return;
	}

	char *name = next_token();
	if (!name) {
		fail(interp, "ffi-variadic: expected a name");
		return;
	}

	make_ffi_binding(interp, library_val, function_name, arg_types, return_symbol, fixed_args, name, "ffi-variadic");

	DISPATCH(interp);
}

void p_ffi_call(Interpreter *interp) {
	POP_INT(index, "ffi-call", "binding");
	FFIBinding *binding = ffi_bindings[index];
	
	int argc = binding->argc;
	if (interp->dsp < argc) {
		fail(interp, "ffi-call: stack too shallow; need %d arguments", argc);
		return;
	}

	union {
		int as_int;
		long as_long;
		double as_double;
		void *as_pointer;
	} arg_cells[FFI_MAX_ARGS];

	void *arg_pointers[FFI_MAX_ARGS];
	int first_arg = interp->dsp - argc;

	for (int i = 0; i < argc; i++) {
		Val argument = interp->data_stack[first_arg + i];
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
					fail(interp, "ffi-call: argument %d expected a pointer; got %s", i, tag_name(VAL_TAG(argument)));
					return;
				}
				pthread_mutex_lock(&ffi_pointers_lock);
				arg_cells[i].as_pointer = ffi_pointers[VAL_DATA(argument)];
				pthread_mutex_unlock(&ffi_pointers_lock);
				arg_pointers[i] = &arg_cells[i].as_pointer;
				break;
			case FFI_STRING:
				if (VAL_TAG(argument) != T_STRING) {
					fail(interp, "ffi-call: argument %d expected a string; got %s", i, tag_name(VAL_TAG(argument)));
					return;
				}
				arg_cells[i].as_pointer = OBJECT_AT(VAL_DATA(argument))->bytes;
				arg_pointers[i] = &arg_cells[i].as_pointer;
				break;
			default:
				fail(interp, "ffi-call: argument %d has return-only type :void", i);
				return;
		}
	}

	union {
		ffi_arg as_int;
		double as_double;
		void *as_pointer;
	} result;
	ffi_call(&binding->cif, FFI_FN(binding->function), &result, arg_pointers);

	interp->dsp -= argc;

	switch (binding->return_tag) {
		case FFI_VOID:
			break;
		case FFI_INT:
			push(interp, make_float((double)(int)result.as_int));
			break;
		case FFI_LONG:
			push(interp, make_float((double)(long)result.as_int));
			break;
		case FFI_DOUBLE:
			push(interp, make_float(result.as_double));
			break;
		case FFI_PTR:
			push(interp, make_pointer(ffi_pointer_intern(result.as_pointer)));
			break;
		case FFI_STRING:
			if (result.as_pointer)
				push(interp, make_string(object_new_string(interp, result.as_pointer, (int)strlen(result.as_pointer))));
			else
				push(interp, make_tagged(T_NONE, 0));
			break;
	}

	DISPATCH(interp);
}

void p_ffi_free(Interpreter *interp) {
	POP_PTR(index, "ffi-free");
	free(ffi_pointers[index]);
	ffi_pointers[index] = NULL;

	DISPATCH(interp);
}

int ffi_register_call_cfa(int cfa) {
	ffi_call_cfa = cfa;
	return cfa;
}

void p_matrix_to_pointer(Interpreter *interp) {
	POP_MATRIX(matrix, "matrix>pointer");
	push(interp, make_pointer(ffi_pointer_intern(matrix->matrix.elements)));

	DISPATCH(interp);
}

void p_segment_to_pointer(Interpreter *interp) {
	POP_SEGMENT(segment, "segment>pointer");
	push(interp, make_pointer(ffi_pointer_intern(segment->segment.data)));

	DISPATCH(interp);
}
