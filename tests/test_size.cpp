/* Tests for src/terminal/size.hpp.
 *
 * Ported from the test cases in Ghostty src/terminal/size.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 */

#include "test_helpers.h"
#include "size.hpp"

using namespace wisp::terminal;

/* Upstream keeps this test so that any change to Offset's width is noticed
 * deliberately, because a Page stores many of them. */
TEST(size, offset_int_width) {
    ASSERT_EQ(sizeof(OffsetInt), sizeof(uint32_t));
    ASSERT_EQ(sizeof(Offset<uint8_t>), sizeof(OffsetInt));
}

TEST(size, offset_ptr_u8) {
    Offset<uint8_t> offset;
    offset.offset = 42;

    const uintptr_t base_int = reinterpret_cast<uintptr_t>(&offset);
    uint8_t *actual = offset.ptr(&offset);

    ASSERT_EQ((long)(base_int + 42), (long)reinterpret_cast<uintptr_t>(actual));
}

TEST(size, offset_ptr_structural) {
    struct S { uint32_t x; uint32_t y; };

    Offset<S> offset;
    offset.offset = alignof(S) * 4;

    /* Align the base forward so ptr()'s alignment assertion holds. */
    uintptr_t raw = reinterpret_cast<uintptr_t>(&offset);
    uintptr_t base_int = (raw + alignof(S) - 1) & ~(uintptr_t)(alignof(S) - 1);
    uint8_t *base = reinterpret_cast<uint8_t *>(base_int);

    S *actual = offset.ptr(base);

    ASSERT_EQ((long)(base_int + offset.offset),
              (long)reinterpret_cast<uintptr_t>(actual));
}

TEST(size, get_offset_bytes) {
    const char *widgets = "ABCD";
    Offset<const char> offset = get_offset<const char>(widgets, &widgets[2]);
    ASSERT_EQ(offset.offset, 2);
}

TEST(size, get_offset_structs) {
    struct Widget { uint32_t x; uint32_t y; };
    const Widget widgets[] = {
        {1, 2}, {3, 4}, {5, 6}, {7, 8}, {9, 10},
    };

    Offset<const Widget> offset = get_offset<const Widget>(widgets, &widgets[2]);
    ASSERT_EQ(offset.offset, sizeof(Widget) * 2);
}

/* OffsetBuf is exercised only indirectly upstream, through page construction.
 * These cover its arithmetic directly so a regression shows up here rather
 * than as a corrupt page much later. */
TEST(size, offset_buf_member_is_against_true_base) {
    uint8_t backing[256] = {0};

    OffsetBuf buf = OffsetBuf::init(backing);
    ASSERT_EQ((long)reinterpret_cast<uintptr_t>(buf.start()),
              (long)reinterpret_cast<uintptr_t>(backing));

    /* A member 16 bytes into a structure that itself starts 32 bytes in must
     * report offset 48 — measured from the true base, not from the struct. */
    OffsetBuf inner = buf.add(32);
    Offset<uint32_t> m = inner.member<uint32_t>(16);
    ASSERT_EQ(m.offset, 48);

    ASSERT_EQ((long)reinterpret_cast<uintptr_t>(m.ptr(backing)),
              (long)reinterpret_cast<uintptr_t>(backing + 48));
}

TEST(size, offset_buf_rebase_collapses_offset) {
    uint8_t backing[256] = {0};

    OffsetBuf buf = OffsetBuf::init(backing).add(32);
    OffsetBuf rebased = buf.rebase(8);

    ASSERT_EQ(rebased.offset, 0);
    ASSERT_EQ((long)reinterpret_cast<uintptr_t>(rebased.base),
              (long)reinterpret_cast<uintptr_t>(backing + 40));
    ASSERT_EQ((long)reinterpret_cast<uintptr_t>(rebased.start()),
              (long)reinterpret_cast<uintptr_t>(backing + 40));
}

TEST(size, offset_slice) {
    uint8_t backing[256] = {0};
    for (int i = 0; i < 16; i++) backing[64 + i] = (uint8_t)(i + 1);

    Offset<uint8_t>::Slice s;
    s.offset.offset = 64;
    s.len = 16;

    uint8_t *data = s.slice(backing);
    ASSERT_EQ((long)reinterpret_cast<uintptr_t>(data),
              (long)reinterpret_cast<uintptr_t>(backing + 64));
    ASSERT_EQ(data[0], 1);
    ASSERT_EQ(data[15], 16);
    ASSERT_EQ(s.len, 16);
}

TEST(size, count_type_widths) {
    /* These widths are load-bearing: cell IDs, style IDs and hyperlink IDs all
     * have to stay addressable within a single page. */
    ASSERT_EQ(sizeof(CellCountInt), 2);
    ASSERT_EQ(sizeof(StyleCountInt), 2);
    ASSERT_EQ(sizeof(HyperlinkCountInt), 2);
    ASSERT_EQ(sizeof(GraphemeBytesInt), 4);
    ASSERT_EQ(sizeof(StringBytesInt), 4);
}
