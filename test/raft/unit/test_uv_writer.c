#include <string.h>

#include "../../../src/raft/uv_fs.h"
#include "../../../src/raft/uv_writer.h"
#include "../lib/aio.h"
#include "../lib/dir.h"
#include "../lib/loop.h"
#include "../../lib/runner.h"

/* True when the portable libuv-threadpool write backend is in effect, in which
 * case kernel-AIO-specific test cases do not apply. On a kernel-AIO build the
 * portable backend is selected only when explicitly forced via
 * DQLITE_IO_BACKEND=threadpool; when async kernel I/O is merely unavailable
 * (e.g. DQLITE_IO_NO_DIRECT is set) the AIO backend is still used, running
 * io_submit blocking in the threadpool, so KAIO failure modes still apply.
 * On builds without kernel AIO the portable backend is unconditional. */
static bool uvThreadpoolBackend(void)
{
#if defined(DQLITE_HAVE_KAIO)
    const char *backend = getenv("DQLITE_IO_BACKEND");
    return backend != NULL && strcmp(backend, "threadpool") == 0;
#else
    return true;
#endif
}

/******************************************************************************
 *
 * Fixture with a UvWriter and an open file ready for writing.
 *
 *****************************************************************************/

struct fixture
{
    FIXTURE_DIR;
    FIXTURE_LOOP;
    int fd;
    size_t block_size;
    size_t direct_io;
    bool fallocate;
    bool async_io;
    char errmsg[256];
    struct UvWriter writer;
    bool closed;
};

/******************************************************************************
 *
 * Helper macros.
 *
 *****************************************************************************/

struct result
{
    int status;
    bool done;
};

static void closeCb(struct UvWriter *writer)
{
    struct fixture *f = writer->data;
    f->closed = true;
}

static void submitCbAssertResult(struct UvWriterReq *req, int status)
{
    struct result *result = req->data;
    munit_assert_int(status, ==, result->status);
    result->done = true;
}

/* Initialize the fixture's writer. */
#define INIT(MAX_WRITES)                                                   \
    do {                                                                   \
        int _rv;                                                           \
        _rv = UvWriterInit(&f->writer, &f->loop, f->fd, f->direct_io != 0, \
                           f->async_io, MAX_WRITES, f->errmsg);            \
        munit_assert_int(_rv, ==, 0);                                      \
        f->writer.data = f;                                                \
        f->closed = false;                                                 \
    } while (0)

/* Try to initialize the fixture's writer and check that the given error is
 * returned. */
#define INIT_ERROR(RV, ERRMSG)                                             \
    do {                                                                   \
        int _rv;                                                           \
        _rv = UvWriterInit(&f->writer, &f->loop, f->fd, f->direct_io != 0, \
                           f->async_io, 1, f->errmsg);                     \
        munit_assert_int(_rv, ==, RV);                                     \
        munit_assert_string_equal(f->errmsg, ERRMSG);                      \
    } while (0)

/* Close helper. */
#define CLOSE_SUBMIT                    \
    munit_assert_false(f->closed);      \
    UvWriterClose(&f->writer, closeCb); \
    munit_assert_false(f->closed)
#define CLOSE_WAIT LOOP_RUN_UNTIL(&f->closed)
#define CLOSE     \
    CLOSE_SUBMIT; \
    CLOSE_WAIT

#define MAKE_BUFS(BUFS, N_BUFS, CONTENT)                               \
    {                                                                  \
        int __i;                                                       \
        BUFS = munit_malloc(sizeof *BUFS * N_BUFS);                    \
        for (__i = 0; __i < N_BUFS; __i++) {                           \
            uv_buf_t *__buf = &BUFS[__i];                              \
            __buf->len = f->block_size;                                \
            __buf->base = aligned_alloc(f->block_size, f->block_size); \
            munit_assert_ptr_not_null(__buf->base);                    \
            memset(__buf->base, CONTENT + __i, __buf->len);            \
        }                                                              \
    }

#define DESTROY_BUFS(BUFS, N_BUFS)             \
    {                                          \
        int __i;                               \
        for (__i = 0; __i < N_BUFS; __i++) {   \
            RAFT_ALIGNED_FREE(BUFS[__i].base); \
        }                                      \
        free(BUFS);                            \
    }

#define WRITE_REQ(N_BUFS, CONTENT, OFFSET, RV, STATUS)             \
    struct uv_buf_t *_bufs;                                        \
    struct UvWriterReq _req;                                       \
    struct result _result = {STATUS, false};                       \
    int _rv;                                                       \
    MAKE_BUFS(_bufs, N_BUFS, CONTENT);                             \
    _req.data = &_result;                                          \
    _rv = UvWriterSubmit(&f->writer, &_req, _bufs, N_BUFS, OFFSET, \
                         submitCbAssertResult);                    \
    munit_assert_int(_rv, ==, RV);

/* Submit a write request with the given parameters and wait for the operation
 * to successfully complete. Deallocate BUFS when done.
 *
 * N_BUFS is the number of buffers to allocate and write, each of them will have
 * f->block_size bytes.
 *
 * CONTENT must be an unsigned byte value: all bytes of the first buffer will be
 * filled with that value, all bytes of the second buffer will be filled will
 * that value plus one, etc.
 *
 * OFFSET is the offset at which to write the buffers. */
#define WRITE(N_BUFS, CONTENT, OFFSET)                                  \
    do {                                                                \
        WRITE_REQ(N_BUFS, CONTENT, OFFSET, 0 /* rv */, 0 /* status */); \
        LOOP_RUN_UNTIL(&_result.done);                                  \
        DESTROY_BUFS(_bufs, N_BUFS);                                    \
    } while (0)

/* Submit a write request with the given parameters and wait for the operation
 * to fail with the given code and message. */
#define WRITE_FAILURE(N_BUFS, CONTENT, OFFSET, STATUS, ERRMSG)  \
    do {                                                        \
        WRITE_REQ(N_BUFS, CONTENT, OFFSET, 0 /* rv */, STATUS); \
        LOOP_RUN_UNTIL(&_result.done);                          \
        munit_assert_string_equal(f->writer.errmsg, ERRMSG);    \
        DESTROY_BUFS(_bufs, N_BUFS);                            \
    } while (0)

/* Submit a write request with the given parameters, close the writer right
 * after and assert that the request got canceled. */
#define WRITE_CLOSE(N_BUFS, CONTENT, OFFSET, STATUS)            \
    do {                                                        \
        WRITE_REQ(N_BUFS, CONTENT, OFFSET, 0 /* rv */, STATUS); \
        CLOSE_SUBMIT;                                           \
        munit_assert_false(_result.done);                       \
        LOOP_RUN_UNTIL(&_result.done);                          \
        DESTROY_BUFS(_bufs, N_BUFS);                            \
        CLOSE_WAIT;                                             \
    } while (0)

/* Assert that the content of the test file has the given number of blocks, each
 * filled with progressive numbers. */
#define ASSERT_CONTENT(N)                                     \
    do {                                                      \
        size_t _size = N * f->block_size;                     \
        void *_buf = munit_malloc(_size);                     \
        unsigned _i;                                          \
        unsigned _j;                                          \
                                                              \
        DirReadFile(f->dir, "foo", _buf, _size);              \
                                                              \
        for (_i = 0; _i < N; _i++) {                          \
            char *cursor = (char *)_buf + _i * f->block_size; \
            for (_j = 0; _j < f->block_size; _j++) {          \
                munit_assert_int(cursor[_j], ==, _i + 1);     \
            }                                                 \
        }                                                     \
                                                              \
        free(_buf);                                           \
    } while (0)

#define N_BLOCKS 5

/******************************************************************************
 *
 * Set up and tear down.
 *
 *****************************************************************************/

static void *setUpDeps(const MunitParameter params[], void *user_data)
{
    struct fixture *f = munit_malloc(sizeof *f);
    char path[UV__PATH_SZ];
    char errmsg[256];
    int rv;
    SET_UP_DIR;
    SETUP_LOOP;
    rv = UvFsProbeCapabilities(f->dir, &f->direct_io, &f->async_io,
                               &f->fallocate, errmsg);
    munit_assert_int(rv, ==, 0);
    f->block_size = f->direct_io != 0 ? f->direct_io : 4096;
    rv = UvOsJoin(f->dir, "foo", path);
    munit_assert_int(rv, ==, 0);
    rv = UvOsOpen(path, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR, &f->fd);
    munit_assert_int(rv, ==, 0);
    rv = UvOsFallocate(f->fd, 0, f->block_size * N_BLOCKS);
    munit_assert_int(rv, ==, 0);
    return f;
}

static void tearDownDeps(void *data)
{
    struct fixture *f = data;
    if (f == NULL) {
        return; /* Was skipped. */
    }
    UvOsClose(f->fd);
    TEAR_DOWN_LOOP;
    TEAR_DOWN_DIR;
    free(f);
}

static void *setUp(const MunitParameter params[], void *user_data)
{
    struct fixture *f = setUpDeps(params, user_data);
    if (f == NULL) {
        return NULL;
    }
    INIT(1);
    return f;
}

static void tearDown(void *data)
{
    struct fixture *f = data;
    if (f == NULL) {
        return; /* Was skipped. */
    }
    CLOSE;
    tearDownDeps(f);
}

/******************************************************************************
 *
 * UvWriterInit
 *
 *****************************************************************************/

SUITE(UvWriterInit)

#if defined(DQLITE_HAVE_KAIO)
/* The kernel has ran out of available AIO events. */
TEST(UvWriterInit, noResources, setUpDeps, tearDownDeps, 0, NULL)
{
    struct fixture *f = data;
    aio_context_t ctx = 0;
    int rv;
    /* The portable backend does not use kernel AIO, so it cannot run out of
     * AIO events; this failure mode is specific to the AIO backend. */
    if (uvThreadpoolBackend()) {
        return MUNIT_SKIP;
    }
    rv = AioFill(&ctx, 0);
    if (rv != 0) {
        return MUNIT_SKIP;
    }
    INIT_ERROR(RAFT_TOOMANY, "AIO events user limit exceeded");
    AioDestroy(ctx);
    return MUNIT_OK;
}
#endif /* DQLITE_HAVE_KAIO */

/******************************************************************************
 *
 * UvWriterSubmit
 *
 *****************************************************************************/

SUITE(UvWriterSubmit)

TEST(UvWriterSubmit, one, setUp, tearDown, 0, DirAllParams)
{
    struct fixture *f = data;
    SKIP_IF_NO_FIXTURE;
    WRITE(1 /* n bufs */, 1 /* content */, 0 /* offset */);
    ASSERT_CONTENT(1);
    return MUNIT_OK;
}

/* Write two buffers, one after the other. */
TEST(UvWriterSubmit, two, setUp, tearDown, 0, DirAllParams)
{
    struct fixture *f = data;
    SKIP_IF_NO_FIXTURE;
    WRITE(1 /* n bufs */, 1 /* content */, 0 /* offset */);
    WRITE(1 /* n bufs */, 2 /* content */, f->block_size /* offset */);
    ASSERT_CONTENT(2);
    return MUNIT_OK;
}

/* Write the same block twice. */
TEST(UvWriterSubmit, twice, setUp, tearDown, 0, DirAllParams)
{
    struct fixture *f = data;
    SKIP_IF_NO_FIXTURE;
    WRITE(1 /* n bufs */, 0 /* content */, 0 /* offset */);
    WRITE(1 /* n bufs */, 1 /* content */, 0 /* offset */);
    ASSERT_CONTENT(1);
    return MUNIT_OK;
}

/* Write a vector of buffers. */
TEST(UvWriterSubmit, vec, setUp, tearDown, 0, DirAllParams)
{
    struct fixture *f = data;
    SKIP_IF_NO_FIXTURE;
    WRITE(2 /* n bufs */, 1 /* content */, 0 /* offset */);
    ASSERT_CONTENT(1);
    return MUNIT_OK;
}

/* Write a vector of buffers twice. */
TEST(UvWriterSubmit, vecTwice, setUp, tearDown, 0, DirAllParams)
{
    struct fixture *f = data;
    SKIP_IF_NO_FIXTURE;
    WRITE(2 /* n bufs */, 1 /* content */, 0 /* offset */);
    WRITE(2 /* n bufs */, 1 /* content */, 0 /* offset */);
    ASSERT_CONTENT(2);
    return MUNIT_OK;
}

/* Write past the allocated space. */
TEST(UvWriterSubmit, beyondEOF, setUp, tearDown, 0, DirAllParams)
{
    struct fixture *f = data;
    int i;
    SKIP_IF_NO_FIXTURE;
    for (i = 0; i < N_BLOCKS + 1; i++) {
        WRITE(1 /* n bufs */, i + 1 /* content */,
              i * f->block_size /* offset */);
    }
    ASSERT_CONTENT((N_BLOCKS + 1));
    return MUNIT_OK;
}

/* Write two different blocks concurrently. */
TEST(UvWriterSubmit, concurrent, NULL, NULL, 0, DirAllParams)
{
    return MUNIT_SKIP; /* TODO: tests stop responding */
}

/* Write the same block concurrently. */
TEST(UvWriterSubmit, concurrentSame, NULL, NULL, 0, DirAllParams)
{
    return MUNIT_SKIP; /* TODO: tests stop responding */
}

#if defined(DQLITE_HAVE_KAIO)
/* There are not enough resources to create an AIO context to perform the
 * write. */
TEST(UvWriterSubmit, noResources, setUpDeps, tearDown, 0, DirNoAioParams)
{
    struct fixture *f = data;
    aio_context_t ctx = 0;
    int rv;
    SKIP_IF_NO_FIXTURE;
    INIT(2);
    /* The forced portable backend does not use kernel AIO at all, so
     * exhausting AIO events cannot make it fail; this failure mode is
     * specific to the AIO backend's blocking (threadpool) submit path,
     * which needs a per-request io_setup when n_events > 1. (The guard is
     * after INIT because tearDown unconditionally closes the writer.) */
    if (uvThreadpoolBackend()) {
        return MUNIT_SKIP;
    }
    rv = AioFill(&ctx, 0);
    if (rv != 0) {
        return MUNIT_SKIP;
    }
    WRITE_FAILURE(1, 0, 0, RAFT_TOOMANY, "AIO events user limit exceeded");
    AioDestroy(ctx);
    return MUNIT_OK;
}
#endif /* DQLITE_HAVE_KAIO */

/******************************************************************************
 *
 * UvWriterSubmit
 *
 *****************************************************************************/

SUITE(UvWriterClose)

/* Close with an inflight write running in the threadpool. */
TEST(UvWriterClose, threadpool, setUp, tearDownDeps, 0, DirNoAioParams)
{
    struct fixture *f = data;
    SKIP_IF_NO_FIXTURE;
    WRITE_CLOSE(1, 0, 0, 0);
    return MUNIT_OK;
}

/* Close with an inflight AIO write . */
TEST(UvWriterClose, aio, setUp, tearDownDeps, 0, DirAioParams)
{
    struct fixture *f = data;
    SKIP_IF_NO_FIXTURE;
    /* Cancellation-on-close only happens for requests sitting on the AIO
     * backend's poll queue, i.e. submitted via the non-blocking (async)
     * path. With the portable backend forced, or with async I/O unavailable
     * (e.g. DQLITE_IO_NO_DIRECT set), the write instead runs blocking in the
     * threadpool, is drained on close and completes with status 0. (Close
     * before skipping: tearDownDeps does not close the writer.) */
    if (uvThreadpoolBackend() || !f->async_io) {
        CLOSE;
        return MUNIT_SKIP;
    }
    WRITE_CLOSE(1, 0, 0, RAFT_CANCELED);
    return MUNIT_OK;
}
