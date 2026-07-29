/*
 * dqlite Windows port -- <linux/magic.h> minimal shim.
 *
 * src/raft/uv_fs.c includes this for filesystem-type magic numbers used by the
 * Linux direct-I/O probe. That probe is compiled out / unused on the portable
 * (threadpool) write path, so the constants only need to exist. Windows ONLY.
 */
#ifndef DQLITE_COMPAT_LINUX_MAGIC_H
#define DQLITE_COMPAT_LINUX_MAGIC_H

/* Shadowing fence (PORT_TODO.md W3): only TUs built with dqlite's forced-
 * include prelude (clang-cl /FI dqlite_win_prelude.h, which defines
 * DQLITE_WIN_COMPAT) may use this shim; any other consumer that resolves this
 * name -- e.g. a dependency TU compiled inside a dqlite target -- must break
 * loudly here instead of silently picking up stubs. Full rationale in
 * compat/win/dqlite_win_prelude.h. */
#if !defined(_WIN32) || !defined(DQLITE_WIN_COMPAT)
#error "dqlite compat/win shim header reached without the dqlite Windows prelude (DQLITE_WIN_COMPAT undefined); this header must not shadow a real system header -- see compat/win/dqlite_win_prelude.h"
#endif

#define TMPFS_MAGIC 0x01021994
#define XFS_SUPER_MAGIC 0x58465342
#define EXT4_SUPER_MAGIC 0xEF53
#define ZFS_SUPER_MAGIC 0x2fc12fc1
#define BTRFS_SUPER_MAGIC 0x9123683E
#define NFS_SUPER_MAGIC 0x6969

#endif /* DQLITE_COMPAT_LINUX_MAGIC_H */
