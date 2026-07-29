/*
 * dqlite Windows port -- <poll.h> POSIX-compat shim.
 *
 * Winsock2 provides `struct pollfd`, the POLL* event flags and WSAPoll(), the
 * Win32 equivalent of poll(2). Map poll() -> WSAPoll() and declare nfds_t.
 * Windows ONLY (compat/win/). Used by src/client/protocol.c.
 */
#ifndef DQLITE_COMPAT_POLL_H
#define DQLITE_COMPAT_POLL_H

/* Shadowing fence (PORT_TODO.md W3): only TUs built with dqlite's forced-
 * include prelude (clang-cl /FI dqlite_win_prelude.h, which defines
 * DQLITE_WIN_COMPAT) may use this shim; any other consumer that resolves this
 * name -- e.g. a dependency TU compiled inside a dqlite target -- must break
 * loudly here instead of silently picking up stubs. Full rationale in
 * compat/win/dqlite_win_prelude.h. */
#if !defined(_WIN32) || !defined(DQLITE_WIN_COMPAT)
#error "dqlite compat/win shim header reached without the dqlite Windows prelude (DQLITE_WIN_COMPAT undefined); this header must not shadow a real system header -- see compat/win/dqlite_win_prelude.h"
#endif

#include <winsock2.h> /* struct pollfd, POLLIN/POLLOUT/..., WSAPoll */

#ifndef DQLITE_NFDS_T_DEFINED
#define DQLITE_NFDS_T_DEFINED
typedef unsigned long nfds_t;
#endif

/* WSAPoll has the same signature as poll(2). */
#define poll(fds, nfds, timeout) WSAPoll((fds), (ULONG)(nfds), (INT)(timeout))

#endif /* DQLITE_COMPAT_POLL_H */
