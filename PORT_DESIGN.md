# dqlite I/O backend abstraction — design for a Linux-testable macOS/Windows port

## Goal

Make the raft persistence layer portable to macOS and Windows while keeping the
**vast majority of the port testable on Linux/WSL2**, so a Mac is only needed to
confirm a small set of genuinely OS-specific behaviours (not for day-to-day
iteration).

## Key finding: the seam already exists

`struct UvWriter` (`src/raft/uv_writer.{c,h}`) is already the single choke point
for all raft file writes. Everything above it — `uv_segment.c`, `uv_prepare.c`,
`uv_finalize.c`, `uv_snapshot.c`, the log/recovery machinery — is platform-neutral
and already covered by the `raft-uv-*` test suites.

`UvWriter` already contains **two** completion strategies:

1. **Fast path (Linux only):** `io_submit` with `RWF_NOWAIT`, completion signalled
   via an `eventfd` polled by `uv_poll`.
2. **Threadpool fallback:** `uv_queue_work` runs the write in the libuv
   threadpool. This path is *already libuv-based and cross-platform* — it only
   happens to call `io_submit` inside the worker because the AIO code was reused.

The authors even flagged the non-portable calls directly in `uv_os.h`
(`/* TODO: figure a portable abstraction. */` on `UvOsIoSetup/IoSubmit/
IoGetevents`, `UvOsEventfd`, `UvOsSetDirectIo`, `UvOsFallocate`).

## Design

Keep the `UvWriter` interface unchanged and give it two selectable backends:

| Backend | Write mechanism | Platforms |
|---|---|---|
| `aio` (existing) | `io_submit` + `RWF_NOWAIT` + `eventfd`/`uv_poll` | Linux ≥ 4.14 (optimization) |
| `threadpool` (new) | `uv_queue_work` + `UvOsWrite` (= `uv_fs_write`) | Linux, macOS, Windows |

The portable backend needs **none** of the Linux-only primitives: no
`aio_context_t`, no `eventfd`, no `RWF_NOWAIT`, no `uv_poll`. It relies only on
libuv's threadpool and `uv_fs_write`, both fully cross-platform. That removes
exactly the four `TODO`-flagged calls from the portable path.

Selection: on kernel-AIO builds (Linux) the `aio` backend is always the
default, matching upstream — when fully async I/O is unavailable (`async ==
false`, e.g. tmpfs/ZFS) it runs the same `io_submit` + `io_getevents` pair
*blocking in the threadpool* rather than switching mechanism. The environment
variable `DQLITE_IO_BACKEND=threadpool` is the only way to select the portable
backend on such builds (the Linux-testability lever below). Builds without
kernel AIO (macOS/Windows, or Linux with `DQLITE_DISABLE_KAIO`) have only the
portable backend. (An earlier iteration auto-selected `threadpool` whenever
`async == false`; that silently changed the production Linux write mechanism
on tmpfs/ZFS and was reverted — see PORT_TODO.md W6(a).)

Eventual file split (clean form, not required for first validation):
```
uv_writer.h            interface (unchanged)
uv_writer_aio.c        Linux fast path
uv_writer_threadpool.c portable path
```

## The Linux-testability principle

For every platform difference, add a switch that forces the portable/fallback
path, then run the **existing Linux suites** against it. This converts "needs a
Mac" into "runs on WSL2 today":

| macOS/Windows difference | Portable replacement | Linux test method |
|---|---|---|
| kernel AIO / io_uring writer | threadpool + `uv_fs_write` | `DQLITE_IO_BACKEND=threadpool` → run `raft-uv-unit-test` + `raft-uv-integration-test` |
| `eventfd` / `timerfd` | `uv_async_t` / `uv_timer_t` | absent from portable path → suites pass without it |
| `statfs` + `linux/magic.h` FS probe (`uv_fs.c`) | capability probe + fallback | stub probe → test buffered-vs-direct decision on Linux |
| `O_DIRECT` (`UvOsSetDirectIo`) | macOS `F_NOCACHE`, Win `FILE_FLAG_NO_BUFFERING`, else skip + rely on fsync | force "no direct I/O" on Linux |
| `mmap`/**`mremap`** (`vfs.c`) | unmap+remap fallback (macOS has no `mremap`) | `DQLITE_VFS_NO_MREMAP` flag → run `test_vfs` on Linux |
| `AF_UNIX` transport | macOS works; Win → TCP loopback | test loopback fallback on Linux |

Net: the writer backend, event/timer handles, direct-I/O and FS-probe fallbacks,
and the VFS remap fallback all get real regression coverage on WSL2. A Mac is
then only needed for Darwin `statfs` magic values, `F_NOCACHE` semantics, and
`AF_UNIX` edge cases — a small surface, suited to a CI check.

## First implementation (this change)

`src/raft/uv_writer.{c,h}`:
- Add `bool threadpool` to `struct UvWriter` and portable request fields
  (`tp_bufs`, `tp_nbufs`, `tp_offset`) to `struct UvWriterReq`.
- `UvWriterInit`: when the threadpool backend is selected, skip `io_setup`,
  `eventfd`, and the poller; initialise only the `uv_check` handle used to drain
  in-flight work on close.
- `UvWriterSubmit`: route to a portable worker that calls `UvOsWrite`.
- `UvWriterClose` / cleanup: skip AIO-context and eventfd teardown for the
  portable backend.

This is additive and behind an opt-in switch, so default (AIO) behaviour is
unchanged. It lets us run `raft-uv-integration-test` against the portable write
path on WSL2 — the actual proof that the macOS write path is correct, obtained
without a Mac. (Side benefit: the portable path avoids the kernel-AIO subsystem,
sidestepping the AIO-contention flake seen under WSL2.)

## Validation plan

1. Build (CMake + Clang, WSL2). Default config → existing behaviour.
2. `raft-uv-unit-test` + `raft-uv-integration-test` with default backend (AIO)
   → must stay green (no regression).
3. Same suites with `DQLITE_IO_BACKEND=threadpool` → proves the portable path.

## Windows local transport ("@name"): two-tier upstreaming plan (S2)

Scoping decision, 2026-07-30 (PORT_TODO.md §11.2 S2). The "Win → TCP loopback"
row in the table above describes the *first-PR* posture only; this branch
additionally carries a full named-pipe local transport, and the two are
upstreamed in separate tiers.

**Decision.**

1. **The named-pipe transport stays on this branch.** It works — over pipes:
   `cluster` 23/23, `membership` 5/5, `server` 8/8, `client` 6/6, `stress`
   46/46 — and it was expensive to get right. Two non-obvious discoveries are
   baked into it and must not be re-learned: (a) a **non-overlapped pipe makes
   libuv burn one threadpool worker per read**, so a 5-node cluster (20
   connections) exhausts the default 4-worker pool and deadlocks; the connect
   side must open the pipe `FILE_FLAG_OVERLAPPED` so libuv drives it via IOCP
   (PORT_TODO.md §7, rationale comment in `compat/win/dqlite_win_pipe.h:77-93`).
   (b) libuv pipe listeners honor **`uv_pipe_pending_instances()` (default 4),
   not the listen backlog**, so 32-64 concurrent connects fail with
   `ERROR_PIPE_BUSY`; fixed with `uv_pipe_pending_instances(pipe, 128)` in
   `dqliteNodeBindPipe` (PORT_TODO.md §9, `src/server.c:280-287`).
2. **The first upstream Windows PR carries no pipe layer.** Prepare a variant
   in which a Windows `@name` bind returns `DQLITE_MISUSE` (replace the
   dispatch at `src/server.c:319-326` with an explicit rejection) and the tests
   run over TCP loopback. The first PR then has none of the pipe layer's
   timeout/EOF ambiguity (`DqliteWinPipeReadTimed` returns 0 for *both* EOF and
   timeout) to argue about.
3. **The pipe transport lands later as its own PR** with its own argument
   (local IPC parity with the Linux abstract-namespace transport), carrying the
   two discoveries above as its justification for the extra machinery.

**Carve-out surface** (verified against the tree on 2026-07-30; every block is
`#ifdef _WIN32`-guarded, so removal cannot touch the Linux build):

| File | Lines | What to remove for the first PR |
|---|---|---|
| `compat/win/dqlite_win_pipe.h` | whole file (176 lines) | `DqliteWinPipeName` ("@name" → `\\.\pipe\dqlite-<name>`), `DqliteWinPipeWriteAll`, `DqliteWinPipeReadTimed` |
| `src/server.c` | `:12` | `#include "dqlite_win_pipe.h"` |
| `src/server.c` | `:234-303` | `dqliteNodeBindPipe` (function at `:245`; `uv_pipe_pending_instances(pipe, 128)` at `:287`) |
| `src/server.c` | `:319-326` | the `address[0] == '@'` bind dispatch → becomes `return DQLITE_MISUSE` |
| `src/server.c` | `:709-721` | `#elif defined(_WIN32)` peer-cred branch in `listenCb` (`GetNamedPipeClientProcessId`) |
| `src/transport.c` | `:15-18`, `:79-86`, `:99-109`, `:142-148`, `:163-169` | pipe include (`:17`); `is_pipe` discriminator (`:85`); `DqliteWinPipeWriteAll` handshake/CONNECT writes (`:103`, `:144`); `is_pipe` error-path close (keep the `closesocket` arm) |
| `src/transport.c` | `:195-203` | `is_pipe`-style close in `connect_after_work_cb` (keep the `closesocket` arm) |
| `src/client/protocol.c` | `:16-19`, `:21-38`, `:154-188`, `:307-314`, `:460-468` | pipe include; `clientFdIsSocket` (`getsockopt(SO_TYPE)` discriminator, `:30-36`); pipe read branch (`DqliteWinPipeReadTimed` at `:176`); pipe write branch (`DqliteWinPipeWriteAll` at `:310`); pipe-vs-socket close (keep `closesocket`) |
| `src/lib/transport.c` | `:82-159` | `uv_guess_handle(fd) == UV_NAMED_PIPE` → `uv_pipe_open` classification in `transport__stream` (keep only the `uv_tcp_open` arm for the SOCKET case) |
| `test/lib/endpoint.c` | `:8-60`, `:94-108`, `:167-175`, `:187-192`, `:294-328` | pipe include + `endpointConnectPipe` (`DqliteWinPipeName` at `:30`); "@name" synthesis in setup (`:99`); listener/client teardown branches; `uv_pipe_bind` listener (`DqliteWinPipeName` at `:311`) |
| `test/lib/server.c` | `:9`, `:11-58`, `:70-72`, `:117-120` | pipe include; `endpointConnectPipe` (`DqliteWinPipeName` at `:25`); the `'@'` dispatch in the Windows `endpointConnect` (keep its TCP body); `test_server_setup`'s `"@%u"` default address → synthesize a TCP loopback address instead |

What **stays** (Windows, first PR — not pipe code, do not remove): the
`UV_NAMED_PIPE` cases in `src/server.c`'s `listenCb` (`:667`, `:686`) and the
`SO_PEERCRED`/`LOCAL_PEERPID` chain — upstream code, because libuv reports
Linux `AF_UNIX` sockets as `UV_NAMED_PIPE`; all Winsock-only `_WIN32` blocks
(`send`/`recv`/`WSAPoll`/`closesocket`/`accept`+`FIONBIO` in
`src/client/protocol.c`, `src/transport.c`, `test/lib/endpoint.c:240`); and
`test/lib/endpoint.c:69`'s TCP default in `getFamily()` — that default *is*
the first-PR posture and already exists.

Sanity check for the carve-out: after removal, grep for
`DqliteWinPipe|dqliteNodeBindPipe|dqlite_win_pipe` must return nothing, and
`UV_NAMED_PIPE` must survive only in `src/server.c`'s `listenCb`.
