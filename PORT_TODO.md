# dqlite multi-platform port — TODO

Tracking the work to make dqlite build and run on **macOS** and **Windows** in
addition to Linux, with an eye toward upstreaming. Toolchain target is **Clang**
on all three platforms (Linux `clang`, macOS AppleClang, Windows `clang-cl`).

Legend: `[ ]` todo · `[~]` in progress · `[x]` done

---

## 0. Status summary

- [x] Linux build via CMake + Clang (`CMakeLists.txt`, builds + fast tests pass)
- [x] Platform seam established in `CMakeLists.txt` (Linux impl; macOS/Windows
      are explicit `FATAL_ERROR` stubs naming the files to port)
- [x] Portable threadpool write backend implemented + auto-selected + Linux-validated
- [x] Kernel-AIO/eventfd fully gated behind `DQLITE_HAVE_KAIO`; raft I/O compiles
      with no kernel-AIO dependency — verified on Linux via `-DDQLITE_DISABLE_KAIO=ON`
- [~] macOS backend: I/O write path compiles without kernel AIO (verified via
      DISABLE_KAIO) and passes all write-path suites on the portable backend.
      Remaining before it can actually run on macOS: the `statfs`/magic FS-type
      probe (§5 — magic values need a Mac) and real-hardware compile/test.
- [~] Windows backend: env unblocked (clang-cl 19 + CMake/Ninja + vcpkg deps);
      CMake configures; POSIX shim-header compat layer (`compat/win/`, WIN32-only
      include path + `/FI` prelude) + fn shims + mechanical source fixes →
      **378/385 objects compile** (from 28). Mechanical fixes done: `aligned_alloc`
      pairing via `RAFT_ALIGNED_FREE`/`_aligned_free` (Linux path byte-identical),
      `test_transport.c` `uv_buf_t` via `uv_buf_init`. `uv_fs.c` FIXED — was a
      type inconsistency, NOT a real HANDLE port: `uvOsWriteOne`'s param was
      `uv_os_fd_t` but only ever used with `uv_file`; changed to `uv_file`
      (identical type on Linux). LINK stage done: POSIX impls in
      `compat/win/compat_win.c` (`statvfs`/`fstatfs`/`fcntl`/`nftw`/`uname`),
      C99-inline linkage fixed (`flags.c`/`progress.c`: dropped `inline`), and
      `aligned_alloc` reworked to plain `malloc` (C11 says its memory is
      `free()`-able; `_aligned_malloc` violated that and caused a debug-CRT
      heap dialog — Windows needs no alignment since O_DIRECT is off). A CRT
      constructor routes debug diagnostics to stderr so test runs are
      non-interactive. `src/server.c` PORTED (path-based dir helpers instead of
      `openat`/`renameat`; named-pipe peer-cred via `GetNamedPipeClientProcessId`)
      — so **ALL 385 objects now compile on Windows.** Windows RUNS
      `raft-core-integration-test` 191/191, `raft-core-fuzzy-test` 57/57.
      **LINK COMPLETE — all 16 targets link incl. `dqlite.dll` (566KB), zero
      unresolved symbols.** `pthread_*`/`sem_*`/`flock` via Win32 shim
      (SRWLOCK/CONDITION_VARIABLE/_beginthreadex, CreateSemaphore w/ tracked
      count, LockFileEx); `getaddrinfo` interposer guarded `#ifndef _WIN32`;
      **`vfs.c` WAL mmap solved** — key enabler: `sysconf(_SC_PAGESIZE)` on
      Windows returns the 64KB allocation granularity, so the 32KB shm regions
      map at 64KB-aligned offsets (`region_per_map=2`) → `MapViewOfFileEx`
      satisfiable; `memfd_create`→temp file, `mmap`→CreateFileMapping+
      MapViewOfFileEx (MAP_FIXED=unmap+remap), `ftruncate` grows via section
      manager. **Suites now RUN and the WAL/mmap work is VALIDATED on Windows:
      `vfs_extra` 38/38, `VfsShmMap`/`VfsTruncate`/`VfsWrite`/etc. all pass; most
      non-socket `unit-test` suites pass (registry/serialize/tuple/gateway/
      command/sm/replication).** Fixes that unblocked runtime: `vfs.c` base VFS
      `sqlite3_vfs_find("unix")`→`(NULL)` (platform default; identical on Linux);
      Windows temp-dir parent (%TMP% via `_WIN32` guard in test/lib/fs.c,
      test/raft/lib/dir.c, test/unit/test_vfs.c) instead of hardcoded `/tmp`.
      **Remaining RUNTIME work-list (next phase):**
      (a) **Networking** — `conn`/`lib_transport`/`lib_addr` + ALL of
      `integration-test` (client/cluster/fsm/membership/node/server) +
      `raft-uv-integration-test` (0/241) fail; likely needs WSAStartup init +
      AF_UNIX/named-pipe/non-blocking-socket handling (investigate WSAStartup
      first — may be high-leverage). **[~] LARGELY DONE — see §7.** WSAStartup
      WAS needed (dqlite's own `getaddrinfo`/`socket()` run independent of libuv
      → `WSANOTINITIALISED`); added to the `compat_win.c` constructor. TCP path
      ported: `lib_addr` 6/6, `lib_transport` 2/2, `conn` 7/7, `ext_uv` 2/2
      (tcp), `raft-uv-integration` **0/241→199/203** (43 skip), `integration`
      `client` 6/6 + `fsm` 28/28, `cluster/restart` green. Two more REAL bugs
      found+fixed: (i) `uv_fs.c` reinterpret-cast `raft_buffer*`→`uv_buf_t*` but
      Windows `uv_buf_t` is `{len,base}` (opposite order) → garbage metadata/
      snapshot writes = `RAFT_IOERR`; now `uv_buf_init` (guarded). (ii)
      `client/protocol.c` checked `revents != POLLIN` but WSAPoll reports
      `POLLRDNORM` → every response read misread as error. **Multi-node join
      "crash" — FIXED (2026-07-27), NOT a raft bug.** Test bodies hardcoded the
      Linux abstract-unix literal `"@2"` while the Windows harness binds TCP
      loopback → `endpointConnect("@2")` asserted on the missing `:`
      (test/lib/server.c:23) → `abort()` on a threadpool worker (looked like a
      crash; the membership/role_management "hangs" were the same abort). Fixed
      by using the node's REAL configured address `f->servers[id-1].address`
      (== `"@2"` verbatim on Linux `#else`) in test_cluster.c (3),
      test_membership.c (5), test_role_management.c (4); all `_WIN32`-guarded.
      **dqlite's raft membership/replication is CORRECT on Windows** — it
      connects to whatever the config holds. Windows now: `cluster` 23/23,
      `membership` 5/5, `role_management/promote` OK, `dataOnNewNode` 5/5 (incl.
      2200-record snapshot transfer). `stress` is single-node, unrelated,
      passes. Linux re-verified green. **`dqlite_server_start` (high-level API) — FIXED
      (2026-07-27), `server` 4/8→8/8.** Two more REAL Windows bugs in the server
      persistence path: (i) 32-bit `off_t` truncation — MSVC `off_t` is 32-bit
      `long`, so `(off_t)SSIZE_MAX` truncated to `-1` and `full_size >
      (off_t)SSIZE_MAX` was true for ANY file → instant `DQLITE_ERROR`; first
      fixed by widening `off_t`→`__int64` in the Windows-only prelude, later
      (W5, 2026-07-29) re-fixed at the root — the `server.c` comparison now
      casts up to `int64_t` and the prelude uses UCRT's own 32-bit `off_t`.
      (ii) rename-over-open-file — Windows
      can't `rename()` over a file with open handles (POSIX `renameat` can);
      fixed with `serverDirOpen`→`CreateFileA(FILE_SHARE_DELETE)`+`_open_osfhandle`
      (`_O_BINARY` keeps on-disk `\n` layout == Linux) and `serverDirRename`→
      `SetFileInformationByHandle(FileRenameInfoEx, REPLACE_IF_EXISTS|
      POSIX_SEMANTICS)`. Both `_WIN32`-only; Linux re-verified green
      (unit-test 320/320, server/ 8/8). **Remaining Windows work:** the
      named-pipe (`uv_pipe_t`) local-transport workstream (user-chosen, §7);
      **raft-uv edge cases — FIXED (2026-07-28),
      `raft-uv-integration` 199→210 OK / 36 SKIP / 0 FAIL.** (i) `tcp_listen`
      teardown hang — REAL production fix in `uv_tcp_listen.c`:
      `uvTcpIncomingAbort` now `uv_read_stop`s before `uv_close` (on Windows
      `uv_close` on a handle with an active `uv_read_start` doesn't fire its cb
      until the pending WSARecv completes = only on peer send/close); un-skipped
      `badProtocol`/`peerAbort`/`handshake` (+ guarded non-blocking loop macros
      in the test, since accept+handshake IOCP completions coalesce and a
      blocking `UV_RUN_ONCE` on the idle listener hangs). **Also improves real
      node shutdown mid-handshake.** (ii) `append/counter` (+bonus
      `finalizeSegment`) — async threadpool finalize lags the append cb on
      Windows; guarded bounded loop-pump until the finalized file appears (same
      pattern `prepareSegments` uses). (iii) `send/changeToUnconnectedAddress` —
      Windows strictly refuses connects when `listen` backlog is full (Linux
      lenient); harness `test/raft/lib/tcp.c` now `listen(...,16)` on Windows.
      (iv) `send/reconnectAfter{,Multiple}WriteError` — **FIXED (the earlier
      "genuine Winsock limitation" verdict was WRONG).** The test injects the
      fault via `close(socket)` on the peer, but in that TU `close`→CRT
      `_close()` (no-op on a Winsock SOCKET) → peer never actually closed →
      raft's writes legitimately succeeded. Fix: `closePeerSocket()` →
      `closesocket()` on Windows in `test_uv_send.c`; un-skipped both. First
      post-close write now fails `RAFT_IOERR` in 1 iteration deterministically;
      reconnect path genuinely exercised. `send/` 23/23 (0 skip), full
      `raft-uv-integration` 212/212; Linux re-verified. All `_WIN32`-guarded,
      Linux re-verified green (raft-uv-unit 25/25, send/ 23/23, append/ 33/33,
      tcp_listen/ 42/42). **`gateway/dump/checkpointed` — FIXED (2026-07-28),
      and it was NOT a VFS/checkpoint bug: the WAL/mmap path is proven correct
      on Windows** (subagent dumped the image → external `sqlite3 PRAGMA
      integrity_check` = `ok`). Real cause: test-harness `INTEGRITY_CHECK`
      (test_gateway.c) opened its scratch DB file with `open(O_CREAT|O_RDWR)`
      WITHOUT `O_BINARY` → Windows CRT text-mode inserted `\r` before every
      `\n` in the ~512KB binary image → corruption ("invalid page number").
      Fixed by adding `O_BINARY` (with `#ifndef O_BINARY #define O_BINARY 0`
      fallback → provable no-op on Linux). `unit-test.exe` 283/317→300/317
      (17-test cascade cleared). Linux re-verified 320/320. **The sysconf
      64KB-page-size shim is confirmed to only affect WAL-index shm mapping,
      NOT DB-page math.** ~~OLD note:~~ `gateway/dump/checkpointed` SQLite
      integrity error — passed on Linux; in single-process `unit-test`
      its unclean teardown cascaded fails that all pass in
      isolation. **`concurrency/delete` — FIXED (2026-07-28), 10th bug: a shim
      LEAK.** The 64KB page-size shim (`sysconf(_SC_PAGESIZE)`→
      `dwAllocationGranularity`, for WAL mmap alignment) leaked into
      `src/lib/buffer.c`, where `page_size` is the QUERY ROW-BATCH FLUSH
      THRESHOLD (`query__batch` pauses when the buffer fills a "page"). 64KB vs
      the true 4KB swallowed all 300 test rows in one batch → query reported
      *finished* → `RESUME` saw `finished==true` → assert failed (data never
      corrupt, just the pause/resume boundary). Fix: `buffer__init` uses a new
      `dqlite_win_page_size()` (genuine `dwPageSize` 4KB) on Windows; `vfs.c`
      keeps the 64KB `sysconf`. `_WIN32`-guarded (`#else` = verbatim original).
      `concurrency/delete` 8/8, full `concurrency` 17/17; Linux re-verified
      320/320. (Only 2 of 8 delete cases still failed by this point — the "4"
      was stale.) **`role_management/adjust/standby_failure_domains` — FIXED
      (11th bug): `qsort` tie-break instability.** `roles.c:143`
      `compareNodesForPromotion` returned 0 for equivalent candidates; glibc
      `qsort_r` is STABLE (ascending-id → node 2, Linux passes) but Windows UCRT
      `qsort_s` is NOT (→ node 3 → fail). Fixed with a deterministic total-order
      tie-break by node id (UNGUARDED — ascending-id == glibc's stable order, so
      Linux byte-identical; a genuine cross-platform determinism improvement).
      `role_management` 16/16; Linux re-verified (unit 320/320 + integration).
      **ALL WINDOWS FUNCTIONAL FAILURES NOW FIXED — 11 bugs total.** Updated the misleading `compat/win/unistd.h` comment that had
      claimed the coarse value was "still correct" for buffer.c — it wasn't. The
      OLD gateway note below is superseded (that too was a harness artifact,
      isolation. **AF_UNIX: user DECISION pending — abstract namespace (what the
      code uses everywhere) CANNOT work on Windows; see §7 for the fork.**
      (b) **`uv_fs.c` dir/file ops** — [x] DONE. Real Win32 bug: `UvFsSyncDir`
      couldn't open an existing dir without `FILE_FLAG_BACKUP_SEMANTICS`
      (`UV_FS_O_DIRECTORY`==0 on Windows) → `RAFT_IOERR`; fixed with a `_WIN32`
      `CreateFileA(OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS)`+`FlushFileBuffers`
      branch in `uv_fs.c`. Two harness fixes: `test/raft/lib/dir.c` `nftw`
      teardown used POSIX `remove()` (can't rmdir/read-only on Windows) → `_rmdir`
      + clear-readonly under `_WIN32`; 3 Linux-only-path tests in `test_uv_fs.c`
      → `MUNIT_SKIP`. `raft-uv-unit-test` `UvFs*` now exit 0. (`ext_uv`
      read/write are actually SOCKET tests → they belong to (a) networking.)
      All edits `_WIN32`-guarded, POSIX path verbatim in `#else`.
      (c) **2 vfs gaps** — [x] DONE. Both were the SAME root cause as the
      base-VFS fix: `sqlite3_vfs_find("unix")`==NULL on Windows. `VfsOpen/tmp`
      (NULL temp-file open) was production `vfs.c` line ~2304 `find("unix")`
      before delegating temp files to the base VFS → changed to `v->base_vfs`
      (UNGUARDED but Linux-identical: on Linux `find(NULL)`==the unix default
      since dqlite registers with `makeDflt=0`; same substitution already at
      vfs.c:761). `VfsShmLock` crash was `releaseUnix`, a TEST that exercises
      SQLite's OWN unix VFS (`find("unix")`→NULL deref) → `_WIN32` `MUNIT_SKIP`;
      dqlite's real `xShmLock` was already correct (sharedBusy/exclBusy/release
      3/3 pass). Full vfs suite green, `vfs_extra` 38/38. (d) `__declspec`
      exports for the public DLL API — [x] DONE (see §2). 0→42 public
      `dqlite_*` exports; macro tri-stated + `_WIN32`-guarded; raft kept
      internal. **Issues (b)(c)(d) all COMPLETE + Linux re-verified green after
      the batch: `unit-test` 320/320, `raft-uv-unit-test` 25/25.** Only (a)
      **networking** remains — user DECISION: use real `AF_UNIX` (see §7).
      Linux stays green (191/191, 25/25, 320/320).

**Port switches (all env vars / CMake options, default off = unchanged Linux):**
`DQLITE_IO_BACKEND=threadpool`, `DQLITE_IO_NO_DIRECT=1`, `DQLITE_VFS_NO_MREMAP=1`,
CMake `-DDQLITE_DISABLE_KAIO=ON`. Each forces a portable path so it is
regression-testable on Linux/WSL2.

The port is overwhelmingly concentrated in **`src/raft/`** (the libuv-based
persistence backend) plus a few spots in `src/`. The raft *algorithm* core
(log, election, replication, membership, etc.) is already portable C.

---

## 1. Build system (CMake)

- [x] `CMakeLists.txt` at repo root: sources, targets, tests, deps
- [x] Strip `-DNDEBUG` (dqlite asserts are load-bearing; `runner.h` calls
      `__assert_fail` directly)
- [x] Project-root-only include path (avoid shadowing system `<uv.h>` with
      `src/raft/uv.h`)
- [x] Windows dependency wiring via **vcpkg**: repo `vcpkg.json` (manifest,
      valid `builtin-baseline`) + CMake WIN32 branch uses `find_package` →
      `libuv::uv` / `unofficial::sqlite3::sqlite3` / `lz4::lz4`. **Windows CMake
      configure succeeds** (clang-cl 19 + Ninja + vcpkg; deps libuv 1.52.1,
      sqlite3 3.53.3, lz4 1.10.0). Linux build unregressed. `dqlite.lib`
      shared-vs-static name collision FIXED (static → `dqlite_static.lib` on
      Windows); ninja graph loads and compilation runs. Full categorized Windows
      compile-error inventory captured in **`PORT_WINDOWS_ERRORS.md`** — the
      source Win32 port work-list.
- [ ] Decide upstream story: keep autotools for Linux + add CMake, or migrate
      fully. (Recommend: add CMake, keep autotools authoritative on Linux until
      CMake reaches parity.)
- [ ] Port remaining `configure.ac` feature checks to CMake `check_*`
      (`AC_SYS_LARGEFILE`, pthread flags, backtrace/libunwind, coverage)
- [x] `--enable-backtrace` equivalent — `DQLITE_ENABLE_BACKTRACE` CMake option
      (2026-07-28), default ON on non-Windows / OFF on Windows. Replicates
      `configure.ac` precedence: `check_include_file(backtrace.h)`→`-lbacktrace`,
      elif `execinfo.h`→`-rdynamic`, elif pkg `libunwind-generic>=1.7`→libunwind,
      else FATAL_ERROR. Defines BOTH the master gate `DQLITE_ASSERT_WITH_BACKTRACE`
      AND the matching `HAVE_BACKTRACE_H`/`HAVE_EXECINFO_H`/`HAVE_LIBUNWIND_H`
      (the source in `src/lib/assert.c` needs both). Default Linux build now
      auto-detects execinfo+`-rdynamic` (parity with autotools default=yes);
      unit 320/320, raft-core 262/262. `CMakeLists.txt` only.
- [x] Code-coverage — `DQLITE_CODE_COVERAGE` CMake option (default OFF); when ON
      (non-MSVC) adds `--coverage` compile+link flags + a best-effort `coverage`
      lcov/genhtml target (skipped if tools absent). Verified: `--coverage` in
      compile/link cmds, 385 `.gcno` + 110 `.gcda` produced. WIN32/MSVC=warn+OFF.

---

## 2. Public API / symbol visibility

- [x] `include/dqlite.h`: `DQLITE_API` now tri-states on `_WIN32` —
      `__declspec(dllexport)` when `DQLITE_BUILDING` (set on `dqlite_objs` in
      CMake), `__declspec(dllimport)` for consumers, and a `DQLITE_STATIC`
      opt-out (no decoration) for internal/static-archive targets. Non-Windows
      branch byte-identical (`visibility("default")`). `dumpbin /exports`:
      **0 → 42 public `dqlite_*` functions** (88 total; import lib 1160→20892 B).
- [x] `src/raft.h`: `RAFT_API` kept internal — NO change; the port already
      compiles raft with `-DRAFT_API=` (blank) so its symbols never enter the
      DLL surface. `DQLITE_VISIBLE_TO_TESTS` inherits `DQLITE_API` unchanged.
- [x] Confirm `clang-cl` builds link cleanly against MSVC-ABI deps from vcpkg —
      DLL rebuilds zero-unresolved (582KB), sanity suites pass; CMake wires
      `DQLITE_STATIC` on `dqlite_test`/test exes (they recompile the API).

---

## 3. Async I/O backend  ← the core of the port

The raft log/segment writer is built directly on **Linux kernel AIO
(`io_setup`/`io_submit`/`io_getevents`) and io_uring**. No macOS/Windows
equivalent exists; this is the largest task.

- [x] `src/raft/syscall.c` / `syscall.h`: io_uring + AIO syscall wrappers
      (`<linux/aio_abi.h>`, `<linux/io_uring.h>`) now gated behind
      `DQLITE_HAVE_KAIO` — compiled out entirely when kernel AIO is absent.
- [x] `src/raft/uv_writer.c`: portable threadpool backend added
      (`DQLITE_IO_BACKEND=threadpool`, `uv_fs_write`), Linux-validated — all
      write-path suites 100%. AIO-only tests (`noResources`, `ioSetupError`)
      guarded to skip under this backend.
- [x] batched async segment writes via AIO — ASSESSED, deliberately NOT
      implemented (2026-07-28); MOOT. The current AIO path already submits
      exactly 1 iocb per `io_submit` (`uv_writer.c:512`), strictly sequential
      (`max_concurrent_writes==1`, `uv_append.c:483`; one write in flight per
      segment, each with its own AIO ctx). And `uv_append.c` ALREADY coalesces
      all pending appends into ONE vectored write per submit
      (`uv_append.c:419-435`), so there is nothing to batch at the syscall
      level. Raising concurrency would be pointless (already coalesced) AND
      unsafe — raft needs log entries durable in-order before ack, so multiple
      outstanding writes risk out-of-order/premature completion (post-crash log
      gap). The serialization is deliberate. Left the correct current path.
      (Same `max_concurrent_writes==1` basis as the IOCP decline above.)
- [x] Auto-select the threadpool backend when async I/O is unavailable
      (`UvWriterInit`: `threadpool = forced || !async`), so macOS/Windows (and
      `DQLITE_IO_NO_DIRECT` on Linux) use it with no env var. Linux-verified:
      under `NO_DIRECT` the writer routes to the portable path (`async=0 =>
      threadpool=1`); AIO-only tests skip on effective-backend, not just env.
- [x] Conditionalize the Linux-only AIO types/headers behind `DQLITE_HAVE_KAIO`
      (defined on Linux unless `DQLITE_DISABLE_KAIO`): the portable path no
      longer references `struct iocb` / `aio_context_t` / eventfd. **Verified by
      building on Linux with `-DDQLITE_DISABLE_KAIO=ON` (CMake option): 0 errors,
      portable-backend tests 100%** — a faithful proxy for macOS/Windows AIO
      exclusion. (Physical file split into `uv_writer_aio.c`/`_threadpool.c`
      deferred — cosmetic; `#ifdef` gating already achieves the portability.)
- [x] `src/raft/uv_os.h`: `aio_context_t` / `<linux/aio_abi.h>` gated behind
      `DQLITE_HAVE_KAIO`.
- [x] **macOS**: write path reimplemented on libuv's fs threadpool
      (`uv_fs_write`), auto-selected when async is unavailable. Kernel-AIO fast
      path is now Linux-only (behind `DQLITE_HAVE_KAIO`). (`kqueue`/GCD not
      needed — the libuv threadpool covers it.)
- [x] **Windows**: IOCP/OVERLAPPED fast path — ASSESSED, deliberately NOT
      implemented (2026-07-28); threadpool backend is the correct Windows path.
      Evidence: (A) hooking overlapped file writes onto libuv's loop needs
      `loop->iocp` (PRIVATE field, no user `uv_req_t` type) → a libuv fork,
      fragile/non-upstreamable. (B) a dedicated-IOCP + waiter-thread is just a
      hand-rolled threadpool with NO benefit because `max_concurrent_writes==1`
      (`uv_append.c:484`) → writes are sequential, no worker-pool contention to
      relieve. (C) `uv_poll` is sockets-only on Windows, and buffered
      overlapped writes complete synchronously anyway. The write path is
      durability-bound (`O_DSYNC`→`FILE_FLAG_WRITE_THROUGH`), so async only
      changes who waits, and the loop thread is already never blocked (the
      threadpool worker absorbs it). Revisit ONLY if concurrency>1 AND
      `FILE_FLAG_NO_BUFFERING` is adopted — and even then via option B, never
      libuv's loop IOCP. No code written; the new `UvWriterBackend` seam makes
      adding it later a localized change if ever warranted.
- [x] `raft_io_fs` write-backend abstraction — DONE (2026-07-28). Introduced a
      `struct UvWriterBackend` vtable (`init`/`submit`/`close`/`destroy`) in
      `uv_writer.c` (+`backend` ptr in `UvWriter`, `uv_writer.h`), with two
      impls: `aio` (`#if DQLITE_HAVE_KAIO` — owns aio_context/eventfd/poll +
      the io_submit/RWF_NOWAIT fast path + EAGAIN→threadpool fallback) and
      `threadpool` (always; `uv_fs_write` via `uv_queue_work`). Shared req
      bookkeeping + close/drain machine + `UvWriter`/`UvWriterReq` public
      contract UNCHANGED; segment/prepare/finalize/truncate callers untouched.
      Behavior-preserving refactor, verified on ALL THREE configs: default Linux
      AIO (write-path suites 100%, indep-confirmed raft-uv-unit 25/25 +
      append/truncate/recover green), threadpool (env + `-DDQLITE_DISABLE_KAIO`),
      and Windows (`raft-uv-integration` 212/212, 0 FAIL). The seam is shaped so
      a Windows IOCP backend and a Linux RWF_NOWAIT fast-path plug in as
      additional `UvWriterBackend` instances with NO change to shared logic.
- [x] `RWF_NOWAIT` fast-path preserved + fallback — CONFIRMED (2026-07-28,
      audited post-`UvWriterBackend`-refactor). All 4 `RWF_NOWAIT` sites
      (`uv_fs.c:1360` probe; `uv_writer.c:506` submit, `:537`/`:328` fallback)
      are `DQLITE_HAVE_KAIO`-gated. The aio backend's EAGAIN→threadpool fallback
      is intact at BOTH submit-time (`uv_writer.c:510-545`) and completion-time
      (`:325-332`), routing to `uvWriterWorkCb`→shared `req->cb(req,status)`. No
      `RWF_NOWAIT`/AIO leak into the portable path (KAIO-off build clean, no
      undefined symbols, portable `raft-uv-unit` 24/24). Pre-4.14 kernels
      degrade gracefully: the startup `probeAsyncIO` catches `EINVAL`/
      `EOPNOTSUPP` → `async=false` → threadpool selected, so the aio backend is
      never chosen there. Linux default: `raft-uv-unit` 25/25, `append/` 33/33.

## 4. Event notification (eventfd / timerfd)

- [x] `src/raft/uv_os.c`: `<sys/eventfd.h>` / `UvOsEventfd` gated behind
      `DQLITE_HAVE_KAIO` — eventfd is used only by the kernel-AIO completion
      path, which is compiled out on non-Linux. The portable backend needs no
      event fd (it completes via `uv_queue_work`'s callback). No `uv_async_t`
      replacement required.

## 5. Filesystem detection & direct I/O

- [x] `UvOsSetDirectIo` made portable (`src/raft/uv_os.c`): Linux `O_DIRECT` /
      macOS `F_NOCACHE` / else `UV_ENOTSUP`, behind platform guards so the file
      compiles without `O_DIRECT` (needs a file-level `_GNU_SOURCE` guard so
      glibc exposes `O_DIRECT`). Added `DQLITE_IO_NO_DIRECT` switch (`uv_fs.c`,
      in `UvFsProbeCapabilities`) forcing the buffered-I/O + fsync path.
      Linux-validated: write-path suites 100% under it (portable backend), no
      default regression. Remaining below: the `statfs`/magic FS-type probe.
- [ ] `src/raft/uv_fs.c`: `statfs` + `<linux/magic.h>` filesystem-type probing
      and `<xfs/xfs.h>` (`HAVE_XFS_XFS_H`) direct-I/O handling.
  - [~] **macOS**: DONE-BUT-UNVERIFIED (no Mac here). `uv_fs.c`/`uv_os.c`
        `<sys/vfs.h>`→`<sys/mount.h>`+`<sys/param.h>` under `__APPLE__`;
        `probeDirectIO` reports no-direct on Darwin (no `O_DIRECT`) → buffered/
        threadpool backend; `posix_fadvise` guarded out; `_DARWIN_C_SOURCE`
        added. All `__APPLE__`-guarded, PROVEN Linux/Windows byte-identical
        (`clang -E -P` diff), Linux green + KAIO-off proxy build clean. Needs a
        Mac + the CMakeLists `elseif(APPLE)` stub wired to actually build; a real
        compile will surface further gaps (vfs.c mmap, AF_UNIX, pthread/sem).
  - [x] **Windows**: `GetVolumeInformation` confirmed MOOT (2026-07-28).
        Windows never does aligned direct I/O (`FILE_FLAG_NO_BUFFERING`, the
        Win32 `O_DIRECT`, is deliberately unused — design is buffered+fsync on
        the threadpool), so FS-type detection is unnecessary. FS-probe FIX:
        Windows was misreporting `uv->direct_io=true` (prelude `#define
        O_DIRECT 0` → `UvOsSetDirectIo` is a no-op success → `probeDirectIO`'s
        buffered write "succeeded" with `*size=4096`); worked only by
        coincidence (threadpool chosen via `async=false`). Added an explicit
        `_WIN32` early-return in `probeDirectIO` (`*size=0`, mirroring
        `__APPLE__`) → `direct_io=false`, semantically correct + robust. Only
        `uv_fs.c` (`_WIN32`-guarded); Linux `raft-uv-unit` 25/25 byte-identical.
- [x] `RAFT_HAVE_ZFS_WITH_DIRECT_IO` probe (`/sys/module/zfs/version`) —
      CONFIRMED already gated: it is defined ONLY by `configure.ac` (autotools),
      never by the CMake build, so it is undefined on all CMake platforms
      (Linux/macOS/Windows). Its only C use (`test_uv_fs.c`) is wrapped in
      `#if defined(RAFT_HAVE_ZFS_WITH_DIRECT_IO)` → compiled out everywhere. No
      guard needed. (The ZFS `f_type` magic in `probeDirectIO` is already in the
      non-`__APPLE__` branch.)

## 6. SQLite VFS shared memory (WAL)

- [x] `src/vfs.c`: `mmap`/`mremap`/`munmap` with `MAP_SHARED` for WAL-index
      shared memory — COMPLETE + audited (2026-07-28). Gate `vfsNoMremap()`
      (vfs.c:1894): Linux default uses `mremap(MREMAP_MAYMOVE|MREMAP_FIXED)`
      (all `mremap` sites under `#ifdef MREMAP_MAYMOVE`, absent off-Linux);
      macOS/Windows/`DQLITE_VFS_NO_MREMAP` all take the `mmap(MAP_FIXED)`
      in-place-replace fallback. No dangling `mremap` refs off-Linux (grep-
      confirmed). Verified Linux 320/320 both default AND `NO_MREMAP=1`.
  - [x] **macOS**: no `mremap` — `DQLITE_VFS_NO_MREMAP` fallback added.
        Finding: the shm `mremap` calls are an address-replacement (COW
        shadowing) trick, not growth; fallback replaces the mapping in place
        with `MAP_FIXED` `mmap` (macOS-supported), guarded by
        `#ifdef MREMAP_MAYMOVE`. Linux-validated: `unit-test` 320/320 both
        with and without the fallback forced.
  - [x] **Windows**: `CreateFileMapping` / `MapViewOfFile` — DONE + audited.
        `compat/win/compat_win.c` `mmap`/`munmap`: `CreateFileMappingW` +
        `MapViewOfFile3`/`MapViewOfFileEx`; MAP_SHARED→`PAGE_READWRITE`
        (coherent), MAP_PRIVATE→`PAGE_WRITECOPY` (COW). Atomic `MAP_FIXED`
        replace via Win10-1803+ placeholders (`UnmapViewOfFile2 PRESERVE` +
        `MapViewOfFile3 REPLACE` — no free window, == `mremap(MREMAP_FIXED)`),
        with a pre-1803 unmap/remap fallback (GetProcAddress(kernelbase),
        SRWLOCK-serialized). `memfd_create`→temp file (DELETE_ON_CLOSE).
        Validated: `vfs_extra` 38/38, `VfsShmMap`/`VfsTruncate` pass, `stress`
        45/45 incl. multi-DB WAL-index concurrency (`readers=16,databases=4`).

## 7. Network transport

- [~] `src/server.c` + `src/lib/addr.c`: `AF_UNIX` / `sockaddr_un` for the local
      transport. TCP path DONE on Windows (WSAStartup, raw-fd→SOCKET via
      `uv_tcp_open((uv_os_sock_t)(uintptr_t)fd)`, `write/close`→`send/closesocket`,
      `accept4`+`SOCK_CLOEXEC`→`accept`+`ioctlsocket(FIONBIO)`; all `_WIN32`-guarded).
  - [ ] **macOS**: `AF_UNIX` works — likely minimal change.
  - [x] **Windows local transport = libuv named pipes — DONE (2026-07-28).**
        `@name` → `\\.\pipe\dqlite-<name>` (deterministic map in Windows-only
        `compat/win/dqlite_win_pipe.h`, shared by both ends). LISTEN: `server.c`
        `dqliteNodeBindPipe` uses native `uv_pipe_init`+`uv_pipe_bind` (not the
        POSIX socket/bind/transport__stream dance); empty `@` synthesizes a
        process-unique name. CONNECT: `CreateFileA(FILE_FLAG_OVERLAPPED)`+
        `_open_osfhandle`; fast-fail on `ERROR_FILE_NOT_FOUND` == POSIX
        `ECONNREFUSED` for the listen race (raft's timer retries — a blocking
        retry in `connect_work_cb` would pin a bounded-threadpool worker and
        deadlock). `transport__stream` classifies pipe(fd) vs SOCKET via
        `uv_guess_handle`==UV_NAMED_PIPE. **Critical scaling fix:** a
        non-overlapped pipe makes libuv burn ONE threadpool worker per read →
        a 5-node cluster (20 conns) exhausts the 4-worker pool and deadlocks;
        `FILE_FLAG_OVERLAPPED`+IOCP fixes it (synchronous handshake/client I/O
        then use overlapped helpers `DqliteWinPipeWriteAll`/`ReadTimed`, the
        latter honoring the caller's deadline for `roles.c`). Client
        (`protocol.c`) discriminates socket vs pipe via `getsockopt(SO_TYPE)`.
        Windows over pipes: `cluster` 23/23, `membership` 5/5, `server` 8/8,
        `client` 6/6, `fsm` 28/28, `node` local `@123` 25/26 (1 = unrelated
        `flock` fs-helper); pure-TCP path un-regressed (`conn` 7/7, TCP
        `node/*Inet` 6/6). All `_WIN32`-guarded; Linux re-verified green
        (unit 320/320, cluster/membership/role_management/client/fsm pass).
        **Known-open (NOT this batch):** `ext_uv`/endpoint `unix` family stays
        skipped on Windows — its harness wraps a pre-bound raw fd into a
        listener (`transport__stream`+`uv_listen`), which pipes can't model
        (`uv_pipe_bind` only); a harness rearchitecture, separable. Also:
        `integration server/stop_twice` HANGS on Linux/WSL2 — **PROVEN
        PRE-EXISTING: clean HEAD (no port work) hangs 6/6**, a
        `dqlite_server_stop` shutdown-timing issue under this env, out of scope.
  - [ ] ~~**Windows AF_UNIX — DECISION FORK (superseded: user chose named
        pipes above):**~~ dqlite uses the Linux ABSTRACT namespace EVERYWHERE
        (`addr.c` `@name`+leading-null; `server.c`/harness empty `sun_path`).
        Windows `<afunix.h>` `AF_UNIX` supports ONLY real FILESYSTEM-PATH sockets
        — abstract CANNOT bind. So "real AF_UNIX on Windows" is NOT a drop-in; it
        needs: (1) a new non-abstract address convention in `AddrParse` (Windows
        can't do `@`), (2) socket-FILE lifecycle (unlink-before-rebind, path-len
        limits) that doesn't exist today, (3) edits in `addr.c`/`server.c`/
        `test/lib/{endpoint,server}.c`. Options: **(A)** stay TCP-loopback on
        Windows + skip unix variants (CURRENT state — everything works over TCP);
        **(B)** implement filesystem-path AF_UNIX (real work, changes address
        format); **(C)** use libuv `uv_pipe_t` = Windows named pipes (idiomatic,
        but not literally AF_UNIX). **USER CHOSE (C) libuv named pipes
        (2026-07-27)** — separate workstream, deferred behind the user-prioritized
        multi-node join-crash debug. On (A) TCP-loopback as the interim.
        Abstract-unix test variants
        skipped on `_WIN32`: `ext_uv unix` (2), endpoint default flipped to tcp,
        `test/lib/server.c` `@ID`→TCP loopback, `test_node.c` setUp `@123`
        (~50 node tests fast-fail), raft-uv `ADDRINFO_TEST` tcp_connect×4/
        tcp_listen×6 + `invalidAddress`.
- [~] `pthread_*` — done via Win32 shim (see §0).
- [ ] `pthread_*` usage (`src/server.c`, threadpool) → libuv threads
      (`uv_thread_t`, `uv_mutex_t`) or keep pthreads via a shim on Windows.

## 8. Misc portability

- [x] Global `_GNU_SOURCE` → `_DARWIN_C_SOURCE` (macOS): `uv_fs.c`/`uv_os.c`
      already had it; added the guarded analog to `src/vfs.c` for consistency
      (`__APPLE__`-only, Linux/Windows byte-identical). CMake also appends
      `_DARWIN_C_SOURCE` globally on APPLE. Windows handling via the compat
      layer. (2026-07-28)
- [x] Large-file support — CONFIRMED already handled: `CMakeLists.txt` Linux
      branch defines `_FILE_OFFSET_BITS=64` (== `AC_SYS_LARGEFILE`), so `off_t`
      is 64-bit and matches libuv's `int64_t` fs offsets; Windows keeps UCRT's
      32-bit `off_t` (since W5) — every compat entry point taking an
      offset/length is declared `long long`, and no dqlite `off_t` value can
      exceed 2 GiB by construction. No change needed.
- [x] `DQLITE_PACKED` audit — CONFIRMED no-op: `src/utils.h` gates it on
      `__has_attribute(packed)`, which clang-cl 19 (the Windows toolchain)
      supports; a `#pragma pack` fallback would only matter for a pure-MSVC
      `cl.exe` build (not used), and `DQLITE_PACKED` is only used for internal
      `tracing.h` crash-records (not wire/on-disk format). No change.
- [x] Swept `src/` for Linux-only `/proc`,`/sys` runtime paths — NONE (only
      gated `<sys/syscall.h>`/AIO header includes). Confirmed guarded.
- [x] `#ifdef _WIN32` / `__APPLE__` guards added throughout the port as needed.

---

## 9. Testing & CI

- [x] **`integration-test` stress matrix GREEN on Windows (2026-07-28):
      125/173 → 170/170 (0 FAIL).** The `stress/read_write` `readers=16` crash
      uncovered TWO serious real bugs: (1) **WAL-index shm virtual-address
      aliasing / memory corruption** — the Windows `mmap` shim emulated
      `MAP_FIXED` as `UnmapViewOfFile`+`MapViewOfFileEx` (non-atomic); in the
      free window another worker's mapping grabbed the address → two databases'
      shm views aliased → corruption → spurious `SQLITE_NOMEM`. Fixed by
      rewriting `compat/win/compat_win.c` `mmap`/`munmap` to the Win10-1803+
      PLACEHOLDER APIs (`VirtualAlloc2 MEM_RESERVE_PLACEHOLDER` +
      `MapViewOfFile3 MEM_REPLACE_PLACEHOLDER` + `UnmapViewOfFile2
      MEM_PRESERVE_PLACEHOLDER`, via `GetProcAddress(kernelbase)` + fallback) —
      atomic replace, matching Linux `mremap(MREMAP_FIXED)`. Real
      production-affecting corruption under concurrent multi-DB load. (2)
      **named-pipe connect starvation** — libuv pipe listeners honor
      `uv_pipe_pending_instances()` (default 4), NOT the listen backlog → 32-64
      concurrent connects got `ERROR_PIPE_BUSY`; fixed with
      `uv_pipe_pending_instances(pipe,128)` in `dqliteNodeBindPipe` (`_WIN32`).
      (3) **`dqlite_server` restart node leak (~10KB)** — `dqlite_server_stop`
      never `destroy`s the node, `start` overwrites `server->local`; fixed
      (UNGUARDED, real cross-platform leak, fork-masked on Linux). Linux
      re-verified: unit 320/320, server/restart_* + start_twice pass. raft-uv
      AIO flakiness under WSL2 is environmental (separate).
- [ ] Port `test/lib/fs.c` / `test/raft/lib/dir.c` filesystem setup helpers
      (they assume Linux loopback filesystems; CI runs them under `sudo`).
- [ ] Add a GitHub Actions matrix: `{ubuntu, macos, windows} × clang`.
- [x] Sanitizer parity (ASan/UBSan) via Clang — **Linux CLEAN (2026-07-28)**:
      built `-fsanitize=address,undefined` (instrumentation confirmed), ran
      `unit-test` 320/320, `raft-core-unit` 262/262, `raft-uv-unit` 25/25 (incl.
      the `DQLITE_VFS_NO_MREMAP` portable-mmap branch) + `dqlite_server`
      restart path — **zero ASan/Leak/UBSan errors attributable to any
      port-touched file**; the unguarded `dqlite_server_start` leak fix
      confirmed free of UAF/double-free/leak. Only output = pre-existing UBSan
      pedantry in `munit.c:1077` (VLA) + untouched raft core (`start.c`/`byte.c`),
      which upstream CI tolerates in report mode. **Windows ASan (clang-cl 19)
      = CLEAN (2026-07-28)**: ALL suites 0 heap/UAF/double-free/stack errors
      (unit 319, raft-uv-integration 212/212, cluster/membership/server, stress
      45/45 incl. `readers=16,databases=4`) — placeholder `mmap`/`munmap` +
      pthread/sem shims all clean; NO source fix needed. Recipe (frictions
      solved): the DYNAMIC ASan runtime crashes at `__asan_init` on Win 26200
      (clang-19 `interception_win` bug) → use STATIC CRT `/MT` + static ASan
      runtime + an `x64-windows-static` vcpkg triplet (kept separate so the
      dynamic build-win is undisturbed); RelWithDebInfo with NDEBUG stripped +
      `/Od` (keeps asserts + dodges a C99-inline issue in replication.c);
      supply `clang_rt.asan*-x86_64.lib` explicitly via linker flags (lld-link
      ignores `-fsanitize`). CMakeLists got one WIN32-guarded static-triplet
      libuv alias (inert for the dynamic build/Linux). **`node/stopInflightReads`
      hang — RESOLVED as a build-config ARTIFACT, not a product bug (A/B'd):**
      passes normal Windows (dynamic libuv, 8/8), Linux (3/3), AND Linux+ASan
      (2/2); hangs ONLY on the Windows ASan build (static libuv + `/Od`). Cause:
      the ASan+unoptimized slowdown saturates loopback socket buffers so ~17/20
      read responses park at stop time, and dqlite's write-driven read flow
      can't interrupt a query parked on a socket write the (stopped) client
      never drains → `dqlite_node_stop`'s `pthread_join` waits forever. Not on
      any shipping config. (Latent fragility noted: stop defers transport-close
      until in-flight execs finish; a real fix would rearchitect shutdown
      ordering — not warranted for a synthetic slow-client+slow-build case.) For
      a fully-green Windows ASan suite, build ASan with `/O1` not `/Od`, or skip
      this one case under that config. `compat/win/` not exercised on Linux.)

---

## 10. Suggested sequencing

1. **macOS first** — closer to Linux (POSIX, `AF_UNIX`, `mmap`, `statfs`); the
   only hard part is the async-I/O backend. Proves the abstraction layer.
2. Extract the `raft_io_fs` abstraction while doing macOS, so Windows reuses it.
3. **Windows** — the heaviest lift (IOCP, file mapping, `__declspec`, vcpkg).
4. Upstream the abstraction + macOS backend first (lower controversy), then
   Windows.

## Notes

- Cross-platform *dependencies* (libuv, SQLite, lz4) are not blockers — all
  build on macOS/Windows (Homebrew / vcpkg).
- Keep Clang as the single toolchain family across platforms to minimize
  "compiles on GCC, breaks elsewhere" surprises.
- The raft algorithm core needs **no** changes — scope is the I/O backend + VFS
  shmem + transport + build/export plumbing.

## 11. Actions from fork review

Source: *"dqlite cross-platform forks: critical review & upstream PR roadmap"*
(2026-07-29), reviewing this branch (**Fork B**, `pw_windows_tests` @ `dd94af8`)
alongside `amachado-pie/dqlite@cross-portable` (**Fork A**, = upstream PR #899)
against `canonical/dqlite@ffae341`.
Artifact: `https://claude.ai/code/artifact/b97e139c-c58b-4d70-8c76-94125c2ebec4`

Order of business (as directed): **§11.1 = the §4.2 findings in review order**
(CRITICAL → MINOR), then **§11.2 = the §4.3 simplifications, planned**.

Every claim below was re-verified against the working tree before being written
down; file:line anchors are current as of `dd94af8` + the uncommitted `PORT_*.md`.

**Ground rules carried over from the review's §7** (they constrain *how* each
item is done, not whether):
- Every change must keep the Linux autotools build and its ASAN CI matrix green
  and unchanged. `Makefile.am`/`configure.ac` stay byte-identical to upstream.
- No Linux default or behaviour change except as an explicit, separately-argued
  bug fix in its own commit (see W6).
- Portability code that Linux CI cannot execute will bit-rot. The
  `DQLITE_DISABLE_KAIO` / `DQLITE_VFS_NO_MREMAP` / `DQLITE_IO_BACKEND` switches
  are the branch's strongest asset here — keep them working (but see W6c).
- Each item below should land as one or two commits reviewable in a sitting
  (<~400 functional lines), so the branch can be replayed as an upstream series.

### 11.1 Issues from review §4.2 (in order)

- [x] **W1 — CRITICAL: the raft log is not crash-durable on Windows.**
      *This is the single blocking correctness defect on the branch; do it first
      and do not let anything else land in front of it.*
      **DONE (2026-07-29), routes A+B both implemented.** (A) prelude `O_DSYNC`
      `0`→`0x04000000` (= libuv `UV_FS_O_DSYNC`, verified in vcpkg uv/win.h:693
      → `FILE_FLAG_WRITE_THROUGH` in fs__open; runtime-probed via
      `NtQueryInformationFile(FileModeInformation)` = mode `0x22`, flag SET);
      `_Static_assert(O_DSYNC == UV_FS_O_DSYNC)` in `uv_os.c` guards libuv
      upgrades. (B) `UvWriter.sync` (`_WIN32`-only field) detected at init via
      NtQueryInformationFile (GetProcAddress, no ntdll link);
      `uvWriterWorkCbPortable` issues `UvOsFdatasync` (FlushFileBuffers =
      device cache flush; WRITE_THROUGH alone only requests FUA, which consumer
      disks ignore) after every successful write; flush failure → RAFT_IOERR.
      `UvFsSyncDir` `_WIN32` branch now opens `GENERIC_READ|GENERIC_WRITE`
      (FlushFileBuffers on a read-only dir handle fails gle=5 — the old code's
      swallowed error meant the flush NEVER happened) and PROPAGATES failure;
      ACL-denied fallback = read-only open, flush skipped, NTFS metadata
      journaling ordering relied on (documented in comment + README). False
      "buffered+fsync" comment rewritten; README gained "Durability on
      Windows". **Regression found+fixed:** real write-through made the
      `_chsize_s` fallocate shim zero-fill at ~537ms/8MiB segment (starved
      loop-budgeted tests) → `SetFileInformationByHandle(FileEndOfFileInfo)`
      (metadata-only, 0.3ms, still reserves NTFS clusters so ENOSPC surfaces at
      allocate). The `~O_DSYNC` reopen pair is now live and correct
      (`UvFsAllocateFile` 6/6 under `fallocate=0`). Cost: ~1.1ms/op
      write+flush vs 0.039 buffered (NVMe) — the price of the guarantee; suite
      wall-time unchanged (fallocate fix pays it back). Verified:
      `raft-uv-integration` 212/212 (34 skip), `unit-test` 319/319,
      `raft-uv-unit` 20/20, `stress` 45/45. Linux proven byte-identical
      (all functional edits `_WIN32`-guarded; mechanical `-U_WIN32` view diff
      of the 4 shared files = identical). Pre-existing (baseline-reproduced,
      NOT W1): full `integration-test.exe` hangs at `node/stopInflightReads`
      on this machine — cf. §9's known ASan-build hang, now seen on the normal
      build too; needs its own investigation.
  - **Verified evidence.** (i) `compat/win/dqlite_win_prelude.h:182-184` defines
    `O_DSYNC` as `0` (self-documented as dropping the semantics). (ii)
    `src/raft/uv_fs.c:347` opens every segment with
    `open_flags = O_WRONLY | O_DSYNC`, i.e. plain `O_WRONLY` on Windows; the
    `~O_DSYNC` reopen dance at `uv_fs.c:377-412` is therefore a no-op pair.
    (iii) the portable write path — `uvWriterWorkCbPortable`,
    `src/raft/uv_writer.c:606-613` — is a bare `UvOsWrite` (`uv_fs_write`) with
    **no** `fsync`/`fdatasync`. (iv) `UvFsSyncDir`'s `_WIN32` branch
    (`src/raft/uv_fs.c:92-112`) deliberately ignores the `FlushFileBuffers`
    result. (v) the comment block at `src/raft/uv_fs.c:1223-1245` asserts a
    "buffered writes + fsync on the libuv threadpool" Windows design **that does
    not exist in code** — that false comment is itself part of the bug, because
    it is what makes the gap invisible on a read-through.
  - **Impact.** A Windows node can ack a raft entry, lose power, restart without
    it, and diverge from the cluster. Consensus-safety, not performance.
  - **Work.** Pick and implement one of:
    - **(A) flag route.** Make `O_DSYNC` map to libuv's `UV_FS_O_DSYNC` on
      Windows so `uv_fs_open` passes `FILE_FLAG_WRITE_THROUGH`. Feasible because
      `uvFsOpenFile` → `UvOsOpen` (`src/raft/uv_os.c:41`) hands `flags` straight
      to `uv_fs_open`, and libuv's Windows `UV_FS_O_*` values coincide with the
      UCRT `_O_*` ones it wraps. **Verify the actual `UV_FS_O_DSYNC` value in
      the vcpkg libuv headers before relying on this**, and check the
      `& ~O_DSYNC` fallocate-emulation branch still behaves (with a non-zero
      `O_DSYNC` that branch becomes live on Windows for the first time).
    - **(B) explicit flush route.** Track "this fd was opened O_DSYNC" on
      `struct UvWriter` and issue `uv_fs_fdatasync` at the end of
      `uvWriterWorkCbPortable` (still on the threadpool worker, so the loop
      thread is unaffected). More code, but platform-independent and testable on
      Linux via `DQLITE_IO_BACKEND=threadpool`.
    - Recommended: **A + B** — `WRITE_THROUGH` alone does not guarantee the
      drive's own cache is flushed, so keep an explicit flush for the full
      guarantee. Measure the cost; if it is severe, that is a finding to report,
      not a reason to silently skip it.
  - **Also required in the same batch:** decide and document the directory-sync
    story (NTFS has no directory fsync — state that the rename+metadata ordering
    is relied upon instead, and stop swallowing the `FlushFileBuffers` error
    silently); rewrite the false comment at `uv_fs.c:1223-1245`; update the
    prelude note at `:180-184`; and add a prominent "Windows durability" section
    to `README.md` stating exactly what is and is not guaranteed.
  - **Verification.** Confirm `FILE_FLAG_WRITE_THROUGH` is really set (Process
    Monitor, or a probe that opens a segment and inspects the handle); re-run
    `raft-uv-integration` (expect 212/212), `unit-test`, `integration` incl.
    `stress`; record the write-throughput delta before/after. Linux must be
    byte-identical — `clang -E -P` diff the touched files to prove it.

- [x] **W2 — CRITICAL: `mmap(MAP_FIXED)` failure is unrecoverable and unhandled.**
      **DONE (2026-07-29), two commits.** (1) Shim: MAP_FIXED failure now
      recovers — unmap-fails → old view intact, return MAP_FAILED/ENOMEM;
      replace-fails → RESTORE a view of the same section/offset at the same
      address (falls back PAGE_WRITECOPY→PAGE_READWRITE) then MAP_FAILED/
      ENOMEM; restore-also-fails → errno=ENOTRECOVERABLE + stderr diagnostic,
      range stays a reserved placeholder (never re-allocatable → cannot alias;
      later munmap=EINVAL by design). Pre-1803 MapViewOfFileEx fallback
      DELETED (unfixable unmap/map race): placeholders are a hard requirement,
      one-time diagnostic + ENOSYS, README documents the 1803+ floor.
      Test-only fault hook `DQLITE_WIN_MMAP_FIXED_FAIL_AT=N` (cached getenv,
      inert unset). (2) VFS (shared, option (a) as directed):
      `vfsShmPublishRegion`→int, `vfsPublishShm` propagates
      SQLITE_IOERR_SHMMAP — all error returns gated `vfsNoMremap()`; Linux
      mremap-path asserts verbatim (mechanical linux-view diff clean).
      Fault-sweep positions 1-6: publish-path failures recover transparently
      (test OK), redirect-path failures → clean `SQLITE 7 out of memory`, no
      AV/abort at any position. **Sweep found+fixed an 12th real bug (own
      unguarded commit, Linux-relevant):** `vfsMainFileShmLock` returned
      redirect failure while leaving the just-taken exclusive write lock +
      `exclMask` set → SQLite's retry tripped `PRE((f->exclMask & mask)==0)` =
      guaranteed deferred abort (reproduced: CRT exit 3); now releases the
      lock/mask on that path — upstream-reachable via vfsShmRemap's
      "should never happen" mmap failure. Verified: unit-test 319/319,
      stress 45/45, injection→exit 1 (clean FAIL) not 3 (abort).
  - **Verified evidence.** `compat/win/compat_win.c:833-869`: on the MAP_FIXED
    path the shim does `UnmapViewOfFile2(MEM_PRESERVE_PLACEHOLDER)` then
    `MapViewOfFile3(MEM_REPLACE_PLACEHOLDER)`; the cleanup at `:861-864` is
    `if (base == NULL && !(flags & MAP_FIXED))`, so a **MAP_FIXED failure leaves
    the range as a reserved `PAGE_NOACCESS` placeholder with no view** while
    SQLite still holds pointers into it — any subsequent access is an AV.
    Downstream, `vfsShmPublishRegion` (`src/vfs.c:1953-1975`) **has no error
    return at all**: it `dqlite_assert`s the result, and `NDEBUG` is
    deliberately stripped, so a transient mapping failure aborts the process.
    Its two callers are `src/vfs.c:2039` and `src/vfs.c:2100`
    (`vfsPublishShm`), which likewise cannot propagate. The unguarded `mmap` at
    `vfs.c:2032` and `:2049` also just `dqlite_assert(... != MAP_FAILED)`.
  - **Work.**
    1. **Shim side (Windows-only, cheap):** on MAP_FIXED failure, attempt to
       restore a usable view at the address (re-map the same section/offset) so
       the process stays in a defined state; if even that fails, set a distinct
       `errno` and emit a diagnostic before returning `MAP_FAILED`. Never return
       with the address left view-less.
    2. **VFS side (cross-platform, needs care):** give `vfsShmPublishRegion` an
       `int` return and thread it through `vfsPublishShm` → its callers. Note
       this touches the **Linux** `mremap` path too, where upstream's own code
       asserts; so either (a) keep the Linux assert semantics verbatim and only
       return errors from the `vfsNoMremap()` branch, or (b) propose the full
       error propagation as its own separately-argued upstream commit. Prefer
       (a) on this branch, with (b) noted as a follow-up — it is a genuine
       upstream robustness improvement but it is not a Windows fix.
    3. **Pre-1803 fallback:** the `else` branch at `compat_win.c:864-870`
       (`UnmapViewOfFile` then `MapViewOfFileEx`) has a real unmap-then-map race
       that `dqliteMmapLock` cannot close (another allocator in the process can
       take the address). It is also untestable on modern Windows. **Recommend
       deleting it** and hard-requiring Win10 1803+ (long EOL below that) with a
       clear `CreateFileMappingW`-time error and a README note. If it is kept
       instead, add a test-only hook to force `dqliteHavePlaceholders()` false so
       the path is at least exercised, and document the race in the header.
  - **Verification.** With the placeholder path forced off, run
    `stress` at `readers=32,databases=4` — that is the exact configuration that
    exposed the original aliasing corruption (§9). Then re-run with placeholders
    on. Fault-inject a `MapViewOfFile3` failure (e.g. via a counter in the shim)
    and confirm the process degrades to a SQLite error rather than an AV/abort.
    Linux: `unit-test` 320/320 both default and `DQLITE_VFS_NO_MREMAP=1`.

- [x] **W3 — MAJOR: header shadowing is a live structural hazard.**
      **DONE (2026-07-29), one commit (= S4).** (1) Fence: every header under
      `compat/win/` (28 files incl. `dqlite_win_pipe.h`) now `#error`s unless
      `_WIN32` && `DQLITE_WIN_COMPAT`; the macro is defined ONLY by
      `dqlite_win_prelude.h`, which itself `#error`s outside `_WIN32` — so
      only TUs force-included with the prelude (/FI, every dqlite TU) can use
      the shims, and any other TU that resolves a generic name breaks loudly.
      Proven with a probe TU (`-Icompat/win`, no /FI): silently compiled
      against the pthread stub before, hard `#error` after. (2) Step 2's
      "move off the include path" judged NOT feasible in the cheap cage —
      dqlite's own sources hardcode `#include <unistd.h>`-style lines that
      must keep resolving; that is S1's job (noted in the prelude comment).
      (3) `compat/win/uv/unix.h` (the one shim shadowing a REAL vcpkg header)
      DELETED; its only consumer `src/lib/threadpool.c:9` now guards the
      include with `#ifndef _WIN32` (include still made on Linux — hunk is
      guard-only, Linux preprocessed output unchanged; the only edit outside
      `compat/win/`). PORT_WINDOWS_ERRORS.md §"shim headers" updated to match.
      Verified: full rebuild clean; `unit-test` 319/319, `raft-uv-unit` 20/20,
      `raft-uv-integration` 212/212, `server` 8/8, `stress` 45/45.
  - **Verified evidence.** `CMakeLists.txt:291` puts `compat/win` on the include
    path (PRIVATE, but ahead of the vcpkg dirs) for every target, and that
    directory holds generically-named headers: `pthread.h`, `semaphore.h`,
    `unistd.h`, `dirent.h`, `sched.h`, `poll.h`, `netdb.h`, `ftw.h`,
    `libgen.h`, `dlfcn.h`, plus `sys/{file,mman,random,socket,statvfs,syscall,
    time,uio,un,utsname,vfs}.h`, `netinet/{in,tcp}.h`, `arpa/inet.h`,
    `linux/{limits,magic}.h`. The mechanism is **proven live**:
    `compat/win/uv/unix.h` exists specifically to shadow the *real installed
    vcpkg libuv header*. Any current or future dependency that includes one of
    those names gets dqlite's declaration-only stubs — or, worse, a
    `pthread_mutex_t` whose layout disagrees with a real pthreads
    implementation, which the compiler cannot diagnose.
  - **Work (cheap cage now; see S1 for the real fix).**
    1. Add `#ifdef _WIN32` + a project fence to every compat header: outside
       Windows, or when a project-internal macro is absent, `#error` rather than
       silently defining stubs. That converts an invisible ABI hazard into a
       build break.
    2. Prefer force-inclusion over the include path for the generic names: move
       the shims into `compat/win/posix/` (NOT on the include path) and pull
       them in from `dqlite_win_prelude.h`, so a third-party TU's
       `#include <pthread.h>` resolves to the real header.
    3. Audit `compat/win/uv/unix.h`: the only consumer is
       `src/lib/threadpool.c`'s hardcoded `#include <uv/unix.h>`. Replacing that
       one line with a `#ifndef _WIN32` guard removes the need to shadow a real
       dependency header at all — do that instead of keeping the stub.
  - **Verification.** Add a throwaway TU that includes a vcpkg header known to
    pull `<unistd.h>`; confirm resolution order with clang-cl `/showIncludes`
    before and after. Full Windows build + suites. Zero Linux diff by
    construction (the directory is WIN32-only).

- [x] **W4 — MAJOR: the entire Windows build compiles with warnings off.**
      *(Same work as review §4.3 item 3 — do it here, once. Highest
      payoff-per-line item on the list.)*
      **DONE (2026-07-29), two commits (= S3).** (1) Flag enable:
      `elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")` branch in
      `dqlite_common_flags()` forwards
      `/clang:-fno-strict-aliasing /clang:-Wall /clang:-Wextra
      /clang:-Wno-unknown-warning-option /clang:-Wno-conversion` — surfaced
      **710 warnings** (655 `-Wunused-parameter`, 22 `-Wgnu-folding-constant`,
      11 `-Wunused-function`, 4 `-Wsometimes-uninitialized`,
      2 `-Wsign-compare`). (2) Triage to **zero**, no shared-code defects
      found: munit's `MUNIT_UNUSED`/psnip-clock attribute gates widened
      `__GNUC__`→`|| __clang__` (666 warnings — test code was already
      annotated, the gate just missed clang-cl; Linux truth-value unchanged);
      prelude declares UCRT `_wassert` `__declspec(noreturn)` (UCRT omits it,
      glibc's `__assert_fail` has it) so the shared `default: dqlite_assert(0)`
      idiom stops tripping `-Wsometimes-uninitialized` — fixed at the platform
      layer, diagnostic stays live; two justified suppressions:
      `-Wno-gnu-folding-constant` tree-wide (vendored raft tests, gcc has no
      equivalent diagnostic) and per-file `-Wno-sign-compare` for
      `src/lib/addr.c` (Winsock `socklen_t` is signed int / `ai_addrlen` is
      `size_t`; file must stay byte-identical on Linux). Verified: clean
      rebuild 0 warnings/0 errors; `unit-test` 319/319, `raft-uv-unit` 20/20,
      `raft-uv-integration` 212/212 (re-run mandatory since
      `-fno-strict-aliasing` changes codegen), `server` 8/8, `stress` 45/45.
  - **Verified evidence.** `CMakeLists.txt:295` gates
    `-fno-strict-aliasing -Wall -Wextra -Wno-unknown-warning-option
    -Wno-conversion` behind `if(NOT MSVC)`, and **clang-cl sets `MSVC=1`** — so
    the Windows build gets none of them. The file's own comment at
    `CMakeLists.txt:273-275` calls `-fno-strict-aliasing` *load-bearing* ("the
    wire/encoding code type-puns"), and the port itself adds strict-aliasing-
    hostile punning (the `FILE_RENAME_INFO` handling in `src/server.c`).
  - **Work.** Add a clang-cl branch to `dqlite_common_flags()`:
    detect `CMAKE_C_COMPILER_ID MATCHES "Clang" AND MSVC` and pass
    `/clang:-fno-strict-aliasing /clang:-Wall /clang:-Wextra
    /clang:-Wno-unknown-warning-option /clang:-Wno-conversion`. Then **triage
    the resulting warning flood**: expect a large first batch across
    `compat/win/` and the `_WIN32` blocks. Fix real defects; for genuine noise
    add narrowly-scoped `-Wno-` with a one-line justification each. Do the flag
    change and the fixes as separate commits so the diff is reviewable.
  - **Verification.** Warning count before/after; every fix re-run through the
    Windows suites. `-fno-strict-aliasing` newly applied may change codegen —
    re-run `raft-uv-integration` (212/212), `unit-test`, `stress`. Linux
    untouched (the branch is WIN32/clang-cl-only).

- [x] **W5 — MAJOR: CRT guard-macro hijacks.**
      **DONE (2026-07-29), three commits.** (1) Root cause, unguarded +
      upstreamable: both `src/server.c` comparisons now
      `(int64_t)full_size > (int64_t)SSIZE_MAX` — casting the limit *down*
      truncated on any platform with `off_t` narrower than `ssize_t`; Linux
      value-identical (`off_t` already 64-bit under `_FILE_OFFSET_BITS=64`).
      (2) `_OFF_T_DEFINED` hijack DELETED — prelude now `#include
      <sys/types.h>` (UCRT's own 32-bit `off_t`); audit found no remaining
      64-bit-through-off_t site (compat entry points all declare `long long`:
      mmap/ftruncate/pread/pwrite/posix_fallocate; UCRT `lseek` returns
      `long`; raft metadata/segment + vfs shm sizes ≪2 GiB by construction),
      so no dqlite-owned typedef remains and the fallback `_Static_assert`s
      are moot. (3) `fcntl()` honest: caller enumeration found the needed
      command set EMPTY on Windows (endpoint.c's call is `#else`-guarded and
      its `_WIN32` branch uses `ioctlsocket(FIONBIO)`; `UvOsSetDirectIo` only
      reached fcntl because the prelude defined `O_DIRECT 0` — that define is
      deleted, routing it to its existing honest `UV_ENOTSUP` branch with NO
      edit to uv_os.c) → fcntl now fails every command `-1`/`ENOSYS`. The
      `probeDirectIO` `_WIN32` early return is kept — it IS the correct
      Windows answer (and the ENOSYS failure would otherwise escalate to
      RAFT_IOERR via the `rv != UV_EINVAL` check) — but no longer masks a
      lie; stale "IMPLEMENTATION deferred" fcntl/accept4 comments rewritten.
      Verified: rebuild 0 warnings; `unit-test` 319/319, `raft-uv-unit`
      20/20, `raft-uv-integration` 212/212, `server` 8/8 (the suite that
      regressed originally), `stress` 45/45, `cluster` 23/23.
  - **Verified evidence.** `compat/win/dqlite_win_prelude.h:80-83` pre-defines
    the UCRT-internal guard `_OFF_T_DEFINED` and supplies
    `typedef __int64 off_t` (keeping `_off_t` as `long`). It fixes a real bug —
    `full_size > (off_t)SSIZE_MAX` truncated to `-1`, failing
    `dqlite_server_start` unconditionally — but hijacking a CRT guard risks a
    silent ABI mismatch if any UCRT or vcpkg header disagrees, and the compiler
    cannot diagnose it. Separately, `fcntl()` (`compat/win/compat_win.c:111-121`)
    **returns success for every command while doing nothing**, so
    `F_SETFL|O_DIRECT` "succeeds" — which is precisely why `probeDirectIO` needs
    a `_WIN32` early return (`src/raft/uv_fs.c:1223-1245`) purely to neutralise
    a false positive.
  - **Work.**
    1. Fix the root cause instead of the type: the `(off_t)SSIZE_MAX` cast in
       `src/server.c` is wrong on *any* platform where `off_t` is narrower than
       `ssize_t`. Change the comparison to use an explicit 64-bit type
       (`int64_t`/`uv_off_t`). That is an **unguarded, upstreamable correctness
       fix** — argue it on its own merits in its own commit.
    2. Then delete the `_OFF_T_DEFINED` hijack and re-run the Windows suites. If
       any other site still needs a 64-bit `off_t`, fix that site the same way
       rather than reinstating the typedef. If the typedef genuinely cannot go,
       add a `_Static_assert(sizeof(off_t) == 8, ...)` in the prelude and a
       second one in a TU that includes UCRT headers *after* it, so a future
       disagreement is a compile error.
    3. Make `fcntl()` honest: return `-1`/`ENOSYS` (or `EINVAL`) for commands it
       does not implement, and succeed only for the ones callers actually need.
       Enumerate the callers first (`grep -rn "fcntl(" src/ test/`). Keep the
       `probeDirectIO` early return — it is semantically correct for Windows —
       but it must stop being load-bearing.
  - **Verification.** Full Windows suites after each of the three steps; the
    `server` suite (8/8) is the one that regressed originally. Linux: the
    `off_t` comparison fix is unguarded, so re-run `unit-test` 320/320 +
    `integration server/`.

- [x] **W6 — MAJOR: unguarded Linux behaviour changes in a "Windows build" commit.**
      Five distinct changes; each needs its own decision and its own commit.
      **DONE (2026-07-29/30), five commits (a–e).**
      (a) KAIO-in-threadpool RESTORED as the Linux `!async` behaviour:
      `threadpool = uvWriterThreadpoolForced()` only, portable backend
      reachable on KAIO builds solely via `DQLITE_IO_BACKEND=threadpool`.
      Direct proof the mechanism had diverged: `UvWriterSubmit/noResources`
      on tmpfs FAILED at HEAD (portable write succeeded where upstream KAIO
      fails RAFT_TOOMANY), passes after. tmpfs benchmark (WSL2): mechanisms
      performance-equivalent within noise (numbers in the commit message).
      Full base-vs-mod WSL matrix (default / tmpfs / threadpool-forced /
      no-direct / DISABLE_KAIO build): zero mod-only regressions; two
      KAIO-behaviour tests gained skip guards. Windows unaffected (always
      portable). (b) Tie-break KEPT; the missing half added — two pinning
      tests (voter + standby promotion among fully-equivalent candidates →
      ascending id) pass on glibc (stable mergesort) and UCRT (unstable
      introsort) alike; standalone upstream argument in the commit message.
      (c) Switches KEPT, silence removed: one-time stderr warning when any of
      the three is honoured (stderr because both tracers are opt-in/disabled
      by default; verified each fires exactly once via munit
      `--no-fork --show-stderr` — munit captures test stderr, so normal suite
      output hides them by design). **Flagged for maintainers, not decided:**
      whether release builds should compile the switches out (test/CI-only
      build option). (d) Stress params reverted to upstream (count 1000,
      readers {0,1,4,16}); the aliasing-bug load (count=1500, writers=4,
      readers=32, databases=4) kept as ONE commented extra parameter set.
      Suite count 45→46 run (+3 skips) on both platforms. (e) Leak fix KEPT;
      audit confirmed no public API exposes the internal node; ownership
      contract documented at `dqlite_server_start` declaration + destroy
      site. Verified totals: Windows unit 321/321 (+2 new tests), raft-uv-unit
      20/20, raft-uv-integration 212/212, server 8/8, cluster 23/23, stress
      46/46; Linux (WSL) unit 322/322, stress 46/46.
  - **(a) The `!async` path on Linux changed mechanism.**
    `src/raft/uv_writer.c:709` — `threadpool = uvWriterThreadpoolForced() ||
    !async` — routes `!async` to the portable `uv_fs_write` backend. Upstream
    (`ffae341:src/raft/uv_writer.c:68-130`, `uvWriterWorkCb`) instead ran **KAIO
    synchronously in the threadpool** (`io_submit` + `io_getevents` on a
    dedicated context). Real Linux configurations hit `!async` — tmpfs, ZFS,
    and any filesystem where `probeAsyncIO` fails. **Action:** restore
    upstream's KAIO-in-threadpool as the `!async` behaviour on Linux (route
    `w->async == false` into the aio backend's blocking submit rather than the
    portable backend), and make the portable backend reachable on Linux only by
    explicit opt-in. Benchmark both on tmpfs before finalising, and state the
    numbers in the commit message. This is the most likely of the five to draw a
    maintainer objection, so it must be provably a no-op.
  - **(b) `roles.c` consensus-relevant tie-break.** `src/roles.c:165-181` adds a
    final ascending-`id` tie-break to `compareNodesForPromotion`. The reasoning
    holds (glibc `qsort_r` is a stable merge sort, so ascending-id reproduces
    the order Linux already produced; UCRT `qsort_s` is not stable). **Action:**
    keep it, but split it into its own commit with that argument in the message
    and add a test that pins the ordering for fully-equivalent candidates, so it
    can be sent upstream as a standalone determinism improvement (roadmap PR #7)
    rather than riding inside a Windows commit.
  - **(c) Three env vars can silently downgrade a production Linux binary.**
    `DQLITE_IO_BACKEND` (`uv_writer.c:54-60`), `DQLITE_IO_NO_DIRECT`
    (`uv_fs.c:1202-1206`), `DQLITE_VFS_NO_MREMAP` (`vfs.c:1894-1908`). They are
    also the branch's anti-bit-rot testability lever, which the review credits
    as the key to keeping the port alive in Linux-only CI — so do **not** just
    delete them. **Action:** keep the mechanism, remove the silence: emit a
    prominent one-time warning through the tracing/log path whenever a switch is
    honoured, and consider gating them behind a build option that is ON for
    test/CI builds and OFF for release. Flag the release-default question for
    the maintainers rather than deciding it unilaterally.
  - **(d) Stress-test parameters raised with no explanation.** Verified in
    `test/integration/test_stress.c`: `READ_COUNT`/`WRITE_COUNT` 1000 → 1500 and
    `readers` `"16"` → `"32"`. **Action:** revert to upstream values and, if the
    higher load is what actually exposed the shm aliasing bug (§9), add the
    heavier configuration as an *additional* parameter set with a comment saying
    exactly that — don't silently make everyone's CI slower.
  - **(e) `dqlite_server_start` destroys the previous node.** A real
    cross-platform leak fix (~10KB per restart, §9) but a potential
    use-after-free for a caller still holding the old node pointer. **Action:**
    keep the fix; audit `include/dqlite.h` for any API that hands the node
    pointer to the caller; document the ownership contract at the declaration;
    and if a caller *can* retain it, gate the destroy or version the behaviour.
    Own commit, argued as a leak fix.

- [x] **W7 — MINOR: shim semantics that are quietly wrong.**
      **DONE (2026-07-30), two commits** (a+b compat shims; c+d shared-file
      fixes). (a) `nanosleep` (the surviving sleep shim post-S1; `usleep` was
      already deleted with zero consumers) no longer ms-truncates:
      `dqliteWinNanosleep()` uses `CreateWaitableTimerExW` +
      `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` (same Win10-1803 floor as W2's
      placeholders), fallback plain timer → documented 1ms-min `Sleep`, never
      a spin. Measured: 500µs request 0.13µs/call (yield/busy-spin) →
      ~1.04ms (timer floor ~0.5ms); 1000µs 15.5ms→1.5ms. Per-call handle
      (create+close ~1µs ≪ timer floor; TLS cache would leak per exited
      thread). Stress wall-time unchanged (−0.5%, noise). (b) `pread`/`pwrite`
      now truly positional: `ReadFile`/`WriteFile` + `OVERLAPPED` offset,
      file position saved/restored (libuv's approach), `ERROR_HANDLE_EOF`→0,
      ENOSPC mapped (fallocate emulation keys on it). Caller audit CONFIRMED
      the old single-thread-per-fd invariant at all four sites (server.c,
      vfs.c under the exclusive WAL write lock, uv_os.c worker-owned fd,
      test_compress.c) — recorded in the commit. (c) The four
      `errno == EINTR` checks after WSAPoll/recv/send in
      src/client/protocol.c now fail fast under `_WIN32` with the
      unreachability documented (Winsock doesn't set errno; WSAEINTR is
      impossible without Winsock 1.1's WSACancelBlockingCall) — POSIX
      branches token-identical. (d) Correction to this item: the alignment
      test was NOT deleted — it survived with the assert `#ifndef _WIN32`-
      guarded (asserting nothing on Windows). Now: relaxed contract
      documented at `raft_aligned_alloc` (src/raft.h) + heap.c, and the
      test's Windows branch pins `MEMORY_ALLOCATION_ALIGNMENT` (16); Linux
      branch unchanged (full 1024-byte assert). Note: heap tests live in
      raft-core-INTEGRATION, not -unit. Verified: Windows rebuild 0 warnings,
      unit 321/321, raft-uv-unit 20/20, raft-uv-integration 212/212,
      raft-core-unit 262/262, raft-core-integration 191/191, server 8/8,
      stress 46/46; WSL unit 322/322, raft-core-integration 191/191;
      clang -E -P token streams identical for all four shared files.
  - **(a) Sub-millisecond sleeps become busy-spins.**
    `compat/win/unistd.h:140-145` (`usleep`) and `:237-246` (`nanosleep`)
    truncate to whole milliseconds via `Sleep`, so any sub-ms request becomes
    `Sleep(0)` — a yield, not a sleep. `src/vfs.c`'s WAL-contention backoff
    therefore spins under contention. **Fix:** use a high-resolution waitable
    timer (`CreateWaitableTimerExW` + `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`,
    Win10 1803+ — same floor as W2), falling back to a documented 1 ms minimum.
    Measure the WAL-contention path before/after (`stress` with high `writers`).
  - **(b) `pread`/`pwrite` mutate the shared file offset.**
    `compat/win/unistd.h:195-210` are `_lseeki64` + `_read`/`_write`, i.e. not
    positional and not atomic — safe today only because callers are
    single-threaded per fd. **Fix:** reimplement on `ReadFile`/`WriteFile` with
    an `OVERLAPPED` offset (no handle-offset mutation, no seek race). Audit
    callers first to confirm the current invariant, and record it either way.
  - **(c) `errno` checked after Winsock calls.** `src/client/protocol.c:215`
    and `:252` (after `poll`, which is `WSAPoll` on Windows) and `:322`/`:347`
    check `errno == EINTR`; Winsock reports through `WSAGetLastError()` and does
    not set `errno`. The same commit does it correctly in `test/lib/endpoint.c`.
    **Fix:** add a `_WIN32` branch translating `WSAGetLastError() == WSAEINTR`
    (and confirm whether the retry is reachable at all on Windows — if not, say
    so in the comment rather than leaving dead-but-wrong code).
  - **(d) `aligned_alloc` → `malloc` voids a documented contract.**
    `compat/win/dqlite_win_prelude.h:268-290`. The reasoning is correct
    (`_aligned_malloc` memory cannot be `free()`d, and C11 requires that it
    can), and Windows needs no alignment because direct I/O is off — but the
    alignment *test* was deleted rather than the relaxed contract documented.
    **Fix:** restore the test with a Windows-appropriate expectation (or a
    documented skip naming this reason), and document the platform-dependent
    alignment guarantee at `raft_aligned_alloc` in `src/raft.h` and
    `src/raft/heap.c:34`.

- [x] **W8 — MINOR: a library constructor mutates host-process-global state.**
      **DONE (2026-07-30), one commit.** Constructor REMOVED (not kept as
      belt-and-braces). (i) CRT abort/report quieting moved to new
      `test/lib/win.c`, compiled directly into each test executable via the
      `dqlite_test()` WIN32 branch (deliberately NOT into the convenience
      archive — an unreferenced constructor-only member would never be pulled
      in, the exact link-order-luck failure being removed). (ii) Winsock init
      made structural: `dqliteWinSocketsInit()` (INIT_ONCE one-shot
      WSAStartup 2.2, no WSACleanup — teardown reclaims it and cleanup tied
      to object counts could yank Winsock from under libuv/host) called from
      `dqlite_node_create` + `dqlite_server_create`, through which every
      socket-using library path is reachable (audited include/dqlite.h — no
      other public function touches sockets first). Probe-verified: consumer
      binary w/o dqlite call → `getaddrinfo` fails WSANOTINITIALISED (hidden
      init gone); after create → works; abort() in a consumer runs WER again
      (flags identical to dqlite-free baseline) while a harness-style binary
      aborts silently, exit 3, <100ms. Verified: rebuild 0 warnings; unit
      321/321, raft-uv-unit 20/20, raft-uv-integration 212/212, raft-core-unit
      262/262, raft-core-integration 191/191, fuzzy 57/57, stress 46/46,
      server 8/8, cluster 23/23, membership 5/5, client 6/6. Linux hunks all
      guarded (gcc -E -P token streams identical for src/server.c).
  - **Verified evidence.** `compat/win/compat_win.c:65-82`:
    `__attribute__((constructor)) dqliteWinQuietCrt()` calls
    `_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT)`, which
    **suppresses WER crash dumps for any process that embeds dqlite**, in
    release builds, with no opt-out. The same constructor is where `WSAStartup`
    lives, so Winsock initialisation also depends on the linker happening to
    pull `compat_win.obj` out of a plain static library.
  - **Work.** (i) Move the abort-behaviour change out of the library: put it in
    the test harness (`test/lib/`) or gate it on a test-only macro, so shipping
    consumers keep their crash dumps. (ii) Make Winsock init **structural** —
    refcounted `WSAStartup`/`WSACleanup` from `dqlite_node_create` /
    `dqlite_initialize` (and the client entry points), rather than relying on
    constructor pull-in. If the constructor is kept as a belt-and-braces path,
    force the object in with `/INCLUDE:` so the dependency is explicit.
  - **Verification.** Confirm a crash in a test binary still produces the
    expected non-interactive output; confirm a *consumer* linking
    `dqlite_static.lib` gets WER back. Re-run the socket suites (`conn`,
    `lib_addr`, `lib_transport`, `client`) to prove Winsock is still up.

- [x] **W9 — MINOR: misleading surface in `compat/win/`.**
      **DONE (2026-07-30), one commit** (post-S1 re-audit — S1 had already
      deleted the threading shims). (1) Six dead headers deleted with
      per-symbol grep proofs + fence/rebuild backstop: dirent.h, sys/uio.h,
      netinet/tcp.h, dlfcn.h, libgen.h, sys/utsname.h; compat_win.c shed
      uname() (~50 lines) and dead includes; a full live/dead map of the 18
      surviving headers (all live, consumers named) is in the W9 agent
      report/commit. compat/win now 18 files / 2352 lines (was 30/~3000 at
      review time). (2) Stale "IMPLEMENTATION deferred" banners corrected in
      ftw.h, sys/file.h, sys/statvfs.h, sys/socket.h, sys/vfs.h,
      linux/magic.h, mkstemp, accept4. (3) Falsehoods: `getuid()`=0 KEPT,
      labelled DELIBERATE FALSEHOOD naming its exact two test callers;
      `uname()`="10.0" ELIMINATED (only caller kernel_version_above_equal
      got a `_WIN32` false-stub — the semantically correct answer);
      `sysconf(_SC_PAGESIZE)` now returns the TRUE dwPageSize (4KiB), the
      64KiB unit moved to a named `dqlite_win_allocation_granularity()`
      called only by `vfsGetMapSize()` — runtime values unchanged at every
      site, names now match meanings, buffer.c and test mirrors collapsed to
      upstream-verbatim sysconf lines. (4) test_uv_tcp_listen.c settle loop:
      the item's evidence was stale (the IOCP-coalescing reason was already
      in the macro comment); extended it to record that no test-visible
      condition exists to wait on (partial-handshake state is private to
      uv_tcp_listen.c), so the bounded poll loop stays. Verified: Windows
      rebuild 0 warnings, unit 321/321, raft-uv-unit 20/20,
      raft-uv-integration 212/212, server 8/8, stress 46/46; WSL unit
      322/322, raft-uv-integration 237/237, stress 46/46.
  - **Verified evidence.** Dead-or-vestigial headers with declared-but-
    unimplemented symbols: `compat/win/dirent.h` (42 lines),
    `compat/win/sys/uio.h` (21), `compat/win/netinet/tcp.h` (13),
    `compat/win/dlfcn.h` (24), `compat/win/libgen.h` (13) — 19 symbols across
    the five. Roughly ten header banners still say "IMPLEMENTATION deferred" for
    code that *is* implemented. Three deliberate falsehoods that current code
    depends on: `getuid()` → 0, `uname().release` → `"10.0"`, and
    `sysconf(_SC_PAGESIZE)` → 64 KiB. Timing-based test fix at
    `test/raft/integration/test_uv_tcp_listen.c:188` (`uv_sleep(1)` poll loop)
    replaces an event-driven wait.
  - **Work.**
    1. Delete each dead header after proving nothing includes it (remove, then
       rebuild; or check clang-cl `/showIncludes`). Keep only what is reached.
    2. Correct the stale "deferred" banners — a wrong banner is worse than none.
    3. For each of the three falsehoods: add a comment naming the **exact
       caller** it exists for, and prefer converting the call site to a
       `dqlite_win_*`-prefixed guarded helper so a future caller cannot be
       silently burned. `sysconf(_SC_PAGESIZE)` is the dangerous one — it has
       **already** leaked once, into `src/lib/buffer.c`'s row-batch threshold
       (bug #10, §0). Strongly consider removing the `sysconf` lie entirely:
       return the true `dwPageSize` and have `src/vfs.c` ask for the allocation
       granularity explicitly via its own named helper.
    4. `test_uv_tcp_listen.c:188`: either convert to a condition-driven wait
       with a timeout, or — since §0 records a real reason (accept+handshake
       IOCP completions coalesce, so a blocking `UV_RUN_ONCE` hangs) — write
       that reason into the macro's comment so it does not read as flake-hiding.

### 11.2 Simplifications from review §4.3 (planned)

Sequencing note: **S3 and S4 are the cheap cages and should land first**
(they are the same work as W4 and W3 — do it once, in the W item, and tick the
S item off). **S1 is the large structural lever** and it substantially subsumes
W3, W7a and W9; do it after the cages are in place so the newly-enabled
warnings catch substitution mistakes, then re-audit what is left of W7/W9.
**S2 is a scoping decision, not a code change on this branch.**

- [x] **S1 — Shrink the compat layer by adopting Fork A's libuv-substitution
      strategy.**
      **DONE (2026-07-30), four commits** (one per primitive family + the
      deletion sweep), each verified with full suites on BOTH platforms
      (Windows: unit 321, raft-uv-unit 20, raft-uv-int 212, server 8,
      cluster 23, stress 46; WSL: unit 322, raft-uv-unit 25, raft-core-unit
      262, raft-uv-int 237, server 8, stress 46):
      (1) pthread→uv (thread/mutex/cond). Non-mechanical corners:
      thread-return values (uv_thread_join has no out-param → taskRun result
      stashed in the node, read after join) and `pthread_cond_timedwait`
      (absolute REALTIME) → `uv_cond_timedwait` (relative ns, monotonic) —
      refreshTask recomputed its deadline per iteration so the relative form
      is drift-free. (2) sem→uv_sem; `sem_getvalue` (no uv equivalent) in
      test_node.c replaced by an atomic post-counter with the off-by-one
      (consumed token) mapped exactly. (3) getrandom→uv_random,
      gettimeofday→uv_gettimeofday; the GRND_NONBLOCK "don't block on
      entropy" nuance documented at the seed site (uv_random blocks
      pre-entropy-init; only affects first moments after boot, 4-byte
      non-crypto srand seed — judged acceptable, fallback kept for real
      errors). (4) Deleted compat/win/{pthread,semaphore,sched,sys/random,
      sys/time}.h + compat_win.c's ~225-line threads/sync section + six
      zero-consumer unistd.h symbols + the `_CRT_RAND_S` define:
      **29 files/2937 lines → 24 files/2447 lines**. sys/time.h went too —
      munit's <sys/time.h> includes are psnip-clock GETTIMEOFDAY/GETRUSAGE
      branches that _WIN32 never selects (proven via ninja dep DB). The
      threadpool.c `<uv/unix.h>` guard + shadow-stub deletion landed earlier
      in W3. Caveats: `uv_cond_timedwait` clock-domain checked as the item
      asked; W7a (sub-ms sleep) remains open — vfs.c's backoff is nanosleep,
      still ms-truncated in compat unistd.h. WSL-only pre-existing
      membership/client env failures documented in the family-1 commit. *The review's headline recommendation: the forced-include
      prelude that fakes `<unistd.h>`/`<pthread.h>` for ~50k lines is the port's
      biggest ongoing liability, and libuv (already a hard dependency) provides
      portable threads, semaphores, mutexes, fs ops and randomness.*
  - **Verified scope.** POSIX sync/thread primitives appear in exactly:
    `src/server.c` (26 hits), `src/lib/threadpool.c` (7), `src/server.h`,
    `src/vfs.c`, plus tests `test/integration/test_node.c`,
    `test/integration/test_stress.c`, `test/test_integration.c`. That is a
    small, enumerable surface — this is a mechanical change, not a rewrite.
  - **Substitution map.** `pthread_t` → `uv_thread_t`; `pthread_mutex_*` →
    `uv_mutex_*`; `pthread_cond_*` → `uv_cond_*`; `sem_t`/`sem_*` →
    `uv_sem_t`/`uv_sem_*`; C11 `mtx_t`/`thrd_*` → the `uv_*` equivalents;
    `getrandom` → `uv_random`; `gettimeofday` → `uv_gettimeofday`.
    **Caveat:** `uv_sleep` is millisecond-granular too, so it does **not**
    solve the sub-ms backoff problem — W7a still needs its own answer
    (high-resolution waitable timer), and `uv_cond_timedwait` should be checked
    for its clock domain against the current shimmed `clock_gettime`.
  - **Then delete** `compat/win/pthread.h`, `compat/win/semaphore.h`,
    `compat/win/sched.h`, and most of `compat/win/unistd.h`. What legitimately
    remains becomes a small, explicitly-prefixed module rather than 30 shadow
    headers: the `mmap`/`munmap` placeholder implementation, `flock`
    (`LockFileEx`), the overlapped named-pipe helpers (if S2 keeps them), and
    the `statvfs`/`fstatfs`/`nftw`/`uname` stubs that only exist for tests.
  - **How to do it.** One mechanical, zero-functional-change commit per
    primitive family (this is roadmap PRs #3 and #4), each proven byte-identical
    on Linux by `clang -E -P` diff plus a full suite run. Do **not** mix a
    substitution with a behaviour change. Land Windows verification after each.
  - **Also in scope:** replace `src/lib/threadpool.c`'s hardcoded
    `#include <uv/unix.h>` with a guarded include, which lets
    `compat/win/uv/unix.h` — the one shim that shadows a real dependency
    header — be deleted outright (see W3.3).
  - **Verification.** Linux `unit-test` 320/320, `raft-core-unit` 262/262,
    `raft-uv-unit` 25/25, integration suites, plus the ASAN build; Windows full
    suite parity with the numbers in §0/§7/§9.

- [ ] **S2 — Carve the named-pipe local transport out of the first upstream
      iteration** (defaulting Windows to TCP loopback, which already works).
  - **Correction to the review, important for planning:** §4.3 states the pipe
    transport "is only reachable through the test harness's custom connect
    function". On this branch that is **not** accurate — it is production code
    reachable by any caller using an `@name` address: `src/server.c:317-321`
    (`dqliteNodeBindPipe`), `src/transport.c:103` and `:144`
    (`DqliteWinPipeWriteAll`), `src/client/protocol.c:34`/`:176`/`:291`
    (`SO_TYPE` discriminator, `DqliteWinPipeReadTimed`, pipe write),
    `src/lib/transport.c:91-140` (`uv_guess_handle` → `UV_NAMED_PIPE`).
    So this is a **scoping decision, not deletion**.
  - **Plan.** (i) Keep the pipe layer on this branch — it works and it was
    expensive to get right (`cluster` 23/23, `membership` 5/5, `server` 8/8,
    plus the `uv_pipe_pending_instances` and non-overlapped-pipe
    threadpool-deadlock discoveries, §7/§9). (ii) For the upstream series,
    prepare a variant in which Windows local IPC returns `DQLITE_MISUSE` and the
    tests default to TCP loopback, so the first Windows PR carries no pipe layer
    and none of its timeout/EOF ambiguity. (iii) Land the pipe transport later
    as its own PR with its own argument. (iv) Record the exact carve-out
    surface now, while it is fresh: `compat/win/dqlite_win_pipe.h` (166 lines) +
    the guarded blocks listed above + `test/lib/endpoint.c` (`:14`, `:30`,
    `:99`, `:311`) and `test/lib/server.c` (`:9`, `:25`, `:119`).
  - **Deliverable for this branch:** a short note in `PORT_DESIGN.md` recording
    the two-tier plan and the carve-out surface. No functional change.

- [x] **S3 — Fix the warning gate.** (DONE 2026-07-29 — delivered by W4's two
      commits: clang-cl flag branch + 710→0 warning triage; see W4 for the
      breakdown.) Same work as **W4** (`CMakeLists.txt:295`,
      `if(NOT MSVC)` → detect clang-cl and pass
      `/clang:-Wall /clang:-fno-strict-aliasing`). One line to enable, then the
      warning triage. Tracked in W4; tick both together.

- [x] **S4 — Cage the header-shadowing hazard cheaply.** (DONE 2026-07-29 —
      delivered by W3's commit: `DQLITE_WIN_COMPAT` fence in all 28 compat
      headers + prelude-only definition; the "move off the include path" half
      deferred to S1 as W3 records.) Same work as **W3**
      steps 1–2 (`#ifdef _WIN32` + a project fence macro on every compat header;
      prefix/move the directory so generic names leave the include path).
      Do this even though S1 will later remove most of the headers — it is a few
      lines and it closes a hazard that is live *today*. Tracked in W3.

### 11.3 Housekeeping also flagged by the review (§4.1 callout)

- [x] **Fix the stale `CMakeLists.txt` header.** Lines 1-14 still say
      "(sketch)" and "macOS and Windows branches are stubbed with explicit
      TODOs", which contradicts both the README and reality (Windows is fully
      wired: all 385 objects compile, 16 targets link, suites run). Rewrite to
      state what each platform branch actually does today.
      (DONE 2026-07-29: banner rewritten, comment-only — dropped "(sketch)",
      now states per-branch reality: Linux = reference build (pkg-config,
      `_GNU_SOURCE`/`_FILE_OFFSET_BITS=64`, io_uring/aio/xfs probes,
      `RWF_NOWAIT` gate); Windows = fully wired (vcpkg manifest `find_package`,
      `compat/win` include dir + `/FI` prelude, `dqlite_win_compat`,
      `DQLITE_BUILDING`, `dqlite_static.lib` rename, no `DQLITE_HAVE_KAIO`);
      macOS = still a deliberate `FATAL_ERROR` stub — **verified in the file**,
      the `elseif(APPLE)` branch at `CMakeLists.txt:131-139` is unchanged, so
      only the `__APPLE__` *source* work is done (§5), not the CMake wiring.
      Also corrected the Windows configure example, which was missing
      `CMAKE_TOOLCHAIN_FILE`/`VCPKG_TARGET_TRIPLET`.)

- [x] **Commit the `PORT_*.md` docs so the in-tree references resolve.**
      `PORT_DESIGN.md`, `PORT_TODO.md` and `PORT_WINDOWS_ERRORS.md` were
      untracked while six tracked files pointed at `PORT_DESIGN.md`
      (`CMakeLists.txt:163`, `compat/win/sys/mman.h:11`,
      `compat/win/sys/vfs.h:7`, `src/raft/uv_fs.c:1201`,
      `src/raft/uv_writer.c:57`, `src/vfs.c:1893`), so those pointers dangled
      for anyone reading the branch. (DONE 2026-07-29: all three committed, so
      the references now resolve. Scope note: these docs are committed on this
      fork branch **only** and are deliberately NOT an upstream deliverable —
      the review's point stands that in-tree design docs belong in the PR
      description, so the upstream series must either drop them or convert the
      six `see PORT_DESIGN.md` comments into self-contained prose.)
