/*
 * dqlite Windows port -- <sys/file.h> minimal shim (flock).
 *
 * src/server.c uses flock() to lock the data-directory lock file. Declaration
 * + LOCK_* constants here; the implementation (LockFileEx/UnlockFileEx on the
 * fd's OS handle) lives in compat/win/compat_win.c. Windows ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_SYS_FILE_H
#define DQLITE_COMPAT_SYS_FILE_H

/* Shadowing fence -- see the rationale in dqlite_win_prelude.h. */
#if !defined(_WIN32) || !defined(DQLITE_WIN_COMPAT)
#error "dqlite compat/win shim header reached without the dqlite Windows prelude (DQLITE_WIN_COMPAT undefined); this header must not shadow a real system header -- see compat/win/dqlite_win_prelude.h"
#endif

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

int flock(int fd, int operation);

#endif /* DQLITE_COMPAT_SYS_FILE_H */
