/*
 * dqlite Windows port -- <linux/magic.h> minimal shim.
 *
 * src/raft/uv_fs.c includes this for filesystem-type magic numbers used by the
 * Linux direct-I/O probe. That probe is compiled out / unused on the portable
 * (threadpool) write path, so the constants only need to exist. Windows ONLY.
 */
#ifndef DQLITE_COMPAT_LINUX_MAGIC_H
#define DQLITE_COMPAT_LINUX_MAGIC_H

#define TMPFS_MAGIC 0x01021994
#define XFS_SUPER_MAGIC 0x58465342
#define EXT4_SUPER_MAGIC 0xEF53
#define ZFS_SUPER_MAGIC 0x2fc12fc1
#define BTRFS_SUPER_MAGIC 0x9123683E
#define NFS_SUPER_MAGIC 0x6969

#endif /* DQLITE_COMPAT_LINUX_MAGIC_H */
