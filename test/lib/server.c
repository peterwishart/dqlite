#include <unistd.h>

#include "fs.h"
#include "server.h"

#ifdef _WIN32
#include <fcntl.h> /* _O_BINARY, _O_RDWR */
#include <io.h>    /* _open_osfhandle */
#include "dqlite_win_pipe.h" /* DqliteWinPipeName: "@name" -> named-pipe path */

/* Windows connect helper. The dqlite node's local ("@name") transport is
 * carried over a Win32 named pipe (the Linux abstract-namespace AF_UNIX address
 * cannot bind on Windows), so an "@name" address is opened by CreateFile on the
 * mapped pipe path. A TCP ("host:port") address still does a blocking TCP
 * connect. In both cases the returned descriptor is what the raft transport and
 * the synchronous client expect: a named-pipe CRT fd (via _open_osfhandle) or a
 * Winsock SOCKET widened to int. */
static int endpointConnectPipe(const char *address, int *fd)
{
	char pipe_name[256];
	HANDLE h;
	int osfd;
	int attempts;

	munit_assert_int(DqliteWinPipeName(address, pipe_name,
					   sizeof pipe_name), ==, 0);

	/* Open the pipe. A "not found" here means the peer has not reached
	 * uv_listen() yet: fail FAST and let the caller retry, exactly as a
	 * POSIX connect() to an unbound abstract socket returns ECONNREFUSED
	 * immediately and raft's reconnection timer retries later. Blocking here
	 * (this runs on libuv's shared, bounded threadpool via connect_work_cb)
	 * would pin a worker per not-yet-started peer and deadlock a multi-node
	 * cluster. "Busy" (all instances momentarily in use) is transient, so
	 * give it a bounded wait. The budget is generous (WaitNamedPipeA returns
	 * as soon as an instance frees, not after the full timeout) so heavy
	 * connection fan-in — e.g. the stress suite's readers*databases thundering
	 * herd — doesn't spuriously exhaust it. */
	for (attempts = 0;; attempts++) {
		h = CreateFileA(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
				OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
		if (h != INVALID_HANDLE_VALUE) {
			break;
		}
		if (GetLastError() == ERROR_PIPE_BUSY && attempts < 200) {
			WaitNamedPipeA(pipe_name, 100);
			continue;
		}
		return -1;
	}

	osfd = _open_osfhandle((intptr_t)h, _O_RDWR | _O_BINARY);
	if (osfd == -1) {
		CloseHandle(h);
		return -1;
	}
	*fd = osfd;
	return 0;
}

static int endpointConnect(void *data, const char *address, int *fd)
{
	struct sockaddr_in addr;
	const char *colon;
	char host[64];
	size_t host_len;
	SOCKET s;
	int rv;
	(void)data;

	if (address[0] == '@') {
		return endpointConnectPipe(address, fd);
	}

	colon = strrchr(address, ':');
	munit_assert_ptr(colon, !=, NULL);
	host_len = (size_t)(colon - address);
	munit_assert_size(host_len, <, sizeof host);
	memcpy(host, address, host_len);
	host[host_len] = '\0';

	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)atoi(colon + 1));
	addr.sin_addr.s_addr = inet_addr(host);

	s = socket(AF_INET, SOCK_STREAM, 0);
	munit_assert_int((int)s, !=, -1);
	rv = connect(s, (struct sockaddr *)&addr, sizeof addr);
	*fd = (int)s;
	return rv;
}
#else
static int endpointConnect(void *data, const char *address, int *fd)
{
	struct sockaddr_un addr;
	int rv;
	(void)address;
	(void)data;
	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path + 1, address + 1);
	*fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	munit_assert_int(*fd, !=, -1);
	rv = connect(*fd, (struct sockaddr *)&addr,
		     sizeof(sa_family_t) + strlen(address + 1) + 1);
	return rv;
}
#endif

void test_server_setup(struct test_server *s,
		       const unsigned id,
		       const MunitParameter params[])
{
	(void)params;

	s->id = id;
	/* Local abstract-namespace address on Linux; on Windows the same "@name"
	 * address is carried over a Win32 named pipe (see endpointConnect and
	 * dqliteNodeBindPipe in src/server.c). Both namespaces are machine-global,
	 * so a fixed name like "@1" collides with any concurrent test run (or a
	 * leaked child of an aborted one): bind() fails with EADDRINUSE and
	 * dqlite_node_set_bind_address() returns 1. Qualify with the pid. */
	snprintf(s->address, sizeof s->address, "@dqlite-%lu-%u",
		 (unsigned long)getpid(), id);

	s->dir = test_dir_setup();
	s->role_management = false;

	memset(s->others, 0, sizeof s->others);
}

void test_server_stop(struct test_server *s)
{
	int rv;

	clientClose(&s->client);

	if (s->role_management) {
		dqlite_node_handover(s->dqlite);
		rv = dqlite_node_stop(s->dqlite);
	} else {
		rv = dqlite_node_stop(s->dqlite);
	}
	munit_assert_int(rv, ==, 0);

	dqlite_node_destroy(s->dqlite);
}

void test_server_tear_down(struct test_server *s)
{
	test_server_stop(s);
	test_dir_tear_down(s->dir);
}

void test_server_prepare(struct test_server *s, const MunitParameter params[])
{
	int rv;

	rv = dqlite_node_create(s->id, s->address, s->dir, &s->dqlite);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_set_bind_address(s->dqlite, s->address);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_set_connect_func(s->dqlite, endpointConnect, s);
	munit_assert_int(rv, ==, 0);

	rv = dqlite_node_set_network_latency_ms(s->dqlite, 10);
	munit_assert_int(rv, ==, 0);

	const char *snapshot_threshold_param =
	    munit_parameters_get(params, SNAPSHOT_THRESHOLD_PARAM);
	if (snapshot_threshold_param != NULL) {
		unsigned threshold = (unsigned)atoi(snapshot_threshold_param);
		rv = dqlite_node_set_snapshot_params(s->dqlite, threshold,
						     threshold);
		munit_assert_int(rv, ==, 0);
	}

	const char *snapshot_compression_param =
	    munit_parameters_get(params, SNAPSHOT_COMPRESSION_PARAM);
	if (snapshot_compression_param != NULL) {
		bool snapshot_compression =
		    (bool)atoi(snapshot_compression_param);
		rv = dqlite_node_set_snapshot_compression(s->dqlite,
							  snapshot_compression);
		munit_assert_int(rv, ==, 0);
	}

	const char *target_voters_param =
	    munit_parameters_get(params, "target_voters");
	if (target_voters_param != NULL) {
		int n = atoi(target_voters_param);
		rv = dqlite_node_set_target_voters(s->dqlite, n);
		munit_assert_int(rv, ==, 0);
	}

	const char *target_standbys_param =
	    munit_parameters_get(params, "target_standbys");
	if (target_standbys_param != NULL) {
		int n = atoi(target_standbys_param);
		rv = dqlite_node_set_target_standbys(s->dqlite, n);
		munit_assert_int(rv, ==, 0);
	}

	const char *role_management_param =
	    munit_parameters_get(params, "role_management");
	if (role_management_param != NULL) {
		bool role_management = (bool)atoi(role_management_param);
		s->role_management = role_management;
		if (role_management) {
			rv = dqlite_node_enable_role_management(s->dqlite);
			munit_assert_int(rv, ==, 0);
		}
	}
}

void test_server_run(struct test_server *s)
{
	int rv;

	rv = dqlite_node_start(s->dqlite);
	munit_assert_int(rv, ==, 0);

	test_server_client_connect(s, &s->client);
}

void test_server_start(struct test_server *s, const MunitParameter params[])
{
	test_server_prepare(s, params);
	test_server_run(s);
}

struct client_proto *test_server_client(struct test_server *s)
{
	return &s->client;
}

void test_server_client_reconnect(struct test_server *s, struct client_proto *c)
{
	clientClose(c);
	test_server_client_connect(s, c);
}

void test_server_client_connect(struct test_server *s, struct client_proto *c)
{
	int rv;
	int fd;

	rv = endpointConnect(NULL, s->address, &fd);
	munit_assert_int(rv, ==, 0);

	memset(c, 0, sizeof *c);
	buffer__init(&c->read);
	buffer__init(&c->write);
	c->fd = fd;
}

static void setOther(struct test_server *s, struct test_server *other)
{
	unsigned i = other->id - 1;
	munit_assert_ptr_null(s->others[i]);
	s->others[i] = other;
}
void test_server_network(struct test_server *servers, unsigned n_servers)
{
	unsigned i;
	unsigned j;
	for (i = 0; i < n_servers; i++) {
		for (j = 0; j < n_servers; j++) {
			struct test_server *server = &servers[i];
			struct test_server *other = &servers[j];
			if (i == j) {
				continue;
			}
			setOther(server, other);
		}
	}
}
