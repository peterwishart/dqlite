/* durability_child.c -- child half of the crash-durability test
 * (append/crashDurability in test/raft/integration/test_uv_append.c).
 *
 * Usage: raft-durability-child DIR N_ENTRIES
 *
 * Creates its own libuv loop and raft_uv instance on DIR, appends N_ENTRIES
 * entries of 64 bytes one at a time (the first 8 bytes of each payload encode
 * the 0-based entry index), and writes one '!' byte to stdout every time an
 * append callback reports success - that is, every time the write backend
 * claims the entry is durably on disk. It then waits forever: the parent
 * test kills it outright (TerminateProcess / SIGKILL), giving it no chance
 * to flush or close anything, and verifies that every acknowledged entry
 * survived. Exits nonzero on any failure, which the parent observes as EOF
 * on the pipe before all acknowledgements arrived.
 *
 * Built by CMake only: on POSIX the test uses fork() instead (see
 * durabilityChildRun in test_uv_append.c - keep the two in sync), so the
 * autotools build never needs this executable. Keep it strictly portable C:
 * it must build and run on both Windows and Linux CMake builds. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "../../src/raft.h"

/* Mirror the geometry used by the test fixture in test_uv_append.c. */
#define BLOCK_SIZE 4096
#define SEGMENT_SIZE (4 * BLOCK_SIZE)
#define ENTRY_SIZE 64

struct ack
{
	bool done;
	int status;
};

static void appendCb(struct raft_io_append *req, int status)
{
	struct ack *ack = req->data;
	ack->status = status;
	ack->done = true;
}

int main(int argc, char *argv[])
{
	struct uv_loop_s loop;
	struct raft_uv_transport transport;
	struct raft_io io;
	const char *dir;
	long n_entries;
	long i;
	int rv;

	if (argc != 3) {
		fprintf(stderr, "usage: %s DIR N_ENTRIES\n", argv[0]);
		return 2;
	}
	dir = argv[1];
	n_entries = strtol(argv[2], NULL, 10);
	if (n_entries <= 0) {
		fprintf(stderr, "invalid N_ENTRIES '%s'\n", argv[2]);
		return 2;
	}

	rv = uv_loop_init(&loop);
	if (rv != 0) {
		fprintf(stderr, "uv_loop_init: %s\n", uv_strerror(rv));
		return 10;
	}
	transport.version = 1;
	rv = raft_uv_tcp_init(&transport, &loop);
	if (rv != 0) {
		fprintf(stderr, "raft_uv_tcp_init: %d\n", rv);
		return 11;
	}
	rv = raft_uv_init(&io, &loop, dir, &transport);
	if (rv != 0) {
		fprintf(stderr, "raft_uv_init: %d\n", rv);
		return 12;
	}
	raft_uv_set_auto_recovery(&io, false);
	raft_uv_set_block_size(&io, BLOCK_SIZE);
	raft_uv_set_segment_size(&io, SEGMENT_SIZE);
	rv = io.init(&io, 1, "1");
	if (rv != 0) {
		fprintf(stderr, "io->init: %s (%d)\n", io.errmsg, rv);
		return 13;
	}

	for (i = 0; i < n_entries; i++) {
		struct raft_io_append req;
		struct raft_entry entry;
		uint64_t payload[ENTRY_SIZE / sizeof(uint64_t)];
		struct ack ack = {false, -1};

		memset(payload, 0, sizeof payload);
		payload[0] = (uint64_t)i;
		entry.term = 1;
		entry.type = RAFT_COMMAND;
		entry.buf.base = payload;
		entry.buf.len = sizeof payload;
		entry.batch = NULL;
		req.data = &ack;
		rv = io.append(&io, &req, &entry, 1, appendCb);
		if (rv != 0) {
			fprintf(stderr, "io->append: %s (%d)\n", io.errmsg,
				rv);
			return 14;
		}
		while (!ack.done) {
			if (uv_run(&loop, UV_RUN_ONCE) == 0 && !ack.done) {
				fprintf(stderr, "loop ran dry\n");
				return 15;
			}
		}
		if (ack.status != 0) {
			fprintf(stderr, "append status %d\n", ack.status);
			return 16;
		}
		/* The append callback has fired: per the write backend's
		 * contract the entry is durable now. Acknowledge it. */
		if (fwrite("!", 1, 1, stdout) != 1 || fflush(stdout) != 0) {
			return 17;
		}
	}

	/* All appends acknowledged. Wait to be killed; do NOT close the
	 * raft_uv instance or exit, either would flush and defeat the point
	 * of the test. */
	for (;;) {
		uv_sleep(1000);
	}
}
