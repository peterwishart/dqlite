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
 * NOTE: some of these are compile-enabling shims that are NOT semantically
 * complete (e.g. sysconf answers exactly one question, mkstemp/mkdtemp are
 * best-effort); see each function's comment for its precise contract.
 */
#ifndef DQLITE_COMPAT_UNISTD_H
#define DQLITE_COMPAT_UNISTD_H

/* Shadowing fence (PORT_TODO.md W3): only TUs built with dqlite's forced-
 * include prelude (clang-cl /FI dqlite_win_prelude.h, which defines
 * DQLITE_WIN_COMPAT) may use this shim; any other consumer that resolves this
 * name -- e.g. a dependency TU compiled inside a dqlite target -- must break
 * loudly here instead of silently picking up stubs. Full rationale in
 * compat/win/dqlite_win_prelude.h. */
#if !defined(_WIN32) || !defined(DQLITE_WIN_COMPAT)
#error "dqlite compat/win shim header reached without the dqlite Windows prelude (DQLITE_WIN_COMPAT undefined); this header must not shadow a real system header -- see compat/win/dqlite_win_prelude.h"
#endif

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

/* sysconf(_SC_PAGESIZE) -- the TRUE CPU memory page size (dwPageSize, 4KiB),
 * exactly what POSIX sysconf reports. The only Windows-compiled caller is
 * src/lib/buffer.c (buffer__init), which uses it as the query row-batch flush
 * threshold in query__batch() and therefore must see the same 4KiB value as
 * Linux (plus the test mirrors of that math in test/unit/lib/test_buffer.c
 * and test/unit/test_gateway.c).
 *
 * HISTORY / CAUTION: an earlier version of this shim lied and returned
 * dwAllocationGranularity (64KiB) because src/vfs.c needs that value for
 * WAL-index mmap alignment. That lie leaked into buffer.c's flush threshold
 * and changed the observable query pause/resume boundary (bug #10 in
 * PORT_TODO.md, concurrency/delete failures). sysconf now tells the truth;
 * code that needs the mapping-alignment unit must ask for it by name via
 * dqlite_win_allocation_granularity() below. */
#define _SC_PAGESIZE 1
#define _SC_PAGE_SIZE _SC_PAGESIZE

static inline long sysconf(int name)
{
	SYSTEM_INFO si;
	if (name == _SC_PAGESIZE) {
		GetSystemInfo(&si);
		return (long)si.dwPageSize;
	}
	return -1;
}

/* Win32 allocation granularity (dwAllocationGranularity, 64KiB): the unit that
 * BOTH the file offset passed to MapViewOfFile3 and the fixed base address a
 * view is (re)mapped at must be a multiple of. The only caller is
 * vfsGetMapSize() (src/vfs.c), which sizes the WAL-index shm mappings with it
 * so that each 64KiB mapping covers two 32KiB regions and every mmap
 * offset/base the vfs uses is granularity-aligned -- exactly what the mapping
 * APIs backing compat_win.c's mmap() require. This is deliberately NOT what
 * sysconf(_SC_PAGESIZE) returns (see above): the granularity is a
 * mapping-alignment unit, not the size of a memory page. */
static inline unsigned dqlite_win_allocation_granularity(void)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	return (unsigned)si.dwAllocationGranularity;
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

/* sleep -> Win32 Sleep (millisecond granularity). Only tests call it (1s
 * waits); production code sleeps via nanosleep below or libuv timers. */
static inline unsigned int sleep(unsigned int seconds)
{
	Sleep((DWORD)(seconds * 1000));
	return 0;
}

/* mkstemp: create+open a unique temp file from a "XXXXXX" template. The open
 * IS O_EXCL, so it never silently reuses an existing file; best-effort only in
 * that a losing race with a concurrent creator of the same name fails instead
 * of retrying with a new name the way POSIX mkstemp does (_mktemp_s picks the
 * name in a separate step). Adequate for the tests' temp-file setup. */
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

/* getuid: Windows has no POSIX uid. DELIBERATE FALSEHOOD, returns 0 ("root").
 * The only two callers are tests that check `getuid() == 0` to skip
 * permission-denied (EACCES) scenarios when running as root:
 *   - test/raft/unit/test_uv_fs.c, UvFsProbeCapabilities/noAccess
 *   - test/raft/integration/test_uv_load.c, load/openSegmentWithNoAccessPermission
 * Those scenarios rely on POSIX execute/read permission bits that chmod-style
 * calls cannot enforce on Windows, so "pretend root" makes exactly those two
 * tests skip. Any future caller wanting a real identity must NOT use this. */
static inline int getuid(void)
{
	return 0;
}

/* pread / pwrite -- positional I/O, implemented in compat/win/compat_win.c on
 * ReadFile/WriteFile with an OVERLAPPED offset (a single positional syscall,
 * no seek+rw pair that a concurrent same-fd user could interleave with), and
 * with the fd's file position saved/restored so it is not moved, matching
 * POSIX. An earlier version was _lseeki64+_read/_write, which both mutated the
 * shared offset and was two racy steps. The `offset` parameter is declared
 * `long long` (not off_t): the UCRT's off_t is 32-bit (see the off_t notes in
 * dqlite_win_prelude.h). */
ssize_t pread(int fd, void *buf, size_t count, long long offset);
ssize_t pwrite(int fd, const void *buf, size_t count, long long offset);

/* strcasecmp -> UCRT _stricmp (src/query.c compares column type names). */
#include <string.h>
static inline int strcasecmp(const char *a, const char *b)
{
	return _stricmp(a, b);
}

/* POSIX reentrant time helper -> UCRT *_s form (tracing.c uses gmtime_r). */
#include <time.h>
static inline struct tm *gmtime_r(const time_t *timep, struct tm *result)
{
	return gmtime_s(result, timep) == 0 ? result : NULL;
}

/* nanosleep -> high-resolution waitable timer (implemented in
 * compat/win/compat_win.c, see dqliteWinNanosleep there for the full story).
 *
 * The sole production caller is vfsSleep (src/vfs.c), SQLite's xSleep, which
 * SQLite's WAL code drives with MICROSECOND backoffs (1us..~350us) while
 * spinning on WAL-index lock contention. An earlier version of this shim
 * truncated to whole milliseconds via Sleep(), turning every sub-ms request
 * into Sleep(0) -- a yield, not a sleep -- so the backoff busy-spun under
 * contention. The real function honours sub-millisecond durations.
 *
 * `rem` is ignored: Windows has no signal interruption, so the sleep never
 * returns early and there is never a remainder to report (POSIX writes *rem
 * only on EINTR). */
int dqliteWinNanosleep(long long sec, long long nsec);
#define nanosleep(req, rem) dqliteWinNanosleep((req)->tv_sec, (req)->tv_nsec)

#endif /* DQLITE_COMPAT_UNISTD_H */
