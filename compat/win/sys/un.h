/*
 * dqlite Windows port -- <sys/un.h> POSIX-compat shim.
 *
 * struct sockaddr_un lives in <afunix.h> on Windows (AF_UNIX support, Win10
 * 1803+). Windows ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_SYS_UN_H
#define DQLITE_COMPAT_SYS_UN_H

#include <winsock2.h>
#include <afunix.h> /* struct sockaddr_un, UNIX_PATH_MAX */

#endif /* DQLITE_COMPAT_SYS_UN_H */
