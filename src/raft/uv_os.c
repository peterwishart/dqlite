#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
/* UNVERIFIED-NEEDS-MAC: macOS analog of _GNU_SOURCE. Exposes the BSD
 * <sys/mount.h> statfs API and the F_NOCACHE fcntl. Guarded so Linux and
 * Windows preprocessed output is unchanged. */
#define _DARWIN_C_SOURCE
#endif

#include "uv_os.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#if defined(DQLITE_HAVE_KAIO)
#include <sys/eventfd.h>
#endif
#include <sys/types.h>
#if defined(__APPLE__)
/* UNVERIFIED-NEEDS-MAC: macOS/BSD provide `struct statfs` (with f_bsize used by
 * UvOsFallocateEmulation) + fstatfs() via <sys/mount.h>, not <sys/vfs.h>. */
#include <sys/mount.h>
#include <sys/param.h>
#else
#include <sys/vfs.h>
#endif
#include <unistd.h>
#include <uv.h>

#include "../lib/assert.h"
#include "err.h"
#include "syscall.h"

/* Default permissions when creating a directory. */
#define DEFAULT_DIR_PERM 0700

#if defined(_WIN32)
/* Windows durability (route A): the compat prelude defines O_DSYNC as the
 * numeric value of libuv's UV_FS_O_DSYNC, which uv_fs_open() (the only path by
 * which dqlite's open flags reach the kernel on Windows, via UvOsOpen below)
 * translates to CreateFile's FILE_FLAG_WRITE_THROUGH. Guard against a libuv
 * upgrade silently changing the value. */
_Static_assert(O_DSYNC == UV_FS_O_DSYNC,
	       "compat O_DSYNC must equal libuv's UV_FS_O_DSYNC");
#endif

int UvOsOpen(const char *path, int flags, int mode, uv_file *fd)
{
	struct uv_fs_s req;
	int rv;
	rv = uv_fs_open(NULL, &req, path, flags, mode, NULL);
	if (rv < 0) {
		return rv;
	}
	*fd = rv;
	return 0;
}

int UvOsClose(uv_file fd)
{
	struct uv_fs_s req;
	return uv_fs_close(NULL, &req, fd, NULL);
}

/* Emulate fallocate(). Mostly taken from glibc's implementation. */
int UvOsFallocateEmulation(int fd, off_t offset, off_t len)
{
	ssize_t increment;
	struct statfs f;
	int rv;

	rv = fstatfs(fd, &f);
	if (rv != 0) {
		return -errno;
	}

	if (f.f_bsize == 0) {
		increment = 512;
	} else if (f.f_bsize < 4096) {
		increment = (ssize_t)f.f_bsize;
	} else {
		increment = 4096;
	}

	for (offset += (len - 1) % increment; len > 0; offset += increment) {
		len -= increment;
		rv = (int)pwrite(fd, "", 1, offset);
		if (rv != 1) {
			return -errno;
		}
	}

	return 0;
}

int UvOsFallocate(uv_file fd, off_t offset, off_t len)
{
	int rv;
retry:
	rv = posix_fallocate(fd, offset, len);
	if (rv == EINTR) {
		goto retry;
	}

	/* From the manual page:
	 *
	 *   posix_fallocate() returns zero on success, or an error
	 *   number on failure.  Note that errno is not set.
	 *   The negation is here as all UV_XXX errors are just the 
	 *   negation of whatever POSIX error code is.
	 */
	return -rv;
}

int UvOsTruncate(uv_file fd, off_t offset)
{
	struct uv_fs_s req;
	return uv_fs_ftruncate(NULL, &req, fd, offset, NULL);
}

int UvOsFsync(uv_file fd)
{
	struct uv_fs_s req;
	return uv_fs_fsync(NULL, &req, fd, NULL);
}

int UvOsFdatasync(uv_file fd)
{
	struct uv_fs_s req;
	return uv_fs_fdatasync(NULL, &req, fd, NULL);
}

int UvOsStat(const char *path, uv_stat_t *sb)
{
	struct uv_fs_s req;
	int rv;
	rv = uv_fs_stat(NULL, &req, path, NULL);
	if (rv != 0) {
		return rv;
	}
	memcpy(sb, &req.statbuf, sizeof *sb);
	return 0;
}

int UvOsWrite(uv_file fd,
	      const uv_buf_t bufs[],
	      unsigned int nbufs,
	      int64_t offset)
{
	struct uv_fs_s req;
	return uv_fs_write(NULL, &req, fd, bufs, nbufs, offset, NULL);
}

int UvOsUnlink(const char *path)
{
	struct uv_fs_s req;
	return uv_fs_unlink(NULL, &req, path, NULL);
}

int UvOsRename(const char *path1, const char *path2)
{
	struct uv_fs_s req;
	return uv_fs_rename(NULL, &req, path1, path2, NULL);
}

int UvOsJoin(const char *dir, const char *filename, char *path)
{
	if (!UV__DIR_HAS_VALID_LEN(dir) ||
	    !UV__FILENAME_HAS_VALID_LEN(filename)) {
		return -1;
	}
	strcpy(path, dir);
	strcat(path, "/");
	strcat(path, filename);
	return 0;
}

#if defined(DQLITE_HAVE_KAIO)
int UvOsIoSetup(unsigned nr, aio_context_t *ctxp)
{
	int rv;
	rv = io_setup(nr, ctxp);
	if (rv == -1) {
		return -errno;
	}
	return 0;
}

int UvOsIoDestroy(aio_context_t ctx)
{
	int rv;
	rv = io_destroy(ctx);
	if (rv == -1) {
		return -errno;
	}
	return 0;
}

int UvOsIoSubmit(aio_context_t ctx, long nr, struct iocb **iocbpp)
{
	int rv;
	rv = io_submit(ctx, nr, iocbpp);
	if (rv == -1) {
		return -errno;
	}
	dqlite_assert(rv == nr); /* TODO: can something else be returned? */
	return 0;
}

int UvOsIoGetevents(aio_context_t ctx,
		    long min_nr,
		    long max_nr,
		    struct io_event *events,
		    struct timespec *timeout)
{
	int rv;
	do {
		rv = io_getevents(ctx, min_nr, max_nr, events, timeout);
	} while (rv == -1 && errno == EINTR);

	if (rv == -1) {
		return -errno;
	}
	dqlite_assert(rv >= min_nr);
	dqlite_assert(rv <= max_nr);
	return rv;
}

int UvOsEventfd(unsigned int initval, int flags)
{
	int rv;
	/* At the moment only UV_FS_O_NONBLOCK is supported */
	dqlite_assert(flags == UV_FS_O_NONBLOCK);
	flags = EFD_NONBLOCK | EFD_CLOEXEC;
	rv = eventfd(initval, flags);
	if (rv == -1) {
		return -errno;
	}
	return rv;
}
#endif /* DQLITE_HAVE_KAIO */

int UvOsSetDirectIo(uv_file fd)
{
	int rv;
#if defined(O_DIRECT)
	/* Linux: request O_DIRECT via the file status flags. */
	int flags = fcntl(fd, F_GETFL);
	rv = fcntl(fd, F_SETFL, flags | O_DIRECT);
#elif defined(__APPLE__)
	/* macOS has no O_DIRECT; F_NOCACHE disables the buffer cache. */
	rv = fcntl(fd, F_NOCACHE, 1);
#else
	/* No direct-I/O primitive on this platform (e.g. Windows uses
	 * FILE_FLAG_NO_BUFFERING at open time instead). */
	(void)fd;
	return UV_ENOTSUP;
#endif
	if (rv == -1) {
		return -errno;
	}
	return 0;
}
