#include "tcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#ifdef _WIN32
/* Every fd handled in this file is a Winsock SOCKET (widened to int). On
 * Windows read()/write()/close() do not operate on sockets, so route the two
 * socket operations this harness performs -- close and write -- through
 * closesocket()/send(). accept4() is already provided by the compat prelude
 * (accept() + dropped flags). Scoped to this translation unit only. */
#define close(fd) closesocket((SOCKET)(uintptr_t)(fd))
#define write(fd, buf, n) \
	send((SOCKET)(uintptr_t)(fd), (const char *)(buf), (int)(n), 0)
#endif

void TcpServerInit(struct TcpServer *s)
{
    struct sockaddr_in addr;
    socklen_t size = sizeof addr;
    int rv;

    /* Initialize the socket address structure. */
    memset(&addr, 0, size);

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0; /* Get a random free port */

    /* Create the server socket. */
    s->socket = socket(AF_INET, SOCK_STREAM, 0);
    if (s->socket == -1) {
        munit_errorf("tcp server: socket(): %s", strerror(errno));
    }

    /* Bind the socket. */
    rv = bind(s->socket, (struct sockaddr *)&addr, size);
    if (rv == -1) {
        munit_errorf("tcp server: bind(): %s", strerror(errno));
    }

    /* Start listening. */
#ifdef _WIN32
    /* Windows enforces the listen backlog strictly and actively refuses
     * (RST -> WSAECONNREFUSED) any connect once the accept queue is full,
     * whereas Linux is lenient (rounds the backlog up and silently drops
     * SYNs so the client just retransmits). Some tests (e.g.
     * send/changeToUnconnectedAddress) open a connection that is never
     * accept()ed and then expect a *further* connection to the same server
     * to still succeed. A backlog of 1 makes the second connect fail on
     * Windows only, so use a larger backlog here. */
    rv = listen(s->socket, 16);
#else
    rv = listen(s->socket, 1);
#endif
    if (rv == -1) {
        munit_errorf("tcp server: listen(): %s", strerror(errno));
    }

    /* Get the actual addressed assigned by the kernel and save it back in the
     * relevant field. */
    rv = getsockname(s->socket, (struct sockaddr *)&addr, &size);
    if (rv != 0) {
        munit_errorf("tcp: getsockname(): %s", strerror(errno));
    }

    s->port = htons(addr.sin_port);
    sprintf(s->address, "127.0.0.1:%d", s->port);
}

void TcpServerClose(struct TcpServer *s)
{
    int rv;

    if (s->socket == -1) {
        return;
    }

    rv = close(s->socket);
    if (rv == -1) {
        munit_errorf("tcp server: close(): %s", strerror(errno));
    }
}

int TcpServerAccept(struct TcpServer *s)
{
    int socket;
    struct sockaddr_in address;
    socklen_t size;

    size = sizeof(address);

    socket = accept4(s->socket, (struct sockaddr *)&address, &size, SOCK_CLOEXEC);
    if (socket < 0) {
        munit_errorf("tcp server: accept4(): %s", strerror(errno));
    }

    return socket;
}

void TcpServerStop(struct TcpServer *s)
{
    int rv;

    rv = close(s->socket);
    if (rv == -1) {
        munit_errorf("tcp server: close(): %s", strerror(errno));
    }
    s->socket = -1;
}

void test_tcp_setup(const MunitParameter params[], struct test_tcp *t)
{
    (void)params;
    t->server.socket = -1;
    t->client.socket = -1;
}

void test_tcp_tear_down(struct test_tcp *t)
{
    int rv;

    if (t->server.socket != -1) {
        rv = close(t->server.socket);
        if (rv == -1) {
            munit_errorf("tcp: close(): %s", strerror(errno));
        }
    }

    if (t->client.socket != -1) {
        rv = close(t->client.socket);
        if (rv == -1) {
            munit_errorf("tcp: close(): %s", strerror(errno));
        }
    }
}

void test_tcp_listen(struct test_tcp *t)
{
    struct sockaddr_in addr;
    socklen_t size = sizeof addr;
    int rv;

    /* Initialize the socket address structure. */
    memset(&addr, 0, size);

    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = 0; /* Get a random free port */

    /* Create the server socket. */
    t->server.socket = socket(AF_INET, SOCK_STREAM, 0);
    if (t->server.socket == -1) {
        munit_errorf("tcp: socket(): %s", strerror(errno));
    }

    /* Bind the socket. */
    rv = bind(t->server.socket, (struct sockaddr *)&addr, size);
    if (rv == -1) {
        munit_errorf("tcp: bind(): %s", strerror(errno));
    }

    /* Start listening. */
    rv = listen(t->server.socket, 1);
    if (rv == -1) {
        munit_errorf("tcp: listen(): %s", strerror(errno));
    }

    /* Get the actual addressed assigned by the kernel and save it back in
     * the relevant test_socket__server field (pointed to by address). */
    rv = getsockname(t->server.socket, (struct sockaddr *)&addr, &size);
    if (rv != 0) {
        munit_errorf("tcp: getsockname(): %s", strerror(errno));
    }

    sprintf(t->server.address, "127.0.0.1:%d", htons(addr.sin_port));
}

const char *test_tcp_address(struct test_tcp *t)
{
    return t->server.address;
}

void test_tcp_connect(struct test_tcp *t, int port)
{
    struct sockaddr_in addr;
    int rv;

    /* Create the client socket. */
    t->client.socket = socket(AF_INET, SOCK_STREAM, 0);
    if (t->client.socket == -1) {
        munit_errorf("tcp: socket(): %s", strerror(errno));
    }

    /* Initialize the socket address structure. */
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(port);

    /* Connect */
    rv = connect(t->client.socket, (struct sockaddr *)&addr, sizeof addr);
    if (rv == -1) {
        munit_errorf("tcp: connect(): %s", strerror(errno));
    }
}

void test_tcp_close(struct test_tcp *t)
{
    int rv;

    rv = close(t->client.socket);
    if (rv == -1) {
        munit_errorf("tcp: close(): %s", strerror(errno));
    }
    t->client.socket = -1;
}

void test_tcp_stop(struct test_tcp *t)
{
    int rv;

    rv = close(t->server.socket);
    if (rv == -1) {
        munit_errorf("tcp: close(): %s", strerror(errno));
    }
    t->server.socket = -1;
}

void test_tcp_send(struct test_tcp *t, const void *buf, int len)
{
    int rv;

    rv = write(t->client.socket, buf, len);
    if (rv == -1) {
        munit_errorf("tcp: write(): %s", strerror(errno));
    }
    if (rv != len) {
        munit_errorf("tcp: write(): only %d bytes written", rv);
    }
}

int test_tcp_accept(struct test_tcp *t)
{
    int socket;
    struct sockaddr_in address;
    socklen_t size;

    size = sizeof(address);

    socket = accept4(t->server.socket, (struct sockaddr *)&address, &size, SOCK_CLOEXEC);
    if (socket < 0) {
        munit_errorf("tcp: accept4(): %s", strerror(errno));
    }

    return socket;
}
