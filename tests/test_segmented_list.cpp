/* Transliterated from the test blocks in Ghostty
 * src/datastruct/segmented_list.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names. testSegmentedList
 * is a comptime-generic helper upstream, and a template here.
 */

#include "test_helpers.h"
#include "../datastruct/segmented_list.hpp"

using namespace wisp::datastruct;

template <size_t prealloc>
static void testSegmentedList() {
    SegmentedList<int32_t, prealloc> list;

    {
        size_t i = 0;
        while (i < 100) {
            ASSERT_TRUE(list.append((int32_t)(i + 1)));
            ASSERT_TRUE(list.len == i + 1);
            i += 1;
        }
    }

    {
        size_t i = 0;
        while (i < 100) {
            ASSERT_TRUE(*list.at(i) == (int32_t)(i + 1));
            i += 1;
        }
    }

    {
        auto it = list.iterator(0);
        int32_t x = 0;
        while (int32_t *item = it.next()) {
            x += 1;
            ASSERT_TRUE(*item == x);
        }
        ASSERT_TRUE(x == 100);
        while (int32_t *item = it.prev()) {
            ASSERT_TRUE(*item == x);
            x -= 1;
        }
        ASSERT_TRUE(x == 0);
    }

    {
        auto it = list.constIterator(0);
        int32_t x = 0;
        while (const int32_t *item = it.next()) {
            x += 1;
            ASSERT_TRUE(*item == x);
        }
        ASSERT_TRUE(x == 100);
        while (const int32_t *item = it.prev()) {
            ASSERT_TRUE(*item == x);
            x -= 1;
        }
        ASSERT_TRUE(x == 0);
    }

    int32_t popped;
    ASSERT_TRUE(list.pop(&popped) && popped == 100);
    ASSERT_TRUE(list.len == 99);

    {
        const int32_t three[] = {1, 2, 3};
        ASSERT_TRUE(list.appendSlice(three, 3));
    }
    ASSERT_TRUE(list.len == 102);
    ASSERT_TRUE(list.pop(&popped) && popped == 3);
    ASSERT_TRUE(list.pop(&popped) && popped == 2);
    ASSERT_TRUE(list.pop(&popped) && popped == 1);
    ASSERT_TRUE(list.len == 99);

    ASSERT_TRUE(list.appendSlice(nullptr, 0));
    ASSERT_TRUE(list.len == 99);

    {
        int32_t i = 99;
        int32_t item;
        while (list.pop(&item)) {
            ASSERT_TRUE(item == i);
            list.shrinkCapacity(list.len);
            i -= 1;
        }
    }

    {
        int32_t control[100];
        int32_t dest[100];

        int32_t i = 0;
        while (i < 100) {
            ASSERT_TRUE(list.append(i + 1));
            control[i] = i + 1;
            i += 1;
        }

        memset(dest, 0, sizeof(dest));
        list.writeToSlice(dest, 100, 0);
        ASSERT_TRUE(memcmp(control, dest, sizeof(control)) == 0);

        memset(dest, 0, sizeof(dest));
        list.writeToSlice(dest + 50, 50, 50);
        ASSERT_TRUE(memcmp(control + 50, dest + 50, sizeof(int32_t) * 50) == 0);
    }

    ASSERT_TRUE(list.setCapacity(0));
    list.deinit();
}

TEST(segmented_list, basic_usage) {
    testSegmentedList<0>();
    testSegmentedList<1>();
    testSegmentedList<2>();
    testSegmentedList<4>();
    testSegmentedList<8>();
    testSegmentedList<16>();
}

TEST(segmented_list, clearRetainingCapacity) {
    SegmentedList<int32_t, 1> list;

    const int32_t two[] = {4, 5};
    ASSERT_TRUE(list.appendSlice(two, 2));
    list.clearRetainingCapacity();
    ASSERT_TRUE(list.append(6));
    ASSERT_TRUE(*list.at(0) == 6);
    ASSERT_TRUE(list.len == 1);
    list.clearRetainingCapacity();
    ASSERT_TRUE(list.len == 0);

    list.deinit();
}
