#ifndef WATER_PLATFORM_H
#define WATER_PLATFORM_H

#include <stddef.h>

struct Interpreter;

#define unlikely(condition) __builtin_expect(!!(condition), 0)

#define SYNC_REGISTERS(interp, reg_ip, reg_sp) do { \
	(interp)->ip = (int)((reg_ip) - vocab.dict); \
	(interp)->dsp = (int)((reg_sp) - (interp)->data_stack); \
} while (0)

#define REQUIRE_STACK_DEPTH(interp, reg_ip, reg_sp, popped) do { \
	if (unlikely((reg_sp) - (popped) < (interp)->data_stack)) { \
		SYNC_REGISTERS(interp, reg_ip, reg_sp); \
		fail(interp, "stack underflow"); \
		return; \
	} \
} while (0)

#define REQUIRE_STACK_ROOM(interp, reg_ip, reg_sp, pushed) do { \
	if (unlikely((reg_sp) + (pushed) > (interp)->data_stack + DATA_STACK_DEPTH)) { \
		SYNC_REGISTERS(interp, reg_ip, reg_sp); \
		fail(interp, "stack overflow"); \
		return; \
	} \
} while (0)

#define RETARGET_OP(handler) do { \
	if (!in_parallel) \
		chain_ip[-1] = (cell)(handler); \
} while (0)

#ifdef __wasm__
#define MUSTTAIL
#define DISPATCH(interp) do { return; } while (0)

#define DISPATCH_REGISTERS(interp, reg_ip, reg_sp) do { \
	(interp)->ip = (int)((reg_ip) - vocab.dict); \
	(interp)->dsp = (int)((reg_sp) - (interp)->data_stack); \
	return; \
} while (0)

#else

#define MUSTTAIL __attribute__((musttail))
#define DISPATCH(interp) do { \
	if (unlikely((interp)->unwinding || (interp)->error_flag || (interp)->gc_pending)) \
		return; \
	cfa_handler next_op = (cfa_handler)vocab.dict[(interp)->ip++]; \
	MUSTTAIL \
	return next_op(interp, vocab.dict + (interp)->ip, (interp)->data_stack + (interp)->dsp); \
} while (0)

#define DISPATCH_REGISTERS(interp, reg_ip, reg_sp) do { \
	int next_handler_index = (int)((reg_ip) - vocab.dict); \
	(interp)->ip = next_handler_index; \
	(interp)->dsp = (int)((reg_sp) - (interp)->data_stack); \
	if (unlikely((interp)->unwinding || (interp)->error_flag || (interp)->gc_pending)) \
		return; \
	(interp)->ip = next_handler_index + 1; \
	cfa_handler next_op = (cfa_handler)*(reg_ip); \
	MUSTTAIL \
	return next_op(interp, (reg_ip) + 1, (reg_sp)); \
} while (0)
#endif

#ifdef __wasm__
typedef int platform_thread_t;
typedef int platform_mutex_t;
#define PLATFORM_MUTEX_INIT 0
static inline int platform_thread_create(platform_thread_t *thread, void *(*start)(void *), void *arg) {
	(void)thread; (void)start; (void)arg;
	return 1;
}
static inline void platform_thread_join(platform_thread_t thread) { (void)thread; }
static inline void platform_mutex_lock(platform_mutex_t *mutex) { (void)mutex; }
static inline void platform_mutex_unlock(platform_mutex_t *mutex) { (void)mutex; }
#else
#include <pthread.h>
typedef pthread_t platform_thread_t;
typedef pthread_mutex_t platform_mutex_t;
#define PLATFORM_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
static inline int platform_thread_create(platform_thread_t *thread, void *(*start)(void *), void *arg) {
	return pthread_create(thread, NULL, start, arg);
}
static inline void platform_thread_join(platform_thread_t thread) { pthread_join(thread, NULL); }
static inline void platform_mutex_lock(platform_mutex_t *mutex) { pthread_mutex_lock(mutex); }
static inline void platform_mutex_unlock(platform_mutex_t *mutex) { pthread_mutex_unlock(mutex); }
#endif

void *platform_reserve(size_t requested, size_t *reserved_out);
void platform_init(void);
int platform_repl_begin(struct Interpreter *interp, int want_interactive);
int platform_read_chunk(char *dst, int dst_avail, int interactive);
void platform_qsort_r(void *base, size_t n, size_t size, void *thunk,
		int (*cmp)(void *, const void *, const void *));

#endif
