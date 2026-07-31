/*
 * dqlite Windows port -- forced-include prelude (POSIX-compat shim layer).
 *
 * Force-included (clang-cl /FI, wired from the `if(WIN32)` branch of
 * CMakeLists.txt) into every translation unit on Windows ONLY; compat/win/
 * is never on the Linux/macOS include path.
 *
 * Responsibilities:
 *
 *  1. Resolve the winuser.h `IN`/`OUT` SAL-annotation cascade. dqlite's
 *     src/utils.h defines a *function-like* macro `IN(E, ...)`; if that macro
 *     is seen before <windows.h>, the SDK headers parse their `IN`-annotated
 *     prototypes with a bare undefined token (~1000 errors). Fix: include the
 *     Winsock/Windows SDK headers HERE, first, so every SAL annotation
 *     resolves to the SDK's empty macros, then `#undef IN` so utils.h can
 *     later define its own. The SDK include guards make the later includes
 *     via <uv.h> no-ops, so a bare `IN` is never re-parsed.
 *
 *  2. Provide the POSIX scalar types (ssize_t, mode_t) universally, so code
 *     that uses them without including <unistd.h> still compiles.
 */
#ifndef DQLITE_WIN_PRELUDE_H
#define DQLITE_WIN_PRELUDE_H

/*
 * This prelude is Windows-only by construction (wired in from CMakeLists.txt
 * inside `if(WIN32)`); reaching it from a non-Windows compile means the build
 * system is misconfigured, so fail loudly rather than emit Win32 stubs.
 */
#ifndef _WIN32
#error "compat/win/dqlite_win_prelude.h is Windows-only; it must never be reached from a non-Windows build"
#endif

/*
 * Shim fence. The compat/win directory holds POSIX shim
 * headers under GENERIC names (<unistd.h>, <poll.h>, <sys/mman.h>, ...)
 * and sits on the include path AHEAD of the vcpkg/system directories. Any
 * translation unit compiled with that include path -- including a third-party
 * dependency TU built inside a dqlite target -- that includes one of those
 * names would otherwise silently resolve to dqlite's declaration-only stubs,
 * or worse, to types whose layout disagrees with a real implementation: an
 * ABI hazard the compiler cannot diagnose.
 *
 * DQLITE_WIN_COMPAT is the fence: it is defined HERE and only here, and this
 * prelude is force-included (clang-cl /FI, see CMakeLists.txt) as the very
 * first thing in every dqlite TU on Windows -- before any #include line of
 * the TU itself can reach a shim header. Every shim header #errors when the
 * macro is absent, so a TU compiled without dqlite's prelude that resolves
 * one of these names gets a hard build break instead of an invisible
 * mismatch. (The real fix -- taking the generic names off the include path
 * entirely -- is still pending; dqlite's own sources hardcode plain
 * `#include <unistd.h>`-style lines that must keep resolving until then.)
 */
#define DQLITE_WIN_COMPAT 1

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

/*
 * Winsock2 must precede <windows.h>. Include the same SDK headers that
 * libuv's uv/win.h pulls in (winsock2 -> mswsock -> ws2tcpip -> windows), so
 * that they are all fully parsed -- with IN/OUT defined as the SDK's empty
 * annotation macros -- and guarded before any dqlite header can define IN.
 */
#include <winsock2.h>
#include <mswsock.h>
#include <ws2tcpip.h>
#include <windows.h>

/*
 * Hand the `IN` identifier back to dqlite. Every SDK header that uses IN/OUT
 * as a parameter annotation has already been parsed above; OUT is left as the
 * SDK's empty macro (dqlite never defines OUT), only IN needs releasing so
 * utils.h can install its function-like IN(E, ...) form.
 */
#undef IN

/* ---- POSIX scalar types absent from MSVC/UCRT ---------------------------- */
#include <BaseTsd.h> /* SSIZE_T */

#if !defined(_SSIZE_T_) && !defined(_SSIZE_T_DEFINED)
typedef SSIZE_T ssize_t;
#define _SSIZE_T_
#define _SSIZE_T_DEFINED
#endif

/*
 * off_t: use the UCRT's own definition -- a 32-bit `long` (its `_off_t`) --
 * included here so every TU sees the name without needing <sys/types.h>
 * itself. (UCRT only typedefs the non-underscore POSIX name when __STDC__ is
 * off, which is the case under clang-cl's MSVC emulation; nothing in this
 * build defines _CRT_DECLARE_NONSTDC_NAMES to change that.)
 *
 * A 32-bit off_t means dqlite must NEVER funnel a 64-bit file offset or size
 * through off_t on Windows. Every compat entry point that takes an offset or
 * length is therefore declared with `long long` (mmap, ftruncate, pread,
 * pwrite, posix_fallocate), the UCRT lseek() this shim layer exposes returns
 * `long` to match, and the one comparison that mixes off_t with the 64-bit
 * SSIZE_MAX (src/server.c, dqlite_server_start) casts up to int64_t. The
 * remaining off_t-typed values in the tree (raft metadata/segment file sizes,
 * the WAL-index shm file size in vfs.c) are all far below 2 GiB by
 * construction. Do NOT pre-define the CRT-internal guard _OFF_T_DEFINED to
 * widen off_t: that risks a silent type mismatch with any UCRT or third-party
 * header that disagrees.
 */
#include <sys/types.h>

#ifndef DQLITE_MODE_T_DEFINED
#define DQLITE_MODE_T_DEFINED
typedef unsigned short mode_t;
#endif

#ifndef DQLITE_PID_T_DEFINED
#define DQLITE_PID_T_DEFINED
typedef int pid_t;
#endif

/* ---- POSIX libc gap-fillers (absent from UCRT) --------------------------- */
/* Provided in the prelude so every TU has them regardless of which POSIX
 * header it happened to include. All are static inline (no link surface). */
#include <stdlib.h>
#include <string.h>
#include <time.h> /* struct timespec (C11) */

/* strndup: bounded string duplicate. */
static inline char *strndup(const char *s, size_t n)
{
	size_t len = 0;
	char *copy;
	while (len < n && s[len] != '\0') {
		len++;
	}
	copy = (char *)malloc(len + 1);
	if (copy == NULL) {
		return NULL;
	}
	memcpy(copy, s, len);
	copy[len] = '\0';
	return copy;
}

/* clock_gettime: CLOCK_REALTIME via the system clock, CLOCK_MONOTONIC via the
 * performance counter. Enough for dqlite's timing/tracing needs. */
#ifndef DQLITE_CLOCKID_T_DEFINED
#define DQLITE_CLOCKID_T_DEFINED
typedef int clockid_t;
#endif
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

static inline int clock_gettime(clockid_t clk_id, struct timespec *tp)
{
	if (tp == NULL) {
		return -1;
	}
	if (clk_id == CLOCK_MONOTONIC) {
		LARGE_INTEGER freq, ctr;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&ctr);
		tp->tv_sec = (time_t)(ctr.QuadPart / freq.QuadPart);
		tp->tv_nsec = (long)(((ctr.QuadPart % freq.QuadPart) *
				      1000000000ULL) /
				     (unsigned long long)freq.QuadPart);
		return 0;
	} else {
		FILETIME ft;
		unsigned long long t;
		GetSystemTimePreciseAsFileTime(&ft);
		t = ((unsigned long long)ft.dwHighDateTime << 32) |
		    ft.dwLowDateTime;
		t -= 116444736000000000ULL; /* 1601 -> 1970 */
		tp->tv_sec = (time_t)(t / 10000000ULL);
		tp->tv_nsec = (long)((t % 10000000ULL) * 100ULL);
		return 0;
	}
}

/* ---- POSIX constants absent from UCRT ------------------------------------ */
/* File-mode permission bits (POSIX <sys/stat.h>). Windows only honours the
 * owner read/write bits; group/other are accepted and ignored. Values are the
 * standard octal constants so bitwise combinations compile as expected. */
#ifndef S_IRUSR
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXU 0700
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXG 0070
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#define S_IRWXO 0007
#endif

/* O_DSYNC: UCRT has no O_DSYNC, but libuv's Windows uv_fs_open() understands
 * UV_FS_O_DSYNC (0x04000000, see uv/win.h) and maps it to CreateFile's
 * FILE_FLAG_WRITE_THROUGH (libuv src/win/fs.c, fs__open). Every open(2)-flag
 * word in dqlite that carries O_DSYNC reaches the kernel exclusively through
 * uv_fs_open() (src/raft/uv_os.c UvOsOpen), so defining O_DSYNC to the
 * UV_FS_O_DSYNC value restores real synchronized-write semantics on Windows:
 * raft segment writes go through the OS cache straight to the device. The
 * value is asserted to match UV_FS_O_DSYNC in src/raft/uv_os.c (which can see
 * both macros). It deliberately does NOT collide with any UCRT _O_* flag.
 * See the "Durability on Windows" section in README.md. */
#ifndef O_DSYNC
#define O_DSYNC 0x04000000
#endif

/* open(2) flags with no UCRT/libuv equivalent -> defined as 0 (no-op) so the
 * flag arithmetic compiles. NOTE: this drops the semantics (O_NONBLOCK,
 * O_CLOEXEC) -- acceptable because no durability guarantee depends on them.
 *
 * O_DIRECT is deliberately NOT defined: code that probes for direct I/O does
 * so with `#if defined(O_DIRECT)` (src/raft/uv_os.c, UvOsSetDirectIo), and
 * leaving the macro undefined routes that probe to its honest "no direct-I/O
 * primitive on this platform" branch (UV_ENOTSUP). An earlier iteration
 * defined O_DIRECT as 0, which made UvOsSetDirectIo take the Linux fcntl()
 * path and "succeed" as a no-op -- a false positive that probeDirectIO
 * (src/raft/uv_fs.c) then had to neutralise. */
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

/* PATH_MAX -- sized generously for path buffers. */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif

/* SIGPIPE has no meaning on Windows (no broken-pipe signal); define it so
 * signal(SIGPIPE, SIG_IGN) style code compiles. */
#ifndef SIGPIPE
#define SIGPIPE 13
#endif

/* ssize_t upper bound (libuv normally defines this alongside its own ssize_t
 * typedef, which we suppress via _SSIZE_T_DEFINED above, so define it here). */
#ifndef SSIZE_MAX
#include <stdint.h>
#define SSIZE_MAX INTPTR_MAX
#endif

/* sa_family_t: Winsock spells the address family as ADDRESS_FAMILY / u_short
 * and does not provide the POSIX name. */
#ifndef DQLITE_SA_FAMILY_T_DEFINED
#define DQLITE_SA_FAMILY_T_DEFINED
typedef unsigned short sa_family_t;
#endif

/* Linux socket-type flags (OR'd into the socket()/accept4() type). */
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0
#endif

/* fcntl(2): declare the descriptor-flag commands and the function itself so
 * POSIX-shaped code compiles. The Windows implementation (compat_win.c)
 * implements NO command -- every call fails with ENOSYS -- and no
 * Windows-compiled code calls it (the POSIX call sites are compiled out
 * here). */
#ifndef F_GETFD
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#endif
#ifndef FD_CLOEXEC
#define FD_CLOEXEC 1
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
int fcntl(int fd, int cmd, ...);

/* dqliteWinSocketsInit(): idempotent, process-wide WSAStartup (Winsock 2.2),
 * implemented in compat/win/compat_win.c. Called from the library's public
 * object-creation entry points (dqlite_node_create, dqlite_server_create in
 * src/server.c) so Winsock is initialised before any raw socket()/
 * getaddrinfo() call the library makes -- structurally, not as a link-order
 * side effect. Declared here in the prelude so the guarded
 * call sites in shared sources need no Windows-only #include. */
void dqliteWinSocketsInit(void);

/* accept4(2) (Linux): accept() plus an atomic flag-set on the new socket.
 * Winsock has only accept(); wrap it and drop the flags. SOCK_CLOEXEC and
 * SOCK_NONBLOCK are shimmed to 0 (above), so no flag work is required today --
 * the caller (test/lib/endpoint.c) sets non-blocking mode separately, via
 * ioctlsocket(FIONBIO) in its _WIN32 branch.
 * Declared in the prelude (not <sys/socket.h>) because those callers use it
 * without including that header. The SOCKET result is narrowed to int to match
 * the POSIX fd-typed callers; INVALID_SOCKET maps to -1 so their `< 0` error
 * checks still work (the narrowing is safe in practice: Windows socket
 * handles fit in 32 bits even though the SOCKET type is pointer-sized). */
static inline int accept4(int fd,
			  struct sockaddr *addr,
			  socklen_t *addrlen,
			  int flags)
{
	SOCKET s = accept((SOCKET)fd, addr, addrlen);
	(void)flags;
	return s == INVALID_SOCKET ? -1 : (int)s;
}

/* ---- aligned_alloc (C11) -> malloc --------------------------------------
 * UCRT has no C11 aligned_alloc(), and _aligned_malloc() memory MUST be freed
 * with _aligned_free(), not the plain free() that C11 guarantees and that the
 * raft/dqlite call sites use (heap-corrupting when missed). So map
 * aligned_alloc() to plain malloc(), intentionally ignoring `alignment`: the
 * free() contract holds and no heap bookkeeping mismatch is possible. Why the
 * lost over-alignment is sound (only O_DIRECT needs it, never used on Windows)
 * is documented at raft_aligned_alloc() in src/raft.h -- the canonical home of
 * this platform-dependent contract, pinned by test/raft/integration/test_heap.c.
 *
 * Provided as a static inline function, NOT a function-like macro, so the raft
 * heap vtable member call `currentHeap->aligned_alloc(...)` is not mis-parsed. */
static inline void *aligned_alloc(size_t alignment, size_t size)
{
	(void)alignment;
	return malloc(size);
}

/* bcmp: legacy alias for memcmp. */
#ifndef bcmp
#define bcmp(a, b, n) memcmp((a), (b), (n))
#endif

/* MIN/MAX: some sources expect the sys/param.h spellings. */
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

/* ---- Endianness (POSIX <endian.h> / BSD <machine/endian.h>) --------------
 * src/raft/byte.c bundles a public-domain SHA1 that needs BYTE_ORDER together
 * with LITTLE_ENDIAN/BIG_ENDIAN to select its blk0() byte-swap. Its built-in
 * fallback only recognises legacy arch macros (__i386__, MIPSEL, ...) and so
 * never matches clang-cl on x64 -> BYTE_ORDER is left undefined -> #error
 * "Undefined or invalid BYTE_ORDER" (and the follow-on undeclared blk0).
 * Every Windows target dqlite supports (x64, arm64) is little-endian, so
 * define the macros here -- force-included first, before byte.c's own
 * `#ifndef BYTE_ORDER` block, which is then skipped with the values in scope.
 * Values match byte.c's own so any cross-file comparison stays consistent. */
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN 4321
#endif
#ifndef PDP_ENDIAN
#define PDP_ENDIAN 3412
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif

/* ---- qsort_r (GNU/BSD reentrant qsort) -----------------------------------
 * UCRT has qsort_s, but its comparator takes the context argument FIRST,
 * whereas the GNU qsort_r that src/roles.c expects takes it LAST:
 *   glibc:  int cmp(const void *a, const void *b, void *arg)
 *   UCRT:   int cmp(void *ctx, const void *a, const void *b)
 * Provide the glibc-signature qsort_r as a thin adapter over qsort_s. The
 * context is threaded through qsort_s (never a global), so this stays
 * reentrant/thread-safe. roles.c's comparators use the glibc (arg-last)
 * order, matching this wrapper. */
struct dqlite_qsort_r_ctx {
	int (*compar)(const void *, const void *, void *);
	void *arg;
};
static inline int dqlite_qsort_r_trampoline(void *ctx,
					    const void *a,
					    const void *b)
{
	struct dqlite_qsort_r_ctx *c = (struct dqlite_qsort_r_ctx *)ctx;
	return c->compar(a, b, c->arg);
}
static inline void qsort_r(void *base,
			   size_t nmemb,
			   size_t size,
			   int (*compar)(const void *, const void *, void *),
			   void *arg)
{
	struct dqlite_qsort_r_ctx c;
	c.compar = compar;
	c.arg = arg;
	qsort_s(base, nmemb, size, dqlite_qsort_r_trampoline, &c);
}

/* strtok_r -> UCRT strtok_s (identical 3-argument signature/semantics). */
#ifndef strtok_r
#define strtok_r(str, delim, saveptr) strtok_s((str), (delim), (saveptr))
#endif

/* ---- posix_fadvise: advisory, safe no-op --------------------------------
 * src/raft/uv_fs.c hints sequential access. Windows has no equivalent syscall
 * (the hint would be given as FILE_FLAG_SEQUENTIAL_SCAN at CreateFile time, a
 * later port item); dropping the hint is always safe and returns success. */
#ifndef POSIX_FADV_NORMAL
#define POSIX_FADV_NORMAL 0
#define POSIX_FADV_RANDOM 1
#define POSIX_FADV_SEQUENTIAL 2
#define POSIX_FADV_WILLNEED 3
#define POSIX_FADV_DONTNEED 4
#define POSIX_FADV_NOREUSE 5
#endif
static inline int posix_fadvise(int fd, long long offset, long long len, int advice)
{
	(void)fd;
	(void)offset;
	(void)len;
	(void)advice;
	return 0;
}

/* ---- posix_fallocate: implemented via SetEndOfFile ------------------------
 * Ensure the file backing `fd` has at least offset+len bytes. We only ever
 * grow (never truncate) by taking max(current, offset+len). POSIX contract:
 * return 0 on success or a positive errno-style code on failure, and do NOT
 * set errno.
 *
 * Implementation note: extending via SetFileInformationByHandle(
 * FileEndOfFileInfo) is metadata-only. On NTFS it still reserves the clusters
 * immediately (ENOSPC surfaces here, as posix_fallocate requires, for
 * non-sparse files) but defers zero-filling via the valid-data-length
 * mechanism, which sequential writers -- raft only ever appends to its
 * segments -- never pay for. The previous implementation used _chsize_s,
 * which zero-fills by physically writing: ~537 ms for an 8 MiB segment on a
 * FILE_FLAG_WRITE_THROUGH handle (measured, NVMe). Since O_DSYNC maps to
 * write-through on Windows (see above), that would make every raft segment
 * preparation take ~0.5 s and starve the event loop tests. */
#include <errno.h>
#include <io.h>    /* _get_osfhandle, _lseeki64 */
#include <stdio.h> /* SEEK_END */
static inline int posix_fallocate(int fd, long long offset, long long len)
{
	long long want = offset + len;
	long long cur = _lseeki64(fd, 0, SEEK_END);
	if (cur < 0) {
		return errno;
	}
	if (want <= cur) {
		return 0;
	}
	{
		HANDLE h = (HANDLE)_get_osfhandle(fd);
		FILE_END_OF_FILE_INFO info;
		if (h == INVALID_HANDLE_VALUE) {
			return EBADF;
		}
		info.EndOfFile.QuadPart = want;
		if (!SetFileInformationByHandle(h, FileEndOfFileInfo, &info,
						sizeof info)) {
			return GetLastError() == ERROR_DISK_FULL ? ENOSPC
								 : EIO;
		}
	}
	return 0;
}

/* ---- _wassert: restore the noreturn contract ------------------------------
 * glibc declares assert()'s failure handler (__assert_fail) noreturn, so on
 * Linux the compiler knows an `assert(0)` (dqlite's IMPOSSIBLE()/
 * dqlite_assert(0) unreachable-default idiom in bind.c, tuple.c, server.c)
 * terminates the enclosing branch. UCRT's <assert.h> declares _wassert()
 * WITHOUT noreturn, so on Windows clang-cl assumes the assert can fall
 * through and reports -Wsometimes-uninitialized for variables assigned in
 * every reachable case. _wassert never returns in practice (it aborts or
 * raises); declare it first -- this prelude is force-included before any TU
 * line -- with the attribute so later UCRT redeclarations merge with it and
 * the flow analysis matches Linux. (Signature per UCRT <assert.h>; the
 * declaration is harmless under NDEBUG, where assert() never references it.) */
_ACRTIMP __declspec(noreturn) void __cdecl _wassert(wchar_t const *_Message,
						    wchar_t const *_File,
						    unsigned _Line);

/* ---- __assert_fail (glibc) -----------------------------------------------
 * The test runner (test/lib/runner.h) and src/lib/assert.c call __assert_fail
 * directly with glibc's 4-argument signature. UCRT provides _wassert instead
 * and has no __assert_fail symbol, so supply one. static inline keeps it off
 * the link surface and, crucially, avoids -Wunused-function (-> -Werror) in
 * the many TUs force-included by this prelude that never reference it. */
#include <stdlib.h> /* abort */
static inline
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((noreturn))
#endif
    void
    __assert_fail(const char *assertion,
		  const char *file,
		  unsigned int line,
		  const char *function)
{
	fprintf(stderr, "%s:%u: %s: Assertion `%s' failed.\n", file, line,
		function != NULL ? function : "", assertion);
	abort();
}

#endif /* DQLITE_WIN_PRELUDE_H */
