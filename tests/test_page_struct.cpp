/* Tests for the Page struct in src/terminal/page.hpp.
 *
 * Related to Ghostty src/terminal/page.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Where test_page.cpp checks bit layouts in isolation, this checks that the
 * ported modules actually compose: rows and cells addressed through offsets,
 * styles interned through the ref-counted set, and the whole thing relocatable
 * to a different address. Those are behavioral properties, so they are
 * testable without any reference to how upstream arranges its bytes.
 */

#include "test_helpers.h"
#include "page.hpp"

using namespace wisp::terminal;

/* Backing store large enough for the pages used below. */
struct PageBuf {
    uint64_t words[16384];
    uint8_t *base() { return reinterpret_cast<uint8_t *>(words); }
    PageBuf() { memset(words, 0, sizeof(words)); }
};

/* ─── construction ───────────────────────────────────────────────────────── */

TEST(pagestruct, init_points_every_row_at_its_own_cells) {
    PageBuf buf;
    Capacity cap(20, 8);
    ASSERT_TRUE(PageLayout::init(cap).total_size <= sizeof(buf.words));

    Page p = Page::init(buf.base(), cap);

    /* Rows must be contiguous, in-order slices of the cell array. A shared or
     * misordered offset would have two rows writing the same cells. */
    for (CellCountInt y = 0; y + 1 < cap.rows; y++) {
        const OffsetInt a = p.get_row(y)->cells().offset;
        const OffsetInt b = p.get_row((CellCountInt)(y + 1))->cells().offset;
        ASSERT_EQ(b - a, cap.cols * sizeof(Cell));
    }
}

TEST(pagestruct, fresh_page_is_empty) {
    PageBuf buf;
    Capacity cap(10, 4);
    Page p = Page::init(buf.base(), cap);

    for (CellCountInt y = 0; y < cap.rows; y++) {
        for (CellCountInt x = 0; x < cap.cols; x++) {
            ASSERT_FALSE(p.get_cell(x, y)->has_text());
            ASSERT_EQ(p.get_cell(x, y)->style_id(), style::DEFAULT_ID);
        }
        ASSERT_FALSE(p.get_row(y)->styled());
    }
    ASSERT_EQ(p.style_count(), 0);
}

TEST(pagestruct, cells_round_trip) {
    PageBuf buf;
    Capacity cap(20, 8);
    Page p = Page::init(buf.base(), cap);

    for (CellCountInt y = 0; y < cap.rows; y++) {
        for (CellCountInt x = 0; x < cap.cols; x++) {
            const uint32_t cp = (uint32_t)(0x41 + ((y * cap.cols + x) % 26));
            p.get_cell(x, y)->set_codepoint(cp);
        }
    }

    for (CellCountInt y = 0; y < cap.rows; y++) {
        for (CellCountInt x = 0; x < cap.cols; x++) {
            const uint32_t want = (uint32_t)(0x41 + ((y * cap.cols + x) % 26));
            ASSERT_EQ(p.get_cell(x, y)->codepoint(), want);
        }
    }
}

TEST(pagestruct, writing_one_cell_does_not_touch_its_neighbours) {
    PageBuf buf;
    Capacity cap(10, 4);
    Page p = Page::init(buf.base(), cap);

    p.get_cell(5, 2)->set_codepoint(0x58);

    for (CellCountInt y = 0; y < cap.rows; y++) {
        for (CellCountInt x = 0; x < cap.cols; x++) {
            if (x == 5 && y == 2) continue;
            ASSERT_FALSE(p.get_cell(x, y)->has_text());
        }
    }
}

/* ─── style interning ────────────────────────────────────────────────────── */

TEST(pagestruct, styling_a_cell_interns_the_style) {
    PageBuf buf;
    Capacity cap(10, 4);
    Page p = Page::init(buf.base(), cap);

    style::Style s;
    s.flags.bold = true;
    s.fg_color = style::StyleColor::from_palette(3);

    ASSERT_TRUE(p.set_cell_style(0, 0, s));

    ASSERT_EQ(p.style_count(), 1);
    ASSERT_TRUE(p.get_cell(0, 0)->style_id() != style::DEFAULT_ID);
    ASSERT_TRUE(p.get_row(0)->styled());
    ASSERT_TRUE(p.get_cell_style(0, 0).eql(s));
}

TEST(pagestruct, identical_styles_share_one_record) {
    /* The point of interning: a screenful of one style costs one record. */
    PageBuf buf;
    Capacity cap(20, 8);
    Page p = Page::init(buf.base(), cap);

    style::Style s;
    s.flags.italic = true;

    for (CellCountInt y = 0; y < cap.rows; y++) {
        for (CellCountInt x = 0; x < cap.cols; x++) {
            ASSERT_TRUE(p.set_cell_style(x, y, s));
        }
    }

    ASSERT_EQ(p.style_count(), 1);

    const style::Id id = p.get_cell(0, 0)->style_id();
    for (CellCountInt y = 0; y < cap.rows; y++) {
        for (CellCountInt x = 0; x < cap.cols; x++) {
            ASSERT_EQ(p.get_cell(x, y)->style_id(), id);
        }
    }
}

TEST(pagestruct, default_style_is_not_interned) {
    /* ID 0 means default, so applying it must not consume a style slot. */
    PageBuf buf;
    Capacity cap(10, 4);
    Page p = Page::init(buf.base(), cap);

    style::Style def;
    ASSERT_TRUE(p.set_cell_style(0, 0, def));

    ASSERT_EQ(p.style_count(), 0);
    ASSERT_EQ(p.get_cell(0, 0)->style_id(), style::DEFAULT_ID);
}

TEST(pagestruct, reapplying_the_same_style_does_not_disturb_the_refcount) {
    /* The new reference is taken before the old is released. Reversed,
     * re-applying a style would drop its count to zero in between and leave
     * it eligible for reaping. */
    PageBuf buf;
    Capacity cap(10, 4);
    Page p = Page::init(buf.base(), cap);

    style::Style s;
    s.flags.bold = true;

    ASSERT_TRUE(p.set_cell_style(0, 0, s));
    const style::Id id = p.get_cell(0, 0)->style_id();

    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(p.set_cell_style(0, 0, s));
        ASSERT_EQ(p.get_cell(0, 0)->style_id(), id);
        ASSERT_EQ(p.style_count(), 1);
        ASSERT_TRUE(p.get_cell_style(0, 0).eql(s));
    }
}

TEST(pagestruct, changing_a_style_releases_the_previous_one) {
    PageBuf buf;
    Capacity cap(10, 4);
    Page p = Page::init(buf.base(), cap);

    style::Style bold;
    bold.flags.bold = true;

    style::Style italic;
    italic.flags.italic = true;

    ASSERT_TRUE(p.set_cell_style(0, 0, bold));
    ASSERT_EQ(p.style_count(), 1);

    /* The only user of bold moves away, so bold must be released. */
    ASSERT_TRUE(p.set_cell_style(0, 0, italic));
    ASSERT_EQ(p.style_count(), 1);
    ASSERT_TRUE(p.get_cell_style(0, 0).eql(italic));
}

TEST(pagestruct, style_refcounts_balance_across_many_cells) {
    /* Style every cell, then unstyle every cell. A missed release would leave
     * the count above zero — a leak that eventually exhausts the style set on
     * a long-running page. */
    PageBuf buf;
    Capacity cap(20, 8);
    Page p = Page::init(buf.base(), cap);

    style::Style a; a.flags.bold = true;
    style::Style b; b.flags.underline = Underline::curly;

    for (CellCountInt y = 0; y < cap.rows; y++) {
        for (CellCountInt x = 0; x < cap.cols; x++) {
            ASSERT_TRUE(p.set_cell_style(x, y, (x % 2) ? a : b));
        }
    }
    ASSERT_EQ(p.style_count(), 2);

    for (CellCountInt y = 0; y < cap.rows; y++) {
        for (CellCountInt x = 0; x < cap.cols; x++) {
            p.clear_cell_style(x, y);
        }
    }

    ASSERT_EQ(p.style_count(), 0);
    ASSERT_TRUE(p.styles.check_integrity(p.memory));
    ASSERT_TRUE(p.styles.check_reachable(p.memory));
}

TEST(pagestruct, clearing_a_cell_releases_its_style) {
    PageBuf buf;
    Capacity cap(10, 4);
    Page p = Page::init(buf.base(), cap);

    style::Style s;
    s.flags.bold = true;

    p.get_cell(1, 1)->set_codepoint(0x7A);
    ASSERT_TRUE(p.set_cell_style(1, 1, s));
    ASSERT_EQ(p.style_count(), 1);

    p.clear_cell(1, 1);

    ASSERT_EQ(p.style_count(), 0);
    ASSERT_FALSE(p.get_cell(1, 1)->has_text());
    ASSERT_EQ(p.get_cell(1, 1)->style_id(), style::DEFAULT_ID);
}

TEST(pagestruct, style_set_exhaustion_is_reported_not_ignored) {
    /* A page has a finite style budget. Running out must be reported so the
     * caller can reallocate at a larger capacity, rather than silently
     * dropping the styling. */
    PageBuf buf;
    Capacity cap(40, 20);
    cap.styles = 8;
    Page p = Page::init(buf.base(), cap);

    bool refused = false;
    int applied = 0;
    for (int i = 0; i < 400; i++) {
        const CellCountInt x = (CellCountInt)(i % cap.cols);
        const CellCountInt y = (CellCountInt)(i / cap.cols);
        if (y >= cap.rows) break;

        style::Style s;
        s.fg_color = style::StyleColor::from_rgb(
            RGB((uint8_t)i, (uint8_t)(i * 7), (uint8_t)(i * 13)));

        if (!p.set_cell_style(x, y, s)) { refused = true; break; }
        applied++;
    }

    ASSERT_TRUE(refused);
    ASSERT_TRUE(applied > 0);

    /* Still coherent after refusing. */
    ASSERT_TRUE(p.styles.check_integrity(p.memory));
    ASSERT_TRUE(p.styles.check_reachable(p.memory));
}

/* ─── relocation ─────────────────────────────────────────────────────────── */

TEST(pagestruct, survives_relocation_to_a_different_address) {
    /* The reason for all the offset machinery. Copy the page's bytes
     * elsewhere, re-point the base, and rows, cells and interned styles must
     * all still resolve — with no fixups of any kind. */
    PageBuf src;
    PageBuf dst;

    Capacity cap(20, 8);
    Page p = Page::init(src.base(), cap);

    style::Style s;
    s.flags.bold = true;
    s.fg_color = style::StyleColor::from_rgb(RGB(1, 2, 3));

    for (CellCountInt x = 0; x < cap.cols; x++) {
        p.get_cell(x, 0)->set_codepoint((uint32_t)(0x41 + x));
        ASSERT_TRUE(p.set_cell_style(x, 0, s));
    }

    memcpy(dst.base(), src.base(), p.size);
    Page moved = p;
    moved.relocate(dst.base());

    /* Scribble over the original, so anything still reading from it fails. */
    memset(src.base(), 0xCD, p.size);

    for (CellCountInt x = 0; x < cap.cols; x++) {
        ASSERT_EQ(moved.get_cell(x, 0)->codepoint(), (uint32_t)(0x41 + x));
        ASSERT_TRUE(moved.get_cell_style(x, 0).eql(s));
    }
    ASSERT_EQ(moved.style_count(), 1);
    ASSERT_TRUE(moved.styles.check_reachable(moved.memory));
}

TEST(pagestruct, relocation_preserves_the_style_set_itself) {
    /* Not just cell contents: the interning table has to survive the move,
     * since it is addressed by offset like everything else. */
    PageBuf src;
    PageBuf dst;

    Capacity cap(20, 8);
    Page p = Page::init(src.base(), cap);

    style::Style styles[4];
    for (int i = 0; i < 4; i++) {
        styles[i].fg_color = style::StyleColor::from_palette((uint8_t)(i + 1));
        ASSERT_TRUE(p.set_cell_style((CellCountInt)i, 0, styles[i]));
    }
    ASSERT_EQ(p.style_count(), 4);

    memcpy(dst.base(), src.base(), p.size);
    Page moved = p;
    moved.relocate(dst.base());
    memset(src.base(), 0xCD, p.size);

    ASSERT_EQ(moved.style_count(), 4);
    for (int i = 0; i < 4; i++) {
        ASSERT_TRUE(moved.get_cell_style((CellCountInt)i, 0).eql(styles[i]));
    }

    /* And it must still be usable, not merely readable. */
    style::Style extra;
    extra.flags.italic = true;
    ASSERT_TRUE(moved.set_cell_style(5, 0, extra));
    ASSERT_EQ(moved.style_count(), 5);
    ASSERT_TRUE(moved.styles.check_reachable(moved.memory));
}
