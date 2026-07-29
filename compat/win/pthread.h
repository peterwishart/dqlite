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
