/*
 * dqlite Windows port -- <pthread.h> minimal POSIX-threads shim.
 *
 * Only src/server.{c,h} use pthread primitives directly (everything else uses
 * libuv thread wrappers). This shim declares just the surface those files
 * touch so they compile; the IMPLEMENTATION is deferred to a later iteration
 * (the intended port maps these onto uv_thread_t / uv_mutex_t / uv_cond_t, or
 * a pthreads-win32 shim). Windows ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_PTHREAD_H
#define DQLITE_COMPAT_PTHREAD_H

/* Shadowing fence (PORT_TODO.md W3): only TUs built with dqlite's forced-
 * include prelude (clang-cl /FI dqlite_win_prelude.h, which defines
 * DQLITE_WIN_COMPAT) may use this shim; any other consumer that resolves this
 * name -- e.g. a dependency TU compiled inside a dqlite target -- must break
 * loudly here instead of silently picking up stubs. Full rationale in
 * compat/win/dqlite_win_prelude.h. */
#if !defined(_WIN32) || !defined(DQLITE_WIN_COMPAT)
#error "dqlite compat/win shim header reached without the dqlite Windows prelude (DQLITE_WIN_COMPAT undefined); this header must not shadow a real system header -- see compat/win/dqlite_win_prelude.h"
#endif

#include <stdint.h>
#include <time.h> /* struct timespec */

typedef uintptr_t pthread_t;
typedef struct dqlite_pthread_mutex { void *handle; } pthread_mutex_t;
typedef struct dqlite_pthread_cond { void *handle; } pthread_cond_t;
typedef int pthread_attr_t;
typedef int pthread_mutexattr_t;
typedef int pthread_condattr_t;

int pthread_create(pthread_t *thread,
		   const pthread_attr_t *attr,
		   void *(*start_routine)(void *),
		   void *arg);
int pthread_join(pthread_t thread, void **retval);

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_timedwait(pthread_cond_t *cond,
			   pthread_mutex_t *mutex,
			   const struct timespec *abstime);

#endif /* DQLITE_COMPAT_PTHREAD_H */
