/*
 * dqlite Windows port -- <sys/un.h> POSIX-compat shim.
 *
 * struct sockaddr_un lives in <afunix.h> on Windows (AF_UNIX support, Win10
 * 1803+). Windows ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_SYS_UN_H
#define DQLITE_COMPAT_SYS_UN_H

/* Shadowing fence (PORT_TODO.md W3): only TUs built with dqlite's forced-
 * include prelude (clang-cl /FI dqlite_win_prelude.h, which defines
 * DQLITE_WIN_COMPAT) may use this shim; any other consumer that resolves this
 * name -- e.g. a dependency TU compiled inside a dqlite target -- must break
 * loudly here instead of silently picking up stubs. Full rationale in
 * compat/win/dqlite_win_prelude.h. */
#if !defined(_WIN32) || !defined(DQLITE_WIN_COMPAT)
#error "dqlite compat/win shim header reached without the dqlite Windows prelude (DQLITE_WIN_COMPAT undefined); this header must not shadow a real system header -- see compat/win/dqlite_win_prelude.h"
#endif

#include <winsock2.h>
#include <afunix.h> /* struct sockaddr_un, UNIX_PATH_MAX */

#endif /* DQLITE_COMPAT_SYS_UN_H */
