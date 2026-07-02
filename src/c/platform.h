#ifndef LOGICFORTH_PLATFORM_H
#define LOGICFORTH_PLATFORM_H

#include <stddef.h>

struct Interpreter;

#ifdef __wasm__
#define MUSTTAIL
#define DISPATCH(interp) do { return; } while (0)
#else
#define MUSTTAIL __attribute__((musttail))
#define DISPATCH(interp) do { \
	if ((interp)->unwinding || (interp)->error_flag || (interp)->gc_pending) \
		return; \
	MUSTTAIL \
	return ((cfa_handler)vocab.dict[(interp)->ip++])(interp); \
} while (0)
#endif

void *platform_reserve(size_t *reserved_out);
void platform_init(void);
int platform_repl_begin(struct Interpreter *interp, int want_interactive);
int platform_read_chunk(char *dst, int dst_avail, int interactive);
void lf_qsort_r(void *base, size_t n, size_t size, void *thunk,
		int (*cmp)(void *, const void *, const void *));

#endif
