/*
 * dqlite Windows port -- <linux/limits.h> minimal shim.
 *
 * Provides PATH_MAX / NAME_MAX (also defined by the prelude). Windows ONLY.
 */
#ifndef DQLITE_COMPAT_LINUX_LIMITS_H
#define DQLITE_COMPAT_LINUX_LIMITS_H

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#endif /* DQLITE_COMPAT_LINUX_LIMITS_H */
