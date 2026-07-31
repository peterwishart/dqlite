/*
 * dqlite Windows port -- compat_win.c
 *
 * Windows-only implementations of the POSIX functions that the raft/dqlite
 * sources call but that Win32/UCRT does not provide. Compiled ONLY on Windows
 * (guarded on _WIN32 and wired into the build via an if(WIN32) static library
 * `dqlite_win_compat` -- see CMakeLists.txt). No non-Windows build sees this
 * file. Prototypes match the shim headers in compat/win/ (ftw.h, sys/vfs.h,
 * sys/statvfs.h) and the fcntl() declaration in the forced prelude.
 *
 * Behavioural summary:
 *  - dqliteWinSocketsInit(): idempotent process-wide WSAStartup, called from
 *               the library's public entry points (see the function comment).
 *  - fcntl():   implements NO command -- always fails with ENOSYS. See the
 *               function comment; no Windows-compiled code calls it.
 *  - dqliteWinNanosleep(): sub-millisecond sleep on a high-resolution
 *               waitable timer, backing the nanosleep() wrapper in unistd.h
 *               (vfs.c's WAL-contention backoff needs microsecond sleeps).
 *  - pread()/pwrite(): positional I/O via ReadFile/WriteFile with an
 *               OVERLAPPED offset; does not disturb the fd's file position.
 *  - statfs()/fstatfs(): report a generic, tmpfs-like filesystem so the raft
 *               direct-I/O probe (uv_fs.c) concludes "not a direct-I/O capable
 *               fs" and falls back to the portable buffered write path.
 *  - statvfs()/fstatvfs(): report free space (queried via GetDiskFreeSpaceEx,
 *               but CAPPED -- see below) for the test disk-fill helper.
 *  - nftw():    a real recursive directory walk honouring FTW_DEPTH (post-order)
 *               and FTW_PHYS (don't descend into reparse points), used by the
 *               test teardown to remove a temp tree.
 */

#ifdef _WIN32

#include <windows.h>

#include <errno.h>
#include <limits.h> /* INT_MAX (pread/pwrite transfer cap) */
#include <stdint.h> /* uintptr_t */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <fcntl.h>  /* _O_RDWR, _O_BINARY (for _open_osfhandle) */
#include <io.h>     /* _get_osfhandle, _open_osfhandle */
#include <unistd.h> /* pread/pwrite/dqliteWinNanosleep prototypes (compat shim) */

#include <sys/mman.h> /* PROT_*, MAP_*, mmap/munmap/msync/memfd_create protos */

#include <ftw.h>
#include <sys/file.h>
#include <sys/statvfs.h>
#include <sys/vfs.h>

/* --------------------------------------------------------------- Winsock */

/* dqliteWinSocketsInit(): idempotent, process-wide Winsock initialisation.
 *
 * dqlite makes raw socket()/getaddrinfo()/connect() calls (src/transport.c,
 * src/lib/addr.c, src/client/protocol.c) that run independently of libuv's
 * own internal WSAStartup, and getaddrinfo() in particular fails with
 * WSANOTINITIALISED if Winsock has not been started. The library's public
 * object-creation entry points -- dqlite_node_create and
 * dqlite_server_create, through one of which every socket-using code path in
 * the library is reached -- call this helper (declared in the forced prelude,
 * so the guarded call sites in src/server.c need no extra include), making
 * Winsock initialisation STRUCTURAL rather than a side effect of linking: it
 * is NOT a library constructor, so it cannot depend on whether the linker
 * happens to pull this object out of the static archive.
 *
 * One-shot rather than refcounted: WSAStartup runs exactly once per process
 * (INIT_ONCE) and the matching WSACleanup is intentionally omitted. Process
 * teardown reclaims Winsock anyway, libuv handles its own reference the same
 * way (uv__once_init never cleans up either), and a cleanup tied to object
 * destruction could pull the mat out from under libuv or the host
 * application's own sockets. Requesting Winsock 2.2. */

static BOOL CALLBACK dqliteWinSocketsInitOnce(PINIT_ONCE once,
					      PVOID param,
					      PVOID *ctx)
{
	WSADATA wsa_data;
	(void)once;
	(void)param;
	(void)ctx;
	(void)WSAStartup(MAKEWORD(2, 2), &wsa_data);
	return TRUE;
}

void dqliteWinSocketsInit(void)
{
	static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
	InitOnceExecuteOnce(&once, dqliteWinSocketsInitOnce, NULL, NULL);
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

/* Honest stub: implements NO fcntl command -- every call fails with ENOSYS.
 * No Windows-compiled code calls it; the definition exists only to back the
 * prelude's declaration. A future caller must implement its command's real
 * semantics here rather than widening the success path. */
int fcntl(int fd, int cmd, ...)
{
	(void)fd;
	(void)cmd;
	errno = ENOSYS;
	return -1;
}

/* -------------------------------------------------------------- nanosleep */

/* dqliteWinNanosleep(): the real function behind the nanosleep() wrapper in
 * compat/win/unistd.h.
 *
 * The production caller is vfsSleep (src/vfs.c, SQLite's xSleep), which
 * SQLite's WAL code calls with MICROSECOND durations (walTryBeginRead backs
 * off 1us..~350us per retry while the WAL-index locks are contended). Plain
 * Sleep() has millisecond granularity, so the previous ms-truncating shim
 * turned every sub-ms request into Sleep(0): a scheduler yield, i.e. the
 * "backoff" busy-spun under exactly the contention it was meant to relieve.
 *
 * Windows 10 1803+ provides high-resolution waitable timers
 * (CreateWaitableTimerExW with CREATE_WAITABLE_TIMER_HIGH_RESOLUTION), which
 * honour the requested 100ns-unit due time instead of rounding it up to the
 * scheduler tick (15.6ms by default, 1ms at best via timeBeginPeriod). This is
 * the same Windows 10 1803 floor as the placeholder mapping APIs that mmap()
 * below already REQUIRES, so the flag is expected to be supported everywhere
 * dqlite runs; should it not be (CreateWaitableTimerExW returns NULL), we
 * retry without the flag and remember the downgrade, degrading to tick-quantum
 * granularity -- a sleep of at least one scheduler tick, never a spin.
 * If even the plain timer cannot be created, fall back to Sleep() with a
 * documented 1ms minimum so a failed allocation still never yields a spin.
 *
 * The timer handle is created and closed PER CALL rather than cached
 * per-thread. Measured on this port's dev machine, the
 * create+set+wait+close cycle costs ~4-6us per call, which is (a) well under
 * the ~0.5ms floor the high-resolution timer can actually deliver, so caching
 * cannot improve the achieved sleep accuracy, and (b) negligible against even
 * the smallest sub-ms sleep it enables. A thread-local cache would save those
 * microseconds but leak one timer handle per exited thread (libuv threadpool
 * workers, munit's forked children) unless paired with FlsAlloc destructor
 * machinery -- complexity with no measurable benefit on this path. */
int dqliteWinNanosleep(long long sec, long long nsec)
{
	/* Total duration in 100ns units, rounding any nonzero sub-100ns
	 * remainder UP so a tiny-but-nonzero request still waits. */
	long long units = sec * 10000000LL + (nsec + 99) / 100;
	HANDLE timer;
	LARGE_INTEGER due;
	/* 0: undetermined; 1: high-resolution flag supported; -1: not. Races
	 * between threads are benign (both sides write the same conclusion). */
	static LONG hires_state;

	if (units <= 0) {
		return 0;
	}

	timer = NULL;
	if (hires_state >= 0) {
		timer = CreateWaitableTimerExW(
		    NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
		    TIMER_ALL_ACCESS);
		if (timer != NULL) {
			hires_state = 1;
		} else if (GetLastError() == ERROR_INVALID_PARAMETER) {
			/* Pre-1803 kernel: the flag is unknown. Remember, so
			 * later calls skip the doomed attempt. */
			hires_state = -1;
		}
	}
	if (timer == NULL) {
		timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
	}
	if (timer == NULL) {
		/* Out of handles/memory: degrade to Sleep(), which cannot
		 * express sub-ms durations -- round UP to a 1ms minimum so
		 * this fallback still sleeps rather than spins. */
		Sleep((DWORD)((units + 9999) / 10000));
		return 0;
	}

	due.QuadPart = -units; /* negative = relative time, 100ns units */
	if (!SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE) ||
	    WaitForSingleObject(timer, INFINITE) != WAIT_OBJECT_0) {
		CloseHandle(timer);
		Sleep((DWORD)((units + 9999) / 10000)); /* as above */
		return 0;
	}
	CloseHandle(timer);
	return 0;
}

/* ----------------------------------------------------------- pread/pwrite */

/* Positional I/O on a CRT fd via ReadFile/WriteFile with an OVERLAPPED
 * offset: the offset travels IN the call, so the transfer neither depends on
 * nor mutates the fd's shared file position. Current callers are all
 * single-threaded per fd anyway (verified by auditing every call site).
 *
 * One Windows wrinkle: on a handle opened for synchronous I/O (all CRT fds),
 * ReadFile/WriteFile with an OVERLAPPED offset still ADVANCES the handle's
 * file position after the transfer. POSIX pread/pwrite must not move it, so
 * we save and restore the position around the call (the same approach libuv's
 * uv_fs_read takes). The save/restore itself would race a concurrent same-fd
 * seek/read -- true POSIX offset-invisibility for multithreaded fd sharing
 * would need FILE_FLAG_OVERLAPPED handles end to end -- but the transfer
 * itself is positional either way.
 *
 * Note these are raw-byte transfers: no CRT text-mode CRLF translation is
 * applied regardless of the fd's _O_TEXT/_O_BINARY mode (dqlite opens all
 * pread/pwrite'd files binary; raw bytes are what its callers want). */

/* Map a GetLastError() I/O failure onto the errno the POSIX callers test.
 * ENOSPC matters: the raft fallocate emulation (src/raft/uv_os.c) reports
 * -errno upward and its callers key on UV_ENOSPC for disk-full handling. */
static int dqliteWinIoErrno(DWORD err)
{
	switch (err) {
		case ERROR_INVALID_HANDLE:
			return EBADF;
		case ERROR_ACCESS_DENIED:
			return EACCES;
		case ERROR_DISK_FULL:
		case ERROR_HANDLE_DISK_FULL:
			return ENOSPC;
		case ERROR_NOT_ENOUGH_MEMORY:
			return ENOMEM;
		default:
			return EIO;
	}
}

static ssize_t dqlitePositionalIo(int fd,
				  void *buf,
				  size_t count,
				  long long offset,
				  int is_read)
{
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	OVERLAPPED ov;
	LARGE_INTEGER zero;
	LARGE_INTEGER saved;
	BOOL restore;
	DWORD transferred = 0;
	BOOL ok;
	DWORD err;

	if (h == INVALID_HANDLE_VALUE || h == NULL) {
		errno = EBADF;
		return -1;
	}
	if (offset < 0) {
		errno = EINVAL;
		return -1;
	}
	if (count > (size_t)INT_MAX) {
		/* POSIX allows a short transfer; keep the result well inside
		 * ssize_t (matches the old _read()-based unsigned-int cap). */
		count = (size_t)INT_MAX;
	}

	/* Save the file position: on a synchronous handle a positional
	 * ReadFile/WriteFile still advances it (see the comment above).
	 * Failure (e.g. a non-seekable handle) is not an error for the
	 * transfer itself; we just cannot restore afterwards. */
	zero.QuadPart = 0;
	restore = SetFilePointerEx(h, zero, &saved, FILE_CURRENT);

	memset(&ov, 0, sizeof ov);
	ov.Offset = (DWORD)((unsigned long long)offset & 0xFFFFFFFFu);
	ov.OffsetHigh = (DWORD)((unsigned long long)offset >> 32);
	if (is_read) {
		ok = ReadFile(h, buf, (DWORD)count, &transferred, &ov);
	} else {
		ok = WriteFile(h, buf, (DWORD)count, &transferred, &ov);
	}
	err = ok ? 0 : GetLastError();

	if (restore) {
		(void)SetFilePointerEx(h, saved, NULL, FILE_BEGIN);
	}

	if (!ok) {
		if (is_read && err == ERROR_HANDLE_EOF) {
			/* Reading at or past end-of-file: POSIX pread
			 * reports 0, not an error. */
			return 0;
		}
		errno = dqliteWinIoErrno(err);
		return -1;
	}
	return (ssize_t)transferred;
}

ssize_t pread(int fd, void *buf, size_t count, long long offset)
{
	return dqlitePositionalIo(fd, buf, count, offset, 1);
}

ssize_t pwrite(int fd, const void *buf, size_t count, long long offset)
{
	/* Casting away const is safe: the is_read=0 path only passes buf to
	 * WriteFile, which does not modify the source buffer. */
	return dqlitePositionalIo(fd, (void *)(uintptr_t)buf, count, offset, 0);
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
 * Alignment: MapViewOfFile3 requires the target base address AND the file
 * offset to both be multiples of the allocation granularity (64KiB). vfs.c's
 * vfsGetMapSize() derives its mapping size and offsets on Windows from
 * dqlite_win_allocation_granularity() (compat/win/unistd.h). Consequently
 * every mmap offset is 64KiB-aligned, and every MAP_FIXED target is a base
 * returned by an earlier mmap (which the mapping APIs always
 * granularity-align), so the mapping calls can honour them.
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
 * safe). We resolve them dynamically from kernelbase.dll and REQUIRE them:
 * dqlite already assumes Windows 10 1803+ elsewhere (high-resolution waitable
 * timers, AF_UNIX sockets), and any unmap-then-remap fallback has an inherent
 * race -- between UnmapViewOfFile() and MapViewOfFileEx() any other allocation
 * in the process can steal the address, silently aliasing two databases' shm
 * views. On pre-1803 systems mmap() fails cleanly with a diagnostic instead.
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

/* Test-only fault injection for the MAP_FIXED replacement path in mmap(). When
 * the DQLITE_WIN_MMAP_FIXED_FAIL_AT environment variable is set to a positive
 * integer N, the N-th MAP_FIXED view replacement in the process behaves as if
 * MapViewOfFile3() had failed, exercising the recovery path in mmap() and the
 * error propagation in src/vfs.c. Inert (a single cached getenv) when the
 * variable is unset. Must be called with dqliteMmapLock held (which is why a
 * plain static suffices). */
static long dqliteMmapFixedFailAt = -2; /* -2: env unread; -1: disabled */

static int dqliteMmapFixedFaultInjected(void)
{
	if (dqliteMmapFixedFailAt == -2) {
		const char *env = getenv("DQLITE_WIN_MMAP_FIXED_FAIL_AT");
		long v = (env != NULL) ? atol(env) : 0;
		dqliteMmapFixedFailAt = v > 0 ? v : -1;
	}
	if (dqliteMmapFixedFailAt > 0 && --dqliteMmapFixedFailAt == 0) {
		dqliteMmapFixedFailAt = -1;
		fprintf(stderr,
			"dqlite: mmap: injecting MAP_FIXED failure "
			"(DQLITE_WIN_MMAP_FIXED_FAIL_AT)\n");
		SetLastError(ERROR_COMMITMENT_LIMIT);
		return 1;
	}
	return 0;
}

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
	void *base;

	(void)prot; /* vfs.c always maps PROT_READ|PROT_WRITE */

	/* Hard requirement: the Win10 1803+ placeholder APIs. See the comment
	 * at dqliteResolvePlaceholderApis() -- without them a MAP_FIXED replace
	 * cannot be done without a use-after-free-style address race, so we
	 * refuse to map at all rather than run subtly corrupted. */
	if (!dqliteHavePlaceholders()) {
		static LONG diagnosed;
		if (InterlockedExchange(&diagnosed, 1) == 0) {
			fprintf(stderr,
				"dqlite: mmap: this Windows version lacks the "
				"placeholder mapping APIs (VirtualAlloc2/"
				"MapViewOfFile3/UnmapViewOfFile2); dqlite "
				"requires Windows 10 1803 or later\n");
		}
		errno = ENOSYS;
		return MAP_FAILED;
	}

	hFile = (HANDLE)_get_osfhandle(fd);
	if (hFile == INVALID_HANDLE_VALUE || hFile == NULL) {
		errno = EBADF;
		return MAP_FAILED;
	}

	if (flags & MAP_PRIVATE) {
		/* copy-on-write: writes stay private to this view */
		protect = PAGE_WRITECOPY;
	} else { /* MAP_SHARED */
		protect = PAGE_READWRITE;
	}

	/* dwMaximumSize 0 => the file's current size, which vfs.c has already
	 * grown (ftruncate) to cover offset+length before calling us. */
	hMap = CreateFileMappingW(hFile, NULL, protect, 0, 0, NULL);
	if (hMap == NULL) {
		errno = ENOMEM;
		return MAP_FAILED;
	}

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
	 * multi-step sequences for good measure. The placeholder API is a hard
	 * requirement (checked above); there is no pre-1803 fallback. */
	AcquireSRWLockExclusive(&dqliteMmapLock);

	int fixed = (flags & MAP_FIXED) && addr != NULL;
	void *target = addr;
	if (fixed) {
		/* Turn the existing view back into a placeholder, keeping the
		 * address reserved to us. */
		if (!dqliteUnmapViewOfFile2(GetCurrentProcess(), addr,
					    MEM_PRESERVE_PLACEHOLDER)) {
			/* The old view is still intact at addr, so the caller's
			 * pointers stay valid -- same defined state as a failed
			 * MAP_FIXED mmap on Linux, which leaves the previous
			 * mapping untouched. */
			DWORD err = GetLastError();
			ReleaseSRWLockExclusive(&dqliteMmapLock);
			CloseHandle(hMap);
			fprintf(stderr,
				"dqlite: mmap: UnmapViewOfFile2(%p) failed "
				"(GetLastError=%lu)\n",
				addr, err);
			errno = ENOMEM;
			return MAP_FAILED;
		}
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
	if (fixed && dqliteMmapFixedFaultInjected()) {
		base = NULL; /* test-only: simulate MapViewOfFile3 failure */
	} else {
		base = dqliteMapViewOfFile3(hMap, GetCurrentProcess(), target,
					    (ULONG64)offset, length,
					    MEM_REPLACE_PLACEHOLDER, protect,
					    NULL, 0);
	}
	if (base == NULL) {
		DWORD err = GetLastError();
		if (fixed) {
			/* CRITICAL recovery: the old view is gone and the range
			 * is now a PAGE_NOACCESS placeholder, but the caller
			 * (SQLite via vfs.c) still holds pointers into it and
			 * expects mmap(MAP_FIXED) failure to leave a usable
			 * mapping behind (as on Linux). Restore a view of the
			 * same section/offset at the address -- first with the
			 * requested protection, then (for a failed copy-on-write
			 * request, which needs extra commit charge) with plain
			 * PAGE_READWRITE -- so the process stays in a defined
			 * state. mmap() still reports failure either way. */
			void *restored = dqliteMapViewOfFile3(
			    hMap, GetCurrentProcess(), target, (ULONG64)offset,
			    length, MEM_REPLACE_PLACEHOLDER, protect, NULL, 0);
			if (restored == NULL && protect != PAGE_READWRITE) {
				restored = dqliteMapViewOfFile3(
				    hMap, GetCurrentProcess(), target,
				    (ULONG64)offset, length,
				    MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE,
				    NULL, 0);
			}
			if (restored == NULL) {
				/* Unrecoverable: the range stays a reserved
				 * PAGE_NOACCESS placeholder (still owned by us,
				 * so at least it cannot be reallocated and
				 * aliased); any access to it will fault.
				 * Signal it distinctly and loudly. */
				fprintf(stderr,
					"dqlite: mmap: MAP_FIXED replacement at "
					"%p failed (GetLastError=%lu) and the "
					"view could not be restored; the range "
					"is no longer accessible\n",
					target, err);
				errno = ENOTRECOVERABLE;
			} else {
				errno = ENOMEM;
			}
		} else {
			/* Mapping failed: release the placeholder we reserved. */
			VirtualFree(target, 0, MEM_RELEASE);
			errno = ENOMEM;
		}
	}

	ReleaseSRWLockExclusive(&dqliteMmapLock);

	/* The view holds its own reference to the section, so the handle can be
	 * closed now; the mapping stays valid until UnmapViewOfFile. */
	CloseHandle(hMap);

	if (base == NULL) {
		return MAP_FAILED; /* errno set above */
	}
	return base;
}

int munmap(void *addr, size_t length)
{
	BOOL ok;
	(void)length; /* a view is unmapped whole, by its base address */
	if (!dqliteHavePlaceholders()) {
		/* mmap() can never succeed without the placeholder APIs (hard
		 * 1803+ requirement), so there is no view to unmap. */
		errno = ENOSYS;
		return -1;
	}
	AcquireSRWLockExclusive(&dqliteMmapLock);
	/* Unmap the view but keep the placeholder, then release the
	 * reserved range so the address is fully returned to the OS. */
	ok = dqliteUnmapViewOfFile2(GetCurrentProcess(), addr,
				    MEM_PRESERVE_PLACEHOLDER);
	if (ok) {
		VirtualFree(addr, 0, MEM_RELEASE);
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
