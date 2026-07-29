#include <ftw.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "fs.h"
#include "munit.h"

char *test_dir_setup()
{
#ifdef _WIN32
	/* On Windows there is no /tmp; use the real temp dir as the parent so
	 * the mkdtemp shim's _mkdir does not fail with ENOENT. */
	const char *parent = getenv("TMP");
	if (parent == NULL) {
		parent = getenv("TEMP");
	}
	if (parent == NULL) {
		parent = ".";
	}
	char *dir =
	    munit_malloc(strlen(parent) + strlen("\\dqlite-test-XXXXXX") + 1);
	sprintf(dir, "%s\\dqlite-test-XXXXXX", parent);
#else
	char *dir = munit_malloc(strlen(TEST__DIR_TEMPLATE) + 1);

	strcpy(dir, TEST__DIR_TEMPLATE);
#endif

	munit_assert_ptr_not_null(mkdtemp(dir));

	return dir;
}

static int test__dir_tear_down_nftw_fn(const char *path,
				       const struct stat *sb,
				       int type,
				       struct FTW *ftwb)
{
	int rc;

	(void)sb;
	(void)type;
	(void)ftwb;

#ifdef _WIN32
	/* POSIX remove() unlinks a file or removes an (empty) directory; the
	 * Windows CRT remove() only deletes files and fails on directories and
	 * on read-only files. Emulate the POSIX behaviour: clear the read-only
	 * attribute, then _rmdir() a directory or remove() a file. */
	{
		DWORD attrs = GetFileAttributesA(path);
		if (attrs == INVALID_FILE_ATTRIBUTES) {
			rc = -1;
		} else {
			if (attrs & FILE_ATTRIBUTE_READONLY) {
				SetFileAttributesA(
				    path, attrs & ~(DWORD)FILE_ATTRIBUTE_READONLY);
			}
			rc = (attrs & FILE_ATTRIBUTE_DIRECTORY) ? _rmdir(path)
								: remove(path);
		}
	}
#else
	rc = remove(path);
#endif
	munit_assert_int(rc, ==, 0);

	return 0;
}

void test_dir_tear_down(char *dir)
{
	int rc;

	if (dir == NULL) {
		return;
	}

	rc = nftw(dir, test__dir_tear_down_nftw_fn, 10,
		  FTW_DEPTH | FTW_MOUNT | FTW_PHYS);
	munit_assert_int(rc, ==, 0);
	free(dir);
}
