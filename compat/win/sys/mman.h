/*
 * dqlite Windows port -- <sys/mman.h> minimal memory-mapping shim.
 *
 * Declares the mmap/munmap surface and the PROT_ / MAP_ flags used by
 * src/vfs.c (WAL shared-memory). The IMPLEMENTATION (CreateFileMapping plus
 * the Win10 1803+ placeholder APIs VirtualAlloc2/MapViewOfFile3/
 * UnmapViewOfFile2 -- a hard requirement, see mmap() -- and memfd_create via
 * a delete-on-close temp file) lives in compat/win/compat_win.c.
 *
 * NOTE: MREMAP_MAYMOVE is intentionally left UNDEFINED so vfs.c selects its
 * portable "no mremap" fallback (map-at-fixed-address replace), the same path
 * macOS takes (see vfsNoMremap() in src/vfs.c). Windows ONLY (compat/win/).
 *
 * Alignment contract: MapViewOfFile3 requires the target base address AND the
 * file offset to be multiples of the 64KiB allocation granularity. vfs.c's
 * vfsGetMapSize() derives both from dqlite_win_allocation_granularity()
 * (compat/win/unistd.h) on Windows, so every vfs mapping is 64KiB-aligned and
 * satisfiable. (sysconf(_SC_PAGESIZE) reports the true 4KiB page size and is
 * NOT usable for this.)
 */
#ifndef DQLITE_COMPAT_SYS_MMAN_H
#define DQLITE_COMPAT_SYS_MMAN_H

/* Shadowing fence -- see the rationale in dqlite_win_prelude.h. */
#if !defined(_WIN32) || !defined(DQLITE_WIN_COMPAT)
#error "dqlite compat/win shim header reached without the dqlite Windows prelude (DQLITE_WIN_COMPAT undefined); this header must not shadow a real system header -- see compat/win/dqlite_win_prelude.h"
#endif

#include <stddef.h>

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS

#define MAP_FAILED ((void *)-1)

/* offset typed as long long (64-bit) to match dqlite's usage. */
void *mmap(void *addr, size_t length, int prot, int flags, int fd, long long offset);
int munmap(void *addr, size_t length);
int msync(void *addr, size_t length, int flags);

/* memfd_create: anonymous in-memory file (Linux). Used by src/vfs.c for the
 * WAL shared-memory fd. glibc declares it in <sys/mman.h>; the Win32 port
 * (compat_win.c) backs it with a uniquely-named FILE_FLAG_DELETE_ON_CLOSE +
 * FILE_ATTRIBUTE_TEMPORARY temp file exposed as a CRT fd via _open_osfhandle. */
#define MFD_CLOEXEC 0x0001U
#define MFD_ALLOW_SEALING 0x0002U

int memfd_create(const char *name, unsigned int flags);

#define MS_ASYNC 0x1
#define MS_SYNC 0x4
#define MS_INVALIDATE 0x2

#endif /* DQLITE_COMPAT_SYS_MMAN_H */
