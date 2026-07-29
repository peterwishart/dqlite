/*
 * dqlite Windows port -- <sys/vfs.h> minimal shim (statfs).
 *
 * src/raft/uv_fs.c and uv_os.c use `struct statfs` + fstatfs() to probe the
 * filesystem type for the Linux direct-I/O decision. That probe is Linux
 * semantics; on Windows the portable threadpool write path is used instead
 * (see PORT_DESIGN.md), so this shim only needs to let the code COMPILE.
 * A real capability probe / compile-out is a later iteration. Windows ONLY.
 */
#ifndef DQLITE_COMPAT_SYS_VFS_H
#define DQLITE_COMPAT_SYS_VFS_H

#include <stddef.h>

typedef struct dqlite_fsid { int val[2]; } fsid_t;

struct statfs {
	long f_type;    /* filesystem type */
	long f_bsize;   /* optimal transfer block size */
	long f_blocks;  /* total data blocks */
	long f_bfree;   /* free blocks */
	long f_bavail;  /* free blocks available to unprivileged user */
	long f_files;   /* total file nodes */
	long f_ffree;   /* free file nodes */
	fsid_t f_fsid;  /* filesystem id */
	long f_namelen; /* maximum length of filenames */
};

int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);

#endif /* DQLITE_COMPAT_SYS_VFS_H */
