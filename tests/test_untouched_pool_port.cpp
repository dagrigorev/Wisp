/* Transliterated from the test blocks in Ghostty src/datastruct/untouched_pool.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 */

#include "test_helpers.h"
#include "../datastruct/untouched_pool.hpp"

using namespace wisp;

/* Test item: one minimum OS page, page-aligned, like a terminal page. */
struct PageItem {
    uint8_t b[4096];
};
typedef datastruct::UntouchedPool<PageItem, 4096> TestPool;

static bool allEqual(const uint8_t *p, size_t len, uint8_t v) {
    for (size_t i = 0; i < len; i++)
        if (p[i] != v) return false;
    return true;
}

TEST(untouched_pool, UntouchedPool_create_destroy_reuse) {
    TestPool pool;
    ASSERT_TRUE(TestPool::initCapacity(zigstd::testing_allocator(), zigstd::testing_allocator(), 0, &pool));

    TestPool::ItemPtr a = pool.create();
    TestPool::ItemPtr b = pool.create();
    TestPool::ItemPtr c = pool.create();
    ASSERT_TRUE(a != b);
    ASSERT_TRUE(a != c);
    ASSERT_TRUE(b != c);

    /* Freed items are recycled, most recent first. */
    pool.destroy(a);
    pool.destroy(b);
    ASSERT_TRUE(b == pool.create());
    ASSERT_TRUE(a == pool.create());

    pool.destroy(a);
    pool.destroy(b);
    pool.destroy(c);
    pool.deinit();
}

TEST(untouched_pool, UntouchedPool_create_and_destroy_never_touch_items) {
    const size_t preheat = 4;

    /* Back the item allocator with memory we can inspect. */
    uint8_t *backing = (uint8_t *)_aligned_malloc(preheat * TestPool::item_size, 4096);
    zigstd::FixedBufferAllocator fba(backing, preheat * TestPool::item_size);

    TestPool pool;
    ASSERT_TRUE(TestPool::initCapacity(zigstd::testing_allocator(), fba.allocator(), preheat, &pool));
    ASSERT_TRUE(preheat * TestPool::item_size == fba.end_index);

    /* Lay the sentinel down after preheat: allocation itself may write
     * (the Allocator interface fills fresh memory with undefined in
     * safe builds, which valgrind also tracks). The sentinel must differ
     * from Zig's 0xAA undefined pattern so that any write is visible. */
    const uint8_t sentinel = 0x5A;
    memset(backing, sentinel, preheat * TestPool::item_size);

    /* Every preheated item comes out untouched and without allocating. */
    TestPool::ItemPtr items[preheat];
    for (size_t i = 0; i < preheat; i++) {
        items[i] = pool.create();
        ASSERT_TRUE(preheat * TestPool::item_size == fba.end_index);
        ASSERT_TRUE(allEqual(items[i]->b, TestPool::item_size, sentinel));
    }

    /* Destroying and re-creating doesn't touch them either. */
    for (size_t i = 0; i < preheat; i++) pool.destroy(items[i]);
    ASSERT_TRUE(allEqual(backing, preheat * TestPool::item_size, sentinel));
    for (size_t i = 0; i < preheat; i++) {
        items[i] = pool.create();
        ASSERT_TRUE(allEqual(items[i]->b, TestPool::item_size, sentinel));
    }
    for (size_t i = 0; i < preheat; i++) pool.destroy(items[i]);
    pool.deinit();
    _aligned_free(backing);
}

TEST(untouched_pool, UntouchedPool_destroy_never_allocates_from_the_general_allocator) {
    /* A general allocator that fails every allocation: after create has
     * reserved the free-list slot, destroy must not need it. */
    zigstd::FailingAllocator failing(zigstd::testing_allocator(), 0);

    TestPool pool;
    ASSERT_TRUE(TestPool::initCapacity(zigstd::testing_allocator(), zigstd::testing_allocator(), 0, &pool));

    /* The pool grows the free list through its own gpa, so swap in the
     * failing one only around destroy. */
    TestPool::ItemPtr items[64];
    for (size_t i = 0; i < 64; i++) items[i] = pool.create();
    const zigstd::Allocator gpa = pool.gpa;
    pool.gpa = failing.allocator();
    for (size_t i = 0; i < 64; i++) pool.destroy(items[i]);
    pool.gpa = gpa;
    ASSERT_TRUE(64 == pool.free.len);
    pool.deinit();
}

TEST(untouched_pool, UntouchedPool_reset_retains_at_most_the_limit) {
    TestPool pool;
    ASSERT_TRUE(TestPool::initCapacity(zigstd::testing_allocator(), zigstd::testing_allocator(), 4, &pool));

    /* Free everything above the limit */
    ASSERT_TRUE(pool.reset(TestPool::ResetMode::retain_with_limit(2 * TestPool::item_size)));
    ASSERT_TRUE(2 == pool.free.len);
    ASSERT_TRUE(2 == pool.live);

    /* retain_capacity keeps everything */
    ASSERT_TRUE(pool.reset(TestPool::ResetMode::retain_capacity()));
    ASSERT_TRUE(2 == pool.free.len);

    /* Retained items are handed out without allocating; new items
     * beyond them are allocated as needed. */
    TestPool::ItemPtr a = pool.create();
    TestPool::ItemPtr b = pool.create();
    ASSERT_TRUE(2 == pool.live);
    TestPool::ItemPtr c = pool.create();
    ASSERT_TRUE(3 == pool.live);
    pool.destroy(a);
    pool.destroy(b);
    pool.destroy(c);

    ASSERT_TRUE(pool.reset(TestPool::ResetMode::free_all()));
    ASSERT_TRUE(0 == pool.free.len);
    ASSERT_TRUE(0 == pool.live);
    pool.deinit();
}

TEST(untouched_pool, UntouchedPool_release_frees_immediately) {
    TestPool pool;
    ASSERT_TRUE(TestPool::initCapacity(zigstd::testing_allocator(), zigstd::testing_allocator(), 1, &pool));

    TestPool::ItemPtr a = pool.create();
    TestPool::ItemPtr b = pool.create();
    ASSERT_TRUE(2 == pool.live);
    pool.release(a);
    ASSERT_TRUE(1 == pool.live);
    ASSERT_TRUE(0 == pool.free.len);
    pool.destroy(b);
    ASSERT_TRUE(1 == pool.free.len);
    pool.deinit();
}

/* Wisp: std.testing.allocator's leak check. */
TEST(untouched_pool, Wisp_no_leaks) {
    ASSERT_TRUE(zigstd::testing_state().live == 0);
}
