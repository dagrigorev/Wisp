/* Transliterated from the test blocks in Ghostty src/unicode/main.zig and
 * src/unicode/grapheme.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include "test_helpers.h"
#include "../vt/unicode/grapheme.hpp"

using namespace wisp;
using namespace wisp::vt;
using namespace wisp::vt::unicode;

static GraphemeWidth gw(const uint32_t *cps, size_t n) { return graphemeWidth(cps, n); }

#define GW(...)                                                                                                        \
    ([]() -> GraphemeWidth {                                                                                           \
        const uint32_t _cps[] = {__VA_ARGS__};                                                                         \
        return gw(_cps, sizeof(_cps) / sizeof(_cps[0]));                                                               \
    })()

#define EXPECT_GW(len_, width_, actual)                                                                                \
    do {                                                                                                               \
        const GraphemeWidth _a = (actual);                                                                             \
        ASSERT_TRUE(_a.len == (size_t)(len_) && _a.width == (uint8_t)(width_));                                         \
    } while (0)

TEST(unicode, codepointWidth) {
    /* Narrow (width 1) */
    ASSERT_TRUE(1 == codepointWidth('a'));
    ASSERT_TRUE(1 == codepointWidth(' '));
    ASSERT_TRUE(1 == codepointWidth(0x10FFFF)); /* max codepoint */

    /* C0/C1 control characters (width 0) */
    ASSERT_TRUE(0 == codepointWidth(0x00));  /* NUL */
    ASSERT_TRUE(0 == codepointWidth(0x07));  /* BEL */
    ASSERT_TRUE(0 == codepointWidth(0x1B));  /* ESC */
    ASSERT_TRUE(0 == codepointWidth(0x7F));  /* DEL */
    ASSERT_TRUE(0 == codepointWidth(0x80));  /* C1 PAD */

    /* Zero-width codepoints */
    ASSERT_TRUE(0 == codepointWidth(0x0301)); /* combining acute */
    ASSERT_TRUE(0 == codepointWidth(0x200B)); /* zero width space */
    ASSERT_TRUE(0 == codepointWidth(0x200D)); /* ZWJ */
    ASSERT_TRUE(0 == codepointWidth(0xFE0F)); /* VS16 */
    ASSERT_TRUE(0 == codepointWidth(0xD800)); /* surrogate */

    /* Wide (width 2) */
    ASSERT_TRUE(2 == codepointWidth(0x4E00));  /* CJK ideograph */
    ASSERT_TRUE(2 == codepointWidth(0xFF21));  /* fullwidth A */
    ASSERT_TRUE(2 == codepointWidth(0xAC00));  /* Hangul syllable */
    ASSERT_TRUE(2 == codepointWidth(0x1F600)); /* emoji */
    ASSERT_TRUE(2 == codepointWidth(0x1F1E6)); /* regional indicator */
    ASSERT_TRUE(2 == codepointWidth(0x2E3B));  /* three-em dash (clamped) */
}

TEST(unicode, grapheme_break__emoji_modifier) {
    /* Emoji and modifier */
    {
        BreakState state;
        ASSERT_TRUE(!graphemeBreak(0x261D, 0x1F3FF, &state));
    }

    /* Non-emoji and emoji modifier */
    {
        BreakState state;
        ASSERT_TRUE(graphemeBreak(0x22, 0x1F3FF, &state));
    }
}

TEST(unicode, long_emoji_zwj_sequences) {
    BreakState state;
    /* 👩‍👩‍👧‍👦 (family: woman, woman, girl, boy) */
    const uint32_t cps[] = {0x1F469, 0x200D, 0x1F469, 0x200D, 0x1F467, 0x200D, 0x1F466, '_'};
    size_t i = 0;
    uint32_t cp1 = cps[i++];
    uint32_t cp2 = cps[i++];
    ASSERT_TRUE(cp1 == 0x1F469); /* 👩 */
    ASSERT_TRUE(!graphemeBreak(cp1, cp2, &state));

    cp1 = cp2;
    cp2 = cps[i++];
    ASSERT_TRUE(cp1 == 0x200D);
    ASSERT_TRUE(!graphemeBreak(cp1, cp2, &state));

    cp1 = cp2;
    cp2 = cps[i++];
    ASSERT_TRUE(cp1 == 0x1F469); /* 👩 */
    ASSERT_TRUE(!graphemeBreak(cp1, cp2, &state));

    cp1 = cp2;
    cp2 = cps[i++];
    ASSERT_TRUE(cp1 == 0x200D);
    ASSERT_TRUE(!graphemeBreak(cp1, cp2, &state));

    cp1 = cp2;
    cp2 = cps[i++];
    ASSERT_TRUE(cp1 == 0x1F467); /* 👧 */
    ASSERT_TRUE(!graphemeBreak(cp1, cp2, &state));

    cp1 = cp2;
    cp2 = cps[i++];
    ASSERT_TRUE(cp1 == 0x200D);
    ASSERT_TRUE(!graphemeBreak(cp1, cp2, &state));

    cp1 = cp2;
    cp2 = cps[i++];
    ASSERT_TRUE(cp1 == 0x1F466); /* 👦 */
    ASSERT_TRUE(graphemeBreak(cp1, cp2, &state)); /* break */
}

TEST(unicode, grapheme_width__variation_selectors) {
    ASSERT_TRUE(GraphemeWidthEffect::wide == graphemeWidthEffect(0x2764, 0xFE0F));
    ASSERT_TRUE(GraphemeWidthEffect::narrow == graphemeWidthEffect(0x23, 0xFE0E));
    ASSERT_TRUE(GraphemeWidthEffect::ignore == graphemeWidthEffect('x', 0xFE0F));

    EXPECT_GW(2, 2, GW(0x2764, 0xFE0F));
    EXPECT_GW(2, 2, GW(0x23, 0xFE0F));
    EXPECT_GW(2, 1, GW('x', 0xFE0F));
    EXPECT_GW(3, 1, GW('x', 0xFE0F, 0xFE0F));
    EXPECT_GW(2, 1, GW(0x23, 0xFE0E));
    EXPECT_GW(2, 1, GW(0x231A, 0xFE0E));
    EXPECT_GW(3, 1, GW(0x231A, 0xFE0E, 0xFE0F));
    EXPECT_GW(4, 2, GW(0x1F3F4, 0x200D, 0x2620, 0xFE0F));
}

TEST(unicode, grapheme_width__emoji_sequences) {
    EXPECT_GW(5, 2, GW(0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467));
    EXPECT_GW(3, 2, GW(0x23, 0xFE0F, 0x20E3));
    EXPECT_GW(2, 1, GW('1', 0x20E3));
    EXPECT_GW(2, 2, GW(0x1F44B, 0x1F3FF));
}

TEST(unicode, grapheme_width__spacing_marks_can_widen_narrow_clusters) {
    uint32_t mark = 0;
    bool found = false;
    for (uint32_t cp = 0; cp < 0x110000; cp++) {
        const Properties props = Table::get(cp);
        if (props.width() != 1 || props.width_zero_in_grapheme()) continue;

        BreakState state;
        if (!graphemeBreak('a', cp, &state)) {
            mark = cp;
            found = true;
            break;
        }
    }

    ASSERT_TRUE(found);
    ASSERT_TRUE(1 == Table::get(mark).width());
    ASSERT_TRUE(!Table::get(mark).width_zero_in_grapheme());
    const uint32_t cps[] = {'a', mark};
    EXPECT_GW(2, 2, gw(cps, 2));
}

TEST(unicode, grapheme_width__segmentation) {
    EXPECT_GW(1, 1, GW('a'));
    EXPECT_GW(1, 1, GW('a', 'b'));
    EXPECT_GW(2, 2, GW(0x1F1E6, 0x1F1E7, 0x1F1E8));
    EXPECT_GW(1, 2, GW(0x1F1E8));
    EXPECT_GW(0, 0, gw(nullptr, 0));
    EXPECT_GW(2, 0, GW(0x0301, 0x0302));
}

TEST(unicode, grapheme_width__u32_invalid_codepoints_stand_alone) {
    {
        const uint32_t cps[] = {0x110000, 0x0301};
        EXPECT_GW(1, 1, graphemeWidth(cps, 2, true));
    }
    {
        const uint32_t cps[] = {'a', 0x110000};
        EXPECT_GW(1, 1, graphemeWidth(cps, 2, true));
    }
}
