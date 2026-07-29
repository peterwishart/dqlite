/*
 * dqlite Windows port -- <sys/uio.h> minimal shim (struct iovec).
 *
 * Provides the POSIX scatter/gather iovec type. On Windows the equivalent is
 * WSABUF (different field order/types); a real port would translate. Windows
 * ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_SYS_UIO_H
#define DQLITE_COMPAT_SYS_UIO_H

#include <stddef.h>

struct iovec {
	void *iov_base;
	size_t iov_len;
};

ssize_t readv(int fd, const struct iovec *iov, int iovcnt);
ssize_t writev(int fd, const struct iovec *iov, int iovcnt);

#endif /* DQLITE_COMPAT_SYS_UIO_H */
