/*
 * dqlite Windows port -- <sys/file.h> minimal shim (flock).
 *
 * src/server.c uses flock() to lock the data-directory lock file. Win32 uses
 * LockFileEx; declaration + LOCK_* constants only here, IMPLEMENTATION
 * deferred. Windows ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_SYS_FILE_H
#define DQLITE_COMPAT_SYS_FILE_H

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

int flock(int fd, int operation);

#endif /* DQLITE_COMPAT_SYS_FILE_H */
