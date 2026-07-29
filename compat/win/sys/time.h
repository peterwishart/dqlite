/*
 * dqlite Windows port -- <sys/time.h> POSIX-compat shim.
 *
 * struct timeval comes from Winsock2 (pulled by the prelude). Provides
 * gettimeofday() backed by GetSystemTimePreciseAsFileTime. Windows ONLY.
 */
#ifndef DQLITE_COMPAT_SYS_TIME_H
#define DQLITE_COMPAT_SYS_TIME_H

/* Shadowing fence (PORT_TODO.md W3): only TUs built with dqlite's forced-
 * include prelude (clang-cl /FI dqlite_win_prelude.h, which defines
 * DQLITE_WIN_COMPAT) may use this shim; any other consumer that resolves this
 * name -- e.g. a dependency TU compiled inside a dqlite target -- must break
 * loudly here instead of silently picking up stubs. Full rationale in
 * compat/win/dqlite_win_prelude.h. */
#if !defined(_WIN32) || !defined(DQLITE_WIN_COMPAT)
#error "dqlite compat/win shim header reached without the dqlite Windows prelude (DQLITE_WIN_COMPAT undefined); this header must not shadow a real system header -- see compat/win/dqlite_win_prelude.h"
#endif

#include <winsock2.h> /* struct timeval */
#ifndef _INC_WINDOWS
#include <windows.h>
#endif

struct timezone {
	int tz_minuteswest;
	int tz_dsttime;
};

static inline int gettimeofday(struct timeval *tv, void *tz)
{
	FILETIME ft;
	unsigned long long t;
	(void)tz;
	if (tv == NULL) {
		return 0;
	}
	GetSystemTimePreciseAsFileTime(&ft);
	/* FILETIME is 100ns ticks since 1601-01-01; convert to Unix epoch. */
	t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	t -= 116444736000000000ULL; /* 1601 -> 1970 */
	tv->tv_sec = (long)(t / 10000000ULL);
	tv->tv_usec = (long)((t % 10000000ULL) / 10ULL);
	return 0;
}

#endif /* DQLITE_COMPAT_SYS_TIME_H */
