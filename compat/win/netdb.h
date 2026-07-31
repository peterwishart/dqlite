/*
 * dqlite Windows port -- <netdb.h> POSIX-compat shim.
 *
 * getaddrinfo / freeaddrinfo / getnameinfo / struct addrinfo / gai_strerror
 * all come from <ws2tcpip.h> on Windows. Windows ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_NETDB_H
#define DQLITE_COMPAT_NETDB_H

/* Shadowing fence -- see the rationale in dqlite_win_prelude.h. */
#if !defined(_WIN32) || !defined(DQLITE_WIN_COMPAT)
#error "dqlite compat/win shim header reached without the dqlite Windows prelude (DQLITE_WIN_COMPAT undefined); this header must not shadow a real system header -- see compat/win/dqlite_win_prelude.h"
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#endif /* DQLITE_COMPAT_NETDB_H */
