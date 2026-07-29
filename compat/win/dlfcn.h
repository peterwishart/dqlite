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
