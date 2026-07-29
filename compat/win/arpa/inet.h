/*
 * dqlite Windows port -- <arpa/inet.h> POSIX-compat shim.
 *
 * inet_ntop / inet_pton / htons / ntohs / htonl / ntohl come from Winsock2 /
 * ws2tcpip. Windows ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_ARPA_INET_H
#define DQLITE_COMPAT_ARPA_INET_H

#include <winsock2.h>
#include <ws2tcpip.h>

#endif /* DQLITE_COMPAT_ARPA_INET_H */
