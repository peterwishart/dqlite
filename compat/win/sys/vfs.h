/*
 * dqlite Windows port -- <sys/vfs.h> minimal shim (statfs).
 *
 * Consumers: src/raft/uv_fs.c's direct-I/O probe compiles against this but
 * early-returns "no direct I/O" on Windows before calling fstatfs();
 * src/raft/uv_os.c's UvOsFallocateEmulation calls fstatfs() for f_bsize; and
 * test/raft/integration/test_uv_init.c calls statfs() and compares f_type.
 * The implementation (compat/win/compat_win.c) reports a generic tmpfs-like
 * filesystem (TMPFS_MAGIC, 4KiB f_bsize), values those callers key off.
 * Windows ONLY.
 */
#ifndef DQLITE_COMPAT_SYS_VFS_H
#define DQLITE_COMPAT_SYS_VFS_H

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
