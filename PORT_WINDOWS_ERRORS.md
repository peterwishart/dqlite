# dqlite Windows build — compiler error inventory

Native Windows build, clang-cl 19 (MSVC ABI) + Ninja + vcpkg deps
(libuv 1.52.1, sqlite3 3.53.3, lz4 1.10.0). Configured with
`/clang:-ferror-limit=0` and built with `ninja -k 0` so every target is
attempted in one pass.

## Current status (after the aligned-alloc / uv_buf_t source fixes)

- Reproduce: `build-win-configure.bat` then `build-win-after.bat` (both load
  vcvars64 + clang-cl 19 + Ninja).
- **7 object files FAIL** (2 distinct source files, each built in several
  target variants): `src/raft/uv_fs.c` (5) and `src/server.c` (2).
- **Progress this iteration: 15 → 7 failing TUs** (previous: 39 → 15, 357 → 39).
  The header-not-found class and the `IN`/`OUT` `windows.h` cascade remain fully
  resolved. The two platform-guarded source fixes below cleared the remaining
  *mechanically fixable* failures (aligned alloc/free pairing, `uv_buf_t` field
  order); the 7 that remain are the genuine-reimplementation core (see §A(b)).

### Fixed this iteration (platform-guarded source + compat changes)

Both fixes keep the POSIX path byte-identical to before (everything is guarded
on `_WIN32`); verified with no regression on Linux (raft-uv-unit-test 25/25,
unit-test 320/320) and a drop of 8 failing TUs on Windows.

1. **`aligned_alloc` / aligned-free pairing.** On Windows, C11 `aligned_alloc`
   is provided in the prelude as a `static inline` mapping to
   `_aligned_malloc(size, alignment)` (note the reversed argument order). It is
   a function, NOT a macro, so the raft heap vtable member also named
   `aligned_alloc` and its member-call sites (`currentHeap->aligned_alloc(...)`)
   are not mis-parsed as macro invocations. Memory from `_aligned_malloc` must
   be released with `_aligned_free`, so a shared helper `RAFT_ALIGNED_FREE(PTR)`
   was added to `src/raft.h` (`_aligned_free` on Windows, plain `free` on
   POSIX). Every aligned-free site now routes through it:
   `src/raft/heap.c` (`defaultAlignedFree`), `test/raft/lib/heap.c`
   (`heapAlignedFree`), and `test/raft/unit/test_uv_writer.c` (`DESTROY_BUFS`).
   The vtable-driven aligned frees in `src/raft/uv_fs.c` / `src/raft/uv_segment.c`
   go through `defaultAlignedFree`, so they are now runtime-correct too.
   `test/lib/raft_heap.c` needed no edit (it only forwards to the underlying
   heap's `aligned_free`).
2. **`uv_buf_t` field order.** `test/unit/lib/test_transport.c`'s `BUF_ALLOC`
   used a positional aggregate init `{base, len}`, which mis-assigns on Windows
   where `uv_buf_t` is `{ULONG len; char *base;}`. Replaced with the portable
   `uv_buf_init(base, len)` (identical behaviour on Linux).

### Shims added this iteration (mechanical function wrappers)

All are Windows-only (compat/win/, force-included prelude or shim headers);
no non-Windows branch is touched, no source file was edited.

| Function / symbol | Mapping | Where |
|---|---|---|
| `qsort_r` (glibc arg-last cmp) | adapter over UCRT `qsort_s` (arg-first cmp), context threaded through -> reentrant | prelude |
| `strtok_r` | `#define` -> `strtok_s` (identical 3-arg form) | prelude |
| `strverscmp` | compact version-string compare (digit-run magnitude); sufficient for dotted numeric versions, not a byte-exact glibc clone | prelude |
| `posix_fadvise` + `POSIX_FADV_*` | safe no-op returning 0 (hint dropped) | prelude |
| `posix_fallocate` | grow-only via `_chsize_s` (max(current, off+len)); returns 0/errno-style code, errno not set (POSIX contract) | prelude |
| `__assert_fail` (glibc 4-arg, noreturn) | `fprintf` + `abort`; `static inline` to avoid -Wunused-function under -Werror | prelude |
| `accept4` | `accept()` + drop flags (SOCK_* shimmed to 0); SOCKET narrowed to int, INVALID_SOCKET -> -1 | prelude |
| `BYTE_ORDER`/`LITTLE_ENDIAN`/`BIG_ENDIAN`/`PDP_ENDIAN` | little-endian (x64/arm64); unblocks `src/raft/byte.c` `blk0` byte-swap | prelude |
| `mkdtemp` | `_mktemp_s` + `_mkdir` (best-effort, non-atomic) | unistd.h |
| `getuid` / `geteuid` | return 0 ("root") so Windows-inapplicable EACCES tests SKIP | unistd.h |
| `sem_getvalue` | declaration only (matches header-only sem shim; impl deferred to link) | semaphore.h |
| `strcasecmp`/`strncasecmp`, `strndup`, `gmtime_r`/`localtime_r`, `mkstemp` | already present from the prior header layer (unistd.h / prelude) | — |

### RESOLVED: `aligned_alloc` (correctness) — see "Fixed this iteration" above

`aligned_alloc` is used by `src/raft/heap.c`, `test/raft/lib/heap.c` and
`test/raft/unit/test_uv_writer.c`. UCRT has no C11 `aligned_alloc`, and the
obvious `#define aligned_alloc _aligned_malloc` was BOTH a correctness bug
(memory would be freed with plain `free()`, not `_aligned_free` -> heap
corruption) AND a compile error (a function-like macro named `aligned_alloc`
mis-parses the vtable member call `currentHeap->aligned_alloc(...)`).

Fixed by (a) a `static inline aligned_alloc` in the prelude mapping to
`_aligned_malloc(size, alignment)`, and (b) a shared `RAFT_ALIGNED_FREE(PTR)`
helper in `src/raft.h` (`_aligned_free` on Windows, `free` on POSIX) applied at
every aligned-free site. Cleared 6 of the 7 previously-blocked TUs (heap.c x5,
test/raft/lib/heap.c x1); the 7th, `test_uv_writer.c`, also now compiles.

### What fixed it

1. **`compat/win/` shim-header layer** (Windows-only include dir, wired in
   `CMakeLists.txt` under `if(WIN32)` inside `dqlite_common_flags()` — never on
   the Linux/macOS include path). Shims for every missing POSIX header:
   `unistd.h`, `sys/{socket,un,mman,time,uio,vfs,syscall,file,statvfs,random}.h`,
   `netinet/{in,tcp}.h`, `arpa/inet.h`, `netdb.h`, `poll.h`, `pthread.h`,
   `semaphore.h`, `sched.h`, `libgen.h`, `dlfcn.h`, `ftw.h`, `sys/utsname.h`,
   `dirent.h`, `linux/{magic,limits}.h`, plus a Windows-only `uv/unix.h`
   redirect stub (a few sources hardcode `#include <uv/unix.h>`, libuv's
   POSIX platform header, which pulls `<termios.h>`).
2. **Forced-include prelude** (`compat/win/dqlite_win_prelude.h`, via clang-cl
   `/FI`) resolves the `IN`/`OUT` SAL cascade at its **root cause** (see §8) and
   supplies POSIX scalar types + constants universally.
3. CMake Windows defs: `_CRT_NONSTDC_NO_WARNINGS`, `_CRT_NONSTDC_NO_DEPRECATE`,
   `_WINSOCK_DEPRECATED_NO_WARNINGS`, `_CRT_RAND_S`.

The shims deliberately RESOLVE headers + declare types/constants + wrap trivial
UCRT-equivalent calls. Genuine reimplementation of sockets / threads / mmap /
positional+async I/O is intentionally deferred — see §A (the next work-list).

---

## §A. Remaining 7 failing TUs — the genuine-reimplementation core

Every mechanically shimmable POSIX function and every mechanically fixable
source issue is now handled (see the "Shims added" and "Fixed this iteration"
tables above). The 7 TUs still failing reduce to **2 distinct source files**,
neither fixable by a header shim or a mechanical source edit:

### (a) Shimmable-in-principle source changes — DONE

Both the `aligned_alloc`/aligned-free pairing (`src/raft/heap.c`,
`test/raft/lib/heap.c`, `test/raft/unit/test_uv_writer.c`) and the `uv_buf_t`
field-order init (`test/unit/lib/test_transport.c`) have been fixed — see
"Fixed this iteration" above. 8 TUs cleared (15 → 7).

### (b) Genuine reimplementation (libuv fd model / dir-relative I/O / socket internals)

| Source (TUs) | Blocker | Intended Win32 port |
|---|---|---|
| `src/raft/uv_fs.c` (5) | `uv_file` (int) vs `uv_os_fd_t` (`HANDLE`/`void*`) mismatch: on Windows libuv models OS fds as handles, so passing an int `uv_file` where a `uv_os_fd_t` is expected (and vice-versa) is a hard type error | Rework the raft uv-fs layer to carry `uv_os_fd_t`/`HANDLE` (or `_get_osfhandle`) consistently; part of the positional/async I/O port |
| `src/server.c` (2) | `openat`/`renameat` (dir-relative snapshot ops) + `stream->io_watcher.fd` (no `io_watcher` in Windows `uv_stream_s`; used to read peer credentials over a named pipe) | `openat`/`renameat` -> compose path + `CreateFileW`/`MoveFileEx`; peer-cred over `AF_UNIX`/named-pipe -> `GetNamedPipeClientProcessId` or equivalent |

TU accounting: uv_fs.c 5 + server.c 2 = **7**.

### Still deferred (declared-but-not-implemented — will surface at LINK time)

Unchanged from the previous iteration; the shims declare these with correct
signatures so compilation proceeds, but the implementations are stubs and will
produce unresolved externals when linking is eventually attempted:

- **Sockets / transport**: real `WSAStartup` lifecycle, `AF_UNIX` (`afunix.h`)
  vs TCP-loopback fallback, `fcntl(F_SETFL, O_NONBLOCK)` →
  `ioctlsocket(FIONBIO)`. The `accept4` wrapper is now defined but non-blocking
  is still set separately by callers. (§3)
- **Threads / semaphores**: `pthread_*` / `sem_*` (incl. the now-declared
  `sem_getvalue`) → `uv_thread_t`/`uv_mutex_t`/`uv_cond_t`/`uv_sem_t` (or
  pthreads-win32). Only `src/server.{c,h}`. (§4)
- **mmap**: `mmap`/`munmap`/`memfd_create` → `CreateFileMapping`/`MapViewOfFile`
  (`src/vfs.c` WAL shmem). `MREMAP_MAYMOVE` intentionally left undefined so the
  portable no-mremap fallback is selected.
- **Positional/async I/O**: `pread`/`pwrite` (non-atomic seek+rw),
  `posix_fallocate` (grow-only `_chsize_s`), `ftruncate`→`_chsize_s`,
  `fsync`→`_commit` are wired but need review.
- **Directory ops**: `scandir`/`opendir`/`readdir` (`dirent.h`), `nftw`
  (`ftw.h`), `flock` (`sys/file.h`), `statfs`/`statvfs`, `mkdtemp`/`mkstemp`
  (best-effort, non-atomic) — all declared/best-effort only.
- **`__declspec` export plumbing** for the DLL (`include/dqlite.h`
  `DQLITE_API`) — link-stage, not yet reached. (§7)

---

## Historic baseline (before the shim layer) — for reference

- Result: **357 object files FAILED / 385**. Every failure aborted on a
  **fatal** `'<hdr>' file not found`, masking all downstream errors.
- `unistd.h` was the dominant first-hit (149 files), pulled transitively via
  `src/lib/sm.h`, `buffer.h`, `raft/byte.h`, `tuple.h`.
- The `IN`/`OUT` cascade (~1,248 lines) came from just 2 files
  (`src/raft/replication.c`, `recv_append_entries_result.c`) — the only TUs that
  then reached `windows.h`. It would have spread to every TU once headers were
  shimmed.

---

## §8. Root cause of the `windows.h` `IN`/`OUT` cascade (SOLVED)

Not a pure toolchain bug. dqlite's `src/utils.h` defines a **function-like**
macro `IN(E, ...)` (pulled into most TUs via `src/tracing.h`). When that macro
is in scope before `<windows.h>` is parsed, the Windows SDK's
`minwindef.h` guard `#ifndef IN / #define IN` (empty) is **skipped**, so
`winuser.h` prototypes such as `RegisterPowerSettingNotification(IN HANDLE h,…)`
see a bare, undefined `IN` token → `error: unknown type name 'IN'` (then the
`expected ')'` / redefinition follow-ons). Verified by reproducing:
`<winsock2.h>+<windows.h>` alone compiles clean; the same chain **with
`tracing.h`→`utils.h` first** fails exactly as in the build.

**Fix (in `compat/win/dqlite_win_prelude.h`, force-included via `/FI` on Windows
only):** include the Winsock/Windows SDK headers *first* (so every `IN`/`OUT`
annotation resolves to the SDK's empty macros while those headers are parsed),
then `#undef IN` so `utils.h` can install its own function-like `IN()` without
collision. The SDK headers' include guards make the later `<uv.h>` → `uv/win.h`
includes no-ops, so no bare `IN` is ever re-parsed. Zero source files edited.

---

## §6. Packed structs / `__declspec` (unchanged, informational)

- `__attribute__((packed))` / `DQLITE_PACKED`: **clang-cl accepts it**, so no
  error under the current toolchain (only a pure `cl.exe` build would need a
  `#pragma pack` fallback).
- Symbol export (`DQLITE_API` → `__declspec(dllexport/dllimport)`) is a
  link-stage concern, not yet reached (see §A "still deferred").
