/*
 * dqlite Windows port -- <semaphore.h> minimal POSIX-semaphore shim.
 *
 * Only src/server.{c,h} use sem_t (via struct dqlite_node). Declares the
 * surface those files touch; IMPLEMENTATION is deferred (intended port: Win32
 * CreateSemaphore or uv_sem_t). Windows ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_SEMAPHORE_H
#define DQLITE_COMPAT_SEMAPHORE_H

/* Shadowing fence (PORT_TODO.md W3): only TUs built with dqlite's forced-
 * include prelude (clang-cl /FI dqlite_win_prelude.h, which defines
 * DQLITE_WIN_COMPAT) may use this shim; any other consumer that resolves this
 * name -- e.g. a dependency TU compiled inside a dqlite target -- must break
 * loudly here instead of silently picking up stubs. Full rationale in
 * compat/win/dqlite_win_prelude.h. */
#if !defined(_WIN32) || !defined(DQLITE_WIN_COMPAT)
#error "dqlite compat/win shim header reached without the dqlite Windows prelude (DQLITE_WIN_COMPAT undefined); this header must not shadow a real system header -- see compat/win/dqlite_win_prelude.h"
#endif

/* On glibc <semaphore.h> transitively supplies the pthread types; src/server.h
 * relies on that (it uses pthread_t without including <pthread.h>). Mirror it
 * here so the shim layer matches the behaviour the sources depend on. */
#include <pthread.h>

/* handle: the Win32 semaphore HANDLE (CreateSemaphore).
 * count:  a self-maintained mirror of the current semaphore value. Win32 has no
 *         API to read a semaphore's count, so sem_getvalue() needs us to track
 *         it. Adjusted with interlocked ops around the real post/wait. */
typedef struct dqlite_sem { void *handle; volatile long count; } sem_t;

int sem_init(sem_t *sem, int pshared, unsigned int value);
int sem_destroy(sem_t *sem);
int sem_post(sem_t *sem);
int sem_wait(sem_t *sem);
int sem_trywait(sem_t *sem);
/* sem_getvalue: only test/integration/test_node.c reads the current count.
 * Declared here to match the header-only sem shim; the implementation is
 * deferred with the rest of the semaphore port (link-stage). */
int sem_getvalue(sem_t *sem, int *sval);

#endif /* DQLITE_COMPAT_SEMAPHORE_H */
