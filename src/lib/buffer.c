#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>

#include "buffer.h"

#include "../../include/dqlite.h"

/* How large is the buffer currently */
#define SIZE(B) (B->n_pages * B->page_size)

/* How many remaining bytes the buffer currently */
#define CAP(B) (SIZE(B) - B->offset)

int buffer__init(struct buffer *b)
{
#ifdef _WIN32
	/* On Windows sysconf(_SC_PAGESIZE) is shimmed to the allocation
	 * granularity (64KiB) because vfs.c needs that for WAL mmap alignment.
	 * buffer.c instead wants the true memory page size (4KiB): page_size is
	 * the query row-batch flush threshold in query__batch(), and a 64KiB
	 * threshold changes the observable pause/resume boundary versus Linux
	 * (small result sets that pause on Linux would complete in a single
	 * batch). Use the genuine page size so batching matches Linux. */
	b->page_size = dqlite_win_page_size();
#else
	b->page_size = (unsigned)sysconf(_SC_PAGESIZE);
#endif
	b->n_pages = 1;
	b->data = malloc(SIZE(b));
	if (b->data == NULL) {
		return DQLITE_NOMEM;
	}
	b->offset = 0;
	return 0;
}

void buffer__close(struct buffer *b)
{
	free(b->data);
}

/* Ensure that the buffer has at least @size spare bytes */
static inline bool ensure(struct buffer *b, size_t size)
{
	void *data;
	uint32_t n_pages = b->n_pages;

	/* Double the buffer until we have enough capacity */
	while (size > CAP(b)) {
		b->n_pages *= 2;
	}

	/* CAP(b) was insufficient */
	if (b->n_pages > n_pages) {
		data = realloc(b->data, SIZE(b));
		if (data == NULL) {
			b->n_pages = n_pages;
			return false;
		}
		b->data = data;
	}

	return true;
}

void *buffer__advance(struct buffer *b, size_t size)
{
	void *cursor;

	if (!ensure(b, size)) {
		return NULL;
	}

	cursor = buffer__cursor(b, b->offset);
	b->offset += size;
	return cursor;
}

size_t buffer__offset(struct buffer *b)
{
	return b->offset;
}

void *buffer__cursor(struct buffer *b, size_t offset)
{
	return b->data + offset;
}

void buffer__reset(struct buffer *b)
{
	b->offset = 0;
}
