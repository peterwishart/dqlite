/* Unit tests for the Windows local-transport address mapping
 * DqliteWinPipeName() ("@name" -> "\\.\pipe\dqlite-<name>", see
 * compat/win/dqlite_win_pipe.h).
 *
 * Windows-only: this file is registered in CMakeLists.txt's WIN32 branch only
 * (like test/lib/win.c) and never reaches the autotools build, whose files
 * must stay byte-identical to upstream. */

#include "../lib/runner.h"

#include "dqlite_win_pipe.h"

TEST_MODULE(win_pipe);

TEST_SUITE(name);

#define PREFIX "\\\\.\\pipe\\dqlite-"
#define PREFIX_LEN (sizeof(PREFIX) - 1)

/* Basic mapping: "@foo" -> "\\.\pipe\dqlite-foo"; the leading '@' is
 * optional. */
TEST_CASE(name, basic, NULL)
{
	char out[256];
	(void)params;
	(void)data;
	munit_assert_int(DqliteWinPipeName("@foo", out, sizeof out), ==, 0);
	munit_assert_string_equal(out, PREFIX "foo");
	munit_assert_int(DqliteWinPipeName("foo", out, sizeof out), ==, 0);
	munit_assert_string_equal(out, PREFIX "foo");
	return MUNIT_OK;
}

/* Characters a pipe-path component may not contain ('\', '/', ':') are folded
 * to '_'; everything else passes through. */
TEST_CASE(name, sanitize, NULL)
{
	char out[256];
	(void)params;
	(void)data;
	munit_assert_int(DqliteWinPipeName("@a/b\\c:d", out, sizeof out), ==,
			 0);
	munit_assert_string_equal(out, PREFIX "a_b_c_d");
	return MUNIT_OK;
}

/* A bare "@" (empty name) maps to the bare prefix. (Production never binds
 * it: src/server.c synthesizes a process-unique name for "@" first.) */
TEST_CASE(name, empty, NULL)
{
	char out[256];
	(void)params;
	(void)data;
	munit_assert_int(DqliteWinPipeName("@", out, sizeof out), ==, 0);
	munit_assert_string_equal(out, PREFIX);
	return MUNIT_OK;
}

/* A name that does not fit the out buffer is an error, never a truncated
 * path; the boundary case that exactly fits succeeds. */
TEST_CASE(name, too_long, NULL)
{
	char out[256];
	char address[512];
	size_t fit = sizeof(out) - PREFIX_LEN - 1; /* longest name that fits */
	(void)params;
	(void)data;

	address[0] = '@';
	memset(address + 1, 'x', fit);
	address[1 + fit] = '\0';
	munit_assert_int(DqliteWinPipeName(address, out, sizeof out), ==, 0);
	munit_assert_size(strlen(out), ==, sizeof(out) - 1);

	/* One more character no longer fits. */
	address[1 + fit] = 'x';
	address[2 + fit] = '\0';
	munit_assert_int(DqliteWinPipeName(address, out, sizeof out), ==, -1);
	munit_assert_string_equal(out, "");

	/* A buffer too small even for the prefix. */
	munit_assert_int(DqliteWinPipeName("@foo", out, PREFIX_LEN), ==, -1);
	munit_assert_string_equal(out, "");
	return MUNIT_OK;
}

/* Two distinct long addresses must never share a pipe path. Before the guard,
 * names sharing a 127-char prefix were silently truncated onto ONE pipe; now
 * long names that fit map to distinct paths and longer ones error out. */
TEST_CASE(name, long_distinct, NULL)
{
	char out1[256];
	char out2[256];
	char address[256];
	size_t len = 200; /* > the old 127-char internal limit, still fits */
	(void)params;
	(void)data;

	address[0] = '@';
	memset(address + 1, 'y', len);
	address[1 + len] = '\0';
	munit_assert_int(DqliteWinPipeName(address, out1, sizeof out1), ==, 0);
	address[len] = 'z'; /* differ only in the last character */
	munit_assert_int(DqliteWinPipeName(address, out2, sizeof out2), ==, 0);
	munit_assert_string_not_equal(out1, out2);
	return MUNIT_OK;
}
