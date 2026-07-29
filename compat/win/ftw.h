/*
 * dqlite Windows port -- <ftw.h> minimal shim (nftw).
 *
 * Test-only (test/lib/fs.c, test/raft/lib/dir.c use nftw to recursively remove
 * a directory tree). Declarations + flag constants only; IMPLEMENTATION
 * deferred to a later iteration. Windows ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_FTW_H
#define DQLITE_COMPAT_FTW_H

#include <sys/stat.h> /* struct stat (UCRT) */

/* typeflag values passed to the callback. */
#define FTW_F 0
#define FTW_D 1
#define FTW_DNR 2
#define FTW_NS 3
#define FTW_SL 4
#define FTW_DP 5
#define FTW_SLN 6

/* nftw() flags. */
#define FTW_PHYS 1
#define FTW_MOUNT 2
#define FTW_CHDIR 4
#define FTW_DEPTH 8
#define FTW_ACTIONRETVAL 16

struct FTW {
	int base;
	int level;
};

int nftw(const char *dirpath,
	 int (*fn)(const char *fpath,
		   const struct stat *sb,
		   int typeflag,
		   struct FTW *ftwbuf),
	 int nopenfd,
	 int flags);

#endif /* DQLITE_COMPAT_FTW_H */
