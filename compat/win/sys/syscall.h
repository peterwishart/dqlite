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
