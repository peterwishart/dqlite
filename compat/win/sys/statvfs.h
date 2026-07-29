/*
 * dqlite Windows port -- <sys/statvfs.h> minimal shim.
 *
 * struct statvfs + statvfs()/fstatvfs() declarations so includers compile; the
 * Win32 backing (GetDiskFreeSpaceEx) is a later iteration. Windows ONLY.
 */
#ifndef DQLITE_COMPAT_SYS_STATVFS_H
#define DQLITE_COMPAT_SYS_STATVFS_H

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
