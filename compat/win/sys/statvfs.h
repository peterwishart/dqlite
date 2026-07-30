/*
 * dqlite Windows port -- <sys/statvfs.h> minimal shim.
 *
 * struct statvfs + statvfs()/fstatvfs() declarations; implemented in
 * compat/win/compat_win.c via GetDiskFreeSpaceEx, with the reported free space
 * deliberately CAPPED so the raft DirFill() test helper never tries to fill a
 * real volume (see DQLITE_STATVFS_CAP_BYTES there). Windows ONLY.
 */
#ifndef DQLITE_COMPAT_SYS_STATVFS_H
#define DQLITE_COMPAT_SYS_STATVFS_H

/* Shadowing fence (PORT_TODO.md W3): only TUs built with dqlite's forced-
 * include prelude (clang-cl /FI dqlite_win_prelude.h, which defines
 * DQLITE_WIN_COMPAT) may use this shim; any other consumer that resolves this
 * name -- e.g. a dependency TU compiled inside a dqlite target -- must break
 * loudly here instead of silently picking up stubs. Full rationale in
 * compat/win/dqlite_win_prelude.h. */
#if !defined(_WIN32) || !defined(DQLITE_WIN_COMPAT)
#error "dqlite compat/win shim header reached without the dqlite Windows prelude (DQLITE_WIN_COMPAT undefined); this header must not shadow a real system header -- see compat/win/dqlite_win_prelude.h"
#endif

#include <stddef.h>

struct statvfs {
	unsigned long f_bsize;
	unsigned long f_frsize;
	unsigned long long f_blocks;
	unsigned long long f_bfree;
	unsigned long long f_bavail;
	unsigned long long f_files;
	unsigned long long f_ffree;
	unsigned long long f_favail;
	unsigned long f_fsid;
	unsigned long f_flag;
	unsigned long f_namemax;
};

int statvfs(const char *path, struct statvfs *buf);
int fstatvfs(int fd, struct statvfs *buf);

#endif /* DQLITE_COMPAT_SYS_STATVFS_H */
