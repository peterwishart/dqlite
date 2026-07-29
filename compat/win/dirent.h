/*
 * dqlite Windows port -- <dirent.h> minimal shim.
 *
 * dqlite uses scandir() (directory enumeration). UCRT has no POSIX dirent API;
 * a real port would back these with FindFirstFile/FindNextFile. Declarations +
 * types only here so the sources compile; IMPLEMENTATION deferred to a later
 * iteration. Windows ONLY (compat/win/).
 */
#ifndef DQLITE_COMPAT_DIRENT_H
#define DQLITE_COMPAT_DIRENT_H

/* Shadowing fence (PORT_TODO.md W3): only TUs built with dqlite's forced-
 * include prelude (clang-cl /FI dqlite_win_prelude.h, which defines
 * DQLITE_WIN_COMPAT) may use this shim; any other consumer that resolves this
 * name -- e.g. a dependency TU compiled inside a dqlite target -- must break
 * loudly here instead of silently picking up stubs. Full rationale in
 * compat/win/dqlite_win_prelude.h. */
#if !defined(_WIN32) || !defined(DQLITE_WIN_COMPAT)
#error "dqlite compat/win shim header reached without the dqlite Windows prelude (DQLITE_WIN_COMPAT undefined); this header must not shadow a real system header -- see compat/win/dqlite_win_prelude.h"
#endif

/* d_type values. */
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12

struct dirent {
	long d_ino;
	unsigned short d_reclen;
	unsigned char d_type;
	char d_name[260];
};

typedef struct dqlite_DIR DIR;

DIR *opendir(const char *name);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);
void rewinddir(DIR *dirp);

int scandir(const char *dirp,
	    struct dirent ***namelist,
	    int (*filter)(const struct dirent *),
	    int (*compar)(const struct dirent **, const struct dirent **));
int alphasort(const struct dirent **a, const struct dirent **b);

#endif /* DQLITE_COMPAT_DIRENT_H */
