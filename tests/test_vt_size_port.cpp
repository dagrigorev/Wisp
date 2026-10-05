/* Transliterated from the test blocks in Ghostty src/terminal/size.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include "test_helpers.h"
#include "../vt/size.hpp"

#include <type_traits>

using namespace wisp::vt::size;

TEST(size, Offset) {
    /* This test is here so that if Offset changes, we can be very aware
     * of this effect and think about the implications of it. */
    ASSERT_TRUE((std::is_same<OffsetInt, uint32_t>::value));
}

TEST(size, Offset_ptr_u8) {
    const Offset<uint8_t> offset(42);
    const uintptr_t base_int = (uintptr_t)&offset;
    const uint8_t *actual = offset.ptr((const void *)&offset);
    ASSERT_TRUE((uintptr_t)actual == base_int + 42);
}

TEST(size, Offset_ptr_structural) {
    struct Struct { uint32_t x; uint32_t y; };
    const Offset<Struct> offset(alignof(Struct) * 4);
    const uintptr_t base_int = ((uintptr_t)&offset + alignof(Struct) - 1) & ~(uintptr_t)(alignof(Struct) - 1);
    const uint8_t *base = (const uint8_t *)base_int;
    const Struct *actual = offset.ptr((const void *)base);
    ASSERT_TRUE((uintptr_t)actual == base_int + offset.offset);
}

TEST(size, getOffset_bytes) {
    const char *widgets = "ABCD";
    const Offset<char> offset = getOffset<char>((const void *)widgets, &widgets[2]);
    ASSERT_TRUE(offset.offset == 2);
}

TEST(size, getOffset_structs) {
    struct Widget { uint32_t x; uint32_t y; };
    const Widget widgets[] = { { 1, 2 }, { 3, 4 }, { 5, 6 }, { 7, 8 }, { 9, 10 } };
    const Offset<Widget> offset = getOffset<Widget>((const void *)widgets, &widgets[2]);
    ASSERT_TRUE(offset.offset == sizeof(Widget) * 2);
}
