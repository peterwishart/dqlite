/*
 * dqlite Windows port -- <sys/utsname.h> minimal shim (uname).
 *
 * Test-only (test/raft/unit/test_uv_fs.c reads the kernel version). Struct +
 * uname() declaration; IMPLEMENTATION deferred. Windows ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_SYS_UTSNAME_H
#define DQLITE_COMPAT_SYS_UTSNAME_H

#define DQLITE_UTSNAME_LEN 65

struct utsname {
	char sysname[DQLITE_UTSNAME_LEN];
	char nodename[DQLITE_UTSNAME_LEN];
	char release[DQLITE_UTSNAME_LEN];
	char version[DQLITE_UTSNAME_LEN];
	char machine[DQLITE_UTSNAME_LEN];
};

int uname(struct utsname *buf);

#endif /* DQLITE_COMPAT_SYS_UTSNAME_H */
