/*
 * dqlite Windows port -- <sched.h> minimal shim.
 *
 * src/server.c includes <sched.h> but uses no scheduler API on the paths that
 * matter here; sched_yield is mapped to the Win32 yield for completeness.
 * Windows ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_SCHED_H
#define DQLITE_COMPAT_SCHED_H

#ifndef _INC_WINDOWS
#include <windows.h>
#endif

static inline int sched_yield(void)
{
	SwitchToThread();
	return 0;
}

#endif /* DQLITE_COMPAT_SCHED_H */
