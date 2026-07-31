/*
 * dqlite Windows port -- <poll.h> POSIX-compat shim.
 *
 * Winsock2 provides `struct pollfd`, the POLL* event flags and WSAPoll(), the
 * Win32 equivalent of poll(2). Map poll() -> WSAPoll() and declare nfds_t.
 * Windows ONLY (compat/win/). Used by src/client/protocol.c.
 */
#ifndef DQLITE_COMPAT_POLL_H
#define DQLITE_COMPAT_POLL_H

/* Shadowing fence -- see the rationale in dqlite_win_prelude.h. */
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
