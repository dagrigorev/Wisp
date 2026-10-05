/* Tests for src/terminal/page.hpp.
 *
 * Related to Ghostty src/terminal/page.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Cell and Row are hand-packed into a uint64_t each, so the failure mode is a
 * wrong shift or mask: a field that silently overlaps another, or one that
 * loses its high bits. The tests below therefore check field independence —
 * setting every field and confirming none of the others moved — rather than
 * only round-tripping values one at a time, which a bad mask would survive.
 */

#include "test_helpers.h"
#include "page.hpp"

using namespace wisp::terminal;

/* ─── size and layout ────────────────────────────────────────────────────── */

TEST(page, cell_and_row_are_64_bits) {
    /* The whole layout depends on this: a row of cells is memcpy'd and a page
     * is relocated wholesale. */
    ASSERT_EQ(sizeof(Cell), 8);
    ASSERT_EQ(sizeof(Row), 8);
}

TEST(page, cell_fields_do_not_overlap) {
    /* Build the union of every field's bit span and check nothing is claimed
     * twice. Done by setting each field to all ones in isolation. */
    uint64_t seen = 0;

    Cell tag; tag.set_content_tag((ContentTag)0x3);
    Cell content; content.set_content_raw(0xFFFFFF);
    Cell style; style.set_style_id(0xFFFF);
    Cell wide; wide.set_wide((Wide)0x3);
    Cell prot; prot.set_protect(true);
    Cell link; link.set_hyperlink(true);
    Cell sem; sem.set_semantic_content((SemanticContent)0x3);

    const uint64_t spans[] = {
        tag.bits, content.bits, style.bits, wide.bits,
        prot.bits, link.bits, sem.bits,
    };

    for (size_t i = 0; i < sizeof(spans) / sizeof(spans[0]); i++) {
        ASSERT_EQ(seen & spans[i], 0);   /* no overlap with anything prior */
        seen |= spans[i];
    }

    /* Bits 48-63 are padding and must stay clear. */
    ASSERT_EQ(seen >> 48, 0);
}

TEST(page, row_fields_do_not_overlap) {
    uint64_t seen = 0;

    Offset<Cell> off; off.offset = 0xFFFFFFFF;
    Row cells; cells.set_cells(off);
    Row wrap; wrap.set_wrap(true);
    Row wrapc; wrapc.set_wrap_continuation(true);
    Row graph; graph.set_grapheme(true);
    Row styled; styled.set_styled(true);
    Row link; link.set_hyperlink(true);
    Row prompt; prompt.set_semantic_prompt((SemanticPrompt)0x3);
    Row kitty; kitty.set_kitty_virtual_placeholder(true);
    Row dirty; dirty.set_dirty(true);

    const uint64_t spans[] = {
        cells.bits, wrap.bits, wrapc.bits, graph.bits, styled.bits,
        link.bits, prompt.bits, kitty.bits, dirty.bits,
    };

    for (size_t i = 0; i < sizeof(spans) / sizeof(spans[0]); i++) {
        ASSERT_EQ(seen & spans[i], 0);
        seen |= spans[i];
    }

    /* Bits 41-63 are padding. */
    ASSERT_EQ(seen >> 41, 0);
}

/* ─── field independence ─────────────────────────────────────────────────── */

TEST(page, cell_setters_do_not_disturb_other_fields) {
    Cell c;

    /* Fill every field with a distinct non-default value. */
    c.set_content_tag(ContentTag::bg_color_rgb);
    c.set_color_rgb(RGB(0x12, 0x34, 0x56));
    c.set_style_id(0xBEEF);
    c.set_wide(Wide::spacer_head);
    c.set_protect(true);
    c.set_hyperlink(true);
    c.set_semantic_content(SemanticContent::prompt);

    /* All of them must still read back. */
    ASSERT_TRUE(c.content_tag() == ContentTag::bg_color_rgb);
    ASSERT_TRUE(c.color_rgb().eql(RGB(0x12, 0x34, 0x56)));
    ASSERT_EQ(c.style_id(), 0xBEEF);
    ASSERT_TRUE(c.wide() == Wide::spacer_head);
    ASSERT_TRUE(c.protect());
    ASSERT_TRUE(c.hyperlink());
    ASSERT_TRUE(c.semantic_content() == SemanticContent::prompt);

    /* Rewriting one field must leave the rest alone. */
    c.set_style_id(1);
    ASSERT_EQ(c.style_id(), 1);
    ASSERT_TRUE(c.content_tag() == ContentTag::bg_color_rgb);
    ASSERT_TRUE(c.color_rgb().eql(RGB(0x12, 0x34, 0x56)));
    ASSERT_TRUE(c.wide() == Wide::spacer_head);
    ASSERT_TRUE(c.protect());
    ASSERT_TRUE(c.hyperlink());
    ASSERT_TRUE(c.semantic_content() == SemanticContent::prompt);

    /* Clearing a flag must not clear its neighbours. */
    c.set_protect(false);
    ASSERT_FALSE(c.protect());
    ASSERT_TRUE(c.hyperlink());
    ASSERT_TRUE(c.wide() == Wide::spacer_head);
}

TEST(page, row_setters_do_not_disturb_other_fields) {
    Row r;

    Offset<Cell> off; off.offset = 0xDEADBEEF;
    r.set_cells(off);
    r.set_wrap(true);
    r.set_wrap_continuation(true);
    r.set_grapheme(true);
    r.set_styled(true);
    r.set_hyperlink(true);
    r.set_semantic_prompt(SemanticPrompt::prompt_continuation);
    r.set_kitty_virtual_placeholder(true);
    r.set_dirty(true);

    ASSERT_EQ(r.cells().offset, 0xDEADBEEF);
    ASSERT_TRUE(r.wrap());
    ASSERT_TRUE(r.wrap_continuation());
    ASSERT_TRUE(r.grapheme());
    ASSERT_TRUE(r.styled());
    ASSERT_TRUE(r.hyperlink());
    ASSERT_TRUE(r.semantic_prompt() == SemanticPrompt::prompt_continuation);
    ASSERT_TRUE(r.kitty_virtual_placeholder());
    ASSERT_TRUE(r.dirty());

    /* The cells offset is 32 bits directly below the flags, which is the
     * likeliest place for a mask to run over. */
    Offset<Cell> off2; off2.offset = 0;
    r.set_cells(off2);
    ASSERT_EQ(r.cells().offset, 0);
    ASSERT_TRUE(r.wrap());
    ASSERT_TRUE(r.dirty());
    ASSERT_TRUE(r.semantic_prompt() == SemanticPrompt::prompt_continuation);

    r.set_dirty(false);
    ASSERT_FALSE(r.dirty());
    ASSERT_TRUE(r.kitty_virtual_placeholder());
    ASSERT_TRUE(r.styled());
}

/* ─── content union ──────────────────────────────────────────────────────── */

TEST(page, codepoint_round_trips_full_21_bits) {
    Cell c;
    c.set_content_tag(ContentTag::codepoint);

    /* The largest Unicode codepoint, which needs all 21 bits. */
    c.set_codepoint(0x10FFFF);
    ASSERT_EQ(c.codepoint(), 0x10FFFF);
    ASSERT_TRUE(c.content_tag() == ContentTag::codepoint);

    c.set_codepoint('A');
    ASSERT_EQ(c.codepoint(), 'A');

    c.set_codepoint(0);
    ASSERT_EQ(c.codepoint(), 0);
}

TEST(page, codepoint_does_not_reach_the_style_id) {
    /* The content union is 24 bits and the codepoint only 21, so writing a
     * value with high bits set must not spill upward. */
    Cell c;
    c.set_style_id(0xFFFF);
    c.set_codepoint(0x1FFFFF);

    ASSERT_EQ(c.style_id(), 0xFFFF);
    ASSERT_EQ(c.codepoint(), 0x1FFFFF);
}

TEST(page, palette_and_rgb_share_the_content_union) {
    Cell c;

    c.set_content_tag(ContentTag::bg_color_palette);
    c.set_color_palette(200);
    ASSERT_EQ(c.color_palette(), 200);

    c.set_content_tag(ContentTag::bg_color_rgb);
    c.set_color_rgb(RGB(1, 2, 3));
    ASSERT_TRUE(c.color_rgb().eql(RGB(1, 2, 3)));

    /* Full-range components, to catch a byte order mistake. */
    c.set_color_rgb(RGB(0xAA, 0xBB, 0xCC));
    ASSERT_EQ(c.color_rgb().r, 0xAA);
    ASSERT_EQ(c.color_rgb().g, 0xBB);
    ASSERT_EQ(c.color_rgb().b, 0xCC);
}

/* ─── derived predicates ─────────────────────────────────────────────────── */

TEST(page, has_text) {
    Cell empty;
    ASSERT_FALSE(empty.has_text());       /* codepoint 0 */

    Cell letter;
    letter.set_codepoint('x');
    ASSERT_TRUE(letter.has_text());

    Cell grapheme;
    grapheme.set_content_tag(ContentTag::codepoint_grapheme);
    grapheme.set_codepoint('e');
    ASSERT_TRUE(grapheme.has_text());

    /* A background-only cell holds no text. */
    Cell bg;
    bg.set_content_tag(ContentTag::bg_color_rgb);
    bg.set_color_rgb(RGB(1, 2, 3));
    ASSERT_FALSE(bg.has_text());
}

TEST(page, has_grapheme_and_bg_color) {
    Cell plain;
    plain.set_codepoint('a');
    ASSERT_FALSE(plain.has_grapheme());
    ASSERT_FALSE(plain.has_bg_color());

    Cell g;
    g.set_content_tag(ContentTag::codepoint_grapheme);
    ASSERT_TRUE(g.has_grapheme());
    ASSERT_FALSE(g.has_bg_color());

    Cell pal;
    pal.set_content_tag(ContentTag::bg_color_palette);
    ASSERT_TRUE(pal.has_bg_color());
    ASSERT_FALSE(pal.has_grapheme());

    Cell rgb;
    rgb.set_content_tag(ContentTag::bg_color_rgb);
    ASSERT_TRUE(rgb.has_bg_color());
}

TEST(page, default_cell_is_all_zero) {
    /* A zeroed page must read as empty default cells, which is what lets a
     * fresh page skip initialization entirely. */
    Cell c;
    ASSERT_EQ(c.bits, 0);
    ASSERT_TRUE(c.content_tag() == ContentTag::codepoint);
    ASSERT_EQ(c.codepoint(), 0);
    ASSERT_EQ(c.style_id(), style::DEFAULT_ID);
    ASSERT_TRUE(c.wide() == Wide::narrow);
    ASSERT_FALSE(c.protect());
    ASSERT_FALSE(c.hyperlink());
    ASSERT_TRUE(c.semantic_content() == SemanticContent::output);
    ASSERT_FALSE(c.has_text());
}

TEST(page, default_row_is_all_zero) {
    Row r;
    ASSERT_EQ(r.bits, 0);
    ASSERT_EQ(r.cells().offset, 0);
    ASSERT_FALSE(r.wrap());
    ASSERT_FALSE(r.dirty());
    ASSERT_TRUE(r.semantic_prompt() == SemanticPrompt::none);
}

/* ─── style / page cycle ─────────────────────────────────────────────────── */

TEST(page, cell_background_overrides_the_style) {
    /* The reason background-only cells can skip the style map: their own
     * color takes precedence. */
    Palette p = default_palette();

    style::Style s;
    s.bg_color = style::StyleColor::from_rgb(RGB(9, 9, 9));

    Cell c;
    c.set_content_tag(ContentTag::bg_color_rgb);
    c.set_color_rgb(RGB(1, 2, 3));

    RGB out;
    ASSERT_TRUE(s.bg(&c, &p, &out));
    ASSERT_TRUE(out.eql(RGB(1, 2, 3)));
}

TEST(page, style_background_is_used_when_the_cell_has_none) {
    Palette p = default_palette();

    style::Style s;
    s.bg_color = style::StyleColor::from_palette(4);

    Cell c;
    c.set_codepoint('a');

    RGB out;
    ASSERT_TRUE(s.bg(&c, &p, &out));
    ASSERT_TRUE(out.eql(p[4]));
}

TEST(page, no_background_at_all) {
    Palette p = default_palette();

    style::Style s;   /* bg none */
    Cell c;
    c.set_codepoint('a');

    RGB out;
    ASSERT_FALSE(s.bg(&c, &p, &out));
}

TEST(page, bg_cell_builds_a_background_only_cell) {
    style::Style pal;
    pal.bg_color = style::StyleColor::from_palette(7);

    Cell c;
    ASSERT_TRUE(pal.bg_cell(&c));
    ASSERT_TRUE(c.content_tag() == ContentTag::bg_color_palette);
    ASSERT_EQ(c.color_palette(), 7);
    ASSERT_FALSE(c.has_text());
    ASSERT_TRUE(c.has_bg_color());

    style::Style rgb;
    rgb.bg_color = style::StyleColor::from_rgb(RGB(4, 5, 6));

    Cell c2;
    ASSERT_TRUE(rgb.bg_cell(&c2));
    ASSERT_TRUE(c2.content_tag() == ContentTag::bg_color_rgb);
    ASSERT_TRUE(c2.color_rgb().eql(RGB(4, 5, 6)));

    /* A style with no background produces no cell. */
    style::Style none_set;
    Cell c3;
    ASSERT_FALSE(none_set.bg_cell(&c3));
}

/* ─── capacity and layout ────────────────────────────────────────────────── */

TEST(page, layout_regions_do_not_overlap) {
    /* Every region must be disjoint and inside the total. An overlap would
     * have two structures writing the same bytes, which no round-trip test of
     * a single structure would catch. */
    Capacity cap(80, 24);
    PageLayout l = PageLayout::init(cap);

    struct Region { size_t start, size; };
    const Region regions[] = {
        { l.rows_start,           l.rows_size },
        { l.cells_start,          l.cells_size },
        { l.styles_start,         l.styles_layout.total_size },
        { l.grapheme_alloc_start, l.grapheme_alloc_layout.total_size },
        { l.grapheme_map_start,   l.grapheme_map_layout.total_size },
        { l.string_alloc_start,   l.string_alloc_layout.total_size },
    };
    const size_t n = sizeof(regions) / sizeof(regions[0]);

    for (size_t i = 0; i < n; i++) {
        /* Inside the allocation. */
        ASSERT_TRUE(regions[i].start + regions[i].size <= l.total_size);

        for (size_t j = i + 1; j < n; j++) {
            const size_t a0 = regions[i].start, a1 = a0 + regions[i].size;
            const size_t b0 = regions[j].start, b1 = b0 + regions[j].size;
            /* Disjoint: one ends before the other begins. */
            ASSERT_TRUE(a1 <= b0 || b1 <= a0);
        }
    }
}

TEST(page, layout_regions_are_in_order) {
    /* rows, then cells, then the metadata block. */
    Capacity cap(80, 24);
    PageLayout l = PageLayout::init(cap);

    ASSERT_EQ(l.rows_start, 0);
    ASSERT_TRUE(l.cells_start >= l.rows_start + l.rows_size);
    ASSERT_TRUE(l.styles_start >= l.cells_start + l.cells_size);
    ASSERT_TRUE(l.grapheme_alloc_start >= l.styles_start);
    ASSERT_TRUE(l.string_alloc_start >= l.grapheme_map_start);
}

TEST(page, cell_array_is_cache_line_aligned) {
    /* The point of the alignment: a row's cells must never start mid-line. */
    for (CellCountInt rows = 1; rows < 40; rows++) {
        Capacity cap(80, rows);
        PageLayout l = PageLayout::init(cap);
        ASSERT_EQ(l.cells_start % CELLS_ALIGN, 0);
    }
}

TEST(page, layout_sizes_match_the_capacity) {
    Capacity cap(100, 50);
    PageLayout l = PageLayout::init(cap);

    ASSERT_EQ(l.rows_size, 50 * sizeof(Row));
    ASSERT_EQ(l.cells_size, 100 * 50 * sizeof(Cell));
    ASSERT_TRUE(l.total_size > l.cells_size);
}

TEST(page, larger_capacity_needs_more_memory) {
    const size_t small = PageLayout::init(Capacity(80, 24)).total_size;
    const size_t wider = PageLayout::init(Capacity(160, 24)).total_size;
    const size_t taller = PageLayout::init(Capacity(80, 48)).total_size;

    ASSERT_TRUE(wider > small);
    ASSERT_TRUE(taller > small);
}

TEST(page, zero_capacity_still_lays_out) {
    /* Degenerate but valid; must not divide by zero or produce overlap. */
    Capacity cap(0, 0);
    PageLayout l = PageLayout::init(cap);

    ASSERT_EQ(l.rows_size, 0);
    ASSERT_EQ(l.cells_size, 0);
    /* The metadata block still exists. */
    ASSERT_TRUE(l.total_size > 0);
}

TEST(page, grid_bytes_is_rows_plus_cells) {
    Capacity cap(80, 24);
    PageLayout l = PageLayout::init(cap);
    ASSERT_EQ(PageLayout::grid_bytes(cap), l.rows_size + l.cells_size);
}

TEST(page, max_cols_fits_within_the_grid) {
    Capacity cap(80, 24);
    const CellCountInt max = capacity_max_cols(cap);

    ASSERT_TRUE(max > 0);
    /* It must actually fit: one row header plus that many cells. */
    const size_t needed = align_forward(sizeof(Row), CELLS_ALIGN) +
                          (size_t)max * sizeof(Cell);
    ASSERT_TRUE(needed <= PageLayout::grid_bytes(cap));
}

TEST(page, adjust_cols_never_grows_the_allocation) {
    /* The contract: refitting to a new width must not need more memory, which
     * is what makes a resize in place possible. */
    Capacity cap(80, 24);
    const size_t original = PageLayout::init(cap).total_size;

    const CellCountInt widths[] = { 20, 40, 79, 81, 120, 160 };
    for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        Capacity adjusted;
        if (!capacity_adjust_cols(cap, widths[i], &adjusted)) continue;

        ASSERT_EQ(adjusted.cols, widths[i]);
        ASSERT_TRUE(adjusted.rows > 0);
        ASSERT_TRUE(PageLayout::init(adjusted).total_size <= original);
    }
}

TEST(page, adjust_to_a_wider_grid_trades_away_rows) {
    Capacity cap(80, 24);
    Capacity wider;
    ASSERT_TRUE(capacity_adjust_cols(cap, 160, &wider));

    ASSERT_EQ(wider.cols, 160);
    /* Twice the width in the same memory means fewer rows. */
    ASSERT_TRUE(wider.rows < cap.rows);
}
