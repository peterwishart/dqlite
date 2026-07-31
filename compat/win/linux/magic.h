/*
 * dqlite Windows port -- <linux/magic.h> minimal shim.
 *
 * Filesystem-type magic numbers. Includers: src/raft/uv_fs.c (inside its
 * HAVE_XFS_XFS_H block, never compiled on Windows) and
 * test/raft/integration/test_uv_init.c, which compares statfs() f_type against
 * TMPFS_MAGIC at runtime -- and matches, because the compat statfs()
 * (compat_win.c) reports TMPFS_MAGIC. The other constants only need to exist.
 * Windows ONLY.
 */
#ifndef DQLITE_COMPAT_LINUX_MAGIC_H
#define DQLITE_COMPAT_LINUX_MAGIC_H

/* Shadowing fence -- see the rationale in dqlite_win_prelude.h. */
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
