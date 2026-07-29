#include <uv.h>

#include <unistd.h>

#include "../../../src/lib/transport.h"
#include "../../../src/raft.h"
#include "../../lib/endpoint.h"
#include "../../lib/runner.h"
#include "../../lib/uv.h"

/* On Windows the client end (f->client) is one of two things depending on the
 * endpoint family:
 *
 *  - tcp family: a Winsock SOCKET widened to int. read()/write()/close() do not
 *    operate on sockets, so route through recv()/send()/closesocket().
 *  - unix family: a CRT fd wrapping a Win32 named-pipe HANDLE (the local
 *    transport is carried over a named pipe on Windows). Winsock recv()/send()
 *    do not operate on a pipe fd, so route through the blocking CRT
 *    _read()/_write()/_close().
 *
 * The two are told apart with uv_guess_handle() (UV_NAMED_PIPE for the pipe
 * CRT fd; anything else -- a raw SOCKET -- is the tcp case), the same idiom
 * transport__stream() uses. On POSIX these map to the plain read/write/close. */
#ifdef _WIN32
#include <io.h> /* _read, _write, _close */

static int client_sock_recv(int fd, void *buf, size_t n)
{
	if (uv_guess_handle(fd) == UV_NAMED_PIPE) {
		return _read(fd, buf, (unsigned)n);
	}
	return recv((SOCKET)(uintptr_t)fd, (char *)buf, (int)n, 0);
}

static int client_sock_send(int fd, const void *buf, size_t n)
{
	if (uv_guess_handle(fd) == UV_NAMED_PIPE) {
		return _write(fd, buf, (unsigned)n);
	}
	return send((SOCKET)(uintptr_t)fd, (const char *)buf, (int)n, 0);
}

static int client_sock_close(int fd)
{
	if (uv_guess_handle(fd) == UV_NAMED_PIPE) {
		return _close(fd);
	}
	return closesocket((SOCKET)(uintptr_t)fd);
}

#define CLIENT_SOCK_RECV(fd, buf, n) client_sock_recv((fd), (buf), (n))
#define CLIENT_SOCK_SEND(fd, buf, n) client_sock_send((fd), (buf), (n))
#define CLIENT_SOCK_CLOSE(fd) client_sock_close(fd)
#else
#define CLIENT_SOCK_RECV(fd, buf, n) read((fd), (buf), (n))
#define CLIENT_SOCK_SEND(fd, buf, n) write((fd), (buf), (n))
#define CLIENT_SOCK_CLOSE(fd) close(fd)
#endif

TEST_MODULE(ext_uv);

/******************************************************************************
 *
 * Helpers
 *
 ******************************************************************************/

struct fixture
{
	struct uv_loop_s loop;
	struct uv_stream_s *listener;
	struct test_endpoint endpoint;
	int client;
	union {
		uv_tcp_t tcp;
		uv_pipe_t pipe;
		uv_stream_t stream;
	};
};

/* Return a buffer of size TEST_SOCKET_MIN_BUF_SIZE */
static uv_buf_t *buf_malloc(void)
{
	uv_buf_t *buf = munit_malloc(sizeof *buf);
	buf->base = munit_malloc(TEST_SOCKET_MIN_BUF_SIZE);
	buf->len = TEST_SOCKET_MIN_BUF_SIZE;
	return buf;
}

/* Free the buffer returned by buf_malloc() */
static void buf_free(uv_buf_t *buf)
{
	free(buf->base);
	free(buf);
}

/******************************************************************************
 *
 * Parameters
 *
 ******************************************************************************/

/* Run the tests using both TCP and Unix sockets. */
static MunitParameterEnum endpointParams[] = {
    {TEST_ENDPOINT_FAMILY, test_endpoint_family_values},
    {NULL, NULL},
};

/******************************************************************************
 *
 * Setup and tear down
 *
 ******************************************************************************/

static void listenCb(uv_stream_t *listener, int status)
{
	struct fixture *f = listener->data;
	int rv;
	munit_assert_int(status, ==, 0);

	switch (listener->type) {
		case UV_TCP:
			rv = uv_tcp_init(&f->loop, &f->tcp);
			munit_assert_int(rv, ==, 0);
			break;
		case UV_NAMED_PIPE:
			rv = uv_pipe_init(&f->loop, &f->pipe, 0);
			munit_assert_int(rv, ==, 0);
			break;
		default:
			munit_assert(0);
	}

	rv = uv_accept(listener, &f->stream);
	munit_assert_int(rv, ==, 0);
}

static void *setup(const MunitParameter params[], void *user_data)
{
	struct fixture *f = munit_malloc(sizeof *f);
	int rv;
	(void)user_data;

	test_uv_setup(params, &f->loop);
	test_endpoint_setup(&f->endpoint, params);

#ifdef _WIN32
	if (f->endpoint.family == AF_UNIX) {
		/* The "unix" family is a Win32 named pipe here: its listener
		 * stream cannot be produced by wrapping a bound fd
		 * (transport__stream + uv_listen); it must be created directly
		 * with uv_pipe_bind on a fresh handle, which needs the loop. */
		rv = test_endpoint_listen(&f->endpoint, &f->loop, &f->listener);
		munit_assert_int(rv, ==, 0);
	} else
#endif
	{
		rv = transport__stream(&f->loop, f->endpoint.fd, &f->listener);
		munit_assert_int(rv, ==, 0);
	}

	f->listener->data = f;

	rv = uv_listen(f->listener, 128, listenCb);
	munit_assert_int(rv, ==, 0);

	f->client = test_endpoint_connect(&f->endpoint);

	test_uv_run(&f->loop, 1);

	return f;
}

static void tear_down(void *data)
{
	struct fixture *f = data;
	int rv;
	rv = CLIENT_SOCK_CLOSE(f->client);
	munit_assert_int(rv, ==, 0);
	uv_close((struct uv_handle_s *)f->listener, (uv_close_cb)raft_free);
	test_endpoint_tear_down(&f->endpoint);
	uv_close((uv_handle_t *)(&f->stream), NULL);
	test_uv_stop(&f->loop);
	test_uv_tear_down(&f->loop);
	free(f);
}

/******************************************************************************
 *
 * uv_write
 *
 ******************************************************************************/

TEST_SUITE(write);
TEST_SETUP(write, setup);
TEST_TEAR_DOWN(write, tear_down);

/* Writing an amount of data below the buffer size makes that data immediately
 * available for reading. */
TEST_CASE(write, sync, endpointParams)
{
	struct fixture *f = data;
	uv_write_t req;
	uv_buf_t *buf1 = buf_malloc();
	uv_buf_t *buf2 = buf_malloc();
	int rv;

	(void)params;

	rv = uv_write(&req, &f->stream, buf1, 1, NULL);
	munit_assert_int(rv, ==, 0);

	rv = CLIENT_SOCK_RECV(f->client, buf2->base, buf2->len);
	munit_assert_int(rv, ==, buf2->len);

	test_uv_run(&f->loop, 1);

	buf_free(buf1);
	buf_free(buf2);

	return MUNIT_OK;
}

/******************************************************************************
 *
 * uv_read
 *
 ******************************************************************************/

TEST_SUITE(read);
TEST_SETUP(read, setup);
TEST_TEAR_DOWN(read, tear_down);

static void test_read_sync__alloc_cb(uv_handle_t *stream,
				     size_t _,
				     uv_buf_t *buf)
{
	(void)stream;
	(void)_;

	buf->len = TEST_SOCKET_MIN_BUF_SIZE;
	buf->base = munit_malloc(TEST_SOCKET_MIN_BUF_SIZE);
}

static void test_read_sync__read_cb(uv_stream_t *stream,
				    ssize_t nread,
				    const uv_buf_t *buf)
{
	bool *read_cb_called;

	/* Apparently there's an empty read before the actual one. */
	if (nread == 0) {
		free(buf->base);
		return;
	}

	munit_assert_int(nread, ==, TEST_SOCKET_MIN_BUF_SIZE);
	munit_assert_int(buf->len, ==, TEST_SOCKET_MIN_BUF_SIZE);

	read_cb_called = stream->data;

	*read_cb_called = true;

	free(buf->base);
}

/* Reading an amount of data below the buffer happens synchronously. */
TEST_CASE(read, sync, endpointParams)
{
	struct fixture *f = data;
	uv_buf_t *buf = buf_malloc();
	int rv;
	bool read_cb_called;

	(void)params;

	f->stream.data = &read_cb_called;

	rv = uv_read_start(&f->stream, test_read_sync__alloc_cb,
			   test_read_sync__read_cb);

	rv = CLIENT_SOCK_SEND(f->client, buf->base, buf->len);
	munit_assert_int(rv, ==, buf->len);

	test_uv_run(&f->loop, 1);

	munit_assert_true(read_cb_called);

	buf_free(buf);

	return MUNIT_OK;
}
