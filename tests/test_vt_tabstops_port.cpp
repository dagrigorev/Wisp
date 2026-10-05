/* Transliterated from the test blocks in Ghostty src/terminal/Tabstops.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names. Built with
 * WISP_IS_TEST and WISP_SLOW_RUNTIME_SAFETY, as upstream's tests run.
 */

#include "test_helpers.h"
#include "../vt/tabstops.hpp"

using namespace wisp;
using namespace wisp::vt;

typedef Tabstops::Unit Unit;

static zigstd::Allocator talloc() { return zigstd::testing_allocator(); }

TEST(tabstops, Tabstops__basic) {
    Tabstops t;
    ASSERT_TRUE(0 == Tabstops::entry(4));
    ASSERT_TRUE(1 == Tabstops::entry(8));
    ASSERT_TRUE(0 == Tabstops::index(0));
    ASSERT_TRUE(1 == Tabstops::index(1));
    ASSERT_TRUE(1 == Tabstops::index(9));

    ASSERT_TRUE((Unit)0x08 == Tabstops::masks(3));
    ASSERT_TRUE((Unit)0x10 == Tabstops::masks(4));

    ASSERT_TRUE(!t.get(4));
    t.set(4);
    ASSERT_TRUE(t.get(4));
    ASSERT_TRUE(!t.get(3));

    t.reset(0);
    ASSERT_TRUE(!t.get(4));

    t.set(4);
    ASSERT_TRUE(t.get(4));
    t.unset(4);
    ASSERT_TRUE(!t.get(4));

    /* Unsetting a column with no tabstop should be a no-op, not a toggle. */
    t.unset(4);
    ASSERT_TRUE(!t.get(4));
    t.deinit(talloc());
}

TEST(tabstops, Tabstops__dynamic_allocations) {
    Tabstops t;

    /* Grow by less than one unit to verify the allocation rounds up. */
    const size_t cap = t.capacity();
    ASSERT_TRUE(t.resize(talloc(), cap + 5) == Tabstops::Error::none);
    ASSERT_TRUE(cap + Tabstops::unit_bits == t.capacity());

    /* Set something that was out of range of the first */
    t.set(cap + 4);
    ASSERT_TRUE(t.get(cap + 4));
    ASSERT_TRUE(!t.get(cap + 3));

    /* Unsetting a column with no tabstop should be a no-op, not a toggle. */
    t.unset(cap + 3);
    ASSERT_TRUE(!t.get(cap + 3));

    /* Growing again preserves existing stops and clears the new unit. */
    ASSERT_TRUE(t.resize(talloc(), cap + Tabstops::unit_bits + 1) == Tabstops::Error::none);
    ASSERT_TRUE(t.get(cap + 4));
    ASSERT_TRUE(!t.get(cap + Tabstops::unit_bits));

    /* Prealloc still works */
    ASSERT_TRUE(!t.get(5));
    t.deinit(talloc());
}

TEST(tabstops, Tabstops__resize_to_existing_capacity_does_not_allocate) {
    Unit backing[Tabstops::prealloc_count];
    zigstd::FixedBufferAllocator fixed(backing, sizeof(backing));
    Tabstops t;

    ASSERT_TRUE(t.resize(fixed.allocator(), Tabstops::prealloc_columns * 2) == Tabstops::Error::none);
    ASSERT_TRUE(t.resize(fixed.allocator(), Tabstops::prealloc_columns * 2) == Tabstops::Error::none);
}

TEST(tabstops, Tabstops__interval) {
    Tabstops t;
    ASSERT_TRUE(Tabstops::init(talloc(), 80, 4, &t) == Tabstops::Error::none);
    ASSERT_TRUE(!t.get(0));
    ASSERT_TRUE(t.get(4));
    ASSERT_TRUE(!t.get(5));
    ASSERT_TRUE(t.get(8));
    t.deinit(talloc());
}

TEST(tabstops, Tabstops__interval_with_zero_columns) {
    Tabstops t;
    ASSERT_TRUE(Tabstops::init(talloc(), 0, 8, &t) == Tabstops::Error::none);

    ASSERT_TRUE(0 == t.cols);
    t.deinit(talloc());
}

TEST(tabstops, Tabstops__count_on_80) {
    /* https://superuser.com/questions/710019/why-there-are-11-tabstops-on-a-80-column-console */

    Tabstops t;
    ASSERT_TRUE(Tabstops::init(talloc(), 80, 8, &t) == Tabstops::Error::none);

    /* Count the tabstops */
    size_t count = 0;
    for (size_t i = 0; i < 80; i++) {
        if (t.get(i)) count += 1;
    }

    ASSERT_TRUE(9 == count);
    t.deinit(talloc());
}

TEST(tabstops, Tabstops__resize_alloc_failure_preserves_state) {
    /* This test verifies that if resize() fails during allocation,
     * the original cols value is preserved (not corrupted). */
    Tabstops t;
    ASSERT_TRUE(Tabstops::init(talloc(), 80, 8, &t) == Tabstops::Error::none);

    const size_t original_cols = t.cols;

    /* Trigger allocation failure when resizing beyond prealloc */
    Tabstops::resize_tw::errorAlways(Tabstops::ResizeTw::dynamic_alloc, Tabstops::Error::OutOfMemory);
    const Tabstops::Error result = t.resize(talloc(), Tabstops::prealloc_columns * 2);
    ASSERT_TRUE(result == Tabstops::Error::OutOfMemory);
    ASSERT_TRUE(Tabstops::resize_tw::end(tripwire::ResetMode::reset));

    /* cols should be unchanged after failed resize */
    ASSERT_TRUE(original_cols == t.cols);
    t.deinit(talloc());
}

/* Wisp: std.testing.allocator's leak check. Runs last (registration order). */
TEST(tabstops, zz_Wisp_no_leaks) { ASSERT_TRUE(zigstd::testing_state().live == 0); }
