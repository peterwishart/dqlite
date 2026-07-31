# ---------------------------------------------------------------------------
# dqlite — cross-platform CMake build
#
#   Linux — fully wired; the reference build. Deps via pkg-config (libuv,
#     sqlite3, optional liblz4); requires RWF_NOWAIT (kernel >= 4.14) for the
#     kernel-AIO raft I/O backend.
#   Windows — fully wired; builds and runs the test suites today. Deps via
#     vcpkg in manifest mode. Adds the WIN32-only compat/win shim-header
#     directory plus the forced prelude (/FI) and links the dqlite_win_compat
#     static library; DQLITE_HAVE_KAIO stays undefined off-Linux, so raft I/O
#     uses the portable libuv threadpool write path.
#   macOS — the elseif(APPLE) branch is a deliberate FATAL_ERROR stub; the
#     UNVERIFIED-NEEDS-MAC markers in the sources are the outstanding work-list.
#
# Toolchain: configure with Clang on every platform for consistency
#   Linux : cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang
#   macOS : cmake -B build -G Ninja            (AppleClang is the default)
#   Win   : cmake -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl  (MSVC ABI)
#             -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
#             -DVCPKG_TARGET_TRIPLET=x64-windows
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)

# Version: prefer the VERSION file, fall back to a literal.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" DQLITE_VERSION_RAW)
  string(STRIP "${DQLITE_VERSION_RAW}" DQLITE_VERSION)
else()
  set(DQLITE_VERSION "1.18.7")
endif()

project(dqlite VERSION ${DQLITE_VERSION} LANGUAGES C)

# ------------------------------- Options -----------------------------------
option(DQLITE_BUILD_TESTS "Build the test suite" ON)
option(DQLITE_BUILD_SHARED "Build the shared library" ON)
option(DQLITE_BUILD_STATIC "Build the static library" ON)
option(DQLITE_SANITIZE "Build with AddressSanitizer" OFF)
option(DQLITE_WITH_LZ4 "Use liblz4 snapshot compression when available" ON)
option(DQLITE_DISABLE_KAIO "Build without Linux kernel AIO (simulates macOS/Windows)" OFF)
# --enable-backtrace equivalent (configure.ac:32-50). Prints a stack trace on
# assertion failure. Default ON on Linux/macOS (glibc has <execinfo.h>); OFF on
# Windows, where crash traces are handled by the compat/win layer instead.
if(WIN32)
  option(DQLITE_ENABLE_BACKTRACE "Print a backtrace on assertion failure" OFF)
else()
  option(DQLITE_ENABLE_BACKTRACE "Print a backtrace on assertion failure" ON)
endif()
# AX_CODE_COVERAGE equivalent (configure.ac:76). Adds --coverage (gcov)
# instrumentation to the library + tests. Default OFF; GCC/Clang only.
option(DQLITE_CODE_COVERAGE "Build with code-coverage instrumentation (--coverage)" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)          # dqlite relies on GNU extensions (see below)
set(CMAKE_C_VISIBILITY_PRESET hidden)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# dqlite is never built with NDEBUG upstream: its assertions are load-bearing
# and the test runner (test/lib/runner.h) calls glibc __assert_fail directly,
# whose declaration is hidden under NDEBUG. Strip the -DNDEBUG that CMake's
# Release/RelWithDebInfo/MinSizeRel configs would otherwise inject.
foreach(_cfg RELEASE RELWITHDEBINFO MINSIZEREL)
  string(REGEX REPLACE "(^| )[-/]DNDEBUG( |$)" " "
    CMAKE_C_FLAGS_${_cfg} "${CMAKE_C_FLAGS_${_cfg}}")
endforeach()

include(CheckIncludeFile)
include(CheckSymbolExists)

# --------------------------- Dependencies ----------------------------------
# libuv / SQLite / lz4 are all cross-platform. On Linux/macOS resolve them via
# pkg-config; on Windows the idiomatic path is vcpkg (find_package), wired up
# below.
find_package(Threads REQUIRED)

if(WIN32)
  # Windows resolves deps via vcpkg (manifest mode). Point CMAKE_PREFIX_PATH at
  # vcpkg_installed/<triplet> (or use the vcpkg toolchain file).
  #   find_package(libuv CONFIG REQUIRED)              -> libuv::uv
  #   find_package(unofficial-sqlite3 CONFIG REQUIRED) -> unofficial::sqlite3::sqlite3
  #   find_package(lz4 CONFIG)                         -> lz4::lz4
  find_package(libuv CONFIG REQUIRED)
  # The x64-windows (shared) triplet exports libuv::uv; the x64-windows-static
  # triplet exports libuv::uv_a instead. Normalise to libuv::uv so the link
  # rules below stay triplet-agnostic. This branch only fires when libuv::uv is
  # absent (i.e. a static triplet), so the default shared build is unaffected.
  if(NOT TARGET libuv::uv AND TARGET libuv::uv_a)
    add_library(dqlite_libuv_iface INTERFACE)
    target_link_libraries(dqlite_libuv_iface INTERFACE libuv::uv_a)
    add_library(libuv::uv ALIAS dqlite_libuv_iface)
  endif()
  find_package(unofficial-sqlite3 CONFIG REQUIRED)
  if(DQLITE_WITH_LZ4)
    find_package(lz4 CONFIG)
  endif()
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(UV REQUIRED IMPORTED_TARGET libuv>=1.34.0)
  pkg_check_modules(SQLITE REQUIRED IMPORTED_TARGET sqlite3>=3.34.0)
  if(DQLITE_WITH_LZ4)
    pkg_check_modules(LZ4 IMPORTED_TARGET liblz4>=1.7.1)
  endif()
endif()

# ------------------- Platform detection & I/O backend ----------------------
# This block is the heart of the portability story. Everything Linux-specific
# in the raft persistence layer is gated here.
set(DQLITE_PLATFORM_DEFS "")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND DQLITE_PLATFORM_DEFS _GNU_SOURCE _FILE_OFFSET_BITS=64)

  # These map to configure.ac's AC_CHECK_HEADERS / AC_CHECK_DECLS. The sources
  # test them with `#if HAVE_...`, so an undefined macro correctly means "0".
  check_include_file("linux/io_uring.h" HAVE_LINUX_IO_URING_H)
  check_include_file("linux/aio_abi.h"  HAVE_LINUX_AIO_ABI_H)
  check_include_file("xfs/xfs.h"        HAVE_XFS_XFS_H)
  if(HAVE_LINUX_IO_URING_H)
    list(APPEND DQLITE_PLATFORM_DEFS HAVE_LINUX_IO_URING_H=1)
  endif()
  if(HAVE_LINUX_AIO_ABI_H)
    list(APPEND DQLITE_PLATFORM_DEFS HAVE_LINUX_AIO_ABI_H=1)
  endif()
  if(HAVE_XFS_XFS_H)
    list(APPEND DQLITE_PLATFORM_DEFS HAVE_XFS_XFS_H)
  endif()

  # Linux >= 4.14 is required by the async-I/O backend (RWF_NOWAIT).
  check_symbol_exists(RWF_NOWAIT "linux/aio_abi.h" HAVE_DECL_RWF_NOWAIT)
  if(NOT HAVE_DECL_RWF_NOWAIT)
    message(FATAL_ERROR "Linux kernel >= 4.14 required (RWF_NOWAIT not declared).")
  endif()

elseif(APPLE)
  list(APPEND DQLITE_PLATFORM_DEFS _DARWIN_C_SOURCE)
  message(FATAL_ERROR
    "macOS I/O backend not implemented yet. Port required for: "
    "src/raft/syscall.c, uv_os.c, uv_fs.c, uv_writer.c")

elseif(WIN32)
  # This branch defines the Windows platform macros (CRT/Winsock deprecation
  # silencing, WIN32_LEAN_AND_MEAN, NOMINMAX); the compat/win include dir, the
  # forced prelude (/FI) and the dqlite_win_compat link are applied per-target
  # in dqlite_common_flags() below. DQLITE_HAVE_KAIO stays undefined off-Linux,
  # so raft I/O uses the portable libuv threadpool write path.
  list(APPEND DQLITE_PLATFORM_DEFS _CRT_SECURE_NO_WARNINGS WIN32_LEAN_AND_MEAN NOMINMAX)
  # POSIX-compat shim layer (compat/win): silence the UCRT deprecation of the
  # POSIX I/O aliases (read/write/close/...) and the legacy Winsock names.
  # Windows-only; see compat/win/*.h and the WIN32 branch of
  # dqlite_common_flags() below.
  list(APPEND DQLITE_PLATFORM_DEFS
    _CRT_NONSTDC_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE
    _WINSOCK_DEPRECATED_NO_WARNINGS)

else()
  message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

# Simulate a platform without Linux kernel AIO (macOS/Windows) so the portable
# threadpool I/O path can be built and regression-tested on Linux. Defining
# DQLITE_DISABLE_KAIO makes src/raft/uv_os.h leave DQLITE_HAVE_KAIO undefined,
# compiling out every kernel-AIO/eventfd reference.
if(DQLITE_DISABLE_KAIO)
  list(APPEND DQLITE_PLATFORM_DEFS DQLITE_DISABLE_KAIO)
endif()

# ------------------ Backtrace on assertion failure -------------------------
# Mirrors configure.ac:37-48. src/lib/assert.c gates the whole feature on
# DQLITE_ASSERT_WITH_BACKTRACE and then selects an implementation via exactly
# one of HAVE_BACKTRACE_H / HAVE_EXECINFO_H / HAVE_LIBUNWIND_H (checked in that
# precedence). We must set BOTH the master gate and the matching HAVE_* macro,
# plus the corresponding link flag/library:
#   <backtrace.h>  -> HAVE_BACKTRACE_H  + -lbacktrace
#   <execinfo.h>   -> HAVE_EXECINFO_H   + -rdynamic   (glibc default path)
#   libunwind>=1.7 -> HAVE_LIBUNWIND_H  + libunwind libs
# The link flags are collected in DQLITE_BACKTRACE_LINK and attached to every
# linkable target in dqlite_link_deps() (matching AM_LDFLAGS += $(BACKTRACE_LIBS)).
set(DQLITE_BACKTRACE_LINK "")
if(DQLITE_ENABLE_BACKTRACE)
  if(WIN32)
    # Not wired on Windows (compat/win handles crash traces). Defaulted OFF; if
    # a user force-enables it, warn and define nothing rather than break the build.
    message(WARNING
      "DQLITE_ENABLE_BACKTRACE is not supported on Windows "
      "(crash traces are handled by compat/win); ignoring.")
  else()
    check_include_file("backtrace.h" HAVE_BACKTRACE_H)
    check_include_file("execinfo.h"  HAVE_EXECINFO_H)
    if(HAVE_BACKTRACE_H)
      list(APPEND DQLITE_PLATFORM_DEFS DQLITE_ASSERT_WITH_BACKTRACE HAVE_BACKTRACE_H=1)
      list(APPEND DQLITE_BACKTRACE_LINK backtrace)   # -lbacktrace
      message(STATUS "dqlite backtrace: libbacktrace (<backtrace.h> + -lbacktrace)")
    elseif(HAVE_EXECINFO_H)
      list(APPEND DQLITE_PLATFORM_DEFS DQLITE_ASSERT_WITH_BACKTRACE HAVE_EXECINFO_H=1)
      list(APPEND DQLITE_BACKTRACE_LINK -rdynamic)
      message(STATUS "dqlite backtrace: <execinfo.h> + -rdynamic")
    else()
      # Last resort: libunwind (>= 1.7) via pkg-config, matching configure.ac.
      pkg_check_modules(LIBUNWIND IMPORTED_TARGET libunwind-generic>=1.7)
      if(TARGET PkgConfig::LIBUNWIND)
        list(APPEND DQLITE_PLATFORM_DEFS DQLITE_ASSERT_WITH_BACKTRACE HAVE_LIBUNWIND_H)
        list(APPEND DQLITE_BACKTRACE_LINK PkgConfig::LIBUNWIND)
        message(STATUS "dqlite backtrace: libunwind (-DHAVE_LIBUNWIND_H)")
      else()
        message(FATAL_ERROR
          "backtrace support was requested, but neither <backtrace.h> nor "
          "<execinfo.h> was found and libunwind-generic >= 1.7 is unavailable. "
          "Reconfigure with -DDQLITE_ENABLE_BACKTRACE=OFF to build without it.")
      endif()
    endif()
  endif()
endif()

# ------------------------- Code coverage -----------------------------------
# Mirrors AX_CODE_COVERAGE (configure.ac:76): adds --coverage (-fprofile-arcs
# -ftest-coverage) to compile AND link for the library + tests. GCC/Clang only;
# no-op on Windows/MSVC. Flags are applied in dqlite_common_flags (compile) and
# dqlite_link_deps (link) so every dqlite target is instrumented uniformly.
if(DQLITE_CODE_COVERAGE)
  if(WIN32 OR MSVC)
    message(WARNING
      "DQLITE_CODE_COVERAGE requires a GCC/Clang toolchain; ignoring on this compiler.")
    set(DQLITE_CODE_COVERAGE OFF)
  else()
    message(STATUS "dqlite code coverage: --coverage instrumentation enabled")
  endif()
endif()

# ------------------------------ Sources ------------------------------------
# Mirrors basic_dqlite_sources in Makefile.am.
set(DQLITE_CORE_SOURCES
  src/bind.c src/client/protocol.c src/command.c src/conn.c src/db.c
  src/dqlite.c src/error.c src/format.c src/fsm.c src/gateway.c src/leader.c
  src/lib/assert.c src/lib/addr.c src/lib/buffer.c src/lib/sm.c
  src/lib/threadpool.c src/lib/transport.c src/logger.c src/message.c
  src/metrics.c src/config.c src/query.c src/registry.c src/request.c
  src/response.c src/roles.c src/server.c src/stmt.c src/tracing.c
  src/transport.c src/translate.c src/tuple.c src/vfs.c)

# Mirrors libraft_la_SOURCES. The vendored raft algorithm core is portable;
# the platform-specific I/O lives in syscall.c / uv_os.c / uv_fs.c / uv_writer.c
# (gated by the platform block above).
set(RAFT_SOURCES
  src/raft/byte.c src/raft/callbacks.c src/raft/client.c
  src/raft/configuration.c src/raft/convert.c src/raft/election.c
  src/raft/entry.c src/raft/err.c src/raft/fixture.c src/raft/flags.c
  src/raft/heap.c src/raft/log.c src/raft/membership.c src/raft/progress.c
  src/raft/raft.c src/raft/recv.c src/raft/recv_append_entries.c
  src/raft/recv_append_entries_result.c src/raft/recv_request_vote.c
  src/raft/recv_request_vote_result.c src/raft/recv_install_snapshot.c
  src/raft/recv_timeout_now.c src/raft/replication.c src/raft/snapshot.c
  src/raft/start.c src/raft/state.c src/raft/syscall.c src/raft/tick.c
  src/raft/uv.c src/raft/uv_append.c src/raft/uv_encoding.c
  src/raft/uv_finalize.c src/raft/uv_fs.c src/raft/uv_ip.c src/raft/uv_list.c
  src/raft/uv_metadata.c src/raft/uv_os.c src/raft/uv_prepare.c
  src/raft/uv_recv.c src/raft/uv_segment.c src/raft/uv_send.c
  src/raft/uv_snapshot.c src/raft/uv_tcp.c src/raft/uv_tcp_listen.c
  src/raft/uv_tcp_connect.c src/raft/uv_timer.c src/raft/uv_truncate.c
  src/raft/uv_work.c src/raft/uv_writer.c)

# --------------------------- Build-tag define ------------------------------
find_package(Git QUIET)
set(DQLITE_BUILD_TAG "")
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --abbrev=4 --dirty --always
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE DQLITE_BUILD_TAG OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()

# --------------------- Common compile settings helper ----------------------
# Applied to every dqlite target. -fno-strict-aliasing is load-bearing (the
# wire/encoding code type-puns), matching configure.ac.
function(dqlite_common_flags tgt)
  target_compile_definitions(${tgt} PRIVATE
    ${DQLITE_PLATFORM_DEFS}
    DQLITE_VERSION_BUILD_TAG="${DQLITE_BUILD_TAG}")
  # Project root ONLY, matching the autotools build's `-I.`. All cross-dir
  # includes in the tree are relative or root-relative. Adding src/raft here
  # would shadow the system <uv.h> with dqlite's own src/raft/uv.h.
  target_include_directories(${tgt} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  if(WIN32)
    # POSIX-compatibility shim headers (compat/win/): resolve <unistd.h>,
    # <sys/socket.h>, <poll.h>, ... to Win32/UCRT/Winsock equivalents WITHOUT
    # editing the POSIX sources. This directory is added ONLY on Windows, so it
    # can never shadow the real system headers on Linux/macOS. The forced-include
    # prelude (/FI) fixes the winuser.h IN/OUT SAL cascade (see the header for
    # the root-cause writeup) and supplies the POSIX scalar types universally.
    target_include_directories(${tgt} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat/win)
    target_compile_options(${tgt} PRIVATE
      /FI${CMAKE_CURRENT_SOURCE_DIR}/compat/win/dqlite_win_prelude.h)
  endif()
  if(NOT MSVC)  # true GNU/Clang driver (clang-cl takes MSVC-style flags)
    target_compile_options(${tgt} PRIVATE
      -fno-strict-aliasing -Wall -Wextra
      -Wno-unknown-warning-option -Wno-conversion)
  elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
    # clang-cl sets MSVC=1, so the branch above never fires for it. Forward the
    # same GNU-style flags through /clang: so the Windows build gets the exact
    # warning set (and the load-bearing -fno-strict-aliasing) the Linux build has.
    target_compile_options(${tgt} PRIVATE
      /clang:-fno-strict-aliasing /clang:-Wall /clang:-Wextra
      /clang:-Wno-unknown-warning-option /clang:-Wno-conversion)
    # -- Windows-only warning triage (separate from the base flags above) --
    # -Wno-gnu-folding-constant: the vendored raft tests (test_uv_init.c,
    # test_uv_set_term.c) size arrays with const-qualified variables; clang
    # accepts that as a GNU extension and gcc (the Linux compiler) has no such
    # diagnostic. Pure extension-acceptance noise, not a defect.
    target_compile_options(${tgt} PRIVATE
      /clang:-Wno-gnu-folding-constant)
  endif()
  if(DQLITE_SANITIZE)
    target_compile_options(${tgt} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(${tgt} PRIVATE -fsanitize=address)
  endif()
  if(DQLITE_CODE_COVERAGE)
    # --coverage == -fprofile-arcs -ftest-coverage; must appear at compile AND
    # link time. Produces .gcno next to each object (and .gcda when run).
    target_compile_options(${tgt} PRIVATE --coverage)
  endif()
endfunction()

# Attach dependency imported targets (Linux/macOS pkg-config path).
function(dqlite_link_deps tgt)
  target_link_libraries(${tgt} PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
  if(NOT WIN32)
    target_link_libraries(${tgt} PRIVATE PkgConfig::UV PkgConfig::SQLITE)
    if(TARGET PkgConfig::LZ4)
      target_link_libraries(${tgt} PRIVATE PkgConfig::LZ4)
      target_compile_definitions(${tgt} PRIVATE LZ4_AVAILABLE LZ4_ENABLED)
    endif()
  else()
    # Windows: vcpkg imported targets (see find_package block above).
    target_link_libraries(${tgt} PRIVATE libuv::uv unofficial::sqlite3::sqlite3)
    if(TARGET lz4::lz4)
      target_link_libraries(${tgt} PRIVATE lz4::lz4)
      target_compile_definitions(${tgt} PRIVATE LZ4_AVAILABLE LZ4_ENABLED)
    endif()
  endif()
  if(UNIX AND NOT APPLE)
    target_link_libraries(${tgt} PRIVATE m)
  endif()
  if(WIN32)
    # POSIX functions with no Win32/UCRT equivalent (statvfs, fstatfs, fcntl,
    # nftw, uname), implemented in compat/win/compat_win.c. Linked into every
    # target here so each executable resolves them. Windows ONLY -- the target
    # is defined under if(WIN32) below and this branch never runs elsewhere.
    target_link_libraries(${tgt} PRIVATE dqlite_win_compat)
  endif()
  # Backtrace link flags (-rdynamic / -lbacktrace / libunwind), matching
  # AM_LDFLAGS += $(BACKTRACE_LIBS). Empty unless DQLITE_ENABLE_BACKTRACE resolved.
  if(DQLITE_BACKTRACE_LINK)
    target_link_libraries(${tgt} PRIVATE ${DQLITE_BACKTRACE_LINK})
  endif()
  if(DQLITE_CODE_COVERAGE)
    target_link_options(${tgt} PRIVATE --coverage)
  endif()
endfunction()

# -- Windows-only warning triage, per-file scope (see the clang-cl block in
# dqlite_common_flags above for the tree-wide ones) --
# src/lib/addr.c compares socket lengths whose types differ per platform:
# Winsock declares socklen_t as (signed) int and addrinfo.ai_addrlen as
# size_t, where POSIX has unsigned socklen_t on both sides -- so two
# comparisons in this SHARED file are signed/unsigned on Windows only. The
# values are small and non-negative for every valid call, and the file must
# stay byte-identical on Linux, so suppress -Wsign-compare for this one file
# instead of editing it. Source properties are directory-scoped; every dqlite
# target is defined in this file, so the property reaches all of them.
if(WIN32 AND CMAKE_C_COMPILER_ID MATCHES "Clang")
  set_source_files_properties(src/lib/addr.c PROPERTIES
    COMPILE_OPTIONS "/clang:-Wno-sign-compare")
endif()

# test/raft/lib/addrinfo.c interposes getaddrinfo/freeaddrinfo (ELF symbol
# interposition + dlsym(RTLD_NEXT)) so the ADDRINFO_TEST injection tests can
# fake DNS results. CMAKE_C_VISIBILITY_PRESET=hidden (above) compiles those
# interposing definitions as .hidden, so they never reach the executable's
# dynamic symbol table and a SHARED libuv keeps binding libc's getaddrinfo:
# the injection silently never fires and tcp_connect/{firstIP,secondIP,
# closeDuringSecondConnect} fail against real DNS. Compile just this file
# with default visibility to restore parity with the automake build (which
# sets no visibility preset). Non-Windows only: on Windows these tests are
# skipped (see test/raft/lib/addrinfo.h) and the names would clash with
# ws2_32's exports.
if(NOT WIN32)
  set_source_files_properties(test/raft/lib/addrinfo.c PROPERTIES
    COMPILE_OPTIONS "-fvisibility=default")
endif()

# Windows-only static library backing the POSIX-compat shim headers. Defined
# before the first dqlite_link_deps() call so it is visible when targets link
# it. It only needs the compat include dir + forced prelude (dqlite_common_flags
# supplies both); it must NOT go through dqlite_link_deps() itself, which would
# make it depend on itself.
if(WIN32)
  add_library(dqlite_win_compat STATIC compat/win/compat_win.c)
  dqlite_common_flags(dqlite_win_compat)
endif()

# ------------------------------ Libraries ----------------------------------
# Object library so shared + static share one compile.
add_library(dqlite_objs OBJECT ${DQLITE_CORE_SOURCES} ${RAFT_SOURCES})
set_target_properties(dqlite_objs PROPERTIES POSITION_INDEPENDENT_CODE ON)
dqlite_common_flags(dqlite_objs)
dqlite_link_deps(dqlite_objs)   # propagates include dirs for uv.h/sqlite3.h
# Hide the vendored raft symbols inside libdqlite (Makefile passes -DRAFT_API='').
target_compile_definitions(dqlite_objs PRIVATE RAFT_API=)
if(WIN32)
  # These object files feed both the shared DLL and the static archive.
  # DQLITE_BUILDING flips DQLITE_API to __declspec(dllexport) (see dqlite.h) so
  # the public API is actually exported from dqlite.dll. No-op on Linux/macOS,
  # where DQLITE_API stays the ELF visibility attribute; hence the WIN32 guard.
  target_compile_definitions(dqlite_objs PRIVATE DQLITE_BUILDING)
endif()

if(DQLITE_BUILD_SHARED)
  add_library(dqlite SHARED $<TARGET_OBJECTS:dqlite_objs>)
  set_target_properties(dqlite PROPERTIES
    OUTPUT_NAME dqlite VERSION 0.0.1 SOVERSION 0)   # libtool -version-info 0:1:0
  dqlite_link_deps(dqlite)
endif()

if(DQLITE_BUILD_STATIC)
  add_library(dqlite_static STATIC $<TARGET_OBJECTS:dqlite_objs>)
  if(WIN32)
    # On Windows the SHARED target's import library is also named dqlite.lib,
    # which collides with a static archive named dqlite.lib (ninja: "multiple
    # rules generate dqlite.lib"). Give the static lib a distinct name so the
    # shared import lib stays dqlite.lib and the static becomes dqlite_static.lib.
    set_target_properties(dqlite_static PROPERTIES OUTPUT_NAME dqlite_static)
  else()
    set_target_properties(dqlite_static PROPERTIES OUTPUT_NAME dqlite)
  endif()
  dqlite_link_deps(dqlite_static)
endif()

# Install rules (public header + libs + pkg-config).
install(FILES include/dqlite.h DESTINATION include)
if(DQLITE_BUILD_SHARED)
  install(TARGETS dqlite LIBRARY DESTINATION lib)
endif()
if(DQLITE_BUILD_STATIC)
  install(TARGETS dqlite_static ARCHIVE DESTINATION lib)
endif()

# ------------------------------- Tests -------------------------------------
if(DQLITE_BUILD_TESTS)
  enable_testing()

  # libtest.la — shared test helpers (munit, fixtures). Compiled with the
  # warning relaxations from libtest_la_CFLAGS.
  set(LIBTEST_SOURCES
    test/lib/endpoint.c test/lib/fault.c test/lib/fs.c test/lib/heap.c
    test/lib/logger.c test/lib/munit.c test/lib/raft_heap.c test/lib/server.c
    test/lib/sqlite.c test/lib/uv.c
    test/raft/lib/addrinfo.c test/raft/lib/fault.c test/raft/lib/fsm.c
    test/raft/lib/heap.c test/raft/lib/tcp.c test/raft/lib/cluster.c
    test/raft/lib/aio.c test/raft/lib/dir.c test/raft/lib/loop.c)
  add_library(dqlite_test STATIC ${LIBTEST_SOURCES})
  dqlite_common_flags(dqlite_test)
  dqlite_link_deps(dqlite_test)
  target_compile_definitions(dqlite_test PUBLIC MUNIT_TEST_NAME_LEN=60)
  if(WIN32)
    # Test helpers embed/consume the dqlite API rather than build the DLL, so
    # opt out of the __declspec(dllimport) decoration (WIN32-only; see dqlite.h).
    target_compile_definitions(dqlite_test PRIVATE DQLITE_STATIC)
  endif()

  # libraft.la — raft sources as a static convenience lib for the raft tests.
  add_library(dqlite_raft STATIC ${RAFT_SOURCES})
  dqlite_common_flags(dqlite_raft)
  dqlite_link_deps(dqlite_raft)

  # Helper: define a munit test executable and register it with CTest.
  function(dqlite_test name)
    add_executable(${name} ${ARGN})
    dqlite_common_flags(${name})
    dqlite_link_deps(${name})
    target_link_libraries(${name} PRIVATE dqlite_test)
    if(WIN32)
      # Test executables embed/consume the dqlite API (some recompile core
      # sources such as tracing.c, some link the static archive) rather than
      # build the DLL, so blank the DQLITE_API decoration: no __declspec(
      # dllimport) on declarations they also define, and no import thunks for
      # symbols resolved statically. WIN32-only; no effect on Linux/macOS.
      target_compile_definitions(${name} PRIVATE DQLITE_STATIC)
      # Per-binary test-harness process setup: WSAStartup for the tests' raw
      # socket calls + quiet, non-interactive CRT abort/report behaviour (see
      # test/lib/win.c). Compiled into the executable itself, NOT into the
      # dqlite_test archive: an archive member containing only a constructor
      # and no referenced symbol would never be pulled in by the linker.
      target_sources(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/test/lib/win.c)
    endif()
    add_test(NAME ${name} COMMAND ${name})
  endfunction()

  # unit-test: core sources + raft lib + unit tests.
  dqlite_test(unit-test
    ${DQLITE_CORE_SOURCES}
    test/test_error.c test/test_integration.c
    test/unit/ext/test_uv.c test/unit/ext/test_uv_pool.c
    test/unit/lib/test_addr.c test/unit/lib/test_buffer.c
    test/unit/lib/test_byte.c test/unit/lib/test_registry.c
    test/unit/lib/test_serialize.c test/unit/lib/test_transport.c
    test/unit/test_command.c test/unit/test_conn.c test/unit/test_gateway.c
    test/unit/test_concurrency.c test/unit/test_registry.c
    test/unit/test_replication.c test/unit/test_request.c
    test/unit/test_role_management.c test/unit/test_sm.c
    test/unit/test_tuple.c test/unit/test_vfs.c test/unit/test_vfs_extra.c
    test/unit/main.c)
  target_compile_definitions(unit-test PRIVATE RAFT_API=)
  target_link_libraries(unit-test PRIVATE dqlite_raft)
  if(WIN32)
    # Windows-only unit tests for the compat/win named-pipe address mapping.
    # CMake-only, like test/lib/win.c: the autotools build never sees it.
    target_sources(unit-test PRIVATE test/unit/test_win_pipe.c)
  endif()

  # integration-test: links the full library.
  dqlite_test(integration-test
    test/integration/test_client.c test/integration/test_cluster.c
    test/integration/test_fsm.c test/integration/test_membership.c
    test/integration/test_node.c test/integration/test_role_management.c
    test/integration/test_server.c test/integration/test_stress.c
    test/integration/main.c)
  if(DQLITE_BUILD_STATIC)
    target_link_libraries(integration-test PRIVATE dqlite_static)
  else()
    target_link_libraries(integration-test PRIVATE dqlite)
  endif()

  # raft-core-unit-test
  dqlite_test(raft-core-unit-test
    ${RAFT_SOURCES} src/lib/sm.c src/lib/threadpool.c src/tracing.c
    test/raft/unit/main_core.c test/raft/unit/test_byte.c
    test/raft/unit/test_compress.c test/raft/unit/test_configuration.c
    test/raft/unit/test_encoding_limits.c test/raft/unit/test_err.c
    test/raft/unit/test_flags.c test/raft/unit/test_log.c
    test/raft/unit/test_queue.c)

  # raft-core-integration-test
  dqlite_test(raft-core-integration-test
    src/tracing.c src/lib/sm.c src/lib/threadpool.c
    test/raft/integration/main_core.c test/raft/integration/test_apply.c
    test/raft/integration/test_assign.c test/raft/integration/test_barrier.c
    test/raft/integration/test_bootstrap.c test/raft/integration/test_digest.c
    test/raft/integration/test_election.c test/raft/integration/test_fixture.c
    test/raft/integration/test_heap.c test/raft/integration/test_init.c
    test/raft/integration/test_membership.c
    test/raft/integration/test_recover.c
    test/raft/integration/test_replication.c
    test/raft/integration/test_snapshot.c test/raft/integration/test_start.c
    test/raft/integration/test_strerror.c test/raft/integration/test_tick.c
    test/raft/integration/test_transfer.c
    test/raft/integration/test_voter_contacts.c)
  target_link_libraries(raft-core-integration-test PRIVATE dqlite_raft)

  # raft-core-fuzzy-test
  dqlite_test(raft-core-fuzzy-test
    src/lib/sm.c src/lib/threadpool.c src/tracing.c
    test/raft/fuzzy/main_core.c test/raft/fuzzy/test_election.c
    test/raft/fuzzy/test_liveness.c test/raft/fuzzy/test_membership.c
    test/raft/fuzzy/test_replication.c)
  target_link_libraries(raft-core-fuzzy-test PRIVATE dqlite_raft)

  # raft-uv-unit-test: a subset of the raft I/O sources built directly.
  dqlite_test(raft-uv-unit-test
    src/tracing.c src/raft/err.c src/raft/heap.c src/raft/syscall.c
    src/raft/uv_fs.c src/raft/uv_os.c src/raft/uv_writer.c
    test/raft/unit/main_uv.c test/raft/unit/test_uv_fs.c
    test/raft/unit/test_uv_os.c test/raft/unit/test_uv_writer.c)

  # raft-uv-integration-test: built against raft sources directly (tests
  # non-exported internals), NOT linked to the raft convenience lib.
  dqlite_test(raft-uv-integration-test
    ${RAFT_SOURCES} src/tracing.c src/lib/sm.c src/lib/threadpool.c
    test/raft/integration/main_uv.c test/raft/integration/test_uv_init.c
    test/raft/integration/test_uv_append.c
    test/raft/integration/test_uv_bootstrap.c
    test/raft/integration/test_uv_load.c
    test/raft/integration/test_uv_recover.c
    test/raft/integration/test_uv_recv.c test/raft/integration/test_uv_send.c
    test/raft/integration/test_uv_set_term.c
    test/raft/integration/test_uv_tcp_connect.c
    test/raft/integration/test_uv_tcp_listen.c
    test/raft/integration/test_uv_snapshot_put.c
    test/raft/integration/test_uv_timer.c
    test/raft/integration/test_uv_truncate.c
    test/raft/integration/test_uv_truncate_snapshot.c
    test/raft/integration/test_uv_work.c)

  # raft-durability-child: tiny helper process for the crash-durability test
  # (append/crashDurability in test_uv_append.c). The test spawns it, reads
  # one acknowledgement byte per completed append from its stdout, then kills
  # it without warning and verifies every acknowledged entry is still on
  # disk. Not a test itself (no dqlite_test/add_test): the parent test drives
  # it, expecting it next to the test executables. CMake-only on purpose - on
  # POSIX the test forks instead, so the autotools build never needs it (and
  # Makefile.am must not change); the parent test skips its Windows arm when
  # the helper is absent.
  add_executable(raft-durability-child
    test/tools/durability_child.c
    src/tracing.c src/lib/sm.c src/lib/threadpool.c)
  dqlite_common_flags(raft-durability-child)
  dqlite_link_deps(raft-durability-child)
  target_link_libraries(raft-durability-child PRIVATE dqlite_raft)
  if(WIN32)
    # Same rationale as dqlite_test(): consumes the raft/dqlite API statically,
    # so blank the __declspec(dllimport) decoration.
    target_compile_definitions(raft-durability-child PRIVATE DQLITE_STATIC)
  endif()
endif()

# Best-effort lcov report target (optional; only if lcov + genhtml are present).
# `cmake --build <dir> --target coverage` runs the suite then captures an HTML
# report. Mirrors the aminclude_static.am lcov machinery at a basic level,
# including the assert(...) branch exclusion from Makefile.am:363.
if(DQLITE_CODE_COVERAGE AND NOT WIN32)
  find_program(LCOV_EXECUTABLE lcov)
  find_program(GENHTML_EXECUTABLE genhtml)
  if(LCOV_EXECUTABLE AND GENHTML_EXECUTABLE)
    add_custom_target(coverage
      COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
      COMMAND ${LCOV_EXECUTABLE} --directory ${CMAKE_BINARY_DIR} --capture
              --output-file coverage.info
              --rc "lcov_excl_br_line=assert\\("
      COMMAND ${LCOV_EXECUTABLE} --remove coverage.info "/usr/include/*"
              --output-file coverage.info
      COMMAND ${GENHTML_EXECUTABLE} coverage.info --output-directory coverage
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
      COMMENT "Running tests and capturing coverage with lcov"
      VERBATIM)
  else()
    message(STATUS "dqlite code coverage: lcov/genhtml not found; "
                   "'coverage' report target not created (flags still active)")
  endif()
endif()

if(TARGET PkgConfig::LZ4 OR TARGET lz4::lz4)
  set(_dqlite_lz4 yes)
else()
  set(_dqlite_lz4 no)
endif()
message(STATUS "dqlite ${DQLITE_VERSION} (${DQLITE_BUILD_TAG}) "
               "platform=${CMAKE_SYSTEM_NAME} compiler=${CMAKE_C_COMPILER_ID} "
               "lz4=${_dqlite_lz4}")
