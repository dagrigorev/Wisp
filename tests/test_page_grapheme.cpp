/* Tests for the grapheme and erase operations in src/terminal/page.hpp.
 *
 * Related to Ghostty src/terminal/page.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * These are behavioral: write codepoints onto a cell, read them back, erase,
 * and check the page's storage returns to where it started. None of it
 * depends on how the bytes happen to be arranged.
 */

#include "test_helpers.h"
#include "page.hpp"

using namespace wisp::terminal;

struct PageBuf {
    uint64_t words[16384];
    uint8_t *base() { return reinterpret_cast<uint8_t *>(words); }
    PageBuf() { memset(words, 0, sizeof(words)); }
};

/* Combining acute accent, the canonical "one cell, two codepoints" case. */
static const uint32_t COMBINING_ACUTE = 0x0301;
static const uint32_t COMBINING_GRAVE = 0x0300;

/* ─── appending ──────────────────────────────────────────────────────────── */

TEST(grapheme, plain_cell_has_none) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(20, 8));

    p.get_cell(0, 0)->set_codepoint(0x65);   /* 'e' */

    const uint32_t *cps = nullptr;
    uint32_t len = 0;
    ASSERT_FALSE(page_grapheme_codepoints(&p, 0, 0, &cps, &len));
    ASSERT_FALSE(p.get_cell(0, 0)->has_grapheme());
}

TEST(grapheme, append_one_codepoint) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(20, 8));

    p.get_cell(0, 0)->set_codepoint(0x65);
    ASSERT_TRUE(page_append_grapheme(&p, 0, 0, COMBINING_ACUTE));

    /* The base codepoint stays in the cell; only the extra moves out. */
    ASSERT_EQ(p.get_cell(0, 0)->codepoint(), 0x65);
    ASSERT_TRUE(p.get_cell(0, 0)->has_grapheme());
    ASSERT_TRUE(p.get_row(0)->grapheme());

    const uint32_t *cps = nullptr;
    uint32_t len = 0;
    ASSERT_TRUE(page_grapheme_codepoints(&p, 0, 0, &cps, &len));
    ASSERT_EQ(len, 1);
    ASSERT_EQ(cps[0], COMBINING_ACUTE);
}

TEST(grapheme, append_several_keeps_order) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(20, 8));

    p.get_cell(1, 1)->set_codepoint(0x61);   /* 'a' */

    const uint32_t marks[] = { COMBINING_ACUTE, COMBINING_GRAVE, 0x0308 };
    for (int i = 0; i < 3; i++) {
        ASSERT_TRUE(page_append_grapheme(&p, 1, 1, marks[i]));
    }

    const uint32_t *cps = nullptr;
    uint32_t len = 0;
    ASSERT_TRUE(page_grapheme_codepoints(&p, 1, 1, &cps, &len));
    ASSERT_EQ(len, 3);

    /* Order matters: combining marks apply in sequence. */
    for (int i = 0; i < 3; i++) ASSERT_EQ(cps[i], marks[i]);
}

TEST(grapheme, cells_keep_their_own_clusters) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(20, 8));

    for (CellCountInt x = 0; x < 5; x++) {
        p.get_cell(x, 0)->set_codepoint(0x61 + x);
        ASSERT_TRUE(page_append_grapheme(&p, x, 0, 0x0300 + x));
    }

    for (CellCountInt x = 0; x < 5; x++) {
        const uint32_t *cps = nullptr;
        uint32_t len = 0;
        ASSERT_TRUE(page_grapheme_codepoints(&p, x, 0, &cps, &len));
        ASSERT_EQ(len, 1);
        ASSERT_EQ(cps[0], (uint32_t)(0x0300 + x));
        ASSERT_EQ(p.get_cell(x, 0)->codepoint(), (uint32_t)(0x61 + x));
    }
}

TEST(grapheme, high_codepoints_round_trip) {
    /* Emoji modifiers live well above the BMP, so the storage has to be 32
     * bits wide rather than 16. */
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(20, 8));

    p.get_cell(0, 0)->set_codepoint(0x1F469);          /* woman */
    ASSERT_TRUE(page_append_grapheme(&p, 0, 0, 0x1F3FD));  /* skin tone */

    const uint32_t *cps = nullptr;
    uint32_t len = 0;
    ASSERT_TRUE(page_grapheme_codepoints(&p, 0, 0, &cps, &len));
    ASSERT_EQ(len, 1);
    ASSERT_EQ(cps[0], 0x1F3FD);
    ASSERT_EQ(p.get_cell(0, 0)->codepoint(), 0x1F469);
}

/* ─── clearing ───────────────────────────────────────────────────────────── */

TEST(grapheme, clear_returns_the_cell_to_plain) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(20, 8));

    p.get_cell(0, 0)->set_codepoint(0x65);
    ASSERT_TRUE(page_append_grapheme(&p, 0, 0, COMBINING_ACUTE));

    page_clear_grapheme(&p, 0, 0);

    ASSERT_FALSE(p.get_cell(0, 0)->has_grapheme());
    /* The cell keeps its own codepoint. */
    ASSERT_EQ(p.get_cell(0, 0)->codepoint(), 0x65);

    const uint32_t *cps = nullptr;
    uint32_t len = 0;
    ASSERT_FALSE(page_grapheme_codepoints(&p, 0, 0, &cps, &len));
}

TEST(grapheme, clear_reclaims_storage) {
    /* Unlike interned links, grapheme runs are owned outright by one cell, so
     * clearing hands the bytes straight back. */
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(20, 8));

    const size_t before = p.grapheme_alloc.used_bytes(p.memory);

    p.get_cell(0, 0)->set_codepoint(0x65);
    ASSERT_TRUE(page_append_grapheme(&p, 0, 0, COMBINING_ACUTE));
    ASSERT_TRUE(p.grapheme_alloc.used_bytes(p.memory) > before);

    page_clear_grapheme(&p, 0, 0);
    ASSERT_EQ(p.grapheme_alloc.used_bytes(p.memory), before);
}

TEST(grapheme, repeated_append_does_not_leak) {
    /* Each append reallocates and frees the previous run. If the old run were
     * not released, a cell built up one mark at a time would consume storage
     * quadratically. */
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(20, 8));

    const size_t before = p.grapheme_alloc.used_bytes(p.memory);

    p.get_cell(0, 0)->set_codepoint(0x61);
    for (uint32_t i = 0; i < 8; i++) {
        ASSERT_TRUE(page_append_grapheme(&p, 0, 0, 0x0300 + i));
    }

    const uint32_t *cps = nullptr;
    uint32_t len = 0;
    ASSERT_TRUE(page_grapheme_codepoints(&p, 0, 0, &cps, &len));
    ASSERT_EQ(len, 8);

    /* Exactly one run of eight is held, not eight runs of growing length. */
    const size_t held = p.grapheme_alloc.used_bytes(p.memory) - before;
    ASSERT_TRUE(held <= 8 * sizeof(uint32_t) + 8);

    page_clear_grapheme(&p, 0, 0);
    ASSERT_EQ(p.grapheme_alloc.used_bytes(p.memory), before);
}

TEST(grapheme, clearing_a_plain_cell_is_harmless) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(20, 8));

    p.get_cell(2, 2)->set_codepoint(0x78);
    page_clear_grapheme(&p, 2, 2);
    ASSERT_EQ(p.get_cell(2, 2)->codepoint(), 0x78);
}

/* ─── erase ──────────────────────────────────────────────────────────────── */

TEST(erase, cell_releases_style_grapheme_and_link) {
    /* Erasing has to let go of everything a cell referenced, or each is
     * stranded for the page's lifetime. */
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(20, 8));

    const size_t g_before = p.grapheme_alloc.used_bytes(p.memory);

    p.get_cell(0, 0)->set_codepoint(0x65);
    ASSERT_TRUE(page_append_grapheme(&p, 0, 0, COMBINING_ACUTE));

    style::Style s;
    s.flags.bold = true;
    ASSERT_TRUE(p.set_cell_style(0, 0, s));

    const char *uri = "https://example.com/e";
    ASSERT_TRUE(page_set_cell_hyperlink(&p, 0, 0, uri, strlen(uri), NULL, 0, 1));

    ASSERT_EQ(p.style_count(), 1);
    ASSERT_EQ(p.hyperlink_set.count(), 1);

    page_erase_cell(&p, 0, 0);

    ASSERT_EQ(p.style_count(), 0);
    ASSERT_EQ(p.hyperlink_set.count(), 0);
    ASSERT_EQ(p.grapheme_alloc.used_bytes(p.memory), g_before);

    const Cell *c = p.get_cell(0, 0);
    ASSERT_FALSE(c->has_text());
    ASSERT_FALSE(c->has_grapheme());
    ASSERT_FALSE(c->hyperlink());
    ASSERT_EQ(c->style_id(), style::DEFAULT_ID);
}

TEST(erase, row_releases_every_cell) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(20, 8));

    style::Style a; a.flags.bold = true;
    style::Style b; b.flags.italic = true;

    for (CellCountInt x = 0; x < 10; x++) {
        p.get_cell(x, 3)->set_codepoint(0x61 + x);
        ASSERT_TRUE(p.set_cell_style(x, 3, (x % 2) ? a : b));
        ASSERT_TRUE(page_append_grapheme(&p, x, 3, 0x0300 + x));
    }
    ASSERT_EQ(p.style_count(), 2);

    page_erase_row(&p, 3);

    ASSERT_EQ(p.style_count(), 0);
    ASSERT_EQ(p.grapheme_alloc.used_bytes(p.memory), 0);

    for (CellCountInt x = 0; x < p.capacity.cols; x++) {
        ASSERT_FALSE(p.get_cell(x, 3)->has_text());
        ASSERT_EQ(p.get_cell(x, 3)->style_id(), style::DEFAULT_ID);
    }
    ASSERT_TRUE(p.get_row(3)->dirty());
}

TEST(erase, untouched_row_takes_the_fast_path) {
    /* A row that never held a style, grapheme or link can be cleared without
     * per-cell release work. The flags exist precisely to allow that, so the
     * result has to be identical either way. */
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(20, 8));

    for (CellCountInt x = 0; x < 10; x++) {
        p.get_cell(x, 2)->set_codepoint(0x41 + x);
    }

    ASSERT_FALSE(p.get_row(2)->styled());
    ASSERT_FALSE(p.get_row(2)->grapheme());
    ASSERT_FALSE(p.get_row(2)->hyperlink());

    page_erase_row(&p, 2);

    for (CellCountInt x = 0; x < p.capacity.cols; x++) {
        ASSERT_FALSE(p.get_cell(x, 2)->has_text());
    }
    ASSERT_TRUE(p.get_row(2)->dirty());
}

TEST(erase, one_row_does_not_disturb_another) {
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(20, 8));

    style::Style s;
    s.flags.bold = true;

    for (CellCountInt y = 0; y < 4; y++) {
        for (CellCountInt x = 0; x < 6; x++) {
            p.get_cell(x, y)->set_codepoint(0x41 + x);
            ASSERT_TRUE(p.set_cell_style(x, y, s));
        }
    }

    page_erase_row(&p, 1);

    /* Row 1 is gone; the rest still reads back, style included. */
    for (CellCountInt x = 0; x < 6; x++) {
        ASSERT_FALSE(p.get_cell(x, 1)->has_text());

        ASSERT_EQ(p.get_cell(x, 0)->codepoint(), (uint32_t)(0x41 + x));
        ASSERT_TRUE(p.get_cell_style(x, 0).eql(s));
        ASSERT_EQ(p.get_cell(x, 2)->codepoint(), (uint32_t)(0x41 + x));
        ASSERT_TRUE(p.get_cell_style(x, 2).eql(s));
    }

    /* Still one interned style, held by the surviving rows. */
    ASSERT_EQ(p.style_count(), 1);
}

/* ─── relocation ─────────────────────────────────────────────────────────── */

TEST(grapheme, survives_page_relocation) {
    PageBuf src;
    PageBuf dst;

    Page p = Page::init(src.base(), Capacity(20, 8));

    for (CellCountInt x = 0; x < 5; x++) {
        p.get_cell(x, 0)->set_codepoint(0x61 + x);
        ASSERT_TRUE(page_append_grapheme(&p, x, 0, 0x0300 + x));
        ASSERT_TRUE(page_append_grapheme(&p, x, 0, 0x0310 + x));
    }

    memcpy(dst.base(), src.base(), p.size);
    Page moved = p;
    moved.relocate(dst.base());
    memset(src.base(), 0xCD, p.size);

    for (CellCountInt x = 0; x < 5; x++) {
        const uint32_t *cps = nullptr;
        uint32_t len = 0;
        ASSERT_TRUE(page_grapheme_codepoints(&moved, x, 0, &cps, &len));
        ASSERT_EQ(len, 2);
        ASSERT_EQ(cps[0], (uint32_t)(0x0300 + x));
        ASSERT_EQ(cps[1], (uint32_t)(0x0310 + x));
        ASSERT_EQ(moved.get_cell(x, 0)->codepoint(), (uint32_t)(0x61 + x));
    }
}

/* ─── exhaustion ─────────────────────────────────────────────────────────── */

TEST(grapheme, running_out_is_reported_and_leaves_the_cell_intact) {
    /* When storage runs out the cell must keep what it already had, not end
     * up with a half-written run. */
    PageBuf buf;
    Page p = Page::init(buf.base(), Capacity(40, 20));

    CellCountInt fx = 0, fy = 0;
    bool refused = false;

    for (int i = 0; i < 2000 && !refused; i++) {
        const CellCountInt x = (CellCountInt)(i % p.capacity.cols);
        const CellCountInt y = (CellCountInt)(i / p.capacity.cols);
        if (y >= p.capacity.rows) break;

        p.get_cell(x, y)->set_codepoint(0x61);
        if (!page_append_grapheme(&p, x, y, 0x0301)) {
            refused = true;
            fx = x; fy = y;
        }
    }

    ASSERT_TRUE(refused);

    /* The cell that was refused still holds its own codepoint and has no
     * partial cluster. */
    ASSERT_EQ(p.get_cell(fx, fy)->codepoint(), 0x61);

    const uint32_t *cps = nullptr;
    uint32_t len = 0;
    ASSERT_FALSE(page_grapheme_codepoints(&p, fx, fy, &cps, &len));
}
