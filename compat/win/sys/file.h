/*
 * dqlite Windows port -- <sys/file.h> minimal shim (flock).
 *
 * src/server.c uses flock() to lock the data-directory lock file. Declaration
 * + LOCK_* constants here; the implementation (LockFileEx/UnlockFileEx on the
 * fd's OS handle) lives in compat/win/compat_win.c. Windows ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_SYS_FILE_H
#define DQLITE_COMPAT_SYS_FILE_H

/* Shadowing fence (PORT_TODO.md W3): only TUs built with dqlite's forced-
 * include prelude (clang-cl /FI dqlite_win_prelude.h, which defines
 * DQLITE_WIN_COMPAT) may use this shim; any other consumer that resolves this
 * name -- e.g. a dependency TU compiled inside a dqlite target -- must break
 * loudly here instead of silently picking up stubs. Full rationale in
 * compat/win/dqlite_win_prelude.h. */
#if !defined(_WIN32) || !defined(DQLITE_WIN_COMPAT)
#error "dqlite compat/win shim header reached without the dqlite Windows prelude (DQLITE_WIN_COMPAT undefined); this header must not shadow a real system header -- see compat/win/dqlite_win_prelude.h"
#endif

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

int flock(int fd, int operation);

#endif /* DQLITE_COMPAT_SYS_FILE_H */
