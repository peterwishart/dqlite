/*
 * dqlite Windows port -- <uv/unix.h> redirect stub.
 *
 * A few sources (e.g. src/lib/threadpool.c) hardcode `#include <uv/unix.h>`,
 * libuv's Linux/BSD/Darwin platform header, which pulls in <termios.h> and
 * other POSIX-only headers absent on Windows. On Windows the equivalent
 * platform definitions come from <uv/win.h>, which <uv.h> has already included
 * before any such line is reached. This Windows-only stub therefore just needs
 * to resolve to nothing (it shadows the real vcpkg uv/unix.h because compat/win
 * precedes the vcpkg include dir, but ONLY on Windows). No source edits needed.
 */
#ifndef DQLITE_COMPAT_UV_UNIX_H
#define DQLITE_COMPAT_UV_UNIX_H

#include <uv.h> /* ensures uv/win.h platform types are present */

#endif /* DQLITE_COMPAT_UV_UNIX_H */
