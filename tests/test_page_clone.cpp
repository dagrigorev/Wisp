/* Tests for the clone operations in src/terminal/page.hpp.
 *
 * Related to Ghostty src/terminal/page.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * A clone is not a memcpy: style IDs, hyperlink IDs and grapheme runs are all
 * relative to the page they live in, so the interesting question is whether
 * what a cell *means* survives the move, not whether its bits did. These
 * tests read the destination back through the same accessors a renderer would
 * and never look at an ID directly, except to assert that it was allowed to
 * differ.
 */

#include "test_helpers.h"
#include "page.hpp"

using namespace wisp::terminal;

struct PageBuf {
    uint64_t words[16384];
    uint8_t *base() { return reinterpret_cast<uint8_t *>(words); }
    PageBuf() { memset(words, 0, sizeof(words)); }
};

static const uint32_t COMBINING_ACUTE = 0x0301;
static const uint32_t COMBINING_GRAVE = 0x0300;

static style::Style make_style(uint8_t palette_fg, bool bold) {
    style::Style s;
    s.fg_color.tag = style::StyleColor::Tag::palette;
    s.fg_color.palette = palette_fg;
    s.flags.bold = bold;
    return s;
}

static bool uri_is(Page *p, CellCountInt x, CellCountInt y, const char *want) {
    const uint8_t *uri = nullptr;
    size_t len = 0;
    if (!page_get_cell_hyperlink(p, x, y, &uri, &len)) return false;
    return len == strlen(want) && memcmp(uri, want, len) == 0;
}

/* ─── plain text ─────────────────────────────────────────────────────────── */

TEST(clone, copies_codepoints) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    src.get_cell(0, 3)->set_codepoint('h');
    src.get_cell(1, 3)->set_codepoint('i');

    ASSERT_TRUE(page_clone_row(&dst, 0, &src, 3));
    ASSERT_EQ(dst.get_cell(0, 0)->codepoint(), 'h');
    ASSERT_EQ(dst.get_cell(1, 0)->codepoint(), 'i');
}

TEST(clone, copies_row_flags_but_not_cells_offset) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    src.get_row(2)->set_wrap(true);
    src.get_row(2)->set_semantic_prompt(SemanticPrompt::prompt);
    src.get_cell(0, 2)->set_codepoint('x');

    /* The destination row's cells must keep pointing into the destination,
     * or every read after this lands in the source's memory. */
    const OffsetInt before = dst.get_row(5)->cells().offset;
    ASSERT_TRUE(page_clone_row(&dst, 5, &src, 2));

    ASSERT_EQ(dst.get_row(5)->cells().offset, before);
    ASSERT_TRUE(dst.get_row(5)->wrap());
    ASSERT_TRUE(dst.get_row(5)->semantic_prompt() == SemanticPrompt::prompt);
    ASSERT_EQ(dst.get_cell(0, 5)->codepoint(), 'x');
}

TEST(clone, overwrites_destination_row) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    for (CellCountInt x = 0; x < 20; x++) dst.get_cell(x, 0)->set_codepoint('#');
    src.get_cell(0, 0)->set_codepoint('a');

    ASSERT_TRUE(page_clone_row(&dst, 0, &src, 0));
    ASSERT_EQ(dst.get_cell(0, 0)->codepoint(), 'a');
    /* Everything the source did not describe is erased, not left behind. */
    ASSERT_EQ(dst.get_cell(5, 0)->codepoint(), 0);
}

TEST(clone, wider_destination_leaves_extra_columns_blank) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(10, 8));
    Page dst = Page::init(b.base(), Capacity(30, 8));

    for (CellCountInt x = 0; x < 10; x++) src.get_cell(x, 0)->set_codepoint('z');
    for (CellCountInt x = 0; x < 30; x++) dst.get_cell(x, 0)->set_codepoint('#');

    ASSERT_TRUE(page_clone_row(&dst, 0, &src, 0));
    ASSERT_EQ(dst.get_cell(9, 0)->codepoint(), 'z');
    ASSERT_EQ(dst.get_cell(10, 0)->codepoint(), 0);
}

TEST(clone, narrower_destination_refused) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(30, 8));
    Page dst = Page::init(b.base(), Capacity(10, 8));

    ASSERT_FALSE(page_clone_row(&dst, 0, &src, 0));
}

TEST(clone, out_of_range_row_refused) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    ASSERT_FALSE(page_clone_row(&dst, 99, &src, 0));
    ASSERT_FALSE(page_clone_row(&dst, 0, &src, 99));
}

/* ─── styles ─────────────────────────────────────────────────────────────── */

TEST(clone, style_survives_as_value) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    const style::Style s = make_style(9, true);
    src.get_cell(0, 0)->set_codepoint('s');
    ASSERT_TRUE(src.set_cell_style(0, 0, s));

    ASSERT_TRUE(page_clone_row(&dst, 0, &src, 0));

    const style::Style got = dst.get_cell_style(0, 0);
    ASSERT_TRUE(got.fg_color.tag == style::StyleColor::Tag::palette);
    ASSERT_EQ(got.fg_color.palette, 9);
    ASSERT_TRUE(got.flags.bold);
    ASSERT_TRUE(dst.get_row(0)->styled());
}

TEST(clone, style_id_may_differ_from_source) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    /* Fill the destination with styles the source does not have, so the ID
     * the shared style lands on cannot coincide by construction. */
    for (uint8_t i = 1; i < 6; i++) {
        ASSERT_TRUE(dst.set_cell_style(i, 7, make_style(i, false)));
    }

    const style::Style s = make_style(200, false);
    ASSERT_TRUE(src.set_cell_style(0, 0, s));
    const style::Id src_id = src.get_cell(0, 0)->style_id();

    ASSERT_TRUE(page_clone_row(&dst, 0, &src, 0));

    ASSERT_TRUE(dst.get_cell(0, 0)->style_id() != src_id);
    ASSERT_EQ(dst.get_cell_style(0, 0).fg_color.palette, 200);
}

TEST(clone, repeated_style_interned_once) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    const style::Style s = make_style(4, false);
    for (CellCountInt x = 0; x < 20; x++) ASSERT_TRUE(src.set_cell_style(x, 0, s));

    ASSERT_TRUE(page_clone_row(&dst, 0, &src, 0));
    ASSERT_EQ(dst.style_count(), 1u);
}

TEST(clone, replacing_a_row_releases_its_styles) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    ASSERT_TRUE(src.set_cell_style(0, 0, make_style(11, false)));
    ASSERT_TRUE(src.set_cell_style(0, 1, make_style(22, false)));

    ASSERT_TRUE(page_clone_row(&dst, 0, &src, 0));
    ASSERT_EQ(dst.style_count(), 1u);

    /* The second clone lands on the same destination row, so the first row's
     * style must go — otherwise a page rewritten many times accumulates
     * styles nothing references. */
    ASSERT_TRUE(page_clone_row(&dst, 0, &src, 1));
    ASSERT_EQ(dst.style_count(), 1u);
    ASSERT_EQ(dst.get_cell_style(0, 0).fg_color.palette, 22);
}

/* ─── graphemes ──────────────────────────────────────────────────────────── */

TEST(clone, grapheme_cluster_copied) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    src.get_cell(2, 0)->set_codepoint('e');
    ASSERT_TRUE(page_append_grapheme(&src, 2, 0, COMBINING_ACUTE));
    ASSERT_TRUE(page_append_grapheme(&src, 2, 0, COMBINING_GRAVE));

    ASSERT_TRUE(page_clone_row(&dst, 4, &src, 0));

    ASSERT_EQ(dst.get_cell(2, 4)->codepoint(), 'e');
    ASSERT_TRUE(dst.get_cell(2, 4)->has_grapheme());

    const uint32_t *cps = nullptr;
    uint32_t len = 0;
    ASSERT_TRUE(page_grapheme_codepoints(&dst, 2, 4, &cps, &len));
    ASSERT_EQ(len, 2);
    ASSERT_EQ(cps[0], COMBINING_ACUTE);
    ASSERT_EQ(cps[1], COMBINING_GRAVE);
}

TEST(clone, grapheme_storage_is_independent) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    src.get_cell(0, 0)->set_codepoint('e');
    ASSERT_TRUE(page_append_grapheme(&src, 0, 0, COMBINING_ACUTE));
    ASSERT_TRUE(page_clone_row(&dst, 0, &src, 0));

    /* Clearing the source must not disturb the copy. If the clone had kept
     * the source's offsets this would read freed storage. */
    page_clear_grapheme(&src, 0, 0);

    const uint32_t *cps = nullptr;
    uint32_t len = 0;
    ASSERT_TRUE(page_grapheme_codepoints(&dst, 0, 0, &cps, &len));
    ASSERT_EQ(len, 1);
    ASSERT_EQ(cps[0], COMBINING_ACUTE);
}

/* ─── hyperlinks ─────────────────────────────────────────────────────────── */

TEST(clone, hyperlink_uri_copied) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    src.get_cell(1, 0)->set_codepoint('L');
    ASSERT_TRUE(page_set_cell_hyperlink(&src, 1, 0, "https://example.com", 19,
                                        nullptr, 0, 0));

    ASSERT_TRUE(page_clone_row(&dst, 0, &src, 0));
    ASSERT_TRUE(uri_is(&dst, 1, 0, "https://example.com"));
    ASSERT_TRUE(dst.get_row(0)->hyperlink());
}

TEST(clone, hyperlink_strings_are_independent) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    ASSERT_TRUE(page_set_cell_hyperlink(&src, 0, 0, "https://a.test", 14,
                                        nullptr, 0, 0));
    ASSERT_TRUE(page_clone_row(&dst, 0, &src, 0));

    /* Dropping the source's last reference frees its strings. The copy must
     * have its own. */
    page_clear_cell_hyperlink(&src, 0, 0);
    ASSERT_TRUE(uri_is(&dst, 0, 0, "https://a.test"));
}

TEST(clone, shared_link_interned_once) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    /* One OSC 8 run covering several cells is the common case; it should cost
     * one copy of the URI in the destination, not one per cell. */
    for (CellCountInt x = 0; x < 6; x++) {
        ASSERT_TRUE(page_set_cell_hyperlink(&src, x, 0, "https://shared.test", 19,
                                            "id1", 3, 0));
    }

    ASSERT_TRUE(page_clone_row(&dst, 0, &src, 0));

    for (CellCountInt x = 0; x < 6; x++) {
        ASSERT_TRUE(uri_is(&dst, x, 0, "https://shared.test"));
    }
    ASSERT_EQ(dst.hyperlink_set.count(), 1u);
}

TEST(clone, distinct_links_stay_distinct) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    ASSERT_TRUE(page_set_cell_hyperlink(&src, 0, 0, "https://one.test", 16,
                                        "a", 1, 0));
    ASSERT_TRUE(page_set_cell_hyperlink(&src, 1, 0, "https://two.test", 16,
                                        "b", 1, 0));

    ASSERT_TRUE(page_clone_row(&dst, 0, &src, 0));
    ASSERT_TRUE(uri_is(&dst, 0, 0, "https://one.test"));
    ASSERT_TRUE(uri_is(&dst, 1, 0, "https://two.test"));
    ASSERT_EQ(dst.hyperlink_set.count(), 2u);
}

TEST(clone, stale_hyperlink_bit_without_entry) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    /* The cell bit is a hint. A cell carrying it with nothing in the map is
     * copied as an ordinary cell rather than failing the clone. */
    src.get_cell(0, 0)->set_codepoint('q');
    src.get_cell(0, 0)->set_hyperlink(true);

    ASSERT_TRUE(page_clone_row(&dst, 0, &src, 0));
    ASSERT_EQ(dst.get_cell(0, 0)->codepoint(), 'q');
    ASSERT_FALSE(dst.get_cell(0, 0)->hyperlink());
}

/* ─── multiple rows ──────────────────────────────────────────────────────── */

TEST(clone, clone_rows_copies_run) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    for (CellCountInt y = 0; y < 5; y++) {
        src.get_cell(0, y)->set_codepoint('0' + y);
    }

    ASSERT_EQ(page_clone_rows(&dst, 2, &src, 0, 5), 5);
    for (CellCountInt y = 0; y < 5; y++) {
        ASSERT_EQ(dst.get_cell(0, (CellCountInt)(y + 2))->codepoint(),
                  (uint32_t)('0' + y));
    }
}

TEST(clone, clone_rows_stops_at_destination_end) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    for (CellCountInt y = 0; y < 8; y++) src.get_cell(0, y)->set_codepoint('y');

    /* Six rows requested starting at row 5 of an eight-row page: three fit. */
    ASSERT_EQ(page_clone_rows(&dst, 5, &src, 0, 6), 3);
    ASSERT_EQ(dst.get_cell(0, 7)->codepoint(), 'y');
}

TEST(clone, round_trip_through_a_third_page) {
    PageBuf a, b, c;
    Page one = Page::init(a.base(), Capacity(20, 8));
    Page two = Page::init(b.base(), Capacity(20, 8));
    Page three = Page::init(c.base(), Capacity(20, 8));

    /* A row carrying all three kinds of indirection at once, moved twice.
     * Anything that leaks a source-relative offset shows up by the second
     * hop, where the first page is no longer the one being read from. */
    one.get_cell(0, 0)->set_codepoint('e');
    ASSERT_TRUE(page_append_grapheme(&one, 0, 0, COMBINING_ACUTE));
    ASSERT_TRUE(one.set_cell_style(0, 0, make_style(77, true)));
    ASSERT_TRUE(page_set_cell_hyperlink(&one, 0, 0, "https://round.test", 18,
                                        nullptr, 0, 0));

    ASSERT_TRUE(page_clone_row(&two, 1, &one, 0));
    ASSERT_TRUE(page_clone_row(&three, 2, &two, 1));

    ASSERT_EQ(three.get_cell(0, 2)->codepoint(), 'e');

    const uint32_t *cps = nullptr;
    uint32_t len = 0;
    ASSERT_TRUE(page_grapheme_codepoints(&three, 0, 2, &cps, &len));
    ASSERT_EQ(len, 1);
    ASSERT_EQ(cps[0], COMBINING_ACUTE);

    ASSERT_EQ(three.get_cell_style(0, 2).fg_color.palette, 77);
    ASSERT_TRUE(three.get_cell_style(0, 2).flags.bold);
    ASSERT_TRUE(uri_is(&three, 0, 2, "https://round.test"));
}

/* ─── exhaustion ─────────────────────────────────────────────────────────── */

TEST(clone, failure_leaves_destination_row_erased) {
    PageBuf a, b;

    /* A source with room for far more link text than the destination has.
     * Asking for a smaller string capacity would not work: the allocator
     * rounds its bitmap up to a whole word, so any request under 64 chunks
     * lands on the same 512-byte floor the default already has. The
     * difference has to come from the source being bigger, not the
     * destination being smaller. */
    Capacity roomy(20, 8);
    roomy.string_bytes = 4096;
    Page src = Page::init(a.base(), roomy);
    Page dst = Page::init(b.base(), Capacity(20, 8));

    /* Sixteen distinct links of sixty-odd bytes each: comfortably inside the
     * source, comfortably past the destination. The clone must fail rather
     * than leave the row half translated. */
    for (CellCountInt x = 0; x < 16; x++) {
        char uri[64];
        memset(uri, 'u', sizeof(uri));
        memcpy(uri, "https://exhaust", 15);
        uri[15] = (char)('a' + x);
        src.get_cell(x, 0)->set_codepoint('L');
        ASSERT_TRUE(page_set_cell_hyperlink(&src, x, 0, uri, sizeof(uri),
                                            nullptr, 0, x));
    }

    ASSERT_FALSE(page_clone_row(&dst, 0, &src, 0));

    for (CellCountInt x = 0; x < 16; x++) {
        ASSERT_EQ(dst.get_cell(x, 0)->codepoint(), 0);
        ASSERT_FALSE(dst.get_cell(x, 0)->hyperlink());
    }
    /* Every link taken before the failure was released again, so the page is
     * as usable as it was before the attempt. */
    ASSERT_EQ(dst.hyperlink_set.count(), 0u);
}

TEST(clone, source_is_not_modified) {
    PageBuf a, b;
    Page src = Page::init(a.base(), Capacity(20, 8));
    Page dst = Page::init(b.base(), Capacity(20, 8));

    src.get_cell(0, 0)->set_codepoint('e');
    ASSERT_TRUE(page_append_grapheme(&src, 0, 0, COMBINING_ACUTE));
    ASSERT_TRUE(src.set_cell_style(0, 0, make_style(3, false)));
    ASSERT_TRUE(page_set_cell_hyperlink(&src, 0, 0, "https://src.test", 16,
                                        nullptr, 0, 0));

    const uint64_t style_count = src.style_count();
    const uint64_t link_count = src.hyperlink_set.count();

    ASSERT_TRUE(page_clone_row(&dst, 0, &src, 0));

    ASSERT_EQ(src.style_count(), style_count);
    ASSERT_EQ(src.hyperlink_set.count(), link_count);
    ASSERT_EQ(src.get_cell_style(0, 0).fg_color.palette, 3);
    ASSERT_TRUE(uri_is(&src, 0, 0, "https://src.test"));
}
