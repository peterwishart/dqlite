/*
 * dqlite Windows port -- local ("@name") transport over Win32 named pipes.
 *
 * dqlite's local (non-TCP) transport uses the Linux ABSTRACT-namespace AF_UNIX
 * convention: an address of the form "@name" (leading '@', kernel-global name,
 * no filesystem entry). Windows <afunix.h> AF_UNIX cannot bind the abstract
 * namespace, so on Windows the "@name" family is carried over a libuv named
 * pipe (uv_pipe_t == "\\.\pipe\..."), which is the idiomatic Win32 local-IPC
 * primitive and the closest analogue to a kernel-global abstract socket.
 *
 * This header defines the single, deterministic mapping from a dqlite "@name"
 * address to its named-pipe path, shared by BOTH ends so listener and connector
 * agree byte-for-byte:
 *
 *     "@name"  ->  "\\.\pipe\dqlite-<name>"
 *
 * The mapping is a pure function of <name> (no PID/entropy), exactly mirroring
 * the Linux abstract namespace where "@name" is a global rendezvous point: any
 * process that knows the advertised address can connect. (This inherits the
 * same cross-process name-collision semantics as the Linux abstract namespace.)
 *
 * This file lives in compat/win/, which is on the include path on Windows ONLY
 * (see CMakeLists.txt); it is never visible to the Linux/macOS build.
 */
#ifndef DQLITE_WIN_PIPE_H
#define DQLITE_WIN_PIPE_H

/* Shadowing fence (PORT_TODO.md W3): only TUs built with dqlite's forced-
 * include prelude (clang-cl /FI dqlite_win_prelude.h, which defines
 * DQLITE_WIN_COMPAT) may use this shim; any other consumer that resolves this
 * name -- e.g. a dependency TU compiled inside a dqlite target -- must break
 * loudly here instead of silently picking up stubs. Full rationale in
 * compat/win/dqlite_win_prelude.h. */
#if !defined(_WIN32) || !defined(DQLITE_WIN_COMPAT)
#error "dqlite compat/win shim header reached without the dqlite Windows prelude (DQLITE_WIN_COMPAT undefined); this header must not shadow a real system header -- see compat/win/dqlite_win_prelude.h"
#endif

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifndef _INC_WINDOWS
#include <windows.h> /* CreateFile, ReadFile, WriteFile, OVERLAPPED, ... */
#endif

/* Pipe-name prefix. A named pipe path component cannot contain a backslash. */
#define DQLITE_WIN_PIPE_PREFIX "\\\\.\\pipe\\dqlite-"

/* Build the named-pipe path for a dqlite "@name" address into `out`.
 *
 * `at_address` is the dqlite address; a leading '@' (if present) is stripped.
 * The <name> is sanitized so it is a legal pipe-path component: the characters
 * that a pipe name may not contain ('\\', '/', ':') are folded to '_'.
 *
 * Returns 0 on success, -1 if prefix + sanitized name + NUL does not fit in
 * `out_len` bytes. The path is NEVER truncated: truncation would silently
 * collide two distinct long addresses on one pipe, so an over-long address is
 * an error instead (`out` is set to "" so it cannot be used as a path). All
 * callers pass 256-byte buffers, matching the Win32 pipe-name limit. */
static inline int DqliteWinPipeName(const char *at_address,
				    char *out,
				    size_t out_len)
{
	const char *name = (at_address[0] == '@') ? at_address + 1 : at_address;
	const size_t prefix_len = sizeof(DQLITE_WIN_PIPE_PREFIX) - 1;
	size_t i;

	if (out_len < prefix_len + 1) {
		if (out_len > 0) {
			out[0] = '\0';
		}
		return -1;
	}
	memcpy(out, DQLITE_WIN_PIPE_PREFIX, prefix_len);
	for (i = 0; name[i] != '\0'; i++) {
		char c = name[i];
		if (prefix_len + i + 1 >= out_len) {
			out[0] = '\0';
			return -1;
		}
		if (c == '\\' || c == '/' || c == ':') {
			c = '_';
		}
		out[prefix_len + i] = c;
	}
	out[prefix_len + i] = '\0';
	return 0;
}

/*
 * Overlapped synchronous I/O for the local-transport pipe.
 *
 * The connect side opens the pipe with FILE_FLAG_OVERLAPPED so that, once the
 * descriptor is handed to libuv (uv_pipe_open, raft transport), libuv drives it
 * with IOCP rather than the UV_HANDLE_NON_OVERLAPPED_PIPE fallback -- which
 * burns one libuv threadpool worker per pipe read and DEADLOCKS a multi-node
 * cluster (N*(N-1) connections) once the default 4-worker pool is exhausted.
 *
 * Because the handle is overlapped, plain synchronous WriteFile/ReadFile with a
 * NULL OVERLAPPED are invalid (Win32 requires a non-NULL OVERLAPPED for such
 * handles). The synchronous handshake writes (src/transport.c) and the blocking
 * client I/O (src/client/protocol.c) therefore go through these helpers, which
 * drive one overlapped operation to completion using a private event. They run
 * BEFORE (handshake) or entirely OUTSIDE (client) libuv's ownership, so using a
 * private event does not conflict with libuv's IOCP.
 */

/* Write exactly `len` bytes. Returns bytes written, or -1 on error. Blocks to
 * completion (transport handshake and client requests are small). */
static inline int DqliteWinPipeWriteAll(HANDLE h, const void *buf, size_t len)
{
	size_t total = 0;
	OVERLAPPED ov;
	DWORD done;
	BOOL ok;

	while (total < len) {
		memset(&ov, 0, sizeof ov);
		ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
		if (ov.hEvent == NULL) {
			return -1;
		}
		ok = WriteFile(h, (const char *)buf + total,
			       (DWORD)(len - total), NULL, &ov);
		if (!ok && GetLastError() == ERROR_IO_PENDING) {
			WaitForSingleObject(ov.hEvent, INFINITE);
			ok = GetOverlappedResult(h, &ov, &done, FALSE);
		} else if (ok) {
			ok = GetOverlappedResult(h, &ov, &done, FALSE);
		}
		CloseHandle(ov.hEvent);
		if (!ok) {
			return -1;
		}
		if (done == 0) {
			break;
		}
		total += done;
	}
	return (int)total;
}

/* Distinct-from-error sentinel returned by DqliteWinPipeReadTimed when the
 * timeout expires: 0 is reserved for remote EOF (mirroring read(2)), so a
 * caller can tell a closed peer from a slow one. */
#define DQLITE_WIN_PIPE_TIMEOUT (-2)

/* Read up to `len` bytes with a millisecond timeout (`timeout_ms < 0` == wait
 * forever). Returns bytes read (>0), 0 on EOF, DQLITE_WIN_PIPE_TIMEOUT on
 * timeout, -1 on error. */
static inline int DqliteWinPipeReadTimed(HANDLE h,
					 void *buf,
					 size_t len,
					 long long timeout_ms)
{
	OVERLAPPED ov;
	DWORD done = 0;
	DWORD wms;
	BOOL ok;

	memset(&ov, 0, sizeof ov);
	ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
	if (ov.hEvent == NULL) {
		return -1;
	}
	ok = ReadFile(h, buf, (DWORD)len, NULL, &ov);
	if (!ok && GetLastError() == ERROR_IO_PENDING) {
		wms = (timeout_ms < 0)
			  ? INFINITE
			  : ((timeout_ms > 0x7fffffff) ? 0x7fffffff
						       : (DWORD)timeout_ms);
		if (WaitForSingleObject(ov.hEvent, wms) == WAIT_TIMEOUT) {
			CancelIoEx(h, &ov);
			/* Wait for the cancellation to settle before the
			 * OVERLAPPED/buffer go out of scope. The read may have
			 * completed in the WAIT_TIMEOUT..CancelIoEx window;
			 * such bytes are already consumed from the pipe, so
			 * return them rather than dropping them. */
			ok = GetOverlappedResult(h, &ov, &done, TRUE);
			CloseHandle(ov.hEvent);
			return (ok && done > 0) ? (int)done
						: DQLITE_WIN_PIPE_TIMEOUT;
		}
		ok = GetOverlappedResult(h, &ov, &done, FALSE);
	} else if (ok) {
		ok = GetOverlappedResult(h, &ov, &done, FALSE);
	}
	CloseHandle(ov.hEvent);
	if (!ok) {
		DWORD err = GetLastError();
		if (err == ERROR_BROKEN_PIPE || err == ERROR_HANDLE_EOF) {
			return 0; /* EOF */
		}
		return -1;
	}
	return (int)done;
}

#endif /* DQLITE_WIN_PIPE_H */
