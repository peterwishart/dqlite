/*
 * dqlite Windows port -- <netinet/tcp.h> POSIX-compat shim.
 *
 * TCP_NODELAY and friends come from Winsock2 / ws2tcpip on Windows. Windows
 * ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_NETINET_TCP_H
#define DQLITE_COMPAT_NETINET_TCP_H

#include <winsock2.h>
#include <ws2tcpip.h>

#endif /* DQLITE_COMPAT_NETINET_TCP_H */
