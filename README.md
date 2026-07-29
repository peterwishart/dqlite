dqlite [![CI Tests](https://github.com/canonical/dqlite/actions/workflows/build-and-test.yml/badge.svg)](https://github.com/canonical/dqlite/actions/workflows/build-and-test.yml) [![codecov](https://codecov.io/gh/canonical/dqlite/branch/main/graph/badge.svg)](https://codecov.io/gh/canonical/dqlite)
======

[English](./README.md)|[简体中文](./README_CH.md)

[dqlite](https://dqlite.io) is a C library that implements an embeddable and
replicated SQL database engine with high availability and automatic failover.

The acronym "dqlite" stands for "distributed SQLite", meaning that dqlite
extends [SQLite](https://sqlite.org/) with a network protocol that can connect
together various instances of your application and have them act as a
highly-available cluster, with no dependency on external databases.

Design highlights
----------------

* Asynchronous single-threaded implementation using [libuv](https://libuv.org/)
  as event loop.
* Custom wire protocol optimized for SQLite primitives and data types.
* Data replication based on the [Raft](https://raft.github.io/) algorithm.

License
-------

The dqlite library is released under a slightly modified version of LGPLv3,
that includes a copyright exception allowing users to statically link the
library code in their project and release the final work under their own terms.
See the full [license](https://github.com/canonical/dqlite/blob/main/LICENSE)
text.

Compatibility
-------------

dqlite runs on Linux and requires a kernel with support for [native async
I/O](https://man7.org/linux/man-pages/man2/io_setup.2.html) (not to be confused
with [POSIX AIO](https://man7.org/linux/man-pages/man7/aio.7.html)).

Try it
-------

The simplest way to see dqlite in action is to use the demo program that comes
with the Go dqlite bindings. Please see the [relevant
documentation](https://github.com/canonical/go-dqlite#demo) in that project.

Media
-----

A talk about dqlite was given at FOSDEM 2020, you can watch it
[here](https://fosdem.org/2020/schedule/event/dqlite/).

[Here](https://gcore.com/blog/comparing-litestream-rqlite-dqlite/) is a blog
post from 2022 comparing dqlite with rqlite and Litestream, other replication
software for SQLite.

Wire protocol
-------------

If you wish to write a client, please refer to the [wire
protocol](https://dqlite.io/docs/protocol) documentation.

Install
-------

If you are on a Debian-based system, you can get the latest development release
from dqlite's [dev PPA](https://launchpad.net/~dqlite/+archive/ubuntu/dev):

```
sudo add-apt-repository ppa:dqlite/dev
sudo apt update
sudo apt install libdqlite-dev
```

Operating
---------

To inspect the state of a running dqlite instance, create a snapshot of data to load onto a node or even to inspect past database states, install `dqlite-utils`:

```bash
sudo snap install dqlite-utils --classic
```

See the [dqlite documentation](https://canonical.com/dqlite/docs) for more information.

Contributing
------------

See [CONTRIBUTING.md](./CONTRIBUTING.md).

Build
-----

To build libdqlite from source you'll need:

* Build dependencies: pkg-config and GNU Autoconf, Automake, libtool, and make
* A reasonably recent version of [libuv](https://libuv.org/) (v1.8.0 or later), with headers.
* A reasonably recent version of [SQLite](https://sqlite.org/) (v3.22.0 or later), with headers.
* Optionally, a reasonably recent version of [LZ4](https://lz4.org/) (v1.7.1 or later), with headers.

Your distribution should already provide you with these dependencies. For
example, on Debian-based distros:

```
sudo apt install pkg-config autoconf automake libtool make libuv1-dev libsqlite3-dev liblz4-dev
```

With these dependencies installed, you can build and install the dqlite shared
library and headers as follows:

```
$ autoreconf -i
$ ./configure
$ make
$ sudo make install
```

The default installation prefix is `/usr/local`; you may need to run

```
$ sudo ldconfig
```

to enable the linker to find `libdqlite.so`. To install to a different prefix,
replace the configure step with something like

```
$ ./configure --prefix=/usr
```

Building on Windows
-------------------

dqlite also builds on Windows (64-bit). The autotools build above is Linux-only;
on Windows the **CMake** build (`CMakeLists.txt`) is used instead, together with
a small POSIX compatibility layer under `compat/win/`. The toolchain is **Clang**
(`clang-cl`) with **Ninja**, and dependencies are provided by **vcpkg**.

**Windows 10 version 1803 (April 2018) or later is required at runtime.** The
port depends on the placeholder virtual-memory APIs introduced in 1803
(`VirtualAlloc2`/`MapViewOfFile3`/`UnmapViewOfFile2`) to replace WAL-index
shared-memory mappings in place without ever releasing the address range —
the equivalent of Linux's atomic `mremap(MREMAP_FIXED)`. There is deliberately
no pre-1803 fallback: an unmap-then-remap sequence has an unfixable window in
which another thread can be handed the same address, silently corrupting the
WAL index. On older systems dqlite fails cleanly with a diagnostic on the first
shared-memory mapping instead of running incorrectly. (Other parts of the port,
such as `AF_UNIX` socket support, assume the same 1803+ floor.)

### Prerequisites

* **Visual Studio 2022** with the *C++ Clang tools for Windows* component
  (`clang-cl` 19 or later) and the bundled **CMake** and **Ninja**.
* **vcpkg** (the copy bundled with VS 2022, or a standalone checkout) to supply
  the dependencies.

### Dependencies (vcpkg)

libuv, SQLite and LZ4 are declared in the repository's `vcpkg.json` manifest.
You do **not** need to install them by hand — the CMake configure step below
uses the vcpkg toolchain file, which installs the manifest's dependencies
automatically (into `build-win\vcpkg_installed\x64-windows\`, git-ignored).

### Configure and build

1. Start the **"x64 Native Tools Command Prompt for VS 2022"**. This puts the
   64-bit `clang-cl`, `cmake`, `ninja` and the Windows SDK on `PATH`. (Do **not**
   use the plain *Developer PowerShell for VS 2022* — it defaults to a 32-bit
   environment, which makes `find_package` reject the 64-bit vcpkg libraries.)
2. Run `powershell` to get a PowerShell prompt inside that x64 environment.
3. `cd` to the repository root.
4. Configure and build:

   ```
   cmake -S . -B build-win -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_TOOLCHAIN_FILE="${env:vcpkg_root}/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows
   cmake --build build-win
   ```

The vcpkg toolchain file installs the `vcpkg.json` dependencies automatically
(manifest mode — no separate `vcpkg install` needed) into
`build-win\vcpkg_installed\x64-windows\` and wires them up for `find_package`, so
you do **not** need to set `CMAKE_PREFIX_PATH` yourself. `${env:vcpkg_root}`
refers to your vcpkg install; the copy bundled with VS 2022 lives under
`...\VC\vcpkg` and sets this variable for you.

If a configure step fails, clear the build directory and start fresh:

```
Remove-Item -Recurse -Force build-win
```

`CMAKE_TOOLCHAIN_FILE` is only honored on a build dir's *first* configure, so a
stale `build-win\` can leave `find_package(libuv)` failing even after everything
is set up correctly.

This produces `dqlite.dll` and `dqlite_static.lib`, plus the test executables,
in `build-win\`. Add `-- -k 0` to `cmake --build` to keep going past the first
error (handy when working on portability, so all failures surface in one pass).

### Running the tests

The test executables link the vcpkg dependencies as DLLs, so those directories
must be on `PATH` first (the `debug\bin` directory is needed for the default,
unoptimized build). Continuing in the same PowerShell prompt from above:

```
$dll = "$PWD\build-win\vcpkg_installed\x64-windows"
$env:PATH = "$dll\debug\bin;$dll\bin;$env:PATH"
build-win\unit-test.exe
build-win\integration-test.exe
build-win\raft-uv-unit-test.exe
build-win\raft-uv-integration-test.exe
```

### Durability on Windows

On Linux, dqlite makes an acknowledged raft log entry crash-durable by writing
segment files opened with `O_DSYNC` (plus `fsync` on the containing directory
after creates/renames). The Windows port preserves that guarantee with the
following mapping — this is what **is** guaranteed:

* **Log segment writes are synchronized.** `O_DSYNC` is defined (in
  `compat/win/dqlite_win_prelude.h`) as libuv's `UV_FS_O_DSYNC`, which
  `uv_fs_open()` translates to `FILE_FLAG_WRITE_THROUGH`, so every segment
  write bypasses the OS file cache. In addition, because
  `FILE_FLAG_WRITE_THROUGH` alone does not guarantee the *device's* volatile
  write cache is flushed (NTFS issues FUA writes, which consumer SATA drives
  commonly ignore), the write path (`uvWriterWorkCbPortable` in
  `src/raft/uv_writer.c`) follows every write to an `O_DSYNC`-opened file with
  an explicit `fdatasync` — `FlushFileBuffers()` on Windows, which sends a
  cache-flush command to the device. A write is only reported (and therefore
  only acknowledged by raft) after that flush succeeds.
* **Directory updates are flushed.** NTFS directories cannot be `fsync`ed the
  POSIX way, but `UvFsSyncDir()` (`src/raft/uv_fs.c`) opens the data directory
  with backup semantics and write access and calls `FlushFileBuffers()` on it,
  forcing the pending NTFS metadata log records (file creates and renames) to
  stable storage; a flush failure is propagated as an I/O error, not ignored.

What is **not** guaranteed / relied upon instead:

* If the data directory grants the dqlite process no write access, the
  directory flush is skipped and dqlite relies on NTFS metadata journaling
  alone: NTFS preserves the *ordering* of the logged create/rename operations
  across a crash, which the rename-based segment/snapshot commit protocol
  depends on, but the most recent metadata operations may be lost (as if they
  had not happened yet). File *data* durability is unaffected — it comes from
  the write-through + flush write path above.
* Non-NTFS filesystems (FAT/exFAT, network shares) are untested and may
  provide weaker metadata-ordering guarantees; run dqlite on NTFS.
* The SQLite database/WAL files live in dqlite's in-memory VFS and are
  reconstructed from the raft log, so their durability derives entirely from
  the raft log guarantees above.

Building for static linking
---------------------------

If you're building dqlite for eventual use in a statically-linked
binary, there are some additional considerations. You should pass
`--with-static-deps` to the configure script; this disables code that
relies on dependencies being dynamically linked. (Currently it only
affects the test suite, but you should use it even when building
`libdqlite.a` only for future compatibility.)

When linking libdqlite with musl libc, it's recommended to increase
the default stack size, which is otherwise too low for dqlite's
needs:

```
LDFLAGS="-Wl,-z,stack-size=1048576"
```

The `contrib/build-static.sh` script demonstrates building and
testing dqlite with all dependencies (including libc) statically
linked.

Usage notes
-----------

Detailed tracing will be enabled when the environment variable
`LIBDQLITE_TRACE` is set before startup.  The value of it can be in `[0..5]`
range and represents a tracing level, where `0` means "no traces" emitted, `5`
enables minimum (FATAL records only), and `1` enables maximum verbosity (all:
DEBUG, INFO, WARN, ERROR, FATAL records).
