/*
 * dqlite Windows port -- <dlfcn.h> minimal shim.
 *
 * Test-only (test/raft/lib/addrinfo.c interposes getaddrinfo via dlsym). The
 * dlsym(RTLD_NEXT, ...) interposition trick has no direct Win32 equivalent; a
 * real port would drop or replace it. Declarations only here so the test file
 * compiles; IMPLEMENTATION deferred. Windows ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_DLFCN_H
#define DQLITE_COMPAT_DLFCN_H

/* Shadowing fence (PORT_TODO.md W3): only TUs built with dqlite's forced-
 * include prelude (clang-cl /FI dqlite_win_prelude.h, which defines
 * DQLITE_WIN_COMPAT) may use this shim; any other consumer that resolves this
 * name -- e.g. a dependency TU compiled inside a dqlite target -- must break
 * loudly here instead of silently picking up stubs. Full rationale in
 * compat/win/dqlite_win_prelude.h. */
#if !defined(_WIN32) || !defined(DQLITE_WIN_COMPAT)
#error "dqlite compat/win shim header reached without the dqlite Windows prelude (DQLITE_WIN_COMPAT undefined); this header must not shadow a real system header -- see compat/win/dqlite_win_prelude.h"
#endif

#define RTLD_LAZY 0x1
#define RTLD_NOW 0x2
#define RTLD_GLOBAL 0x100
#define RTLD_LOCAL 0x000
#define RTLD_DEFAULT ((void *)0)
#define RTLD_NEXT ((void *)-1)

void *dlopen(const char *filename, int flags);
int dlclose(void *handle);
void *dlsym(void *handle, const char *symbol);
char *dlerror(void);

#endif /* DQLITE_COMPAT_DLFCN_H */
