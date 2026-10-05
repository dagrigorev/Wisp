/* Tests for the scrolling operations in src/terminal/page.hpp.
 *
 * Related to Ghostty src/terminal/page.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Scrolling moves rows without moving cells: a row is a handle holding the
 * offset of its cells, so a scroll is a handful of 64-bit swaps. The tests
 * that matter are the ones checking that nothing hanging off a cell — its
 * style, its grapheme cluster, its hyperlink — needs fixing up afterwards,
 * because all of it is keyed by the cell's offset rather than by where the
 * cell sits on screen.
 */

#include "test_helpers.h"
#include "page.hpp"

using namespace wisp::terminal;

struct PageBuf {
    uint64_t words[16384];
    uint8_t *base() { return reinterpret_cast<uint8_t *>(words); }
    PageBuf() { memset(words, 0, sizeof(words)); }
};

/* Mark each row with a distinct character so a rotation can be read off. */
static void label_rows(Page *p, CellCountInt rows) {
    for (CellCountInt y = 0; y < rows; y++) {
        p->get_cell(0, y)->set_codepoint((uint32_t)('A' + y));
    }
}

static bool rows_read(Page *p, const char *want) {
    for (CellCountInt y = 0; want[y]; y++) {
        const uint32_t got = p->get_cell(0, y)->codepoint();
        const uint32_t expect = want[y] == '.' ? 0 : (uint32_t)want[y];
        if (got != expect) return false;
    }
    return true;
}

static style::Style palette_style(uint8_t idx) {
    style::Style s;
    s.fg_color.tag = style::StyleColor::Tag::palette;
    s.fg_color.palette = idx;
    return s;
}

/* ─── scrolling up ───────────────────────────────────────────────────────── */

TEST(scroll, up_shifts_rows_and_blanks_the_bottom) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));
    label_rows(&p, 6);

    page_scroll_up(&p, 0, 5, 2);
    ASSERT_TRUE(rows_read(&p, "CDEF.."));
}

TEST(scroll, up_by_one) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));
    label_rows(&p, 6);

    page_scroll_up(&p, 0, 5, 1);
    ASSERT_TRUE(rows_read(&p, "BCDEF."));
}

TEST(scroll, up_respects_the_region) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));
    label_rows(&p, 6);

    /* DECSTBM region rows 1..4. Rows outside it must not move, which is what
     * makes a scroll region useful in the first place. */
    page_scroll_up(&p, 1, 4, 1);
    ASSERT_TRUE(rows_read(&p, "ACDE.F"));
}

TEST(scroll, up_by_the_whole_region_clears_it) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));
    label_rows(&p, 6);

    page_scroll_up(&p, 1, 3, 3);
    ASSERT_TRUE(rows_read(&p, "A...EF"));
}

TEST(scroll, up_beyond_the_region_clears_it) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));
    label_rows(&p, 6);

    page_scroll_up(&p, 0, 5, 99);
    ASSERT_TRUE(rows_read(&p, "......"));
}

TEST(scroll, up_by_zero_does_nothing) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));
    label_rows(&p, 6);

    page_scroll_up(&p, 0, 5, 0);
    ASSERT_TRUE(rows_read(&p, "ABCDEF"));
}

/* ─── scrolling down ─────────────────────────────────────────────────────── */

TEST(scroll, down_shifts_rows_and_blanks_the_top) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));
    label_rows(&p, 6);

    page_scroll_down(&p, 0, 5, 2);
    ASSERT_TRUE(rows_read(&p, "..ABCD"));
}

TEST(scroll, down_respects_the_region) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));
    label_rows(&p, 6);

    page_scroll_down(&p, 1, 4, 1);
    ASSERT_TRUE(rows_read(&p, "A.BCDF"));
}

TEST(scroll, down_by_the_whole_region_clears_it) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));
    label_rows(&p, 6);

    page_scroll_down(&p, 2, 4, 3);
    ASSERT_TRUE(rows_read(&p, "AB...F"));
}

TEST(scroll, up_then_down_leaves_blanks_not_the_original) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));
    label_rows(&p, 6);

    /* Scrolling is lossy at the page level: what leaves the region is gone.
     * Keeping it is the scrollback's job, not this one's. */
    page_scroll_up(&p, 0, 5, 2);
    page_scroll_down(&p, 0, 5, 2);
    ASSERT_TRUE(rows_read(&p, "..CDEF"));
}

/* ─── what rides along ───────────────────────────────────────────────────── */

TEST(scroll, styles_ride_along_without_reinterning) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));
    label_rows(&p, 6);
    ASSERT_TRUE(p.set_cell_style(0, 4, palette_style(77)));

    const size_t before = p.style_count();
    page_scroll_up(&p, 0, 5, 2);

    /* The style set is untouched: no cell was copied, so nothing had to be
     * interned again. */
    ASSERT_EQ(p.style_count(), before);
    ASSERT_EQ(p.get_cell_style(0, 2).fg_color.palette, 77);
}

TEST(scroll, graphemes_ride_along) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));
    label_rows(&p, 6);
    ASSERT_TRUE(page_append_grapheme(&p, 0, 3, 0x0301));

    page_scroll_up(&p, 0, 5, 3);

    /* The grapheme map keys on the cell's offset, and the cells did not move,
     * so the entry is still correct with no fixup at all. */
    const uint32_t *cps = nullptr;
    uint32_t len = 0;
    ASSERT_TRUE(page_grapheme_codepoints(&p, 0, 0, &cps, &len));
    ASSERT_EQ(len, 1u);
    ASSERT_EQ(cps[0], 0x0301u);
}

TEST(scroll, hyperlinks_ride_along) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));
    ASSERT_TRUE(page_set_cell_hyperlink(&p, 2, 4, "https://ride.test", 17,
                                        nullptr, 0, 0));

    page_scroll_up(&p, 0, 5, 4);

    const uint8_t *uri = nullptr;
    size_t len = 0;
    ASSERT_TRUE(page_get_cell_hyperlink(&p, 2, 0, &uri, &len));
    ASSERT_EQ(len, 17u);
    ASSERT_TRUE(memcmp(uri, "https://ride.test", 17) == 0);
}

TEST(scroll, wide_cells_ride_along) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));

    Cell *lead = p.get_cell(3, 5);
    lead->set_codepoint(0x4E2D);
    lead->set_wide(Wide::wide);
    p.get_cell(4, 5)->set_wide(Wide::spacer_tail);

    page_scroll_up(&p, 0, 5, 5);

    ASSERT_EQ(p.get_cell(3, 0)->codepoint(), 0x4E2Du);
    ASSERT_TRUE(p.get_cell(3, 0)->wide() == Wide::wide);
    ASSERT_TRUE(p.get_cell(4, 0)->wide() == Wide::spacer_tail);
}

/* ─── what gets released ─────────────────────────────────────────────────── */

TEST(scroll, departing_row_releases_its_style) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));
    ASSERT_TRUE(p.set_cell_style(0, 0, palette_style(3)));
    ASSERT_TRUE(p.set_cell_style(0, 5, palette_style(4)));
    ASSERT_EQ(p.style_count(), 2u);

    /* Row 0 leaves the screen, so the style only it used goes with it. A page
     * scrolled all day would otherwise fill its style set with styles nothing
     * references. */
    page_scroll_up(&p, 0, 5, 1);
    ASSERT_EQ(p.style_count(), 1u);
    ASSERT_EQ(p.get_cell_style(0, 4).fg_color.palette, 4);
}

TEST(scroll, departing_row_releases_its_hyperlink) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));
    ASSERT_TRUE(page_set_cell_hyperlink(&p, 0, 0, "https://gone.test", 17,
                                        nullptr, 0, 0));
    ASSERT_EQ(p.hyperlink_set.count(), 1u);

    page_scroll_up(&p, 0, 5, 1);
    ASSERT_EQ(p.hyperlink_set.count(), 0u);
}

TEST(scroll, departing_row_releases_its_graphemes) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));
    p.get_cell(0, 0)->set_codepoint('e');
    ASSERT_TRUE(page_append_grapheme(&p, 0, 0, 0x0301));

    page_scroll_up(&p, 0, 5, 1);

    /* The cells that were row 0 are now row 5, blank. Nothing should be
     * reachable through them. */
    const uint32_t *cps = nullptr;
    uint32_t len = 0;
    ASSERT_FALSE(page_grapheme_codepoints(&p, 0, 5, &cps, &len));
    ASSERT_FALSE(p.get_cell(0, 5)->has_grapheme());
}

TEST(scroll, repeating_a_link_scrolls_indefinitely) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));

    /* The shape of real use: a line carrying the same link written at the
     * bottom and scrolled up, over and over. The set interns it once, so the
     * page can do this forever. */
    for (int i = 0; i < 500; i++) {
        p.get_cell(0, 5)->set_codepoint('x');
        ASSERT_TRUE(p.set_cell_style(0, 5, palette_style((uint8_t)(i % 4))));
        ASSERT_TRUE(page_set_cell_hyperlink(&p, 1, 5, "https://loop.test", 17,
                                            "same", 4, 0));
        page_scroll_up(&p, 0, 5, 1);
    }

    ASSERT_TRUE(p.style_count() <= 4u);
    ASSERT_TRUE(p.hyperlink_set.count() <= 6u);
}

TEST(scroll, distinct_links_exhaust_the_page_cleanly) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));

    /* A *distinct* link on every scrolled line is a different matter, and the
     * limit is not scrolling's.
     *
     * Releasing a link drops its reference but leaves the entry in place so
     * it can be resurrected if the same link comes back — that is what makes
     * a run of cells sharing a link cheap. The entry's strings are only
     * returned to the page when the slot is genuinely reused, so a page given
     * a few hundred never-repeated links fills its string storage even though
     * almost none of them are still referenced.
     *
     * That is the design, not a leak: upstream reaches for a new page rather
     * than scrolling one forever. What matters here is that the page says no
     * rather than corrupting itself. */
    int wrote = 0;
    for (int i = 0; i < 200; i++) {
        char uri[24];
        memcpy(uri, "https://n.test/", 15);
        uri[15] = (char)('a' + (i % 26));
        uri[16] = (char)('a' + (i / 26));
        if (!page_set_cell_hyperlink(&p, 1, 5, uri, 17, nullptr, 0,
                                     (OffsetInt)i)) {
            break;
        }
        wrote++;
        page_scroll_up(&p, 0, 5, 1);
    }

    /* It got a good way in before refusing, and the links still on screen are
     * intact afterwards. */
    ASSERT_TRUE(wrote > 10);
    ASSERT_TRUE(wrote < 200);
    ASSERT_TRUE(p.hyperlink_set.count() <= 6u);

    const uint8_t *uri = nullptr;
    size_t len = 0;
    ASSERT_TRUE(page_get_cell_hyperlink(&p, 1, 4, &uri, &len));
    ASSERT_EQ(len, 17u);
}

/* ─── row flags ──────────────────────────────────────────────────────────── */

TEST(scroll, arriving_blank_row_has_no_stale_flags) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));
    label_rows(&p, 6);

    p.get_row(0)->set_wrap(true);
    p.get_row(0)->set_semantic_prompt(SemanticPrompt::prompt);

    /* Row 0 becomes the blank row at the bottom. A wrap flag left on it would
     * join that blank line to whatever lands above it later. */
    page_scroll_up(&p, 0, 5, 1);

    ASSERT_FALSE(p.get_row(5)->wrap());
    ASSERT_FALSE(p.get_row(5)->wrap_continuation());
    ASSERT_TRUE(p.get_row(5)->semantic_prompt() == SemanticPrompt::none);
}

TEST(scroll, moving_row_keeps_its_flags) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));
    label_rows(&p, 6);
    p.get_row(3)->set_wrap(true);
    p.get_row(4)->set_wrap_continuation(true);

    page_scroll_up(&p, 0, 5, 2);

    /* A wrapped pair that scrolls stays a wrapped pair. */
    ASSERT_TRUE(p.get_row(1)->wrap());
    ASSERT_TRUE(p.get_row(2)->wrap_continuation());
}

TEST(scroll, everything_is_marked_dirty) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));
    label_rows(&p, 6);
    for (CellCountInt y = 0; y < 6; y++) p.get_row(y)->set_dirty(false);

    page_scroll_up(&p, 0, 5, 2);
    for (CellCountInt y = 0; y < 6; y++) ASSERT_TRUE(p.get_row(y)->dirty());
}

/* ─── the swap itself ────────────────────────────────────────────────────── */

TEST(scroll, swap_moves_the_handle_not_the_cells) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));

    const OffsetInt off0 = p.get_row(0)->cells().offset;
    const OffsetInt off3 = p.get_row(3)->cells().offset;
    p.get_cell(0, 0)->set_codepoint('a');

    page_swap_rows(&p, 0, 3);

    /* The cells stayed put; the rows now point at each other's. That is the
     * whole trick, and it is why the offset-keyed maps need no updating. */
    ASSERT_EQ(p.get_row(0)->cells().offset, off3);
    ASSERT_EQ(p.get_row(3)->cells().offset, off0);
    ASSERT_EQ(p.get_cell(0, 3)->codepoint(), 'a');
}

TEST(scroll, swapping_a_row_with_itself_is_a_no_op) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));
    p.get_cell(0, 2)->set_codepoint('m');

    page_swap_rows(&p, 2, 2);
    ASSERT_EQ(p.get_cell(0, 2)->codepoint(), 'm');
}

TEST(scroll, region_past_the_last_row_is_clamped) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(10, 6));
    label_rows(&p, 6);

    page_scroll_up(&p, 0, 99, 1);
    ASSERT_TRUE(rows_read(&p, "BCDEF."));
}
