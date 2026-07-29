/*
 * dqlite Windows port -- <sys/utsname.h> minimal shim (uname).
 *
 * Test-only (test/raft/unit/test_uv_fs.c reads the kernel version). Struct +
 * uname() declaration; IMPLEMENTATION deferred. Windows ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_SYS_UTSNAME_H
#define DQLITE_COMPAT_SYS_UTSNAME_H

/* Shadowing fence (PORT_TODO.md W3): only TUs built with dqlite's forced-
 * include prelude (clang-cl /FI dqlite_win_prelude.h, which defines
 * DQLITE_WIN_COMPAT) may use this shim; any other consumer that resolves this
 * name -- e.g. a dependency TU compiled inside a dqlite target -- must break
 * loudly here instead of silently picking up stubs. Full rationale in
 * compat/win/dqlite_win_prelude.h. */
#if !defined(_WIN32) || !defined(DQLITE_WIN_COMPAT)
#error "dqlite compat/win shim header reached without the dqlite Windows prelude (DQLITE_WIN_COMPAT undefined); this header must not shadow a real system header -- see compat/win/dqlite_win_prelude.h"
#endif

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
