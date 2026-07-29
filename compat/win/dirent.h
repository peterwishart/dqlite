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
