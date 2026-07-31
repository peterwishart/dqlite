/* Windows-only, per-test-binary process setup for the dqlite test harness.
 *
 * This file is compiled directly into EVERY test executable (see the WIN32
 * branch of the dqlite_test() helper in CMakeLists.txt). It is added to each
 * executable's own source list rather than to the dqlite_test convenience
 * archive on purpose: an archive member that contains only a constructor and
 * no symbol anybody references would never be pulled in by the linker, so the
 * setup would silently not run -- exactly the link-order-luck failure mode
 * this file exists to remove. An object listed on the
 * executable's link line is always included, so the constructor below is
 * guaranteed to run, before main() and before any munit suite-registration
 * constructor can matter.
 *
 * The CRT abort/report changes used to live in a constructor in
 * compat/win/compat_win.c, i.e. inside the shipped library, where they
 * suppressed WER crash dumps for ANY host process embedding dqlite. They are
 * test-harness policy, not library behaviour, so they were moved here;
 * consumers of dqlite.dll / dqlite_static.lib keep the CRT defaults (and
 * their crash dumps). The library keeps only the Winsock initialisation, done
 * structurally from its public entry points (dqliteWinSocketsInit(), see
 * compat/win/compat_win.c).
 */

#ifdef _WIN32

#include <winsock2.h>

#include <stdlib.h>

#ifdef _DEBUG
#include <crtdbg.h>
#endif

__attribute__((constructor)) static void testWinHarnessInit(void)
{
	/* Winsock: the tests make raw socket()/getaddrinfo() calls
	 * (test/lib/endpoint.c, test/raft/lib/tcp.c, and the src/lib/addr.c
	 * unit tests) that can run before any dqlite entry point or libuv
	 * loop has initialised Winsock, and getaddrinfo() fails with
	 * WSANOTINITIALISED if it has not been started. WSAStartup is
	 * reference-counted by Windows, so this extra init is harmless
	 * alongside the library's own dqliteWinSocketsInit(); the matching
	 * WSACleanup is intentionally omitted (process teardown reclaims it,
	 * and the test process needs sockets for its whole lifetime). */
	WSADATA wsa_data;
	(void)WSAStartup(MAKEWORD(2, 2), &wsa_data);

	/* Keep automated/CI test runs non-interactive. By default a failed
	 * assert()/abort() writes an abort message and hands the process to
	 * Windows Error Reporting (_CALL_REPORTFAULT), which can block an
	 * unattended run behind crash-reporting UI; the debug CRT additionally
	 * pops modal dialogs for asserts and heap errors. Suppress the abort
	 * message + WER hand-off and route the debug-CRT reports to stderr, so
	 * a crashing test prints its diagnostic and exits. Test binaries
	 * ONLY -- shipped library consumers keep the CRT defaults. */
	_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
	int modes[] = { _CRT_WARN, _CRT_ERROR, _CRT_ASSERT };
	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
		_CrtSetReportMode(modes[i], _CRTDBG_MODE_FILE);
		_CrtSetReportFile(modes[i], _CRTDBG_FILE_STDERR);
	}
#endif
}

#endif /* _WIN32 */
