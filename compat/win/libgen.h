/*
 * dqlite Windows port -- <libgen.h> minimal shim (basename/dirname).
 *
 * Used by src/raft/uv_os.c for path splitting. Declarations only; the
 * IMPLEMENTATION is deferred to a later iteration. Windows ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_LIBGEN_H
#define DQLITE_COMPAT_LIBGEN_H

char *basename(char *path);
char *dirname(char *path);

#endif /* DQLITE_COMPAT_LIBGEN_H */
