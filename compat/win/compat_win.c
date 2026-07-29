/*
 * dqlite Windows port -- compat_win.c
 *
 * Windows-only implementations of the POSIX functions that the raft/dqlite
 * sources call but that Win32/UCRT does not provide. Compiled ONLY on Windows
 * (guarded on _WIN32 and wired into the build via an if(WIN32) static library
 * `dqlite_win_compat` -- see CMakeLists.txt). No non-Windows build sees this
 * file. Prototypes match the shim headers in compat/win/ (ftw.h, sys/vfs.h,
 * sys/statvfs.h, sys/utsname.h) and the fcntl() declaration in the forced
 * prelude.
 *
 * Behavioural summary:
 *  - fcntl():   no-op the descriptor-flag commands and return success. O_DIRECT
 *               is #defined to 0 on Windows (prelude), so "enabling" direct I/O
 *               is a genuine no-op -- the OS keeps buffered semantics. This
 *               preserves the intended "no real direct I/O on Windows" path.
 *  - statfs()/fstatfs(): report a generic, tmpfs-like filesystem so the raft
 *               direct-I/O probe (uv_fs.c) concludes "not a direct-I/O capable
 *               fs" and falls back to the portable buffered write path.
 *  - statvfs()/fstatvfs(): report free space (queried via GetDiskFreeSpaceEx,
 *               but CAPPED -- see below) for the test disk-fill helper.
 *  - nftw():    a real recursive directory walk honouring FTW_DEPTH (post-order)
 *               and FTW_PHYS (don't descend into reparse points), used by the
 *               test teardown to remove a temp tree.
 *  - uname():   fill struct utsname with reasonable Windows values.
 */

#ifdef _WIN32

#include <windows.h>

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <fcntl.h>   /* _O_RDWR, _O_BINARY (for _open_osfhandle) */
#include <io.h>      /* _get_osfhandle, _open_osfhandle */
#include <process.h> /* _beginthreadex */
#include <time.h>    /* struct timespec */

#include <sys/mman.h> /* PROT_*, MAP_*, mmap/munmap/msync/memfd_create protos */

#include <ftw.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/file.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <sys/vfs.h>

#ifdef _DEBUG
#include <crtdbg.h>
#endif

/* Keep automated/CI test runs non-interactive. By default the Windows debug CRT
 * pops a modal dialog on a failed assert()/abort() or a heap error, which blocks
 * unattended runs. Route those diagnostics to stderr and suppress the abort()
 * message box instead. Runs once at startup (the object is always linked because
 * the raft/test code references the POSIX shims below). Debug-only reporting
 * changes; no effect on release builds or on program behaviour/correctness. */
__attribute__((constructor)) static void dqliteWinQuietCrt(void)
{
	/* Initialise Winsock process-wide before any socket call. dqlite makes
	 * raw socket()/getaddrinfo()/connect() calls (src/transport.c,
	 * src/lib/addr.c, src/client/protocol.c) that run independently of
	 * libuv's own internal WSAStartup, and getaddrinfo() in particular fails
	 * with WSANOTINITIALISED if Winsock has not been started. WSAStartup is
	 * reference-counted, so an extra init here is harmless even though libuv
	 * also initialises Winsock. Requesting Winsock 2.2. The matching
	 * WSACleanup is intentionally omitted: process teardown reclaims it, and
	 * a premature cleanup could pull the mat out from under libuv. */
	{
		WSADATA wsa_data;
		(void)WSAStartup(MAKEWORD(2, 2), &wsa_data);
	}

	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
	int modes[] = { _CRT_WARN, _CRT_ERROR, _CRT_ASSERT };
	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		_CrtSetReportMode(modes[i], _CRTDBG_MODE_FILE);
		_CrtSetReportFile(modes[i], _CRTDBG_FILE_STDERR);
	}
#endif
}

/* TMPFS_MAGIC: uv_fs.c's probeDirectIO() treats this filesystem type as one
 * that does not support O_DIRECT and sets the direct-I/O block size to 0, i.e.
 * selects the portable buffered path. Reporting it here means that if the probe
 * ever reaches the fstatfs() switch, it draws the correct "no direct I/O"
 * conclusion instead of erroring out on an unknown filesystem. */
#define DQLITE_TMPFS_MAGIC 0x01021994L

/* Cap the free space reported by statvfs(). The raft DirFill() test helper
 * allocates a file spanning (reported_free - n) bytes to drive the filesystem
 * to a known near-full state. Reporting the true free space of a large Windows
 * volume would make that helper attempt to fill the entire physical disk, which
 * is destructive. Cap it to a modest value so the allocation stays bounded and
 * temporary. (The disk-fill tests that depend on actually exhausting the volume
 * will not trigger their ENOSPC path and will fail at runtime instead -- an
 * acceptable, non-destructive outcome for the port.) */
#define DQLITE_STATVFS_CAP_BYTES (64ULL * 1024ULL * 1024ULL)
#define DQLITE_STATVFS_BSIZE     4096UL

/* ------------------------------------------------------------------ fcntl */

int fcntl(int fd, int cmd, ...)
{
	(void)fd;
	(void)cmd;
	/* All descriptor-flag operations (F_GETFL/F_SETFL/F_GETFD/F_SETFD) are
	 * no-ops on Windows: non-blocking mode is handled elsewhere in the
	 * (deferred) socket port, and O_DIRECT is 0 so requesting direct I/O
	 * changes nothing. Returning 0 (success) keeps callers on the correct,
	 * buffered path. */
	return 0;
}

/* ----------------------------------------------------------- statfs family */

static int fillStatfs(struct statfs *buf)
{
	memset(buf, 0, sizeof *buf);
	buf->f_type = DQLITE_TMPFS_MAGIC;
	buf->f_bsize = (long)DQLITE_STATVFS_BSIZE;
	buf->f_blocks = (long)(DQLITE_STATVFS_CAP_BYTES / DQLITE_STATVFS_BSIZE);
	buf->f_bfree = buf->f_blocks;
	buf->f_bavail = buf->f_blocks;
	buf->f_namelen = 255;
	return 0;
}

int statfs(const char *path, struct statfs *buf)
{
	(void)path;
	return fillStatfs(buf);
}

int fstatfs(int fd, struct statfs *buf)
{
	(void)fd;
	return fillStatfs(buf);
}

/* ---------------------------------------------------------- statvfs family */

static void fillStatvfs(struct statvfs *buf, unsigned long long free_bytes)
{
	unsigned long long blocks;

	if (free_bytes > DQLITE_STATVFS_CAP_BYTES) {
		free_bytes = DQLITE_STATVFS_CAP_BYTES;
	}
	blocks = free_bytes / DQLITE_STATVFS_BSIZE;

	memset(buf, 0, sizeof *buf);
	buf->f_bsize = DQLITE_STATVFS_BSIZE;
	buf->f_frsize = DQLITE_STATVFS_BSIZE;
	buf->f_blocks = blocks;
	buf->f_bfree = blocks;
	buf->f_bavail = blocks;
	buf->f_namemax = 255;
}

int statvfs(const char *path, struct statvfs *buf)
{
	ULARGE_INTEGER free_avail;
	ULARGE_INTEGER total;
	ULARGE_INTEGER total_free;
	unsigned long long free_bytes = DQLITE_STATVFS_CAP_BYTES;

	if (GetDiskFreeSpaceExA(path, &free_avail, &total, &total_free)) {
		free_bytes = (unsigned long long)free_avail.QuadPart;
	}
	fillStatvfs(buf, free_bytes);
	return 0;
}

int fstatvfs(int fd, struct statvfs *buf)
{
	(void)fd;
	/* No path available from a bare fd; report the capped generic value. */
	fillStatvfs(buf, DQLITE_STATVFS_CAP_BYTES);
	return 0;
}

/* -------------------------------------------------------------------- nftw */

typedef int (*dqliteNftwFn)(const char *fpath,
			    const struct stat *sb,
			    int typeflag,
			    struct FTW *ftwbuf);

static int nftwWalk(const char *path, dqliteNftwFn fn, int flags, int level)
{
	struct stat st;
	struct FTW ftwbuf;
	const char *sep;
	DWORD attrs;
	int is_dir;
	int is_link;
	int descend;
	int typeflag;
	int rv;

	memset(&st, 0, sizeof st);
	(void)stat(path, &st); /* best-effort; callers ignore sb */

	attrs = GetFileAttributesA(path);
	is_dir = attrs != INVALID_FILE_ATTRIBUTES &&
		 (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
	is_link = attrs != INVALID_FILE_ATTRIBUTES &&
		  (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

	/* FTW_PHYS: do not follow symlinks/reparse points -- report them as
	 * symlinks and never descend into them. */
	descend = is_dir && !(is_link && (flags & FTW_PHYS));

	/* Basename offset for struct FTW (callers ignore it, but fill it). */
	sep = strrchr(path, '/');
	if (sep == NULL) {
		sep = strrchr(path, '\\');
	}
	ftwbuf.base = sep != NULL ? (int)(sep + 1 - path) : 0;
	ftwbuf.level = level;

	/* Pre-order: for a directory without FTW_DEPTH, visit it before its
	 * children. */
	if (is_dir && !(flags & FTW_DEPTH)) {
		rv = fn(path, &st, FTW_D, &ftwbuf);
		if (rv != 0) {
			return rv;
		}
	}

	if (descend) {
		char pattern[4096];
		WIN32_FIND_DATAA fd;
		HANDLE h;

		(void)snprintf(pattern, sizeof pattern, "%s/*", path);
		h = FindFirstFileA(pattern, &fd);
		if (h != INVALID_HANDLE_VALUE) {
			do {
				char child[4096];

				if (strcmp(fd.cFileName, ".") == 0 ||
				    strcmp(fd.cFileName, "..") == 0) {
					continue;
				}
				(void)snprintf(child, sizeof child, "%s/%s",
					       path, fd.cFileName);
				rv = nftwWalk(child, fn, flags, level + 1);
				if (rv != 0) {
					FindClose(h);
					return rv;
				}
			} while (FindNextFileA(h, &fd));
			FindClose(h);
		}
	}

	/* Determine the typeflag for this entry. */
	if (is_dir) {
		typeflag = (flags & FTW_DEPTH) ? FTW_DP : FTW_D;
	} else if (is_link && (flags & FTW_PHYS)) {
		typeflag = FTW_SL;
	} else {
		typeflag = FTW_F;
	}

	/* Post-order (FTW_DEPTH) or leaf: visit now. For a pre-order directory
	 * we already emitted FTW_D above, so skip the second visit. */
	if (is_dir && !(flags & FTW_DEPTH)) {
		return 0;
	}
	return fn(path, &st, typeflag, &ftwbuf);
}

int nftw(const char *dirpath,
	 int (*fn)(const char *fpath,
		   const struct stat *sb,
		   int typeflag,
		   struct FTW *ftwbuf),
	 int nopenfd,
	 int flags)
{
	(void)nopenfd;
	if (dirpath == NULL || fn == NULL) {
		errno = EINVAL;
		return -1;
	}
	return nftwWalk(dirpath, fn, flags, 0);
}

/* ------------------------------------------------------------------ uname */

int uname(struct utsname *buf)
{
	DWORD len;
	SYSTEM_INFO si;

	if (buf == NULL) {
		errno = EFAULT;
		return -1;
	}
	memset(buf, 0, sizeof *buf);

	(void)snprintf(buf->sysname, sizeof buf->sysname, "%s", "Windows");

	len = sizeof buf->nodename - 1;
	if (!GetComputerNameA(buf->nodename, &len)) {
		(void)snprintf(buf->nodename, sizeof buf->nodename, "%s",
			       "localhost");
	}

	/* release/version: a plausible, high version string. Callers compare it
	 * with strverscmp() against Linux kernel minimums, so any modern-looking
	 * dotted number compares "greater". */
	(void)snprintf(buf->release, sizeof buf->release, "%s", "10.0");
	(void)snprintf(buf->version, sizeof buf->version, "%s", "Windows");

	GetNativeSystemInfo(&si);
	switch (si.wProcessorArchitecture) {
		case PROCESSOR_ARCHITECTURE_AMD64:
			(void)snprintf(buf->machine, sizeof buf->machine, "%s",
				       "x86_64");
			break;
		case PROCESSOR_ARCHITECTURE_ARM64:
			(void)snprintf(buf->machine, sizeof buf->machine, "%s",
				       "aarch64");
			break;
		case PROCESSOR_ARCHITECTURE_INTEL:
			(void)snprintf(buf->machine, sizeof buf->machine, "%s",
				       "x86");
			break;
		default:
			(void)snprintf(buf->machine, sizeof buf->machine, "%s",
				       "unknown");
			break;
	}

	return 0;
}

/* ============================================================ threads / sync
 *
 * Minimal POSIX threading/synchronisation implemented on Win32 primitives, just
 * enough for the symbols src/server.{c,h}, src/lib/threadpool.c and the test
 * harness reference. Only the referenced surface is implemented.
 *
 * Type mapping (see compat/win/{pthread,semaphore}.h):
 *  - pthread_mutex_t { void *handle }  -> an SRWLOCK stored INLINE in `handle`.
 *    An SRWLOCK is a single pointer-sized opaque field, so the void* slot holds
 *    it directly; InitializeSRWLock simply zeroes it. Using SRWLOCK (not
 *    CRITICAL_SECTION) lets it pair with a CONDITION_VARIABLE.
 *  - pthread_cond_t  { void *handle }  -> a CONDITION_VARIABLE stored INLINE the
 *    same way, driven by SleepConditionVariableSRW / WakeConditionVariable.
 *  - sem_t { void *handle; long count } -> a Win32 semaphore HANDLE plus a
 *    self-maintained count for sem_getvalue().
 *
 * All functions follow the POSIX convention: return 0 on success, a positive
 * errno-style code on failure.
 */

/* ------------------------------------------------------------ pthread_mutex */

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
	(void)attr; /* default attributes only */
	InitializeSRWLock((PSRWLOCK)&mutex->handle);
	return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
	/* SRWLOCKs need no teardown. */
	(void)mutex;
	return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
	AcquireSRWLockExclusive((PSRWLOCK)&mutex->handle);
	return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
	ReleaseSRWLockExclusive((PSRWLOCK)&mutex->handle);
	return 0;
}

/* ------------------------------------------------------------- pthread_cond */

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
	(void)attr;
	InitializeConditionVariable((PCONDITION_VARIABLE)&cond->handle);
	return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
	/* CONDITION_VARIABLEs need no teardown. */
	(void)cond;
	return 0;
}

int pthread_cond_signal(pthread_cond_t *cond)
{
	WakeConditionVariable((PCONDITION_VARIABLE)&cond->handle);
	return 0;
}

/* pthread_cond_timedwait: abstime is an absolute CLOCK_REALTIME (Unix epoch)
 * deadline. Win32 wants a relative millisecond timeout, so convert against the
 * current wall-clock time (GetSystemTimeAsFileTime, also Unix-epoch based once
 * the 1601->1970 offset is removed). Returns 0 when signalled, ETIMEDOUT on
 * timeout, matching the callers' expectations in src/server.c. */
int pthread_cond_timedwait(pthread_cond_t *cond,
			   pthread_mutex_t *mutex,
			   const struct timespec *abstime)
{
	/* 100ns ticks between 1601-01-01 and 1970-01-01. */
	static const unsigned long long EPOCH_DELTA = 116444736000000000ULL;
	FILETIME ft;
	unsigned long long now_ms;
	unsigned long long abs_ms;
	DWORD wait_ms;

	GetSystemTimeAsFileTime(&ft);
	now_ms = ((unsigned long long)ft.dwHighDateTime << 32) |
		 ft.dwLowDateTime;
	now_ms = (now_ms - EPOCH_DELTA) / 10000ULL; /* 100ns -> ms */

	abs_ms = (unsigned long long)abstime->tv_sec * 1000ULL +
		 (unsigned long long)abstime->tv_nsec / 1000000ULL;

	wait_ms = abs_ms > now_ms ? (DWORD)(abs_ms - now_ms) : 0;

	if (SleepConditionVariableSRW((PCONDITION_VARIABLE)&cond->handle,
				      (PSRWLOCK)&mutex->handle, wait_ms, 0)) {
		return 0;
	}
	if (GetLastError() == ERROR_TIMEOUT) {
		return ETIMEDOUT;
	}
	return EINVAL;
}

/* ----------------------------------------------------------- pthread_create */

/* Heap-allocated control block bridging the POSIX start routine (void *(*)(void
 * *)) to the Win32 thread proc (unsigned __stdcall). Also holds the join handle
 * and the routine's return value so pthread_join can hand it back. The block's
 * address is what pthread_t carries. */
struct dqliteWinThread {
	HANDLE handle;
	void *(*start)(void *);
	void *arg;
	void *ret;
};

static unsigned __stdcall dqliteWinThreadTrampoline(void *p)
{
	struct dqliteWinThread *t = p;
	t->ret = t->start(t->arg);
	return 0;
}

int pthread_create(pthread_t *thread,
		   const pthread_attr_t *attr,
		   void *(*start_routine)(void *),
		   void *arg)
{
	struct dqliteWinThread *t;
	uintptr_t h;

	(void)attr; /* default attributes only */

	t = malloc(sizeof *t);
	if (t == NULL) {
		return ENOMEM;
	}
	t->start = start_routine;
	t->arg = arg;
	t->ret = NULL;

	h = _beginthreadex(NULL, 0, dqliteWinThreadTrampoline, t, 0, NULL);
	if (h == 0) {
		int rv = errno;
		free(t);
		return rv != 0 ? rv : EAGAIN;
	}
	t->handle = (HANDLE)h;
	*thread = (pthread_t)(uintptr_t)t;
	return 0;
}

int pthread_join(pthread_t thread, void **retval)
{
	struct dqliteWinThread *t = (struct dqliteWinThread *)(uintptr_t)thread;

	if (t == NULL) {
		return EINVAL;
	}
	WaitForSingleObject(t->handle, INFINITE);
	if (retval != NULL) {
		*retval = t->ret;
	}
	CloseHandle(t->handle);
	free(t);
	return 0;
}

/* ---------------------------------------------------------------- semaphore */

int sem_init(sem_t *sem, int pshared, unsigned int value)
{
	(void)pshared; /* no cross-process semaphores needed */
	sem->handle = CreateSemaphoreA(NULL, (LONG)value, LONG_MAX, NULL);
	if (sem->handle == NULL) {
		errno = ENOSPC;
		return -1;
	}
	sem->count = (long)value;
	return 0;
}

int sem_destroy(sem_t *sem)
{
	if (sem->handle != NULL) {
		CloseHandle((HANDLE)sem->handle);
		sem->handle = NULL;
	}
	return 0;
}

int sem_post(sem_t *sem)
{
	/* Bump the mirror before releasing so a woken waiter never observes a
	 * count that is too low. */
	InterlockedIncrement(&sem->count);
	if (!ReleaseSemaphore((HANDLE)sem->handle, 1, NULL)) {
		InterlockedDecrement(&sem->count);
		errno = EOVERFLOW;
		return -1;
	}
	return 0;
}

int sem_wait(sem_t *sem)
{
	if (WaitForSingleObject((HANDLE)sem->handle, INFINITE) != WAIT_OBJECT_0) {
		errno = EINVAL;
		return -1;
	}
	InterlockedDecrement(&sem->count);
	return 0;
}

/* sem_getvalue: Win32 exposes no way to read a semaphore's count, so return the
 * mirror we maintain. POSIX allows returning 0 (rather than a negative waiter
 * count) when threads are blocked, which is exactly what the mirror yields. */
int sem_getvalue(sem_t *sem, int *sval)
{
	*sval = (int)sem->count;
	return 0;
}

/* -------------------------------------------------------------------- flock */

/* flock(fd, op): advisory whole-file lock via LockFileEx/UnlockFileEx on the
 * underlying OS handle. Honours LOCK_SH/LOCK_EX (shared/exclusive), LOCK_NB
 * (non-blocking) and LOCK_UN (release). src/server.c's acquire_dir only uses
 * LOCK_EX|LOCK_NB; the rest are supported for completeness. Returns 0 / -1+errno
 * like POSIX. Locks are released automatically when the fd/handle is closed. */
int flock(int fd, int operation)
{
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	OVERLAPPED ov;
	DWORD flags = 0;

	if (h == INVALID_HANDLE_VALUE) {
		errno = EBADF;
		return -1;
	}

	memset(&ov, 0, sizeof ov);

	if (operation & LOCK_UN) {
		if (!UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov)) {
			errno = EINVAL;
			return -1;
		}
		return 0;
	}

	if (operation & LOCK_EX) {
		flags |= LOCKFILE_EXCLUSIVE_LOCK;
	}
	/* LOCK_SH -> shared lock: LockFileEx defaults to shared when the
	 * exclusive flag is absent, so nothing extra to set. */
	if (operation & LOCK_NB) {
		flags |= LOCKFILE_FAIL_IMMEDIATELY;
	}

	if (!LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &ov)) {
		/* A non-blocking request that would block reports EWOULDBLOCK,
		 * matching POSIX flock semantics. */
		errno = (operation & LOCK_NB) ? EWOULDBLOCK : EINVAL;
		return -1;
	}
	return 0;
}

/* ================================================= mmap / memfd (vfs.c WAL shm)
 *
 * src/vfs.c backs the SQLite WAL-index shared memory with an in-memory file
 * (memfd_create) that it mmaps at 64KiB-aligned offsets, and it replaces those
 * mappings in place (MAP_FIXED) to switch a region between a shared view and a
 * private copy-on-write view (see vfsRedirectShm / vfsPublishShm /
 * vfsRollbackShm). Windows has no mremap, so vfs.c already selects the portable
 * "unmap + map-at-fixed-address" path (MREMAP_MAYMOVE is left undefined in
 * compat/win/sys/mman.h). This is the Win32 backing for that path.
 *
 * Alignment: MapViewOfFileEx requires the target base address AND the file
 * offset to both be multiples of the allocation granularity (64KiB). vfs.c
 * derives its mapping size and offsets from sysconf(_SC_PAGESIZE), which the
 * compat layer reports as dwAllocationGranularity (64KiB) -- see
 * compat/win/unistd.h. Consequently every mmap offset is 64KiB-aligned, and
 * every MAP_FIXED target is a base returned by an earlier mmap (which
 * MapViewOfFile always granularity-aligns), so MapViewOfFileEx can honour them.
 *
 * The section is created per-call from the file's current size (dwMaximumSize
 * 0). vfs.c ftruncate()s the file large enough before each mmap, and the
 * compat ftruncate() grows even a mapped file (see unistd.h). Closing the
 * section handle immediately is safe: an active view keeps the section alive. */

/* Serializes all mmap()/munmap() address-space transitions in the process. See
 * the detailed rationale in mmap(). SRWLOCK_INIT is a valid static initialiser,
 * so no runtime setup is needed. */
static SRWLOCK dqliteMmapLock = SRWLOCK_INIT;

/* Windows 10 (1803+, RS4) added placeholder virtual-address reservations, which
 * let a file view be swapped in and out of an address range WITHOUT ever
 * releasing that range -- the atomic-replace primitive dqlite's WAL-index shm
 * COW scheme needs (it is what makes Linux MAP_FIXED / mremap(MREMAP_FIXED)
 * safe). We resolve them dynamically from kernelbase.dll: when present, mmap()
 * uses them so a MAP_FIXED replacement never exposes a free window; when absent
 * (pre-1803), we fall back to the best-effort unmap-then-remap under the lock.
 *
 * Constants (winnt.h): MEM_RESERVE_PLACEHOLDER 0x40000, MEM_REPLACE_PLACEHOLDER
 * 0x4000, MEM_PRESERVE_PLACEHOLDER 0x2. */
typedef PVOID(WINAPI *dqliteVirtualAlloc2Fn)(HANDLE,
					     PVOID,
					     SIZE_T,
					     ULONG,
					     ULONG,
					     void *,
					     ULONG);
typedef PVOID(WINAPI *dqliteMapViewOfFile3Fn)(HANDLE,
					      HANDLE,
					      PVOID,
					      ULONG64,
					      SIZE_T,
					      ULONG,
					      ULONG,
					      void *,
					      ULONG);
typedef BOOL(WINAPI *dqliteUnmapViewOfFile2Fn)(HANDLE, PVOID, ULONG);

static dqliteVirtualAlloc2Fn dqliteVirtualAlloc2;
static dqliteMapViewOfFile3Fn dqliteMapViewOfFile3;
static dqliteUnmapViewOfFile2Fn dqliteUnmapViewOfFile2;
static INIT_ONCE dqlitePlaceholderOnce = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK dqliteResolvePlaceholderApis(PINIT_ONCE once,
						  PVOID param,
						  PVOID *ctx)
{
	(void)once;
	(void)param;
	(void)ctx;
	HMODULE kb = GetModuleHandleA("kernelbase.dll");
	if (kb == NULL) {
		return TRUE;
	}
	dqliteVirtualAlloc2 =
	    (dqliteVirtualAlloc2Fn)(void *)GetProcAddress(kb, "VirtualAlloc2");
	dqliteMapViewOfFile3 =
	    (dqliteMapViewOfFile3Fn)(void *)GetProcAddress(kb, "MapViewOfFile3");
	dqliteUnmapViewOfFile2 = (dqliteUnmapViewOfFile2Fn)(void *)GetProcAddress(
	    kb, "UnmapViewOfFile2");
	return TRUE;
}

static int dqliteHavePlaceholders(void)
{
	InitOnceExecuteOnce(&dqlitePlaceholderOnce,
			    dqliteResolvePlaceholderApis, NULL, NULL);
	return dqliteVirtualAlloc2 != NULL && dqliteMapViewOfFile3 != NULL &&
	       dqliteUnmapViewOfFile2 != NULL;
}

#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif

/* memfd_create: an anonymous, in-memory-ish file. Backed by a uniquely named
 * temp file opened FILE_FLAG_DELETE_ON_CLOSE (so it disappears when the CRT fd
 * is closed) and FILE_ATTRIBUTE_TEMPORARY (so the OS keeps it in cache and
 * avoids flushing to disk where possible). Returns a CRT fd via
 * _open_osfhandle so callers can ftruncate()/mmap()/close() it as a normal fd. */
int memfd_create(const char *name, unsigned int flags)
{
	char dir[MAX_PATH];
	char path[MAX_PATH];
	DWORD n;
	HANDLE h;
	int fd;

	(void)name;  /* debugging aid only on Linux */
	(void)flags; /* MFD_CLOEXEC: fds are non-inherited by default on Windows */

	n = GetTempPathA((DWORD)sizeof dir, dir);
	if (n == 0 || n > sizeof dir) {
		errno = EIO;
		return -1;
	}
	if (GetTempFileNameA(dir, "dqs", 0, path) == 0) {
		errno = EIO;
		return -1;
	}
	/* GetTempFileNameA created (and closed) a 0-byte file with a unique name;
	 * reopen it with delete-on-close semantics. */
	h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS,
			FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
			NULL);
	if (h == INVALID_HANDLE_VALUE) {
		errno = EIO;
		return -1;
	}
	fd = _open_osfhandle((intptr_t)h, _O_RDWR | _O_BINARY);
	if (fd < 0) {
		CloseHandle(h);
		errno = EMFILE;
		return -1;
	}
	return fd;
}

void *mmap(void *addr,
	   size_t length,
	   int prot,
	   int flags,
	   int fd,
	   long long offset)
{
	HANDLE hFile;
	HANDLE hMap;
	DWORD protect;
	DWORD access;
	DWORD offHigh;
	DWORD offLow;
	void *base;

	(void)prot; /* vfs.c always maps PROT_READ|PROT_WRITE */

	hFile = (HANDLE)_get_osfhandle(fd);
	if (hFile == INVALID_HANDLE_VALUE || hFile == NULL) {
		errno = EBADF;
		return MAP_FAILED;
	}

	if (flags & MAP_PRIVATE) {
		/* copy-on-write: writes stay private to this view */
		protect = PAGE_WRITECOPY;
		access = FILE_MAP_COPY;
	} else { /* MAP_SHARED */
		protect = PAGE_READWRITE;
		access = FILE_MAP_READ | FILE_MAP_WRITE;
	}

	/* dwMaximumSize 0 => the file's current size, which vfs.c has already
	 * grown (ftruncate) to cover offset+length before calling us. */
	hMap = CreateFileMappingW(hFile, NULL, protect, 0, 0, NULL);
	if (hMap == NULL) {
		errno = ENOMEM;
		return MAP_FAILED;
	}

	(void)access; /* MapViewOfFile3 takes PAGE_* protection directly */
	offHigh = (DWORD)((unsigned long long)offset >> 32);
	offLow = (DWORD)((unsigned long long)offset & 0xFFFFFFFFu);
	(void)offHigh;
	(void)offLow;

	/* dqlite drives the WAL-index shm mappings (vfs.c) from multiple
	 * threadpool worker threads concurrently -- one per database -- and its
	 * COW scheme repeatedly replaces a view at a fixed address in place
	 * (MAP_FIXED) to toggle a region between shared and private. On Linux
	 * that is mremap(MREMAP_FIXED): an atomic replace that never releases the
	 * address. Windows has no such primitive for a plain MapViewOfFileEx --
	 * the address must be UnmapViewOfFile'd first, and during that window ANY
	 * other allocation in the process (another worker's mmap(NULL) scratch,
	 * the CRT/libuv heap via VirtualAlloc, a thread stack) can be handed that
	 * exact address by the kernel. The subsequent MapViewOfFileEx then cannot
	 * reclaim it, so two databases' shm views alias and the WAL index is
	 * corrupted (manifesting as spurious SQLITE_NOMEM / SQLITE_ERROR under
	 * concurrency).
	 *
	 * The fix is the Win10 1803+ placeholder API: a placeholder keeps the
	 * virtual-address range RESERVED to us while no view occupies it, so a
	 * MAP_FIXED replace (UnmapViewOfFile2(...PRESERVE) then
	 * MapViewOfFile3(...REPLACE)) never exposes a free window -- matching the
	 * Linux atomic-replace semantics exactly. The lock still serialises the
	 * multi-step sequences for good measure. If the placeholder API is
	 * unavailable (pre-1803), fall back to the best-effort unmap+remap. */
	AcquireSRWLockExclusive(&dqliteMmapLock);

	if (dqliteHavePlaceholders()) {
		void *target = addr;
		if ((flags & MAP_FIXED) && addr != NULL) {
			/* Turn the existing view back into a placeholder,
			 * keeping the address reserved to us. */
			dqliteUnmapViewOfFile2(GetCurrentProcess(), addr,
					       MEM_PRESERVE_PLACEHOLDER);
		} else {
			/* Reserve a fresh placeholder range; the kernel picks a
			 * free address and it stays ours until we release it. */
			target = dqliteVirtualAlloc2(
			    NULL, NULL, length,
			    MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS,
			    NULL, 0);
			if (target == NULL) {
				ReleaseSRWLockExclusive(&dqliteMmapLock);
				CloseHandle(hMap);
				errno = ENOMEM;
				return MAP_FAILED;
			}
		}
		base = dqliteMapViewOfFile3(hMap, GetCurrentProcess(), target,
					    (ULONG64)offset, length,
					    MEM_REPLACE_PLACEHOLDER, protect,
					    NULL, 0);
		if (base == NULL && !(flags & MAP_FIXED)) {
			/* Mapping failed: release the placeholder we reserved. */
			VirtualFree(target, 0, MEM_RELEASE);
		}
	} else {
		if ((flags & MAP_FIXED) && addr != NULL) {
			UnmapViewOfFile(addr);
		}
		base = MapViewOfFileEx(hMap, access, offHigh, offLow, length,
				       (flags & MAP_FIXED) ? addr : NULL);
	}

	ReleaseSRWLockExclusive(&dqliteMmapLock);

	/* The view holds its own reference to the section, so the handle can be
	 * closed now; the mapping stays valid until UnmapViewOfFile. */
	CloseHandle(hMap);

	if (base == NULL) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	return base;
}

int munmap(void *addr, size_t length)
{
	BOOL ok;
	(void)length; /* a view is unmapped whole, by its base address */
	AcquireSRWLockExclusive(&dqliteMmapLock);
	if (dqliteHavePlaceholders()) {
		/* Unmap the view but keep the placeholder, then release the
		 * reserved range so the address is fully returned to the OS. */
		ok = dqliteUnmapViewOfFile2(GetCurrentProcess(), addr,
					    MEM_PRESERVE_PLACEHOLDER);
		if (ok) {
			VirtualFree(addr, 0, MEM_RELEASE);
		}
	} else {
		ok = UnmapViewOfFile(addr);
	}
	ReleaseSRWLockExclusive(&dqliteMmapLock);
	if (!ok) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

int msync(void *addr, size_t length, int flags)
{
	(void)flags;
	if (!FlushViewOfFile(addr, length)) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

#endif /* _WIN32 */
