#include "../lib/client.h"
#include "../lib/heap.h"
#include "../lib/runner.h"
#include "../lib/server.h"
#include "../lib/sqlite.h"

#include <inttypes.h>
#include <uv.h>

SUITE(stress);

#define READ_COUNT 1000
#define WRITE_COUNT 1000

static char *databases[] = { "1", "2", "4", NULL };
static char *writers[] = { "0", "1", "2", "4", NULL };
static char *readers[] = { "0", "1", "4", "16", NULL };

static MunitParameterEnum stress_params[] = {
	{ "writers", writers },
	{ "readers", readers },
	{ "databases", databases },
	{ NULL, NULL },
};

/* A single heavier configuration, run in addition to (not instead of) the
 * upstream default matrix above. readers=32 with databases=4 under a longer
 * run (count=1500) is the load that exposed the Windows shm virtual-address
 * aliasing corruption (the mmap shim's non-atomic MAP_FIXED emulation let
 * another database's WAL-index mapping steal the address mid-replacement; see
 * PORT_TODO.md section 9). Kept as one extra parameter set so that regression
 * stays covered without slowing down every combination of the default
 * matrix. */
static char *heavy_count[] = { "1500", NULL };
static char *heavy_writers[] = { "4", NULL };
static char *heavy_readers[] = { "32", NULL };
static char *heavy_databases[] = { "4", NULL };

static MunitParameterEnum stress_heavy_params[] = {
	{ "count", heavy_count },
	{ "writers", heavy_writers },
	{ "readers", heavy_readers },
	{ "databases", heavy_databases },
	{ NULL, NULL },
};

struct fixture {
	struct test_server server;
	struct client_proto *client;
	int databases;
	int readers, writers;
	int read_count, write_count;
};

struct worker {
	uv_thread_t thread;
	struct fixture *f;
	char database[16];
};

static void client_read(void *data)
{
	const char *sql =
		"WITH RECURSIVE seq(n, id) AS ("
		"    SELECT 1, random()        "
		"    UNION ALL                 "
		"    SELECT n+1, random()      "
		"    FROM seq                  "
		"    WHERE n < 100             "
		")                             "
		"SELECT MAX(test.n)            "
		"FROM test JOIN seq            "
		"    ON test.rowid = seq.id % ("
		"        SELECT MAX(rowid)     "
		"        FROM test             "
		"    )                         ";

	struct worker *self = data;
	struct client_proto client;
	struct rows rows;
	uint32_t stmt_id;

	test_server_client_connect(&self->f->server, &client);
	HANDSHAKE_C(&client);
	OPEN_C(&client, self->database);
	PREPARE_C(&client, sql, &stmt_id);

	for (int i = 0; i < self->f->read_count; i++) {
		int rv = clientSendQuery(&client, stmt_id, NULL, 0, NULL);
		munit_assert_int(rv, ==, 0);
		for (bool done = false; !done;) {
			rv = clientRecvRows(&client, &rows, &done, NULL);
			if (rv != DQLITE_OK) {
				break;
			}
			clientCloseRows(&rows);
			rows = (struct rows){};
		}
		if (rv == DQLITE_CLIENT_PROTO_RECEIVED_FAILURE) {
			if (client.errcode == SQLITE_BUSY) {
				/* Just retry */
				i--;
				continue;
			}
			munit_errorf("failure: [%" PRIu64 "] %s",
				     client.errcode, client.errmsg);
		} else {
			munit_assert_int(rv, ==, DQLITE_OK);
		}
	}

	clientClose(&client);
}

static void client_write(void *data)
{
	const char *sql = "INSERT INTO test(n) VALUES (random())";

	struct worker *self = data;
	struct client_proto client;
	uint64_t last_insert_id;
	uint64_t rows_affected;
	uint32_t stmt_id;

	test_server_client_connect(&self->f->server, &client);
	HANDSHAKE_C(&client);
	OPEN_C(&client, self->database);
	PREPARE_C(&client, sql, &stmt_id);

	for (int i = 0; i < self->f->write_count; i++) {
		int rv = clientSendExec(&client, stmt_id, NULL, 0, NULL);
		munit_assert_int(rv, ==, DQLITE_OK);

		rv = clientRecvResult(&client, &last_insert_id, &rows_affected,
				      NULL);
		if (rv == DQLITE_CLIENT_PROTO_RECEIVED_FAILURE) {
		    if (client.errcode == SQLITE_BUSY) {
				/* Just retry */
				i--;
				continue;
			}
			munit_errorf("failure: [%" PRIu64 "] %s", client.errcode,
				     client.errmsg);
		}
		munit_assert_int(rv, ==, DQLITE_OK);
		munit_assert_int(last_insert_id, >, 1);
		munit_assert_int(rows_affected, ==, 1);
	}

	clientClose(&client);
}

static void *setUp(const MunitParameter params[], void *user_data)
{
	struct fixture *f = munit_malloc(sizeof *f);
	(void)user_data;
	const char *count = munit_parameters_get(params, "count");
	f->databases = atoi(munit_parameters_get(params, "databases"));
	f->readers = atoi(munit_parameters_get(params, "readers"));
	f->writers = atoi(munit_parameters_get(params, "writers"));
	/* Only the heavy parameter set defines "count"; the default matrix
	 * uses the upstream READ_COUNT/WRITE_COUNT values. */
	f->read_count = count != NULL ? atoi(count) : READ_COUNT;
	f->write_count = count != NULL ? atoi(count) : WRITE_COUNT;
	test_heap_setup(params, user_data);
	test_sqlite_setup(params);
	test_server_setup(&f->server, 1, params);
	test_server_prepare(&f->server, params);
	dqlite_node_set_busy_timeout(f->server.dqlite, 200 * f->writers);
	test_server_run(&f->server);
	f->client = test_server_client(&f->server);

	for (int i = 0; i < f->databases; i++) {
		char name[16];
		uint32_t stmt_id;
		uint64_t last_insert_id;
		uint64_t rows_affected;

		snprintf(name, 16, "test%d", i);
		test_server_client_reconnect(&f->server, f->client);
		HANDSHAKE;
		OPEN_C(f->client, name);

		PREPARE("CREATE TABLE test (n INT)", &stmt_id);
		EXEC(stmt_id, &last_insert_id, &rows_affected);

		PREPARE(
		    "WITH RECURSIVE seq(n) AS ("
		    "    SELECT 1 UNION ALL     "
		    "    SELECT n+1 FROM seq    "
		    "    WHERE  n < 10000       "
		    ")                          "
		    "INSERT INTO test(n)        "
		    "SELECT n FROM seq          ",
		    &stmt_id);
		EXEC(stmt_id, &last_insert_id, &rows_affected);
	}

	return f;
}

static void tearDown(void *data)
{
	struct fixture *f = data;

	test_server_tear_down(&f->server);
	test_sqlite_tear_down();
	test_heap_tear_down(data);
	free(f);
}

static MunitResult run_read_write(struct fixture *f)
{
	if (f->readers == 0 && f->writers == 0) {
		return MUNIT_SKIP;
	}
	if (getenv("SKIP_STRESS") != NULL) {
		return MUNIT_SKIP;
	}

	int num_workers = (f->readers + f->writers) * f->databases;
	struct worker *workers =
	    munit_malloc(num_workers * sizeof(struct worker));
	struct worker *write_workers = workers;
	struct worker *read_workers =
	    write_workers + (f->writers * f->databases);

	for (int i = 0; i < f->readers; i++) {
		for (int j = 0; j < f->databases; j++) {
			struct worker *worker =
			    &read_workers[i * f->databases + j];
			worker->f = f;
			snprintf(worker->database, 16, "test%d", j);
			uv_thread_create(&worker->thread, client_read,
					 worker);
		}
	}

	for (int i = 0; i < f->writers; i++) {
		for (int j = 0; j < f->databases; j++) {
			struct worker *worker =
			    &write_workers[i * f->databases + j];
			worker->f = f;
			snprintf(worker->database, 16, "test%d", j);
			uv_thread_create(&worker->thread, client_write,
					 worker);
		}
	}

	for (int i = 0; i < num_workers; i++) {
		uv_thread_join(&workers[i].thread);
	}

	free(workers);
	return MUNIT_OK;
}

TEST(stress, read_write, setUp, tearDown, 0, stress_params)
{
	(void)params;
	return run_read_write(data);
}

/* The regression configuration for the shm aliasing corruption; see the
 * comment at stress_heavy_params. */
TEST(stress, read_write_heavy, setUp, tearDown, 0, stress_heavy_params)
{
	(void)params;
	return run_read_write(data);
}
