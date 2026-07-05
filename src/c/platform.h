#ifndef WATER_PLATFORM_H
#define WATER_PLATFORM_H

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

void *platform_reserve(size_t *reserved_out);
void platform_init(void);
int platform_repl_begin(struct Interpreter *interp, int want_interactive);
int platform_read_chunk(char *dst, int dst_avail, int interactive);
void platform_qsort_r(void *base, size_t n, size_t size, void *thunk,
		int (*cmp)(void *, const void *, const void *));

#endif
