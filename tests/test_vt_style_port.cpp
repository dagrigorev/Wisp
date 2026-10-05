/* Transliterated from the test blocks in Ghostty src/terminal/style.zig and
 * src/fastprint.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include "test_helpers.h"
#include "../vt/style.hpp"

#include <malloc.h>
#include <stdio.h>

using namespace wisp::vt;
using namespace wisp::vt::style;
typedef ::wisp::terminal::sgr::Attribute::Underline U;
typedef Style::Color C;

static RGB rgb(uint8_t r, uint8_t g, uint8_t b) { return RGB(r, g, b); }

static std::string vt(const Style &s, const Palette *p = nullptr) {
    std::string w;
    Style::VTFormatter f = s.formatterVt();
    f.palette = p;
    f.format(&w);
    return w;
}

static std::string html(const Style &s, const Palette *p = nullptr) {
    std::string w;
    Style::HtmlFormatter f = s.formatterHtml();
    f.palette = p;
    f.format(&w);
    return w;
}

static bool contains(const std::string &h, const char *n) { return h.find(n) != std::string::npos; }

#define FLAG(field) ([] { Style s; s.flags.field = true; return s; }())
#define UNDERLINE(u) ([] { Style s; s.flags.underline = U::u; return s; }())

TEST(style, Style_VT_formatting_empty) { ASSERT_TRUE(vt(Style()) == "\x1b[0m"); }
TEST(style, Style_VT_formatting_bold) { ASSERT_TRUE(vt(FLAG(bold)) == "\x1b[0m\x1b[1m"); }
TEST(style, Style_VT_formatting_faint) { ASSERT_TRUE(vt(FLAG(faint)) == "\x1b[0m\x1b[2m"); }
TEST(style, Style_VT_formatting_italic) { ASSERT_TRUE(vt(FLAG(italic)) == "\x1b[0m\x1b[3m"); }
TEST(style, Style_VT_formatting_blink) { ASSERT_TRUE(vt(FLAG(blink)) == "\x1b[0m\x1b[5m"); }
TEST(style, Style_VT_formatting_inverse) { ASSERT_TRUE(vt(FLAG(inverse)) == "\x1b[0m\x1b[7m"); }
TEST(style, Style_VT_formatting_invisible) { ASSERT_TRUE(vt(FLAG(invisible)) == "\x1b[0m\x1b[8m"); }
TEST(style, Style_VT_formatting_strikethrough) { ASSERT_TRUE(vt(FLAG(strikethrough)) == "\x1b[0m\x1b[9m"); }
TEST(style, Style_VT_formatting_overline) { ASSERT_TRUE(vt(FLAG(overline)) == "\x1b[0m\x1b[53m"); }
TEST(style, Style_VT_formatting_underline_single) { ASSERT_TRUE(vt(UNDERLINE(single)) == "\x1b[0m\x1b[4m"); }
TEST(style, Style_VT_formatting_underline_double) { ASSERT_TRUE(vt(UNDERLINE(double_)) == "\x1b[0m\x1b[4:2m"); }
TEST(style, Style_VT_formatting_underline_curly) { ASSERT_TRUE(vt(UNDERLINE(curly)) == "\x1b[0m\x1b[4:3m"); }
TEST(style, Style_VT_formatting_underline_dotted) { ASSERT_TRUE(vt(UNDERLINE(dotted)) == "\x1b[0m\x1b[4:4m"); }
TEST(style, Style_VT_formatting_underline_dashed) { ASSERT_TRUE(vt(UNDERLINE(dashed)) == "\x1b[0m\x1b[4:5m"); }

TEST(style, Style_VT_formatting_fg_palette) {
    Style s; s.fg_color = C::makePalette(42);
    ASSERT_TRUE(vt(s) == "\x1b[0m\x1b[38;5;42m");
}
TEST(style, Style_VT_formatting_fg_rgb) {
    Style s; s.fg_color = C::makeRgb(rgb(255, 128, 64));
    ASSERT_TRUE(vt(s) == "\x1b[0m\x1b[38;2;255;128;64m");
}
TEST(style, Style_VT_formatting_bg_palette) {
    Style s; s.bg_color = C::makePalette(7);
    ASSERT_TRUE(vt(s) == "\x1b[0m\x1b[48;5;7m");
}
TEST(style, Style_VT_formatting_bg_rgb) {
    Style s; s.bg_color = C::makeRgb(rgb(32, 64, 96));
    ASSERT_TRUE(vt(s) == "\x1b[0m\x1b[48;2;32;64;96m");
}
TEST(style, Style_VT_formatting_underline_color_palette) {
    Style s; s.underline_color = C::makePalette(15);
    ASSERT_TRUE(vt(s) == "\x1b[0m\x1b[58;5;15m");
}
TEST(style, Style_VT_formatting_underline_color_rgb) {
    Style s; s.underline_color = C::makeRgb(rgb(200, 100, 50));
    ASSERT_TRUE(vt(s) == "\x1b[0m\x1b[58;2;200;100;50m");
}

TEST(style, Style_VT_formatting_multiple_flags) {
    Style s;
    s.flags.bold = true; s.flags.italic = true; s.flags.underline = U::single;
    ASSERT_TRUE(vt(s) == "\x1b[0m\x1b[1m\x1b[3m\x1b[4m");
}

TEST(style, Style_VT_formatting_all_flags) {
    Style s;
    s.flags.bold = true; s.flags.faint = true; s.flags.italic = true; s.flags.blink = true;
    s.flags.inverse = true; s.flags.invisible = true; s.flags.strikethrough = true;
    s.flags.overline = true; s.flags.underline = U::curly;
    ASSERT_TRUE(vt(s) == "\x1b[0m\x1b[1m\x1b[2m\x1b[3m\x1b[5m\x1b[7m\x1b[8m\x1b[9m\x1b[53m\x1b[4:3m");
}

TEST(style, Style_VT_formatting_combined_colors_and_flags) {
    Style s;
    s.fg_color = C::makeRgb(rgb(255, 0, 0));
    s.bg_color = C::makePalette(8);
    s.underline_color = C::makeRgb(rgb(0, 255, 0));
    s.flags.bold = true; s.flags.italic = true; s.flags.underline = U::double_;
    ASSERT_TRUE(vt(s) == "\x1b[0m\x1b[1m\x1b[3m\x1b[4:2m\x1b[38;2;255;0;0m\x1b[48;5;8m\x1b[58;2;0;255;0m");
}

TEST(style, Style_VT_formatting_all_colors_rgb) {
    Style s;
    s.fg_color = C::makeRgb(rgb(10, 20, 30));
    s.bg_color = C::makeRgb(rgb(40, 50, 60));
    s.underline_color = C::makeRgb(rgb(70, 80, 90));
    ASSERT_TRUE(vt(s) == "\x1b[0m\x1b[38;2;10;20;30m\x1b[48;2;40;50;60m\x1b[58;2;70;80;90m");
}

TEST(style, Style_VT_formatting_all_colors_palette) {
    Style s;
    s.fg_color = C::makePalette(1);
    s.bg_color = C::makePalette(2);
    s.underline_color = C::makePalette(3);
    ASSERT_TRUE(vt(s) == "\x1b[0m\x1b[38;5;1m\x1b[48;5;2m\x1b[58;5;3m");
}

TEST(style, Style_VT_formatting_palette_with_palette_set_emits_rgb) {
    Style s; s.fg_color = C::makePalette(1);
    ASSERT_TRUE(vt(s, &::wisp::terminal::default_palette()) == "\x1b[0m\x1b[38;2;204;102;102m");
}

TEST(style, Style_VT_formatting_all_palette_colors_with_palette_set) {
    Style s;
    s.fg_color = C::makePalette(1);
    s.bg_color = C::makePalette(2);
    s.underline_color = C::makePalette(3);
    ASSERT_TRUE(vt(s, &::wisp::terminal::default_palette()) ==
                "\x1b[0m\x1b[38;2;204;102;102m\x1b[48;2;181;189;104m\x1b[58;2;240;198;116m");
}

TEST(style, Set_basic_usage) {
    const Set::Layout layout = Set::Layout::init(16);
    uint8_t *buf = (uint8_t *)_aligned_malloc(layout.total_size, Set::base_align);
    const void *base = buf;

    Style st; st.flags.bold = true;
    Style st2; st2.flags.italic = true;

    Set set = Set::init(size::OffsetBuf::init(buf), layout, SetContext());

    /* Add style */
    Id id;
    ASSERT_TRUE(set.add(base, st, &id) == ref_counted_set::AddError::none);
    ASSERT_TRUE(id > 0);

    /* Second add should return the same metadata. */
    {
        Id id2;
        ASSERT_TRUE(set.add(base, st, &id2) == ref_counted_set::AddError::none);
        ASSERT_TRUE(id == id2);
    }

    /* Look it up */
    {
        const Style *v = set.get(base, id);
        ASSERT_TRUE(v->flags.bold);

        const Style *v2 = set.get(base, id);
        ASSERT_TRUE(v == v2);
    }

    /* Add a second style */
    Id id2;
    ASSERT_TRUE(set.add(base, st2, &id2) == ref_counted_set::AddError::none);

    /* Look it up */
    ASSERT_TRUE(set.get(base, id2)->flags.italic);

    /* Ref count */
    ASSERT_TRUE(set.refCount(base, id) == 2);
    ASSERT_TRUE(set.refCount(base, id2) == 1);

    /* Release */
    set.release(base, id);
    ASSERT_TRUE(set.refCount(base, id) == 1);
    set.release(base, id2);
    ASSERT_TRUE(set.refCount(base, id2) == 0);

    /* We added the first one twice, so */
    set.release(base, id);
    ASSERT_TRUE(set.refCount(base, id) == 0);

    _aligned_free(buf);
}

TEST(style, Set_capacities) {
    /* We want to support at least this many styles without overflowing. */
    (void)Set::Layout::init(16384);
}

TEST(style, Set_zero_capacity) {
    /* A zero-capacity set is a valid special case (see Layout.init).
     * It occurs in practice for pages with exact capacities where no
     * cell is styled (see Page.exactRowCapacity). */
    const Set::Layout layout = Set::Layout::init(0);
    ASSERT_TRUE(layout.total_size == 0);

    /* We allocate a nonzero buffer filled with 0xFF to simulate the
     * set being embedded in a larger structure (e.g. a Page) where
     * other data follows it. Lookups must not probe the zero-size
     * table: table[0] would read this adjacent memory and treat it
     * as an item ID, leading to far out-of-bounds item reads. */
    uint8_t *buf = (uint8_t *)_aligned_malloc(64, Set::base_align);
    memset(buf, 0xFF, 64);
    const void *base = buf;

    Set set = Set::init(size::OffsetBuf::init(buf), layout, SetContext());

    Style st; st.flags.bold = true;
    Id id;
    ASSERT_FALSE(set.lookup(base, st, &id));
    ASSERT_TRUE(set.add(base, st, &id) == ref_counted_set::AddError::OutOfMemory);
    ASSERT_TRUE(set.count() == 0);

    /* The adjacent memory must be untouched. */
    for (size_t i = 0; i < 64; i++) ASSERT_TRUE(buf[i] == 0xFF);
    _aligned_free(buf);
}

TEST(style, Style_HTML_formatting_basic_bold) {
    ASSERT_TRUE(html(FLAG(bold)) == "font-weight: bold;");
}

TEST(style, Style_HTML_formatting_fg_color_rgb) {
    Style s; s.fg_color = C::makeRgb(rgb(255, 128, 64));
    ASSERT_TRUE(html(s) == "color: rgb(255, 128, 64);");
}

TEST(style, Style_HTML_formatting_bg_color_palette) {
    Style s; s.bg_color = C::makePalette(7);
    ASSERT_TRUE(html(s) == "background-color: var(--vt-palette-7);");
}

TEST(style, Style_HTML_formatting_combined_colors_and_flags) {
    Style s;
    s.fg_color = C::makeRgb(rgb(255, 0, 0));
    s.bg_color = C::makeRgb(rgb(0, 0, 255));
    s.flags.bold = true; s.flags.italic = true;
    const std::string result = html(s);
    ASSERT_TRUE(contains(result, "color: rgb(255, 0, 0);"));
    ASSERT_TRUE(contains(result, "background-color: rgb(0, 0, 255);"));
    ASSERT_TRUE(contains(result, "font-weight: bold;"));
    ASSERT_TRUE(contains(result, "font-style: italic;"));
}

TEST(style, Style_HTML_formatting_single_decoration_line) {
    const std::string result = html(UNDERLINE(single));
    ASSERT_TRUE(contains(result, "text-decoration-line: underline;"));
    ASSERT_TRUE(contains(result, "text-decoration-style: solid;"));
}

TEST(style, Style_HTML_formatting_multiple_decoration_lines) {
    Style s;
    s.flags.underline = U::curly; s.flags.strikethrough = true; s.flags.overline = true;
    const std::string result = html(s);
    ASSERT_TRUE(contains(result, "text-decoration-line: underline line-through overline;"));
    ASSERT_TRUE(contains(result, "text-decoration-style: wavy;"));
}

TEST(style, Style_HTML_formatting_palette_with_palette_set_emits_rgb) {
    Style s; s.bg_color = C::makePalette(7);
    ASSERT_TRUE(html(s, &::wisp::terminal::default_palette()) == "background-color: rgb(197, 200, 198);");
}

TEST(style, Style_HTML_formatting_all_palette_colors_with_palette_set) {
    Style s;
    s.fg_color = C::makePalette(1);
    s.bg_color = C::makePalette(2);
    s.underline_color = C::makePalette(3);
    ASSERT_TRUE(html(s, &::wisp::terminal::default_palette()) ==
                "color: rgb(204, 102, 102);background-color: rgb(181, 189, 104);text-decoration-color: rgb(240, 198, 116);");
}

/* fastprint.zig */

TEST(fastprint, printDecimal) {
    char buf[16];

    /* u8: exercise 1, 2, and 3 digit values including boundaries. */
    const uint8_t u8_cases[] = { 0, 1, 9, 10, 99, 100, 255 };
    for (size_t i = 0; i < sizeof(u8_cases); i++) {
        char expected[8];
        snprintf(expected, sizeof(expected), "%u", (unsigned)u8_cases[i]);
        const size_t len = wisp::fastprint::printDecimalU8(buf, u8_cases[i]);
        ASSERT_TRUE(std::string(buf, len) == expected);
    }

    /* u21: exercise digit count boundaries up to the maximum. */
    const uint32_t u21_cases[] = { 0, 9, 10, 128, 65535, 1114111, 2097151 };
    for (size_t i = 0; i < 7; i++) {
        char expected[16];
        snprintf(expected, sizeof(expected), "%u", (unsigned)u21_cases[i]);
        const size_t len = wisp::fastprint::printDecimalU21(buf, u21_cases[i]);
        ASSERT_TRUE(std::string(buf, len) == expected);
    }
}
