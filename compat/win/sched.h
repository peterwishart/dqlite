/*
 * dqlite Windows port -- <sched.h> minimal shim.
 *
 * src/server.c includes <sched.h> but uses no scheduler API on the paths that
 * matter here; sched_yield is mapped to the Win32 yield for completeness.
 * Windows ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_SCHED_H
#define DQLITE_COMPAT_SCHED_H

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

static inline int sched_yield(void)
{
	SwitchToThread();
	return 0;
}

#endif /* DQLITE_COMPAT_SCHED_H */
