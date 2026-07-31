/*
 * dqlite Windows port -- <linux/limits.h> minimal shim.
 *
 * Provides PATH_MAX / NAME_MAX (also defined by the prelude). Windows ONLY.
 */
#ifndef DQLITE_COMPAT_LINUX_LIMITS_H
#define DQLITE_COMPAT_LINUX_LIMITS_H

/* Shadowing fence -- see the rationale in dqlite_win_prelude.h. */
#if !defined(_WIN32) || !defined(DQLITE_WIN_COMPAT)
#error "dqlite compat/win shim header reached without the dqlite Windows prelude (DQLITE_WIN_COMPAT undefined); this header must not shadow a real system header -- see compat/win/dqlite_win_prelude.h"
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#endif /* DQLITE_COMPAT_LINUX_LIMITS_H */
