/*
 * dqlite Windows port -- <netdb.h> POSIX-compat shim.
 *
 * getaddrinfo / freeaddrinfo / getnameinfo / struct addrinfo / gai_strerror
 * all come from <ws2tcpip.h> on Windows. Windows ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_NETDB_H
#define DQLITE_COMPAT_NETDB_H

#include <winsock2.h>
#include <ws2tcpip.h>

#endif /* DQLITE_COMPAT_NETDB_H */
