#include "logicforth.h"

#ifndef WASM_ARENA_RESERVE
#define WASM_ARENA_RESERVE ((size_t)1 << 30)
#endif

void *platform_reserve(size_t *reserved_out) {
	void *base = malloc(WASM_ARENA_RESERVE);
	if (!base)
		return NULL;
	*reserved_out = WASM_ARENA_RESERVE;
	return base;
}

void platform_init(void) {
}

int platform_repl_begin(struct Interpreter *interp, int want_interactive) {
	(void)interp;
	if (want_interactive)
		printf("logicforth %s\n", VERSION);
	return want_interactive;
}

int platform_read_chunk(char *dst, int dst_avail, int interactive) {
	(void)interactive;
	if (!fgets(dst, dst_avail, stdin))
		return 0;
	return (int)strlen(dst);
}

static void *wasi_qsort_thunk;
static int (*wasi_qsort_cmp)(void *, const void *, const void *);

static int wasi_qsort_trampoline(const void *a, const void *b) {
	return wasi_qsort_cmp(wasi_qsort_thunk, a, b);
}

void platform_qsort_r(void *base, size_t n, size_t size, void *thunk,
		int (*cmp)(void *, const void *, const void *)) {
	wasi_qsort_thunk = thunk;
	wasi_qsort_cmp = cmp;
	qsort(base, n, size, wasi_qsort_trampoline);
}

#define WASM_UNSUPPORTED(fn, word) \
	void fn(Interpreter *interp) { \
		fail(interp, word ": unsupported on the wasm build"); \
		DISPATCH(interp); \
	}

WASM_UNSUPPORTED(p_start_process, "start-process")
WASM_UNSUPPORTED(p_wait, "wait")
WASM_UNSUPPORTED(p_stop_process, "stop")
WASM_UNSUPPORTED(p_running, "running?")
WASM_UNSUPPORTED(p_ffi_open, "ffi-open")
WASM_UNSUPPORTED(p_ffi_function, "ffi-function")
WASM_UNSUPPORTED(p_ffi_variadic, "ffi-variadic")
WASM_UNSUPPORTED(p_ffi_call, "(ffi-call)")
WASM_UNSUPPORTED(p_ffi_free, "ffi-free")
WASM_UNSUPPORTED(p_matrix_to_pointer, "matrix>pointer")
WASM_UNSUPPORTED(p_segment_to_pointer, "segment>pointer")

int ffi_register_call_cfa(int cfa) {
	return cfa;
}
