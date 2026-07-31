/*
 * dqlite Windows port -- <sys/socket.h> POSIX-compat shim.
 *
 * Pulls in the Winsock2 sockets API (already parsed by the forced-include
 * prelude) and declares the POSIX bits Winsock spells differently. Windows
 * ONLY (compat/win/). WSAStartup is handled structurally by
 * dqliteWinSocketsInit() (compat_win.c, called from the library's public
 * object-creation entry points), and the TCP transport is ported and running
 * on top of these names; this header itself only maps spelling.
 */
#ifndef DQLITE_COMPAT_SYS_SOCKET_H
#define DQLITE_COMPAT_SYS_SOCKET_H

/* Shadowing fence -- see the rationale in dqlite_win_prelude.h. */
#if !defined(_WIN32) || !defined(DQLITE_WIN_COMPAT)
#error "dqlite compat/win shim header reached without the dqlite Windows prelude (DQLITE_WIN_COMPAT undefined); this header must not shadow a real system header -- see compat/win/dqlite_win_prelude.h"
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

/* socklen_t comes from <ws2tcpip.h>; sa_family_t is not defined by Winsock. */
#ifndef DQLITE_SA_FAMILY_T_DEFINED
#define DQLITE_SA_FAMILY_T_DEFINED
typedef unsigned short sa_family_t;
#endif

/* Linux socket-type flags OR'd into the socket()/accept4() type argument; no
 * Winsock equivalent -> 0 (no-op) so the flag arithmetic compiles. A real port
 * sets non-inheritable / non-blocking handles explicitly (worklist). */
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0
#endif

/* NOTE: accept4() is provided by the forced-include prelude (not here), so it
 * is visible to callers such as test/lib/endpoint.c and test/raft/lib/tcp.c
 * that use it without including <sys/socket.h>. */

#endif /* DQLITE_COMPAT_SYS_SOCKET_H */
