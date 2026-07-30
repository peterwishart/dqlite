/* Setup fully blown servers running in standalone threads. */

#ifndef TEST_SERVER_H
#define TEST_SERVER_H

#include <sys/un.h>

#include "../../src/client/protocol.h"

#include "../../include/dqlite.h"

#include "endpoint.h"
#include "munit.h"

#define SNAPSHOT_THRESHOLD_PARAM "snapshot-threshold"
#define SNAPSHOT_COMPRESSION_PARAM "snapshot_compression"

struct test_server
{
	unsigned id; /* Server ID. */
#ifdef _WIN32
	/* On Windows the server uses a TCP loopback address ("127.0.0.1:PORT")
	 * instead of the Linux abstract-namespace AF_UNIX address ("@ID"), which
	 * cannot bind on Windows; that needs a larger buffer. */
	char address[24];
#else
	char address[8]; /* Server address. */
#endif
	char *dir;           /* Data directory. */
	dqlite_node *dqlite; /* Dqlite instance. */
	bool role_management;
	struct client_proto client;    /* Connected client. */
	struct test_server *others[5]; /* Other servers, by ID-1. */
};

/* Initialize the test server. */
void test_server_setup(struct test_server *s,
		       unsigned id,
		       const MunitParameter params[]);

/* Cleanup the test server. */
void test_server_tear_down(struct test_server *s);

/* Set up the test server without running it. */
void test_server_prepare(struct test_server *s, const MunitParameter params[]);

/* Run the test server after setting it up. */
void test_server_run(struct test_server *s);

/* Start the test server. Equivalent to test_server_prepare + test_server_run. */
void test_server_start(struct test_server *s, const MunitParameter params[]);

/* Stop the test server. */
void test_server_stop(struct test_server *s);

/* Connect all the given the servers to each other. */
void test_server_network(struct test_server *servers, unsigned n_servers);

/* Return a client connected to the server. */
struct client_proto *test_server_client(struct test_server *s);

/* Closes and reopens a client connection to the server. */
void test_server_client_reconnect(struct test_server *s,
				  struct client_proto *c);

/* Opens a client connection to the server. */
void test_server_client_connect(struct test_server *s, struct client_proto *c);

#endif /* TEST_SERVER_H */
