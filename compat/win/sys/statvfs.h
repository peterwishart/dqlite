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

/* Shadowing fence -- see the rationale in dqlite_win_prelude.h. */
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
