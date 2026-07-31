#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "endpoint.h"

#ifdef _WIN32
#include <io.h> /* _open_osfhandle */

#include <uv.h>

#include "../../src/raft.h" /* raft_malloc / raft_free */
#include "dqlite_win_pipe.h" /* DqliteWinPipeName: "@name" -> named-pipe path */

/* Connect a client end to the named-pipe listener bound by
 * test_endpoint_listen(). Mirrors the production connect
 * (test/lib/server.c endpointConnectPipe), but opens the handle in SYNCHRONOUS
 * mode (no FILE_FLAG_OVERLAPPED): unlike the production client fd, this one is
 * NOT handed to libuv -- the endpoint tests drive it with blocking CRT
 * read()/write(), which require a synchronous handle. Returns a CRT fd wrapping
 * the pipe HANDLE via _open_osfhandle. */
static int endpointConnectPipe(struct test_endpoint *e)
{
	char pipe_name[256];
	HANDLE h;
	int osfd;
	int attempts;

	munit_assert_int(DqliteWinPipeName(e->address, pipe_name,
					   sizeof pipe_name), ==, 0);

	/* The listener is bound (uv_pipe_bind is synchronous) before this runs,
	 * but give a short bounded retry for "busy"/"not yet there" to be
	 * robust against libuv instance timing. */
	for (attempts = 0;; attempts++) {
		h = CreateFileA(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
				OPEN_EXISTING, 0, NULL);
		if (h != INVALID_HANDLE_VALUE) {
			break;
		}
		if (GetLastError() == ERROR_PIPE_BUSY && attempts < 50) {
			WaitNamedPipeA(pipe_name, 100);
			continue;
		}
		if (GetLastError() == ERROR_FILE_NOT_FOUND && attempts < 50) {
			Sleep(1);
			continue;
		}
		munit_errorf("CreateFile(%s): error %lu", pipe_name,
			     (unsigned long)GetLastError());
	}

	osfd = _open_osfhandle((intptr_t)h, _O_RDWR | _O_BINARY);
	if (osfd == -1) {
		CloseHandle(h);
		munit_errorf("_open_osfhandle() failed");
	}
	return osfd;
}
#endif

static int getFamily(const MunitParameter params[])
{
	const char *family = NULL;
	if (params != NULL) {
		family = munit_parameters_get(params, TEST_ENDPOINT_FAMILY);
	}
	if (family == NULL) {
#ifdef _WIN32
		/* On Windows the "unix" family is carried over a named pipe
		 * (see test_endpoint_listen below), which has no listener
		 * socket fd, so the fd-based test_endpoint_accept()/_pair()
		 * used by suites that take the default cannot serve it. Default
		 * to TCP loopback; "unix" stays available as an explicit opt-in
		 * for loop-based suites (test/unit/ext/test_uv.c). */
		family = "tcp";
#else
		family = "unix";
#endif
	}
	if (strcmp(family, "tcp") == 0) {
		return AF_INET;
	} else if (strcmp(family, "unix") == 0) {
		return AF_UNIX;
	}
	munit_errorf("unexpected socket family: %s", family);
	return -1;
}

void test_endpoint_setup(struct test_endpoint *e, const MunitParameter params[])
{
	struct sockaddr *address;
	socklen_t size;
	int rv;
	e->family = getFamily(params);

#ifdef _WIN32
	if (e->family == AF_UNIX) {
		/* Named-pipe listener. No socket fd is bound here: binding a
		 * pipe needs a libuv loop (see test_endpoint_listen). Just
		 * synthesize a process-unique "@name" address -- both ends
		 * derive the identical pipe path via DqliteWinPipeName(). */
		static long counter;
		long n = InterlockedIncrement(&counter);
		e->fd = -1;
		_snprintf(e->address, sizeof e->address, "@test-%lu-%ld",
			  (unsigned long)GetCurrentProcessId(), n);
		e->address[sizeof e->address - 1] = '\0';
		return;
	}
#endif

	/* Initialize the appropriate socket address structure, depending on the
	 * selected socket family. */
	switch (e->family) {
		case AF_INET:
			/* TCP socket on loopback device */
			memset(&e->in_address, 0, sizeof e->in_address);
			e->in_address.sin_family = AF_INET;
			e->in_address.sin_addr.s_addr = inet_addr("127.0.0.1");
			e->in_address.sin_port = 0; /* Get a random free port */
			address = (struct sockaddr *)(&e->in_address);
			size = sizeof e->in_address;
			break;
		case AF_UNIX:
			/* Abstract Unix socket */
			memset(&e->un_address, 0, sizeof e->un_address);
			e->un_address.sun_family = AF_UNIX;
			strcpy(e->un_address.sun_path, ""); /* Random address */
			address = (struct sockaddr *)(&e->un_address);
			size = sizeof e->un_address;
			break;
		default:
			munit_errorf("unexpected socket family: %d", e->family);
	}

	/* Create the listener fd. */
	e->fd = socket(e->family, SOCK_STREAM, 0);
	if (e->fd < 0) {
		munit_errorf("socket(): %s", strerror(errno));
	}

	/* Bind the listener fd. */
	rv = bind(e->fd, address, size);
	if (rv != 0) {
		munit_errorf("bind(): %s", strerror(errno));
	}

	/* Get the actual addressed assigned by the kernel and save it back in
	 * the relevant struct server field (pointed to by address). */
	rv = getsockname(e->fd, address, &size);
	if (rv != 0) {
		munit_errorf("getsockname(): %s", strerror(errno));
	}

	/* Render the endpoint address. */
	switch (e->family) {
		case AF_INET:
			sprintf(e->address, "127.0.0.1:%d",
				htons(e->in_address.sin_port));
			break;
		case AF_UNIX:
			/* TODO */
			break;
	}
}

void test_endpoint_tear_down(struct test_endpoint *e)
{
#ifdef _WIN32
	if (e->family == AF_UNIX) {
		/* The named-pipe listener stream is owned and closed by the
		 * caller (raft_free via uv_close); there is no listener socket
		 * fd to close here. */
		return;
	}
	closesocket((SOCKET)(uintptr_t)e->fd);
#else
	close(e->fd);
#endif
}

int test_endpoint_connect(struct test_endpoint *e)
{
	struct sockaddr *address;
	socklen_t size;
	int fd;
	int rv;

#ifdef _WIN32
	if (e->family == AF_UNIX) {
		/* Named-pipe client end (see endpointConnectPipe). */
		return endpointConnectPipe(e);
	}
#endif

	switch (e->family) {
		case AF_INET:
			address = (struct sockaddr *)&e->in_address;
			size = sizeof e->in_address;
			break;
		case AF_UNIX:
			address = (struct sockaddr *)&e->un_address;
			size = sizeof e->un_address;
			break;
	}

	/* Create the socket. */
	fd = socket(e->family, SOCK_STREAM, 0);
	if (fd < 0) {
		munit_errorf("socket(): %s", strerror(errno));
	}

	/* Connect to the server */
	rv = connect(fd, address, size);
	if (rv != 0 && errno != ECONNREFUSED) {
		munit_errorf("connect(): %s", strerror(errno));
	}

	return fd;
}

int test_endpoint_accept(struct test_endpoint *e)
{
	struct sockaddr_in in_address;
	struct sockaddr_un un_address;
	struct sockaddr *address;
	socklen_t size;
	int fd;
	int rv;

	switch (e->family) {
		case AF_INET:
			address = (struct sockaddr *)&in_address;
			size = sizeof in_address;
			break;
		case AF_UNIX:
			address = (struct sockaddr *)&un_address;
			size = sizeof un_address;
			break;
	}

#ifdef _WIN32
	/* Winsock has no accept4(); use accept() and set non-blocking mode with
	 * ioctlsocket(FIONBIO). Socket errors report via WSAGetLastError(), not
	 * errno, so translate the "listener was closed" cases explicitly. */
	{
		SOCKET s = accept((SOCKET)(uintptr_t)e->fd, address, &size);
		u_long nonblock = 1;
		if (s == INVALID_SOCKET) {
			int werr = WSAGetLastError();
			if (werr == WSAENOTSOCK || werr == WSAEINVAL ||
			    werr == WSAEINTR || werr == WSAECONNRESET) {
				return -1;
			}
			munit_errorf("accept(): WSA error %d", werr);
		}
		fd = (int)s;
		rv = ioctlsocket(s, (long)FIONBIO, &nonblock);
		if (rv != 0) {
			munit_errorf("set non-blocking mode: WSA error %d",
				     WSAGetLastError());
		}
	}
#else
	/* Accept the client connection. */
	fd = accept4(e->fd, address, &size, SOCK_CLOEXEC);
	if (fd < 0) {
		/* Check if the endpoint has been closed, so this is benign. */
		if (errno == EBADF || errno == EINVAL || errno == ENOTSOCK) {
			return -1;
		}
		munit_errorf("accept4(): %s", strerror(errno));
	}

	/* Set non-blocking mode */
	rv = fcntl(fd, F_SETFL, O_NONBLOCK);
	if (rv != 0) {
		munit_errorf("set non-blocking mode: %s", strerror(errno));
	}
#endif

	return fd;
}

void test_endpoint_pair(struct test_endpoint *e, int *server, int *client)
{
	*client = test_endpoint_connect(e);
	*server = test_endpoint_accept(e);
}

const char *test_endpoint_address(struct test_endpoint *e)
{
	return e->address;
}

#ifdef _WIN32
/* The abstract-namespace AF_UNIX family cannot bind on Windows (only real
 * filesystem-path AF_UNIX is supported there), so the "unix" endpoint family is
 * carried over a Win32 named pipe instead -- created via uv_pipe_bind() in
 * test_endpoint_listen() and connected via CreateFile() in
 * endpointConnectPipe(). This mirrors the production local ("@name") transport
 * (compat/win/dqlite_win_pipe.h, src/server.c). */
int test_endpoint_listen(struct test_endpoint *e,
			 struct uv_loop_s *loop,
			 struct uv_stream_s **listener)
{
	char pipe_name[256];
	struct uv_pipe_s *pipe;
	int rv;

	munit_assert_int(e->family, ==, AF_UNIX);

	munit_assert_int(DqliteWinPipeName(e->address, pipe_name,
					   sizeof pipe_name), ==, 0);

	/* raft_malloc so the caller can free with raft_free via uv_close,
	 * exactly as for the stream produced by transport__stream(). */
	pipe = raft_malloc(sizeof *pipe);
	munit_assert_ptr(pipe, !=, NULL);
	rv = uv_pipe_init(loop, pipe, 0);
	munit_assert_int(rv, ==, 0);
	rv = uv_pipe_bind(pipe, pipe_name);
	if (rv != 0) {
		uv_close((struct uv_handle_s *)pipe, (uv_close_cb)raft_free);
		return rv;
	}
	*listener = (struct uv_stream_s *)pipe;
	return 0;
}

char *test_endpoint_family_values[] = {"tcp", "unix", NULL};
#else
char *test_endpoint_family_values[] = {"tcp", "unix", NULL};
#endif
