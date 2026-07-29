/*
 * dqlite Windows port -- <unistd.h> POSIX-compat shim.
 *
 * Windows/UCRT has no <unistd.h>. This shim maps the POSIX file/process calls
 * dqlite uses onto their UCRT equivalents and declares the POSIX scalar types.
 * It is on the include path on Windows ONLY (compat/win/, see CMakeLists.txt).
 *
 * Most POSIX names (read/write/close/unlink/access/lseek/dup/rmdir/getpid) are
 * already provided by <io.h>/<process.h>/<direct.h> as deprecated aliases for
 * their _-prefixed forms; we pull those headers in and silence the deprecation
 * warnings via _CRT_NONSTDC_NO_WARNINGS (set in the CMake Windows defs). The
 * remaining calls that UCRT lacks under any name get thin wrappers here.
 *
 * NOTE: these are compile-enabling shims. Some (pread/pwrite, sysconf) are
 * best-effort and NOT semantically complete (e.g. pread here is not atomic
 * w.r.t. the file offset); genuine reimplementation is a later port iteration.
 */
#ifndef DQLITE_COMPAT_UNISTD_H
#define DQLITE_COMPAT_UNISTD_H

#include <direct.h>  /* _rmdir, _chdir, _getcwd (+ POSIX aliases) */
#include <fcntl.h>   /* _O_* flags (for mkstemp) */
#include <io.h>      /* _read/_write/_close/_lseek/_dup/_unlink/_access (+ aliases) */
#include <process.h> /* _getpid (+ getpid alias) */
#include <stdio.h>   /* SEEK_SET/SEEK_CUR/SEEK_END */
#include <stdlib.h>
#include <string.h>  /* strlen, _stricmp */
#include <sys/stat.h> /* _S_IREAD/_S_IWRITE (for mkstemp) */
#include <sys/types.h>

#ifndef DQLITE_PID_T_DEFINED
#define DQLITE_PID_T_DEFINED
typedef int pid_t;
#endif

/* ssize_t / mode_t normally come from the forced-include prelude; repeat the
 * guarded definitions so this header also stands alone. */
#include <BaseTsd.h>
#if !defined(_SSIZE_T_) && !defined(_SSIZE_T_DEFINED)
typedef SSIZE_T ssize_t;
#define _SSIZE_T_
#define _SSIZE_T_DEFINED
#endif
#ifndef DQLITE_MODE_T_DEFINED
#define DQLITE_MODE_T_DEFINED
typedef unsigned short mode_t;
#endif

/* <windows.h> (for Sleep, FlushFileBuffers) is provided by the prelude; guard
 * for the stand-alone case. */
#ifndef _INC_WINDOWS
#include <windows.h>
#endif

/* sysconf(_SC_PAGESIZE) -- dqlite queries the page size in buffer.c / vfs.c.
 *
 * We deliberately report dwAllocationGranularity (64KiB on Win32), NOT
 * dwPageSize (4KiB). vfs.c's WAL shared-memory maps 32KiB regions at offsets
 * that are multiples of vfsGetMapSize() == sysconf(_SC_PAGESIZE), and it also
 * remaps regions in place at addresses that are multiples of that value. On
 * Windows BOTH the file offset passed to MapViewOfFileEx and the fixed base
 * address it is mapped at must be multiples of the *allocation granularity*
 * (64KiB), not the page size. Reporting 64KiB makes vfsGetMapSize() return
 * 64KiB, so each mapping covers two 32KiB regions and every mmap offset/base is
 * 64KiB-aligned -- exactly what MapViewOfFileEx requires.
 *
 * CAUTION: this coarse value is ONLY correct for the vfs.c mmap-alignment use.
 * buffer.c uses page_size as its query row-batch flush threshold (see
 * query__batch), where 64KiB vs the true 4KiB changes the observable
 * pause/resume boundary and broke concurrency/delete tests -- so buffer__init
 * now takes the genuine page size from dqlite_win_page_size() below, NOT this
 * shim. Do not route any size-of-a-page (as opposed to allocation-granularity)
 * use through sysconf() on Windows.
 */
#define _SC_PAGESIZE 1
#define _SC_PAGE_SIZE _SC_PAGESIZE

static inline long sysconf(int name)
{
	SYSTEM_INFO si;
	if (name == _SC_PAGESIZE) {
		GetSystemInfo(&si);
		return (long)si.dwAllocationGranularity;
	}
	return -1;
}

/* True CPU memory page size (dwPageSize, 4KiB), distinct from the allocation
 * granularity reported by sysconf(_SC_PAGESIZE) above. Callers that need the
 * genuine page size -- e.g. buffer.c, which uses it as the query row-batch
 * flush threshold in query__batch() and must therefore match the Linux 4KiB
 * value, not the 64KiB mmap-alignment unit vfs.c needs -- use this instead. */
static inline unsigned dqlite_win_page_size(void)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return (unsigned)si.dwPageSize;
}

/* ftruncate -> grow-or-shrink a file to `length`.
 *
 * The straightforward _chsize_s (SetEndOfFile) fails with
 * ERROR_USER_MAPPED_FILE when the file already has an active memory-mapped
 * view -- which is exactly the situation vfs.c creates when it grows the
 * WAL-shm file to add another region while earlier regions are still mapped.
 * Extending a file through a throwaway file-mapping object is performed by the
 * section manager and is NOT subject to that restriction, so use it to grow.
 * Shrinking (or the case where we can't size the handle) falls back to
 * _chsize_s. Returns 0 on success, -1 on error. */
static inline int ftruncate(int fd, long long length)
{
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	LARGE_INTEGER cur;
	if (h != INVALID_HANDLE_VALUE && h != NULL && length >= 0 &&
	    GetFileSizeEx(h, &cur) && length > cur.QuadPart) {
		HANDLE m = CreateFileMappingW(
		    h, NULL, PAGE_READWRITE,
		    (DWORD)((unsigned long long)length >> 32),
		    (DWORD)((unsigned long long)length & 0xFFFFFFFFu), NULL);
		if (m != NULL) {
			CloseHandle(m);
			return 0;
		}
		/* fall through to _chsize_s on failure */
	}
	return _chsize_s(fd, length) == 0 ? 0 : -1;
}

/* fsync / fdatasync -> _commit (flush to disk). */
static inline int fsync(int fd)
{
	return _commit(fd);
}

static inline int fdatasync(int fd)
{
	return _commit(fd);
}

/* usleep / sleep -> Win32 Sleep (millisecond granularity). */
static inline int usleep(unsigned int usec)
{
	Sleep((DWORD)(usec / 1000));
	return 0;
}

static inline unsigned int sleep(unsigned int seconds)
{
	Sleep((DWORD)(seconds * 1000));
	return 0;
}

/* mkstemp: create+open a unique temp file from a "XXXXXX" template. Best-effort
 * (uses _mktemp_s + _open); a robust O_EXCL port comes later. */
static inline int mkstemp(char *template_)
{
	if (_mktemp_s(template_, strlen(template_) + 1) != 0) {
		return -1;
	}
	return _open(template_, _O_RDWR | _O_CREAT | _O_EXCL | _O_BINARY,
		     _S_IREAD | _S_IWRITE);
}

/* mkdtemp: create a uniquely-named directory from a trailing "XXXXXX" template
 * and return the (modified-in-place) template on success, NULL on failure.
 * Best-effort: _mktemp_s chooses a unique name, then _mkdir creates it. This
 * is NOT atomic against a concurrent creator (a robust port loops on EEXIST);
 * adequate for the temp-dir setup in the tests. */
static inline char *mkdtemp(char *template_)
{
	if (_mktemp_s(template_, strlen(template_) + 1) != 0) {
		return NULL;
	}
	if (_mkdir(template_) != 0) {
		return NULL;
	}
	return template_;
}

/* getuid / geteuid: Windows has no POSIX uid. The only callers (raft uv_fs /
 * uv_load tests) test `getuid() == 0` to skip permission-denied checks when
 * running as root. Return 0 ("root") so those Windows-inapplicable EACCES
 * tests are skipped rather than run with POSIX permission semantics that do
 * not apply here. */
static inline int getuid(void)
{
	return 0;
}

static inline int geteuid(void)
{
	return 0;
}

/* pread / pwrite -- positional I/O. Best-effort seek+rw; NOT atomic w.r.t. the
 * shared file offset (a proper OVERLAPPED-based port comes later). */
static inline ssize_t pread(int fd, void *buf, size_t count, long long offset)
{
	if (_lseeki64(fd, offset, SEEK_SET) < 0) {
		return -1;
	}
	return (ssize_t)_read(fd, buf, (unsigned int)count);
}

static inline ssize_t pwrite(int fd, const void *buf, size_t count, long long offset)
{
	if (_lseeki64(fd, offset, SEEK_SET) < 0) {
		return -1;
	}
	return (ssize_t)_write(fd, buf, (unsigned int)count);
}

/* strcasecmp / strncasecmp -> UCRT _stricmp / _strnicmp. */
#include <string.h>
static inline int strcasecmp(const char *a, const char *b)
{
	return _stricmp(a, b);
}

static inline int strncasecmp(const char *a, const char *b, size_t n)
{
	return _strnicmp(a, b, n);
}

/* POSIX reentrant time helpers -> UCRT *_s forms (tracing.c uses gmtime_r). */
#include <time.h>
static inline struct tm *gmtime_r(const time_t *timep, struct tm *result)
{
	return gmtime_s(result, timep) == 0 ? result : NULL;
}

static inline struct tm *localtime_r(const time_t *timep, struct tm *result)
{
	return localtime_s(result, timep) == 0 ? result : NULL;
}

/* nanosleep -> Win32 Sleep (millisecond granularity); vfs.c uses it to back
 * off on WAL contention. */
struct timespec_compat_guard;
static inline int nanosleep_ms(long long ms)
{
	Sleep((DWORD)ms);
	return 0;
}
#define nanosleep(req, rem) \
	nanosleep_ms(((req)->tv_sec * 1000LL) + ((req)->tv_nsec / 1000000LL))

#endif /* DQLITE_COMPAT_UNISTD_H */
