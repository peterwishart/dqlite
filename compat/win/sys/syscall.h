/*
 * dqlite Windows port -- <sys/syscall.h> minimal shim.
 *
 * On Windows the only reachable use is src/tracing.c's syscall(SYS_gettid) to
 * obtain a thread id (raft/syscall.c's syscall() uses are all gated behind
 * DQLITE_HAVE_KAIO, undefined off-Linux). Map SYS_gettid -> the current Win32
 * thread id. Windows ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_SYS_SYSCALL_H
#define DQLITE_COMPAT_SYS_SYSCALL_H

/* Shadowing fence (PORT_TODO.md W3): only TUs built with dqlite's forced-
 * include prelude (clang-cl /FI dqlite_win_prelude.h, which defines
 * DQLITE_WIN_COMPAT) may use this shim; any other consumer that resolves this
 * name -- e.g. a dependency TU compiled inside a dqlite target -- must break
 * loudly here instead of silently picking up stubs. Full rationale in
 * compat/win/dqlite_win_prelude.h. */
#if !defined(_WIN32) || !defined(DQLITE_WIN_COMPAT)
#error "dqlite compat/win shim header reached without the dqlite Windows prelude (DQLITE_WIN_COMPAT undefined); this header must not shadow a real system header -- see compat/win/dqlite_win_prelude.h"
#endif

#ifndef _INC_WINDOWS
#include <windows.h>
#endif

#ifndef DQLITE_PID_T_DEFINED
#define DQLITE_PID_T_DEFINED
typedef int pid_t;
#endif

#define SYS_gettid 1

/* Variadic to match the POSIX syscall() prototype; only SYS_gettid is used. */
static inline long syscall(long number, ...)
{
	if (number == SYS_gettid) {
		return (long)GetCurrentThreadId();
	}
	return -1;
}

#endif /* DQLITE_COMPAT_SYS_SYSCALL_H */
