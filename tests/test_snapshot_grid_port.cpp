/* Transliterated from the test blocks in Ghostty
 * src/terminal/snapshot/grid.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include <string.h>

#include <string>

#include "test_helpers.h"
#include "snapshot_fixture.hpp"
#include "../src/terminal/snapshot/grid.hpp"
#include "../src/zigstd/allocator.hpp"
#include "../src/zigstd/reader.hpp"

using namespace wisp;

namespace g = wisp::terminal::snapshot::grid;
namespace fixture = wisp::test_fixture;
namespace tp = wisp::vt::page;
namespace io = wisp::terminal::snapshot::io;

typedef zigstd::Reader Reader;
typedef zigstd::LimitedReader LimitedReader;
typedef tp::Cell TerminalCell;
typedef tp::Row TerminalRow;
typedef g::EncodedWidth W;

#ifndef WISP_SNAPSHOT_TESTDATA
#define WISP_SNAPSHOT_TESTDATA "snapshot_testdata"
#endif

static std::string fixturePath(const char *name) {
    std::string p = WISP_SNAPSHOT_TESTDATA;
    p += "/";
    p += name;
    return p;
}

/* Wisp: `var page = try TerminalPage.init(...); defer page.deinit();` */
struct PageHolder {
    tp::Page p;
    bool ok;
    explicit PageHolder(const tp::Capacity &cap) { ok = tp::Page::init(cap, &p); }
    ~PageHolder() {
        if (ok) p.deinit();
    }
};

static tp::Capacity cap(uint16_t cols, uint16_t rows) {
    tp::Capacity c;
    c.cols = cols;
    c.rows = rows;
    return c;
}

/* Wisp: `try StyleRemap.init(...); defer remap.deinit(...)` for both. */
struct Remaps {
    g::StyleRemap style;
    g::HyperlinkRemap hyperlink;
    bool ok;
    Remaps() {
        ok = g::StyleRemap::init(zigstd::testing_allocator(), &style) &&
             g::HyperlinkRemap::init(zigstd::testing_allocator(), &hyperlink);
    }
    ~Remaps() {
        style.deinit(zigstd::testing_allocator());
        hyperlink.deinit(zigstd::testing_allocator());
    }
};

static g::Cell wcell(uint8_t kind, uint32_t content) {
    g::Cell c;
    c.kind = kind;
    c.content = content;
    return c;
}

static void putRowHeader(std::string *w, g::Row r, uint16_t count) {
    w->push_back((char)r.bits());
    io::writeInt<uint16_t>(w, count);
}

static g::Row rowWidth(W width) {
    g::Row r;
    r.cell_width = width;
    return r;
}

TEST(snapshot_grid, cell_wire_layout_registry) {
    /* The wire bit positions are format constants. */
    ASSERT_EQ(0, (long)g::Cell::kind_off);
    ASSERT_EQ(2, (long)g::Cell::content_off);
    ASSERT_EQ(26, (long)g::Cell::style_id_off);
    ASSERT_EQ(42, (long)g::Cell::width_off);
    ASSERT_EQ(44, (long)g::Cell::protected_off);
    ASSERT_EQ(45, (long)g::Cell::hyperlink_off);
    ASSERT_EQ(46, (long)g::Cell::semantic_content_off);
    ASSERT_EQ(48, (long)g::Cell::hyperlink_id_off);

    /* The native cell currently matches the wire registry, which enables
     * the bulk row codec. */
    ASSERT_TRUE(g::nativeMatchesWire());
}

TEST(snapshot_grid, encoded_cell_width_transport_registry) {
    g::Cell c;
    c.kind = 1;
    c.content = 0xABCDEF;
    c.style_id = 0x1234;
    c.hyperlink_id = 0x5678;
    const uint64_t word = c.bits();
    ASSERT_TRUE(0xEFu == g::widthTruncate(W::one, word));
    ASSERT_TRUE(0xCDEFu == g::widthTruncate(W::two, word));
    ASSERT_TRUE((word & 0xFFFFFFFFull) == g::widthTruncate(W::four, word));
    ASSERT_TRUE(word == g::widthTruncate(W::eight, word));

    ASSERT_TRUE(0x00000000000003FCull == g::widthMask(W::one));
    ASSERT_TRUE(0x000000000003FFFCull == g::widthMask(W::two));
    ASSERT_TRUE(0x00000000FFFFFFFFull == g::widthMask(W::four));
    ASSERT_TRUE(0xFFFFFFFFFFFFFFFFull == g::widthMask(W::eight));

    struct Case {
        g::Cell cell;
        W width;
    };
    Case cases[12];
    for (size_t i = 0; i < 12; i++) cases[i].cell = g::Cell();
    cases[0].width = W::one;
    cases[1].cell.content = 0xFF; cases[1].width = W::one;
    cases[2].cell.content = 0x100; cases[2].width = W::two;
    cases[3].cell.content = 0xFFFF; cases[3].width = W::two;
    cases[4].cell.content = 0x10000; cases[4].width = W::four;
    cases[5].cell.kind = 2; cases[5].cell.content = 7; cases[5].width = W::four;
    cases[6].cell.content = 'a'; cases[6].cell.style_id = 63; cases[6].width = W::four;
    cases[7].cell.content = 'a'; cases[7].cell.style_id = 64; cases[7].width = W::eight;
    cases[8].cell.width = 1; cases[8].cell.content = 'a'; cases[8].width = W::eight;
    cases[9].cell.protected_ = true; cases[9].width = W::eight;
    cases[10].cell.semantic_content = 1; cases[10].width = W::eight;
    cases[11].cell.hyperlink = true; cases[11].cell.hyperlink_id = 1; cases[11].width = W::eight;

    for (size_t i = 0; i < 12; i++) {
        const uint64_t case_word = cases[i].cell.bits();
        ASSERT_TRUE(cases[i].width == g::widthSelect(case_word));
        ASSERT_TRUE(case_word == g::widthExtend(cases[i].width, g::widthTruncate(cases[i].width, case_word)));
    }
}

TEST(snapshot_grid, grid_golden_encoding_and_decoding) {
    tp::Capacity capacity = cap(3, 4);
    capacity.styles = 0;
    capacity.hyperlink_bytes = 0;
    capacity.grapheme_bytes = 64;
    capacity.string_bytes = 0;
    PageHolder source(capacity);
    ASSERT_TRUE(source.ok);
    tp::Page &src = source.p;

    /* The first row covers semantic metadata, a wide-cell pair, and a
     * multi-codepoint grapheme. */
    tp::Page::RowAndCell wide = src.getRowAndCell(0, 0);
    wide.row->setSemanticPrompt(TerminalRow::SemanticPrompt::prompt);
    *wide.cell = TerminalCell::init('A');
    wide.cell->setWide(TerminalCell::Wide::wide);
    wide.cell->setProtected(true);
    wide.cell->setSemanticContent(TerminalCell::SemanticContent::prompt);

    tp::Page::RowAndCell tail = src.getRowAndCell(1, 0);
    tail.cell->setWide(TerminalCell::Wide::spacer_tail);
    tail.cell->setSemanticContent(TerminalCell::SemanticContent::input);

    tp::Page::RowAndCell grapheme = src.getRowAndCell(2, 0);
    *grapheme.cell = TerminalCell::init('x');
    const uint32_t gcps[] = { 0x0301, 0x0302 };
    ASSERT_TRUE(src.setGraphemes(grapheme.row, grapheme.cell, gcps, 2) == tp::PageError::none);

    /* The second row covers both background-color kinds and a wrapped
     * spacer head with the remaining row semantic value. */
    tp::Page::RowAndCell palette = src.getRowAndCell(0, 1);
    palette.cell->setContentTag(TerminalCell::ContentTag::bg_color_palette);
    palette.cell->setContentColorPalette(7);

    tp::Page::RowAndCell rgb = src.getRowAndCell(1, 1);
    rgb.cell->setContentTag(TerminalCell::ContentTag::bg_color_rgb);
    TerminalCell::RGB c;
    c.r = 0xaa;
    c.g = 0xbb;
    c.b = 0xcc;
    rgb.cell->setContentColorRgb(c);
    rgb.cell->setProtected(true);

    tp::Page::RowAndCell head = src.getRowAndCell(2, 1);
    head.cell->setWide(TerminalCell::Wide::spacer_head);
    head.row->setWrap(true);
    head.row->setWrapContinuation(true);
    head.row->setSemanticPrompt(TerminalRow::SemanticPrompt::prompt_continuation);

    /* The third row is bare ASCII text with an interior blank, which uses
     * the one-byte encoded cell width. */
    *src.getRowAndCell(0, 2).cell = TerminalCell::init('h');
    *src.getRowAndCell(2, 2).cell = TerminalCell::init('i');

    /* The fourth row needs the two-byte width for a codepoint above U+00FF. */
    *src.getRowAndCell(0, 3).cell = TerminalCell::init(0x0416);
    *src.getRowAndCell(1, 3).cell = TerminalCell::init('!');

    std::string encoded;
    ASSERT_TRUE(g::encode(&src, &encoded) == g::EncodeError::none);

    const std::string path = fixturePath("grid-v1.hex");
    std::string text, golden;
    ASSERT_TRUE(fixture::readFile(path.c_str(), &text));
    ASSERT_TRUE(fixture::parse(text.data(), text.size(), &golden));
    ASSERT_TRUE(fixture::expectEqual(fixture::Kind::bytes, path.c_str(),
                                     "snapshot_fixture-grid-v1.hex", golden, encoded));

    PageHolder destination(capacity);
    ASSERT_TRUE(destination.ok);
    tp::Page &dst = destination.p;
    Remaps remaps;
    ASSERT_TRUE(remaps.ok);

    /* Decode the checked-in reference through a one-byte reader buffer. */
    Reader fixture_reader = Reader::fixed(golden.data(), golden.size());
    uint8_t read_buffer[1];
    LimitedReader limited = LimitedReader::limited(&fixture_reader, read_buffer, sizeof read_buffer);
    ASSERT_TRUE(g::decode(&dst, &limited, &remaps.style, &remaps.hyperlink) == g::DecodeError::none);
    ASSERT_TRUE(dst.verifyIntegrity() == tp::PageError::none);

    tp::Page::RowAndCell dw = dst.getRowAndCell(0, 0);
    ASSERT_EQ('A', (long)dw.cell->codepoint());
    ASSERT_TRUE(TerminalCell::Wide::wide == dw.cell->wide());
    ASSERT_TRUE(dw.cell->protected_());
    ASSERT_TRUE(TerminalCell::SemanticContent::prompt == dw.cell->semantic_content());
    ASSERT_TRUE(TerminalRow::SemanticPrompt::prompt == dw.row->semantic_prompt());

    tp::Page::RowAndCell dt = dst.getRowAndCell(1, 0);
    ASSERT_TRUE(TerminalCell::Wide::spacer_tail == dt.cell->wide());
    ASSERT_TRUE(TerminalCell::SemanticContent::input == dt.cell->semantic_content());

    tp::Page::RowAndCell dg = dst.getRowAndCell(2, 0);
    size_t glen = 0;
    const uint32_t *gl = dst.lookupGrapheme(dg.cell, &glen);
    ASSERT_TRUE(gl != nullptr);
    ASSERT_EQ(2, (long)glen);
    ASSERT_EQ(0x0301, (long)gl[0]);
    ASSERT_EQ(0x0302, (long)gl[1]);

    tp::Page::RowAndCell dp = dst.getRowAndCell(0, 1);
    ASSERT_TRUE(TerminalCell::ContentTag::bg_color_palette == dp.cell->content_tag());
    ASSERT_EQ(7, (long)dp.cell->contentColorPalette());

    tp::Page::RowAndCell dr = dst.getRowAndCell(1, 1);
    ASSERT_TRUE(TerminalCell::ContentTag::bg_color_rgb == dr.cell->content_tag());
    const TerminalCell::RGB drgb = dr.cell->contentColorRgb();
    ASSERT_TRUE(drgb.r == 0xaa && drgb.g == 0xbb && drgb.b == 0xcc);
    ASSERT_TRUE(dr.cell->protected_());

    tp::Page::RowAndCell dh = dst.getRowAndCell(2, 1);
    ASSERT_TRUE(TerminalCell::Wide::spacer_head == dh.cell->wide());
    ASSERT_TRUE(dh.row->wrap());
    ASSERT_TRUE(dh.row->wrap_continuation());
    ASSERT_TRUE(TerminalRow::SemanticPrompt::prompt_continuation == dh.row->semantic_prompt());

    ASSERT_EQ('h', (long)dst.getRowAndCell(0, 2).cell->codepoint());
    ASSERT_TRUE(dst.getRowAndCell(1, 2).cell->isZero());
    ASSERT_EQ('i', (long)dst.getRowAndCell(2, 2).cell->codepoint());
    ASSERT_EQ(0x0416, (long)dst.getRowAndCell(0, 3).cell->codepoint());
    ASSERT_EQ('!', (long)dst.getRowAndCell(1, 3).cell->codepoint());

    /* A re-encode proves the decoded native page retains every wire field. */
    std::string reencoded;
    ASSERT_TRUE(g::encode(&dst, &reencoded) == g::EncodeError::none);
    ASSERT_TRUE(golden == reencoded);
}

TEST(snapshot_grid, grid_elides_trailing_default_cells) {
    PageHolder holder(cap(80, 3));
    ASSERT_TRUE(holder.ok);
    tp::Page &page = holder.p;

    /* Row 0 is fully default. Row 1 has content in columns zero and two.
     * Row 2 has one protected-only cell at column four. */
    *page.getRowAndCell(2, 1).cell = TerminalCell::init('b');
    *page.getRowAndCell(0, 1).cell = TerminalCell::init('a');
    page.getRowAndCell(4, 2).cell->setProtected(true);

    std::string encoded;
    ASSERT_TRUE(g::encode(&page, &encoded) == g::EncodeError::none);

    /* 3 bytes per row header and cells only through the last non-default
     * cell, followed by an empty grapheme section. The text row uses the
     * one-byte width while the protected flag forces the full width. */
    ASSERT_EQ((long)(3 + (3 + 3 * 1) + (3 + 5 * 8) + 4), (long)encoded.size());

    PageHolder dholder(cap(80, 3));
    ASSERT_TRUE(dholder.ok);
    tp::Page &destination = dholder.p;
    Remaps remaps;
    ASSERT_TRUE(remaps.ok);

    Reader reader = Reader::fixed(encoded.data(), encoded.size());
    ASSERT_TRUE(g::decode(&destination, &reader, &remaps.style, &remaps.hyperlink) == g::DecodeError::none);
    ASSERT_TRUE(destination.verifyIntegrity() == tp::PageError::none);

    ASSERT_EQ('a', (long)destination.getRowAndCell(0, 1).cell->codepoint());
    ASSERT_EQ('b', (long)destination.getRowAndCell(2, 1).cell->codepoint());
    ASSERT_TRUE(destination.getRowAndCell(4, 2).cell->protected_());
    ASSERT_TRUE(destination.getRowAndCell(79, 1).cell->isZero());
}

TEST(snapshot_grid, grid_rejects_a_row_cell_count_above_the_column_count) {
    PageHolder holder(cap(2, 1));
    ASSERT_TRUE(holder.ok);
    Remaps remaps;
    ASSERT_TRUE(remaps.ok);

    std::string payload;
    putRowHeader(&payload, g::Row(), 3);
    for (size_t i = 0; i < 3; i++) io::writeInt<uint64_t>(&payload, 0);
    io::writeInt<uint32_t>(&payload, 0);

    Reader reader = Reader::fixed(payload.data(), payload.size());
    ASSERT_TRUE(g::decode(&holder.p, &reader, &remaps.style, &remaps.hyperlink) ==
                g::DecodeError::InvalidRowCellCount);
}

TEST(snapshot_grid, grid_normalizes_incomplete_wide_cells) {
    /* One column puts the wide cell at the row end. Two columns put an
     * ordinary narrow cell after it. Neither case supplies a spacer tail. */
    const uint16_t column_cases[] = { 1, 2 };
    for (size_t ci = 0; ci < 2; ci++) {
        const uint16_t columns = column_cases[ci];

        /* Craft the malformed row directly on the wire. Decoding keeps its
         * content but makes the incomplete wide cell narrow. */
        std::string payload;
        putRowHeader(&payload, rowWidth(W::eight), columns);
        g::Cell w = wcell(0, 'A');
        w.width = 1; /* wide */
        io::writeInt<uint64_t>(&payload, w.bits());
        if (columns > 1) io::writeInt<uint64_t>(&payload, wcell(0, 'B').bits());
        io::writeInt<uint32_t>(&payload, 0);

        PageHolder holder(cap(columns, 1));
        ASSERT_TRUE(holder.ok);
        tp::Page &destination = holder.p;
        Remaps remaps;
        ASSERT_TRUE(remaps.ok);

        Reader reader = Reader::fixed(payload.data(), payload.size());
        ASSERT_TRUE(g::decode(&destination, &reader, &remaps.style, &remaps.hyperlink) == g::DecodeError::none);
        ASSERT_TRUE(destination.verifyIntegrity() == tp::PageError::none);

        ASSERT_EQ('A', (long)destination.getRowAndCell(0, 0).cell->codepoint());
        ASSERT_TRUE(TerminalCell::Wide::narrow == destination.getRowAndCell(0, 0).cell->wide());
        if (columns > 1) {
            const TerminalCell *second = destination.getRowAndCell(1, 0).cell;
            ASSERT_EQ('B', (long)second->codepoint());
            ASSERT_TRUE(TerminalCell::Wide::narrow == second->wide());
        }
    }

    /* A wide marker whose spacer tail was elided by a short cell count is
     * also normalized, and the trailing cells stay default. */
    PageHolder holder(cap(4, 1));
    ASSERT_TRUE(holder.ok);
    tp::Page &page = holder.p;
    Remaps remaps;
    ASSERT_TRUE(remaps.ok);

    std::string payload;
    putRowHeader(&payload, rowWidth(W::eight), 1);
    g::Cell w = wcell(0, 'W');
    w.width = 1; /* wide */
    io::writeInt<uint64_t>(&payload, w.bits());
    io::writeInt<uint32_t>(&payload, 0);

    Reader reader = Reader::fixed(payload.data(), payload.size());
    ASSERT_TRUE(g::decode(&page, &reader, &remaps.style, &remaps.hyperlink) == g::DecodeError::none);
    ASSERT_TRUE(page.verifyIntegrity() == tp::PageError::none);
    ASSERT_TRUE(TerminalCell::Wide::narrow == page.getRowAndCell(0, 0).cell->wide());
    ASSERT_TRUE(page.getRowAndCell(1, 0).cell->isZero());
}

TEST(snapshot_grid, grid_normalizes_reserved_cell_values) {
    PageHolder holder(cap(3, 1));
    ASSERT_TRUE(holder.ok);
    tp::Page &page = holder.p;
    Remaps remaps;
    ASSERT_TRUE(remaps.ok);

    std::string payload;

    /* Reserved row flag bits six and seven are ignored while the unknown
     * semantic-prompt value degrades to none and wrap survives. Bits four
     * and five select the full encoded cell width. */
    payload.push_back((char)0xFD);
    io::writeInt<uint16_t>(&payload, 3);

    /* A surrogate codepoint with reserved semantic content 3. */
    g::Cell c0 = wcell(0, 0xD800);
    c0.semantic_content = 3;
    io::writeInt<uint64_t>(&payload, c0.bits());

    /* Reserved palette content bits do not obscure the palette index. */
    io::writeInt<uint64_t>(&payload, wcell(2, 0xFFFF07).bits());

    /* An unknown style reference degrades to the default style, and an
     * unknown hyperlink reference leaves the cell unlinked. */
    g::Cell c2 = wcell(0, 'x');
    c2.style_id = 5;
    c2.hyperlink = true;
    c2.hyperlink_id = 9;
    io::writeInt<uint64_t>(&payload, c2.bits());
    io::writeInt<uint32_t>(&payload, 0);

    Reader reader = Reader::fixed(payload.data(), payload.size());
    ASSERT_TRUE(g::decode(&page, &reader, &remaps.style, &remaps.hyperlink) == g::DecodeError::none);
    ASSERT_TRUE(page.verifyIntegrity() == tp::PageError::none);

    tp::Page::RowAndCell first = page.getRowAndCell(0, 0);
    ASSERT_TRUE(first.row->wrap());
    ASSERT_TRUE(TerminalRow::SemanticPrompt::none == first.row->semantic_prompt());
    ASSERT_EQ(0xFFFD, (long)first.cell->codepoint());
    ASSERT_TRUE(TerminalCell::SemanticContent::output == first.cell->semantic_content());

    const TerminalCell *second = page.getRowAndCell(1, 0).cell;
    ASSERT_TRUE(TerminalCell::ContentTag::bg_color_palette == second->content_tag());
    ASSERT_EQ(7, (long)second->contentColorPalette());

    const TerminalCell *third = page.getRowAndCell(2, 0).cell;
    ASSERT_EQ('x', (long)third->codepoint());
    ASSERT_EQ(0, (long)third->style_id());
    ASSERT_FALSE(third->hyperlink());
    wisp::vt::hyperlink::Id link = 0;
    ASSERT_FALSE(page.lookupHyperlink(third, &link));
}

TEST(snapshot_grid, grid_drops_undeliverable_grapheme_entries) {
    tp::Capacity c = cap(4, 1);
    c.grapheme_bytes = 64;
    PageHolder holder(c);
    ASSERT_TRUE(holder.ok);
    tp::Page &page = holder.p;
    Remaps remaps;
    ASSERT_TRUE(remaps.ok);

    std::string payload;
    putRowHeader(&payload, rowWidth(W::eight), 4);
    /* A kind 1 cell that receives a valid entry. */
    io::writeInt<uint64_t>(&payload, wcell(1, 'x').bits());
    /* A kind 1 cell without an entry decodes as a plain codepoint. */
    io::writeInt<uint64_t>(&payload, wcell(1, 'y').bits());
    /* A background cell cannot carry a suffix. */
    io::writeInt<uint64_t>(&payload, wcell(2, 7).bits());
    /* An empty codepoint cannot carry a suffix. */
    io::writeInt<uint64_t>(&payload, g::Cell().bits());

    io::writeInt<uint32_t>(&payload, 5);
    /* Valid entry with one invalid scalar dropped from within it. */
    io::writeInt<uint16_t>(&payload, 0);
    io::writeInt<uint16_t>(&payload, 0);
    io::writeInt<uint16_t>(&payload, 4);
    io::writeInt<uint32_t>(&payload, 0x0301);
    io::writeInt<uint32_t>(&payload, 0);
    io::writeInt<uint32_t>(&payload, 0xD800);
    io::writeInt<uint32_t>(&payload, 0x0302);
    /* Duplicate entry for the same cell is consumed and dropped. */
    io::writeInt<uint16_t>(&payload, 0);
    io::writeInt<uint16_t>(&payload, 0);
    io::writeInt<uint16_t>(&payload, 1);
    io::writeInt<uint32_t>(&payload, 0x0303);
    /* Entry for the background cell is consumed and dropped. */
    io::writeInt<uint16_t>(&payload, 0);
    io::writeInt<uint16_t>(&payload, 2);
    io::writeInt<uint16_t>(&payload, 1);
    io::writeInt<uint32_t>(&payload, 0x0301);
    /* Entry for the empty cell is consumed and dropped. */
    io::writeInt<uint16_t>(&payload, 0);
    io::writeInt<uint16_t>(&payload, 3);
    io::writeInt<uint16_t>(&payload, 1);
    io::writeInt<uint32_t>(&payload, 0x0301);
    /* Entry outside the grid is consumed and dropped. */
    io::writeInt<uint16_t>(&payload, 7);
    io::writeInt<uint16_t>(&payload, 0);
    io::writeInt<uint16_t>(&payload, 1);
    io::writeInt<uint32_t>(&payload, 0x0301);

    Reader reader = Reader::fixed(payload.data(), payload.size());
    ASSERT_TRUE(g::decode(&page, &reader, &remaps.style, &remaps.hyperlink) == g::DecodeError::none);
    ASSERT_TRUE(page.verifyIntegrity() == tp::PageError::none);

    size_t glen = 0;
    const uint32_t *gl = page.lookupGrapheme(page.getRowAndCell(0, 0).cell, &glen);
    ASSERT_TRUE(gl != nullptr);
    ASSERT_EQ(2, (long)glen);
    ASSERT_EQ(0x0301, (long)gl[0]);
    ASSERT_EQ(0x0302, (long)gl[1]);

    const TerminalCell *second = page.getRowAndCell(1, 0).cell;
    ASSERT_EQ('y', (long)second->codepoint());
    ASSERT_FALSE(second->hasGrapheme());
    ASSERT_FALSE(page.getRowAndCell(2, 0).cell->hasGrapheme());
    ASSERT_FALSE(page.getRowAndCell(3, 0).cell->hasGrapheme());
}

TEST(snapshot_grid, grid_drops_a_complete_grapheme_when_capacity_fails_mid_cluster) {
    tp::Capacity c = cap(4, 1);
    c.grapheme_bytes = 512;
    PageHolder holder(c);
    ASSERT_TRUE(holder.ok);
    tp::Page &page = holder.p;
    Remaps remaps;
    ASSERT_TRUE(remaps.ok);

    std::string payload;
    putRowHeader(&payload, rowWidth(W::eight), 4);
    for (uint32_t i = 0; i < 4; i++) io::writeInt<uint64_t>(&payload, wcell(1, 'x' + i).bits());

    /* BitmapAllocator rounds this capacity to 64 four-codepoint chunks. The
     * first three cells consume 34 chunks. On the fourth cell, appending the
     * 61st suffix needs 16 new chunks while the old 15-chunk slice is still
     * live, exceeding capacity and forcing the complete suffix to be dropped. */
    io::writeInt<uint32_t>(&payload, 4);
    const uint16_t counts[] = { 64, 64, 8, 61 };
    for (uint16_t x = 0; x < 4; x++) {
        io::writeInt<uint16_t>(&payload, 0);
        io::writeInt<uint16_t>(&payload, x);
        io::writeInt<uint16_t>(&payload, counts[x]);
        for (uint32_t i = 0; i < counts[x]; i++) io::writeInt<uint32_t>(&payload, 0x0300 + i);
    }

    Reader reader = Reader::fixed(payload.data(), payload.size());
    ASSERT_TRUE(g::decode(&page, &reader, &remaps.style, &remaps.hyperlink) == g::DecodeError::none);
    ASSERT_TRUE(page.verifyIntegrity() == tp::PageError::none);

    tp::Page::RowAndCell cell = page.getRowAndCell(3, 0);
    ASSERT_EQ('x' + 3, (long)cell.cell->codepoint());
    ASSERT_FALSE(cell.cell->hasGrapheme());
    ASSERT_TRUE(cell.row->grapheme());
    ASSERT_EQ(3, (long)page.graphemeCount());
}

TEST(snapshot_grid, grid_encodes_rows_at_their_narrowest_width) {
    tp::Capacity c = cap(2, 4);
    c.styles = 8;
    PageHolder holder(c);
    ASSERT_TRUE(holder.ok);
    tp::Page &page = holder.p;

    /* Width zero: bare Latin-1 text. */
    *page.getRowAndCell(0, 0).cell = TerminalCell::init('A');
    *page.getRowAndCell(1, 0).cell = TerminalCell::init(0xFF);

    /* Width one: any BMP codepoint. */
    *page.getRowAndCell(0, 1).cell = TerminalCell::init(0x0100);

    /* Width two: a small style ID and a background color. */
    wisp::vt::style::Style bold;
    bold.flags.bold = true;
    wisp::vt::style::Id style_id = 0;
    ASSERT_TRUE(page.styles.add((const void *)page.memory, bold, &style_id) ==
                wisp::vt::ref_counted_set::AddError::none);
    ASSERT_TRUE(style_id <= 63);
    tp::Page::RowAndCell styled = page.getRowAndCell(0, 2);
    *styled.cell = TerminalCell::init('s');
    styled.cell->setStyleId(style_id);
    styled.row->setStyled(true);
    tp::Page::RowAndCell bg = page.getRowAndCell(1, 2);
    bg.cell->setContentTag(TerminalCell::ContentTag::bg_color_palette);
    bg.cell->setContentColorPalette(7);

    /* Width three: a protected cell. */
    page.getRowAndCell(0, 3).cell->setProtected(true);

    std::string bytes;
    ASSERT_TRUE(g::encode(&page, &bytes) == g::EncodeError::none);
    ASSERT_EQ((long)((3 + 2 * 1) + (3 + 1 * 2) + (3 + 2 * 4) + (3 + 1 * 8) + 4), (long)bytes.size());

    /* Each row header carries the expected width bits. */
    ASSERT_EQ(0 << 4, (long)((uint8_t)bytes[0] & 0x30));
    ASSERT_EQ(1 << 4, (long)((uint8_t)bytes[5] & 0x30));
    ASSERT_EQ(2 << 4, (long)((uint8_t)bytes[10] & 0x30));
    ASSERT_EQ(3 << 4, (long)((uint8_t)bytes[21] & 0x30));

    PageHolder dholder(c);
    ASSERT_TRUE(dholder.ok);
    tp::Page &destination = dholder.p;
    Remaps remaps;
    ASSERT_TRUE(remaps.ok);
    wisp::vt::style::Id native_style = 0;
    ASSERT_TRUE(destination.styles.add((const void *)destination.memory, bold, &native_style) ==
                wisp::vt::ref_counted_set::AddError::none);
    remaps.style.put(style_id, native_style);

    Reader reader = Reader::fixed(bytes.data(), bytes.size());
    ASSERT_TRUE(g::decode(&destination, &reader, &remaps.style, &remaps.hyperlink) == g::DecodeError::none);
    ASSERT_TRUE(destination.verifyIntegrity() == tp::PageError::none);

    ASSERT_EQ('A', (long)destination.getRowAndCell(0, 0).cell->codepoint());
    ASSERT_EQ(0xFF, (long)destination.getRowAndCell(1, 0).cell->codepoint());
    ASSERT_EQ(0x0100, (long)destination.getRowAndCell(0, 1).cell->codepoint());
    tp::Page::RowAndCell ds = destination.getRowAndCell(0, 2);
    ASSERT_EQ('s', (long)ds.cell->codepoint());
    ASSERT_EQ((long)native_style, (long)ds.cell->style_id());
    ASSERT_TRUE(ds.row->styled());
    ASSERT_EQ(7, (long)destination.getRowAndCell(1, 2).cell->contentColorPalette());
    ASSERT_TRUE(destination.getRowAndCell(0, 3).cell->protected_());
}

TEST(snapshot_grid, grid_decodes_non_canonical_cell_widths) {
    PageHolder holder(cap(2, 1));
    ASSERT_TRUE(holder.ok);
    tp::Page &page = holder.p;
    Remaps remaps;
    ASSERT_TRUE(remaps.ok);

    /* A bare ASCII row encoded at the full width is wasteful but valid. */
    std::string payload;
    putRowHeader(&payload, rowWidth(W::eight), 2);
    io::writeInt<uint64_t>(&payload, wcell(0, 'o').bits());
    io::writeInt<uint64_t>(&payload, wcell(0, 'k').bits());
    io::writeInt<uint32_t>(&payload, 0);

    Reader reader = Reader::fixed(payload.data(), payload.size());
    ASSERT_TRUE(g::decode(&page, &reader, &remaps.style, &remaps.hyperlink) == g::DecodeError::none);
    ASSERT_TRUE(page.verifyIntegrity() == tp::PageError::none);
    ASSERT_EQ('o', (long)page.getRowAndCell(0, 0).cell->codepoint());
    ASSERT_EQ('k', (long)page.getRowAndCell(1, 0).cell->codepoint());

    /* Re-encoding canonicalizes the row back to the one-byte width. */
    std::string reencoded;
    ASSERT_TRUE(g::encode(&page, &reencoded) == g::EncodeError::none);
    ASSERT_EQ(3 + 2 + 4, (long)reencoded.size());
}

TEST(snapshot_grid, grid_normalizes_surrogates_in_two_byte_cells) {
    PageHolder holder(cap(2, 1));
    ASSERT_TRUE(holder.ok);
    tp::Page &page = holder.p;
    Remaps remaps;
    ASSERT_TRUE(remaps.ok);

    std::string payload;
    putRowHeader(&payload, rowWidth(W::two), 2);
    io::writeInt<uint16_t>(&payload, 0xD800);
    io::writeInt<uint16_t>(&payload, 0x0416);
    io::writeInt<uint32_t>(&payload, 0);

    Reader reader = Reader::fixed(payload.data(), payload.size());
    ASSERT_TRUE(g::decode(&page, &reader, &remaps.style, &remaps.hyperlink) == g::DecodeError::none);
    ASSERT_TRUE(page.verifyIntegrity() == tp::PageError::none);
    ASSERT_EQ(0xFFFD, (long)page.getRowAndCell(0, 0).cell->codepoint());
    ASSERT_EQ(0x0416, (long)page.getRowAndCell(1, 0).cell->codepoint());
}

TEST(snapshot_grid, grid_four_byte_cells_run_full_normalization) {
    tp::Capacity c = cap(3, 1);
    c.styles = 8;
    PageHolder holder(c);
    ASSERT_TRUE(holder.ok);
    tp::Page &page = holder.p;
    Remaps remaps;
    ASSERT_TRUE(remaps.ok);

    std::string payload;
    putRowHeader(&payload, rowWidth(W::four), 3);

    /* The Kitty placeholder does not fit two-byte cells but fits here
     * and must still derive the native row hint. */
    io::writeInt<uint32_t>(&payload, (uint32_t)wcell(0, g::kitty_virtual_placeholder).bits());

    /* An unknown small style reference degrades to the default style. */
    g::Cell s = wcell(0, 'x');
    s.style_id = 63;
    io::writeInt<uint32_t>(&payload, (uint32_t)s.bits());

    /* Reserved palette content bits are cleared at this width too. */
    io::writeInt<uint32_t>(&payload, (uint32_t)wcell(2, 0xFFFF07).bits());
    io::writeInt<uint32_t>(&payload, 0);

    Reader reader = Reader::fixed(payload.data(), payload.size());
    ASSERT_TRUE(g::decode(&page, &reader, &remaps.style, &remaps.hyperlink) == g::DecodeError::none);
    ASSERT_TRUE(page.verifyIntegrity() == tp::PageError::none);

    tp::Page::RowAndCell first = page.getRowAndCell(0, 0);
    ASSERT_EQ((long)g::kitty_virtual_placeholder, (long)first.cell->codepoint());
    ASSERT_TRUE(first.row->kitty_virtual_placeholder());

    const TerminalCell *second = page.getRowAndCell(1, 0).cell;
    ASSERT_EQ('x', (long)second->codepoint());
    ASSERT_EQ(0, (long)second->style_id());

    const TerminalCell *third = page.getRowAndCell(2, 0).cell;
    ASSERT_TRUE(TerminalCell::ContentTag::bg_color_palette == third->content_tag());
    ASSERT_EQ(7, (long)third->contentColorPalette());
}

TEST(snapshot_grid, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
