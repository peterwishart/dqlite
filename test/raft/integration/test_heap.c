#include "../../../src/raft.h"

#include "../../lib/runner.h"

/******************************************************************************
 *
 * Default heap functions
 *
 *****************************************************************************/

SUITE(raft_heap)

TEST(raft_heap, malloc, NULL, NULL, 0, NULL)
{
    void *p;
    p = raft_malloc(8);
    munit_assert_ptr_not_null(p);
    raft_free(p);
    return MUNIT_OK;
}

TEST(raft_heap, calloc, NULL, NULL, 0, NULL)
{
    void *p;
    p = raft_calloc(1, 8);
    munit_assert_ptr_not_null(p);
    munit_assert_int(*(uint64_t *)p, ==, 0);
    raft_free(p);
    return MUNIT_OK;
}

TEST(raft_heap, realloc, NULL, NULL, 0, NULL)
{
    void *p;
    p = raft_realloc(NULL, 8);
    munit_assert_ptr_not_null(p);
    *(uint64_t *)p = 1;
    p = raft_realloc(p, 16);
    munit_assert_ptr_not_null(p);
    munit_assert_int(*(uint64_t *)p, ==, 1);
    raft_free(p);
    return MUNIT_OK;
}

TEST(raft_heap, aligned_alloc, NULL, NULL, 0, NULL)
{
    void *p;
    p = raft_aligned_alloc(1024, 2048);
    munit_assert_ptr_not_null(p);
#ifndef _WIN32
    munit_assert_int((uintptr_t)p % 1024, ==, 0);
#else
    /* On Windows the requested alignment is deliberately NOT honoured (see
     * raft_aligned_alloc() in src/raft.h); pin the relaxed contract instead:
     * malloc's fundamental alignment (MEMORY_ALLOCATION_ALIGNMENT, 16 bytes
     * on x64/arm64). */
    munit_assert_int((uintptr_t)p % MEMORY_ALLOCATION_ALIGNMENT, ==, 0);
#endif
    raft_free(p);
    return MUNIT_OK;
}
