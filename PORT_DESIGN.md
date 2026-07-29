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
