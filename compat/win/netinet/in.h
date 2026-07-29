/*
 * dqlite Windows port -- <netinet/in.h> POSIX-compat shim.
 *
 * struct sockaddr_in / sockaddr_in6 / in_addr / in6_addr, htons/ntohs,
 * INADDR_*, IPPROTO_* all come from Winsock2 / ws2tcpip. Windows ONLY.
 */
#ifndef DQLITE_COMPAT_NETINET_IN_H
#define DQLITE_COMPAT_NETINET_IN_H

#include <winsock2.h>
#include <ws2tcpip.h>

#endif /* DQLITE_COMPAT_NETINET_IN_H */
