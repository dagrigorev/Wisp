/* Transliterated from the test blocks in Ghostty
 * src/terminal/snapshot/page.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Wisp: upstream compares the sparse-page golden with fixture kind `.page`,
 * which only changes how a mismatching candidate is annotated. That kind is
 * not ported (see snapshot_fixture.hpp), so the comparison uses `.bytes`;
 * the bytes compared are identical.
 */

#include <string.h>

#include <string>

#include "test_helpers.h"
#include "snapshot_fixture.hpp"
#include "../src/terminal/snapshot/page.hpp"
#include "../src/zigstd/allocator.hpp"
#include "../src/zigstd/reader.hpp"

using namespace wisp;

namespace sp = wisp::terminal::snapshot::page;
namespace grid = wisp::terminal::snapshot::grid;
namespace hyperlink = wisp::terminal::snapshot::hyperlink;
namespace record = wisp::terminal::snapshot::record;
namespace style = wisp::terminal::snapshot::style;
namespace io = wisp::terminal::snapshot::io;
namespace fixture = wisp::test_fixture;
namespace tp = wisp::vt::page;
namespace th = wisp::vt::hyperlink;
namespace ts = wisp::vt::style;

typedef zigstd::Reader Reader;
typedef zigstd::LimitedReader LimitedReader;
typedef zigstd::ReadError ReadError;
typedef sp::Header Header;
typedef sp::Error Error;
typedef tp::Cell TerminalCell;
typedef tp::Row TerminalRow;
typedef ts::Style TerminalStyle;
typedef ts::Id TerminalStyleId;
typedef th::Id TerminalHyperlinkId;
typedef th::Hyperlink TerminalHyperlink;
typedef wisp::vt::ref_counted_set::AddError AddError;

#ifndef WISP_SNAPSHOT_TESTDATA
#define WISP_SNAPSHOT_TESTDATA "snapshot_testdata"
#endif

static zigstd::Allocator talloc() { return zigstd::testing_allocator(); }

static std::string fixturePath(const char *name) {
    std::string p = WISP_SNAPSHOT_TESTDATA;
    p += "/";
    p += name;
    return p;
}

static bool loadFixture(const char *name, std::string *out) {
    std::string text;
    if (!fixture::readFile(fixturePath(name).c_str(), &text)) return false;
    return fixture::parse(text.data(), text.size(), out);
}

/* Wisp: `var page = try TerminalPage.init(cap); defer page.deinit();` */
struct PageHolder {
    tp::Page p;
    bool ok = false;
    PageHolder() {}
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

static tp::Capacity fullCap(uint16_t cols, uint16_t rows, uint16_t styles, uint16_t hyperlink_bytes,
                            uint32_t grapheme_bytes, uint32_t string_bytes) {
    tp::Capacity c = cap(cols, rows);
    c.styles = styles;
    c.hyperlink_bytes = hyperlink_bytes;
    c.grapheme_bytes = grapheme_bytes;
    c.string_bytes = string_bytes;
    return c;
}

static Header header(uint16_t columns, uint16_t rows, uint16_t style_count, uint16_t hyperlink_count,
                     uint16_t style_capacity, uint16_t hyperlink_capacity_bytes,
                     uint32_t grapheme_capacity_bytes, uint32_t string_capacity_bytes) {
    Header h;
    h.columns = columns;
    h.rows = rows;
    h.style_count = style_count;
    h.hyperlink_count = hyperlink_count;
    h.style_capacity = style_capacity;
    h.hyperlink_capacity_bytes = hyperlink_capacity_bytes;
    h.grapheme_capacity_bytes = grapheme_capacity_bytes;
    h.string_capacity_bytes = string_capacity_bytes;
    return h;
}

static TerminalStyle boldStyle() {
    TerminalStyle s;
    s.flags.bold = true;
    return s;
}

static TerminalStyle italicStyle() {
    TerminalStyle s;
    s.flags.italic = true;
    return s;
}

static TerminalStyle paletteBg(uint8_t idx) {
    TerminalStyle s;
    s.bg_color = TerminalStyle::Color::makePalette(idx);
    return s;
}

static TerminalHyperlink explicitLink(const char *id, const char *uri) {
    TerminalHyperlink h;
    h.id = TerminalHyperlink::Id::makeExplicit((const uint8_t *)id, strlen(id));
    h.uri = (const uint8_t *)uri;
    h.uri_len = strlen(uri);
    return h;
}

static TerminalHyperlink implicitLink(uint32_t id, const char *uri) {
    TerminalHyperlink h;
    h.id = TerminalHyperlink::Id::makeImplicit(id);
    h.uri = (const uint8_t *)uri;
    h.uri_len = strlen(uri);
    return h;
}

static TerminalStyleId addStyle(tp::Page *page, const TerminalStyle &s) {
    TerminalStyleId id = 0;
    AddError e = page->styles.add((const void *)page->memory, s, &id);
    return e == AddError::none ? id : 0;
}

static TerminalHyperlinkId insertLink(tp::Page *page, const TerminalHyperlink &h) {
    TerminalHyperlinkId id = 0;
    return page->insertHyperlink(h, &id) == tp::PageError::none ? id : 0;
}

static bool lookupLink(const tp::Page &page, const TerminalCell *cell, TerminalHyperlinkId *out) {
    return page.lookupHyperlink(cell, out);
}

TEST(snapshot_page, PAGE_header_golden_encoding_and_decoding) {
    const Header h = header(0x0102, 0x0304, 0x0506, 0x0708, 0x090a, 0x0b0c, 0x0d0e0f10, 0x11121314);
    std::string buf;
    h.encode(&buf);

    std::string golden;
    ASSERT_TRUE(loadFixture("page-header-v1.hex", &golden));
    const std::string path = fixturePath("page-header-v1.hex");
    ASSERT_TRUE(fixture::expectEqual(fixture::Kind::bytes, path.c_str(),
                                     "snapshot_fixture-page-header-v1.hex", golden, buf));

    Reader source = Reader::fixed(golden.data(), golden.size());
    uint8_t read_buf[1];
    LimitedReader limited = LimitedReader::limited(&source, read_buf, sizeof read_buf);
    Header decoded;
    ReadError rerr = ReadError::none;
    ASSERT_TRUE(Header::decode(&limited, &decoded, &rerr));
    ASSERT_TRUE(h == decoded);
}

TEST(snapshot_page, reject_every_truncation) {
    std::string golden;
    ASSERT_TRUE(loadFixture("page-header-v1.hex", &golden));
    for (size_t len = 0; len < Header::len; len++) {
        Reader reader = Reader::fixed(golden.data(), len);
        Header out;
        ReadError rerr = ReadError::none;
        ASSERT_FALSE(Header::decode(&reader, &out, &rerr));
        ASSERT_TRUE(rerr == ReadError::EndOfStream);
    }
}

TEST(snapshot_page, framed_PAGE_encode_and_decode_a_sparse_native_page) {
    PageHolder holder(fullCap(3, 2, 8, 512, 128, 256));
    ASSERT_TRUE(holder.ok);
    tp::Page &page = holder.p;
    const void *base = page.memory;

    /* Create holes in both source tables so the encoded IDs exercise sparse
     * table remapping rather than coincidentally matching decoded IDs. */
    const TerminalStyleId style_a = addStyle(&page, boldStyle());
    const TerminalStyleId dead_style = addStyle(&page, italicStyle());
    const TerminalStyleId style_b = addStyle(&page, paletteBg(42));
    ASSERT_TRUE(style_a != 0 && dead_style != 0 && style_b != 0);
    page.styles.release(base, dead_style);

    tp::Page::RowAndCell first = page.getRowAndCell(0, 0);
    first.cell->setStyleId(style_a);
    first.row->setStyled(true);

    tp::Page::RowAndCell second = page.getRowAndCell(1, 0);
    second.cell->setStyleId(style_b);
    second.row->setStyled(true);

    tp::Page::RowAndCell third = page.getRowAndCell(2, 0);
    page.styles.use(base, style_a);
    third.cell->setStyleId(style_a);
    third.row->setStyled(true);

    const TerminalHyperlinkId hyperlink_a = insertLink(&page, explicitLink("a", "alpha"));
    const TerminalHyperlinkId dead_hyperlink = insertLink(&page, explicitLink("dead-id", "dead-uri"));
    const TerminalHyperlinkId hyperlink_b = insertLink(&page, implicitLink(0x01020304, "beta"));
    ASSERT_TRUE(hyperlink_a != 0 && dead_hyperlink != 0 && hyperlink_b != 0);
    page.hyperlink_set.release(base, dead_hyperlink);

    page.hyperlink_set.use(base, hyperlink_a);
    ASSERT_TRUE(page.setHyperlink(first.row, first.cell, hyperlink_a) == tp::PageError::none);
    ASSERT_TRUE(page.setHyperlink(second.row, second.cell, hyperlink_b) == tp::PageError::none);
    ASSERT_TRUE(page.setHyperlink(third.row, third.cell, hyperlink_a) == tp::PageError::none);

    /* Cover graphemes, wide-cell pairs, colors, protection, and semantic
     * metadata in one compact two-row page. */
    tp::Page::RowAndCell grapheme = page.getRowAndCell(0, 1);
    *grapheme.cell = TerminalCell::init('x');
    const uint32_t gcps[] = { 0x0301, 0x0302 };
    ASSERT_TRUE(page.setGraphemes(grapheme.row, grapheme.cell, gcps, 2) == tp::PageError::none);

    first.cell->setContentCodepoint('A');
    first.cell->setWide(TerminalCell::Wide::wide);
    first.cell->setProtected(true);
    first.cell->setSemanticContent(TerminalCell::SemanticContent::prompt);
    first.row->setSemanticPrompt(TerminalRow::SemanticPrompt::prompt);

    second.cell->setWide(TerminalCell::Wide::spacer_tail);
    second.cell->setSemanticContent(TerminalCell::SemanticContent::input);

    third.cell->setContentTag(TerminalCell::ContentTag::bg_color_palette);
    third.cell->setContentColorPalette(7);

    tp::Page::RowAndCell rgb = page.getRowAndCell(1, 1);
    rgb.cell->setContentTag(TerminalCell::ContentTag::bg_color_rgb);
    TerminalCell::RGB c;
    c.r = 0xaa;
    c.g = 0xbb;
    c.b = 0xcc;
    rgb.cell->setContentColorRgb(c);
    rgb.cell->setProtected(true);

    tp::Page::RowAndCell spacer_head = page.getRowAndCell(2, 1);
    spacer_head.cell->setWide(TerminalCell::Wide::spacer_head);
    spacer_head.row->setWrap(true);
    spacer_head.row->setWrapContinuation(true);
    spacer_head.row->setSemanticPrompt(TerminalRow::SemanticPrompt::prompt_continuation);

    const Header h = header(3, 2, 2, 2, 8, 512, 128, 256);
    ASSERT_TRUE(h == Header::init(&page));

    /* Lock the payload bytes and independently verify the counting writer
     * sees the same encoded length. Wisp: a std::string is both. */
    std::string counter;
    ASSERT_TRUE(sp::encodePayload(&page, &counter) == Error::none);

    std::string encoded;
    ASSERT_TRUE(sp::encodePayload(&page, &encoded) == Error::none);

    std::string golden;
    ASSERT_TRUE(loadFixture("page-v1.hex", &golden));
    const std::string path = fixturePath("page-v1.hex");
    ASSERT_TRUE(fixture::expectEqual(fixture::Kind::page, path.c_str(),
                                     "snapshot_fixture-page-v1.hex", golden, encoded));
    ASSERT_EQ((long)golden.size(), (long)counter.size());

    /* Decode the checked-in reference rather than the just-generated bytes.
     * A one-byte backing buffer exercises streaming reads across every field. */
    Reader source = Reader::fixed(golden.data(), golden.size());
    uint8_t read_buf[1];
    LimitedReader limited = LimitedReader::limited(&source, read_buf, sizeof read_buf);
    PageHolder decoded_holder;
    ASSERT_TRUE(sp::decodePayload(&limited, talloc(), &decoded_holder.p) == Error::none);
    decoded_holder.ok = true;
    tp::Page &decoded = decoded_holder.p;
    const void *dbase = decoded.memory;

    ASSERT_TRUE(h == Header::init(&decoded));
    ASSERT_TRUE(decoded.verifyIntegrity() == tp::PageError::none);

    /* Decoding compacts the sparse source IDs while preserving table values
     * and all cell references through the remap tables. */
    auto style_it = decoded.styles.iterator(dbase);
    ts::Set::Entry e;
    ASSERT_TRUE(style_it.next(&e));
    const TerminalStyleId decoded_style_a = e.id;
    ASSERT_EQ(1, (long)decoded_style_a);
    ASSERT_TRUE(boldStyle().eql(*e.value_ptr));
    ASSERT_TRUE(style_it.next(&e));
    const TerminalStyleId decoded_style_b = e.id;
    ASSERT_EQ(2, (long)decoded_style_b);
    ASSERT_TRUE(paletteBg(42).eql(*e.value_ptr));
    ASSERT_FALSE(style_it.next(&e));
    ASSERT_EQ(2, (long)decoded.styles.refCount(dbase, decoded_style_a));
    ASSERT_EQ(1, (long)decoded.styles.refCount(dbase, decoded_style_b));

    auto hyperlink_it = decoded.hyperlink_set.iterator(dbase);
    th::Set::Entry he;
    ASSERT_TRUE(hyperlink_it.next(&he));
    ASSERT_EQ(1, (long)he.id);
    const TerminalHyperlink decoded_hyperlink_a = sp::pageHyperlink(&decoded, he.value_ptr);
    ASSERT_TRUE(decoded_hyperlink_a.id.explicit_len == 1 &&
                memcmp(decoded_hyperlink_a.id.explicit_ptr, "a", 1) == 0);
    ASSERT_TRUE(decoded_hyperlink_a.uri_len == 5 && memcmp(decoded_hyperlink_a.uri, "alpha", 5) == 0);
    ASSERT_TRUE(hyperlink_it.next(&he));
    ASSERT_EQ(2, (long)he.id);
    const TerminalHyperlink decoded_hyperlink_b = sp::pageHyperlink(&decoded, he.value_ptr);
    ASSERT_TRUE(0x01020304u == decoded_hyperlink_b.id.implicit);
    ASSERT_TRUE(decoded_hyperlink_b.uri_len == 4 && memcmp(decoded_hyperlink_b.uri, "beta", 4) == 0);
    ASSERT_FALSE(hyperlink_it.next(&he));

    tp::Page::RowAndCell decoded_first = decoded.getRowAndCell(0, 0);
    ASSERT_EQ(1, (long)decoded_first.cell->style_id());
    TerminalHyperlinkId link = 0;
    ASSERT_TRUE(lookupLink(decoded, decoded_first.cell, &link));
    ASSERT_EQ(1, (long)link);

    tp::Page::RowAndCell decoded_second = decoded.getRowAndCell(1, 0);
    ASSERT_EQ(2, (long)decoded_second.cell->style_id());
    ASSERT_TRUE(lookupLink(decoded, decoded_second.cell, &link));
    ASSERT_EQ(2, (long)link);

    /* The first re-encode reflects compacted IDs; subsequent round trips must
     * stabilize byte-for-byte. */
    std::string reencoded;
    ASSERT_TRUE(sp::encodePayload(&decoded, &reencoded) == Error::none);
    ASSERT_FALSE(encoded == reencoded);

    Reader reencoded_reader = Reader::fixed(reencoded.data(), reencoded.size());
    PageHolder again_holder;
    ASSERT_TRUE(sp::decodePayload(&reencoded_reader, talloc(), &again_holder.p) == Error::none);
    again_holder.ok = true;
    ASSERT_TRUE(again_holder.p.verifyIntegrity() == tp::PageError::none);

    std::string reencoded_again;
    ASSERT_TRUE(sp::encodePayload(&again_holder.p, &reencoded_again) == Error::none);
    ASSERT_TRUE(reencoded == reencoded_again);
}

TEST(snapshot_page, framed_PAGE_golden_empty_record) {
    PageHolder holder(fullCap(1, 1, 0, 0, 0, 0));
    ASSERT_TRUE(holder.ok);

    std::string destination;
    record::Writer stream;
    stream.init(&destination);
    ASSERT_TRUE(sp::encode(&holder.p, &stream) == Error::none);

    std::string golden;
    ASSERT_TRUE(loadFixture("page-empty-record-v1.hex", &golden));
    const std::string path = fixturePath("page-empty-record-v1.hex");
    ASSERT_TRUE(fixture::expectEqual(fixture::Kind::bytes, path.c_str(),
                                     "snapshot_fixture-page-empty-record-v1.hex", golden, destination));

    Reader source = Reader::fixed(golden.data(), golden.size());
    PageHolder decoded;
    ASSERT_TRUE(sp::decode(&source, talloc(), &decoded.p) == Error::none);
    decoded.ok = true;
    ASSERT_TRUE(Header::init(&holder.p) == Header::init(&decoded.p));
}

TEST(snapshot_page, framed_PAGE_rejects_incomplete_wide_cells_transactionally) {
    /* One column puts the wide cell at the row end. Two columns put an
     * ordinary narrow cell after it. Neither case supplies a spacer tail. */
    const uint16_t column_cases[] = { 1, 2 };
    for (size_t ci = 0; ci < 2; ci++) {
        const uint16_t columns = column_cases[ci];
        PageHolder holder(cap(columns, 1));
        ASSERT_TRUE(holder.ok);
        TerminalCell *first = holder.p.getRowAndCell(0, 0).cell;
        *first = TerminalCell::init('A');
        first->setWide(TerminalCell::Wide::wide);
        if (columns > 1) *holder.p.getRowAndCell(1, 0).cell = TerminalCell::init('B');

        std::string destination = "prefix";
        record::Writer stream;
        stream.init(&destination);
        ASSERT_TRUE(sp::encode(&holder.p, &stream) == Error::InvalidWideCell);
        ASSERT_TRUE(destination == "prefix");
    }
}

TEST(snapshot_page, framed_PAGE_rejects_a_different_record_tag) {
    std::string wrong_tag;
    ASSERT_TRUE(loadFixture("page-empty-record-v1.hex", &wrong_tag));
    wrong_tag[0] = (char)(uint8_t)record::Tag::screen;
    wrong_tag[1] = 0;
    Reader reader = Reader::fixed(wrong_tag.data(), wrong_tag.size());
    tp::Page out;
    ASSERT_TRUE(sp::decode(&reader, talloc(), &out) == Error::UnexpectedRecordTag);
}

TEST(snapshot_page, framed_PAGE_preserves_following_bytes) {
    std::string bytes;
    ASSERT_TRUE(loadFixture("page-empty-record-v1.hex", &bytes));
    bytes += "next";
    Reader source = Reader::fixed(bytes.data(), bytes.size());
    PageHolder decoded;
    ASSERT_TRUE(sp::decode(&source, talloc(), &decoded.p) == Error::none);
    decoded.ok = true;
    ReadError rerr = ReadError::none;
    const uint8_t *next = source.take(4, &rerr);
    ASSERT_TRUE(next != nullptr && memcmp(next, "next", 4) == 0);
}

TEST(snapshot_page, decode_sparse_page_rejects_every_truncation) {
    std::string golden;
    ASSERT_TRUE(loadFixture("page-v1.hex", &golden));
    for (size_t len = 0; len < golden.size(); len++) {
        Reader reader = Reader::fixed(golden.data(), len);
        tp::Page out;
        ASSERT_TRUE(sp::decodePayload(&reader, talloc(), &out) == Error::EndOfStream);
    }
}

static void writeCell(std::string *w, grid::Cell c) { io::writeInt<uint64_t>(w, c.bits()); }

static grid::Cell gcell(uint32_t content) {
    grid::Cell c;
    c.content = content;
    return c;
}

static void writeRowHeaderEight(std::string *w, uint16_t count) {
    grid::Row r;
    r.cell_width = grid::EncodedWidth::eight;
    w->push_back((char)r.bits());
    io::writeInt<uint16_t>(w, count);
}

TEST(snapshot_page, decode_accepts_unordered_sparse_style_IDs_and_ignores_zero) {
    std::string descending;
    header(2, 1, 2, 0, 8, 0, 0, 0).encode(&descending);
    io::writeInt<TerminalStyleId>(&descending, 3);
    style::encode(boldStyle(), &descending);
    io::writeInt<TerminalStyleId>(&descending, 2);
    style::encode(italicStyle(), &descending);
    writeRowHeaderEight(&descending, 2);
    grid::Cell a = gcell('A');
    a.style_id = 3;
    writeCell(&descending, a);
    grid::Cell b = gcell('B');
    b.style_id = 2;
    writeCell(&descending, b);
    io::writeInt<uint32_t>(&descending, 0);

    Reader descending_reader = Reader::fixed(descending.data(), descending.size());
    PageHolder decoded_descending;
    ASSERT_TRUE(sp::decodePayload(&descending_reader, talloc(), &decoded_descending.p) == Error::none);
    decoded_descending.ok = true;
    ASSERT_EQ(2, (long)decoded_descending.p.styles.count());
    ASSERT_EQ(1, (long)decoded_descending.p.getRowAndCell(0, 0).cell->style_id());
    ASSERT_EQ(2, (long)decoded_descending.p.getRowAndCell(1, 0).cell->style_id());

    std::string zero;
    header(1, 1, 1, 0, 8, 0, 0, 0).encode(&zero);
    io::writeInt<TerminalStyleId>(&zero, 0);
    style::encode(boldStyle(), &zero);

    PageHolder zero_grid(cap(1, 1));
    ASSERT_TRUE(zero_grid.ok);
    ASSERT_TRUE(grid::encode(&zero_grid.p, &zero) == grid::EncodeError::none);

    Reader zero_reader = Reader::fixed(zero.data(), zero.size());
    PageHolder decoded_zero;
    ASSERT_TRUE(sp::decodePayload(&zero_reader, talloc(), &decoded_zero.p) == Error::none);
    decoded_zero.ok = true;
    ASSERT_EQ(0, (long)decoded_zero.p.styles.count());
}

TEST(snapshot_page, decode_accepts_unordered_sparse_hyperlink_IDs) {
    std::string encoded;
    header(2, 1, 0, 2, 0, 512, 0, 6).encode(&encoded);
    io::writeInt<TerminalHyperlinkId>(&encoded, 3);
    ASSERT_TRUE(hyperlink::encode(implicitLink(1, "one"), &encoded) == hyperlink::EncodeError::none);
    io::writeInt<TerminalHyperlinkId>(&encoded, 2);
    ASSERT_TRUE(hyperlink::encode(implicitLink(2, "two"), &encoded) == hyperlink::EncodeError::none);
    writeRowHeaderEight(&encoded, 2);
    grid::Cell a = gcell('A');
    a.hyperlink = true;
    a.hyperlink_id = 3;
    writeCell(&encoded, a);
    grid::Cell b = gcell('B');
    b.hyperlink = true;
    b.hyperlink_id = 2;
    writeCell(&encoded, b);
    io::writeInt<uint32_t>(&encoded, 0);

    Reader reader = Reader::fixed(encoded.data(), encoded.size());
    PageHolder holder;
    ASSERT_TRUE(sp::decodePayload(&reader, talloc(), &holder.p) == Error::none);
    holder.ok = true;
    tp::Page &decoded = holder.p;
    const void *base = decoded.memory;
    ASSERT_EQ(2, (long)decoded.hyperlink_set.count());
    TerminalHyperlinkId first_id = 0, second_id = 0;
    ASSERT_TRUE(lookupLink(decoded, decoded.getRowAndCell(0, 0).cell, &first_id));
    ASSERT_TRUE(lookupLink(decoded, decoded.getRowAndCell(1, 0).cell, &second_id));
    ASSERT_EQ(1, (long)decoded.hyperlink_set.refCount(base, first_id));
    ASSERT_EQ(1, (long)decoded.hyperlink_set.refCount(base, second_id));
    const th::PageEntry *first = decoded.hyperlink_set.get(base, first_id);
    const th::PageEntry *second = decoded.hyperlink_set.get(base, second_id);
    ASSERT_TRUE(first->uri.len == 3 && memcmp(first->uri.slice(base), "one", 3) == 0);
    ASSERT_TRUE(second->uri.len == 3 && memcmp(second->uri.slice(base), "two", 3) == 0);
}

TEST(snapshot_page, decode_defaults_missing_sparse_cell_references) {
    /* An unknown style ID falls back to the default style. */
    std::string style_encoded;
    header(1, 1, 0, 0, 8, 0, 0, 0).encode(&style_encoded);
    style_encoded.push_back((char)0x30); /* row flags, full cell width */
    io::writeInt<uint16_t>(&style_encoded, 1);
    grid::Cell sc;
    sc.style_id = 1;
    writeCell(&style_encoded, sc);
    io::writeInt<uint32_t>(&style_encoded, 0);

    Reader style_reader = Reader::fixed(style_encoded.data(), style_encoded.size());
    PageHolder style_page;
    ASSERT_TRUE(sp::decodePayload(&style_reader, talloc(), &style_page.p) == Error::none);
    style_page.ok = true;
    ASSERT_EQ(0, (long)style_page.p.getRowAndCell(0, 0).cell->style_id());

    /* An unknown hyperlink ID likewise falls back to no hyperlink. */
    std::string hyperlink_encoded;
    header(1, 1, 0, 0, 0, 512, 0, 0).encode(&hyperlink_encoded);
    hyperlink_encoded.push_back((char)0x30);
    io::writeInt<uint16_t>(&hyperlink_encoded, 1);
    grid::Cell hc;
    hc.hyperlink = true;
    hc.hyperlink_id = 1;
    writeCell(&hyperlink_encoded, hc);
    io::writeInt<uint32_t>(&hyperlink_encoded, 0);

    Reader hyperlink_reader = Reader::fixed(hyperlink_encoded.data(), hyperlink_encoded.size());
    PageHolder hyperlink_page;
    ASSERT_TRUE(sp::decodePayload(&hyperlink_reader, talloc(), &hyperlink_page.p) == Error::none);
    hyperlink_page.ok = true;
    const TerminalCell *cell = hyperlink_page.p.getRowAndCell(0, 0).cell;
    ASSERT_FALSE(cell->hyperlink());
    TerminalHyperlinkId link = 0;
    ASSERT_FALSE(lookupLink(hyperlink_page.p, cell, &link));
}

TEST(snapshot_page, decode_normalizes_invalid_grid_semantics) {
    std::string encoded;
    header(3, 1, 0, 0, 0, 0, 0, 0).encode(&encoded);

    /* Preserve wrap while degrading the unknown semantic-prompt value to
     * none and ignoring all reserved row bits. */
    encoded.push_back((char)0xFD);
    io::writeInt<uint16_t>(&encoded, 3);

    /* A text cell replaces an invalid Unicode scalar with U+FFFD while its
     * reserved semantic content degrades to output. */
    grid::Cell c0 = gcell(0xD800);
    c0.semantic_content = 3;
    writeCell(&encoded, c0);

    /* A declared grapheme suffix cannot fit the advertised zero capacity, so
     * decoding preserves the base character and drops the suffix. */
    grid::Cell c1 = gcell('x');
    c1.kind = 1;
    writeCell(&encoded, c1);

    /* Reserved palette content bits do not obscure the valid low palette
     * byte. */
    grid::Cell c2 = gcell(0xFFFF07);
    c2.kind = 2;
    writeCell(&encoded, c2);

    /* The grapheme section carries the suffix for the second cell. */
    io::writeInt<uint32_t>(&encoded, 1);
    io::writeInt<uint16_t>(&encoded, 0);
    io::writeInt<uint16_t>(&encoded, 1);
    io::writeInt<uint16_t>(&encoded, 1);
    io::writeInt<uint32_t>(&encoded, 0x0301);

    Reader reader = Reader::fixed(encoded.data(), encoded.size());
    PageHolder holder;
    ASSERT_TRUE(sp::decodePayload(&reader, talloc(), &holder.p) == Error::none);
    holder.ok = true;
    tp::Page &page = holder.p;
    ASSERT_TRUE(page.verifyIntegrity() == tp::PageError::none);

    tp::Page::RowAndCell first = page.getRowAndCell(0, 0);
    ASSERT_TRUE(first.row->wrap());
    ASSERT_TRUE(TerminalRow::SemanticPrompt::none == first.row->semantic_prompt());
    ASSERT_EQ(0xFFFD, (long)first.cell->codepoint());
    ASSERT_TRUE(TerminalCell::Wide::narrow == first.cell->wide());
    ASSERT_TRUE(TerminalCell::SemanticContent::output == first.cell->semantic_content());
    ASSERT_FALSE(first.cell->protected_());
    ASSERT_FALSE(first.cell->hasGrapheme());

    const TerminalCell *second = page.getRowAndCell(1, 0).cell;
    ASSERT_EQ('x', (long)second->codepoint());
    ASSERT_FALSE(second->hasGrapheme());

    const TerminalCell *third = page.getRowAndCell(2, 0).cell;
    ASSERT_TRUE(TerminalCell::ContentTag::bg_color_palette == third->content_tag());
    ASSERT_EQ(7, (long)third->contentColorPalette());
    ASSERT_FALSE(third->hasGrapheme());
}

TEST(snapshot_page, decode_validates_dimensions) {
    const Header cases[] = {
        header(0, 24, 0, 0, 0, 0, 0, 0),
        header(80, 0, 0, 0, 0, 0, 0, 0),
    };
    for (size_t i = 0; i < 2; i++) {
        std::string encoded;
        cases[i].encode(&encoded);
        Reader reader = Reader::fixed(encoded.data(), encoded.size());
        tp::Page out;
        ASSERT_TRUE(sp::decodePayload(&reader, talloc(), &out) == Error::InvalidDimensions);
    }
}

/* Serves `bytes_before_failure` bytes, then fails exactly once with
 * ReadFailed, then serves the rest. */
struct FailOnceReader {
    Reader source;
    size_t bytes_before_failure;
    bool failed = false;

    bool readSliceAll(void *dest, size_t n, ReadError *err) {
        if (!failed) {
            if (n > bytes_before_failure) {
                ReadError ignored = ReadError::none;
                source.readSliceAll(dest, bytes_before_failure, &ignored);
                bytes_before_failure = 0;
                failed = true;
                *err = ReadError::ReadFailed;
                return false;
            }
            bytes_before_failure -= n;
        }
        return source.readSliceAll(dest, n, err);
    }

    bool takeByte(uint8_t *out, ReadError *err) { return readSliceAll(out, 1, err); }

    bool discardAll(size_t n, ReadError *err) {
        uint8_t scratch[64];
        while (n > 0) {
            const size_t step = n < sizeof scratch ? n : sizeof scratch;
            if (!readSliceAll(scratch, step, err)) return false;
            n -= step;
        }
        return true;
    }
};

TEST(snapshot_page, decode_propagates_a_style_table_read_failure) {
    /* If the style read error is swallowed, its sixteen zero bytes are then
     * misread as a valid empty grid and the unframed payload appears to
     * decode. */
    std::string encoded;
    header(1, 1, 1, 0, 8, 0, 0, 0).encode(&encoded);
    io::writeInt<TerminalStyleId>(&encoded, 1);
    encoded.append(style::len, '\0');

    FailOnceReader source;
    source.source = Reader::fixed(encoded.data(), encoded.size());
    source.bytes_before_failure = Header::len + sizeof(TerminalStyleId);

    PageHolder decoded;
    const Error err = sp::decodePayload(&source, talloc(), &decoded.p);
    decoded.ok = err == Error::none;
    ASSERT_TRUE(err == Error::ReadFailed);
}

TEST(snapshot_page, decode_normalizes_duplicate_default_and_invalid_style_entries) {
    const TerminalStyle duplicate_style = boldStyle();

    /* Generate a valid grid whose cell references a sparse style ID. The PAGE
     * table below can then give that ID duplicate, default, or invalid
     * contents without hand-authoring the cell wire format. */
    tp::Capacity gcap = cap(1, 1);
    gcap.styles = 8;
    PageHolder grid_holder(gcap);
    ASSERT_TRUE(grid_holder.ok);
    tp::Page &grid_page = grid_holder.p;
    const TerminalStyleId released_style_a = addStyle(&grid_page, italicStyle());
    const TerminalStyleId released_style_b = addStyle(&grid_page, paletteBg(1));
    const TerminalStyleId encoded_style_id = addStyle(&grid_page, duplicate_style);
    ASSERT_TRUE(released_style_a && released_style_b && encoded_style_id);
    grid_page.styles.release((const void *)grid_page.memory, released_style_a);
    grid_page.styles.release((const void *)grid_page.memory, released_style_b);
    tp::Page::RowAndCell grid_cell = grid_page.getRowAndCell(0, 0);
    *grid_cell.cell = TerminalCell::init('A');
    grid_cell.cell->setStyleId(encoded_style_id);
    grid_cell.row->setStyled(true);

    std::string grid_bytes;
    ASSERT_TRUE(grid::encode(&grid_page, &grid_bytes) == grid::EncodeError::none);

    std::string encoded;
    header(1, 1, 2, 0, 16, 512, 0, 256).encode(&encoded);
    io::writeInt<TerminalStyleId>(&encoded, 1);
    style::encode(duplicate_style, &encoded);
    io::writeInt<TerminalStyleId>(&encoded, encoded_style_id);
    style::encode(duplicate_style, &encoded);
    encoded += grid_bytes;

    Reader reader = Reader::fixed(encoded.data(), encoded.size());
    PageHolder dup;
    ASSERT_TRUE(sp::decodePayload(&reader, talloc(), &dup.p) == Error::none);
    dup.ok = true;
    ASSERT_EQ(1, (long)dup.p.styles.count());
    const TerminalCell *duplicate_cell = dup.p.getRowAndCell(0, 0).cell;
    ASSERT_TRUE(duplicate_cell->style_id() != 0);
    ASSERT_TRUE(dup.p.styles.get((const void *)dup.p.memory, duplicate_cell->style_id())->flags.bold);

    std::string default_encoded;
    header(1, 1, 1, 0, 16, 0, 0, 0).encode(&default_encoded);
    io::writeInt<TerminalStyleId>(&default_encoded, encoded_style_id);
    style::encode(TerminalStyle(), &default_encoded);
    default_encoded += grid_bytes;

    Reader default_reader = Reader::fixed(default_encoded.data(), default_encoded.size());
    PageHolder def;
    ASSERT_TRUE(sp::decodePayload(&default_reader, talloc(), &def.p) == Error::none);
    def.ok = true;
    ASSERT_EQ(0, (long)def.p.getRowAndCell(0, 0).cell->style_id());

    /* The strict style codec rejects this kind, but PAGE owns the fixed entry
     * boundary and can safely map the encoded ID to the default style. A
     * zero capacity hint also remains advisory. */
    std::string invalid_encoded;
    header(1, 1, 1, 0, 0, 0, 0, 0).encode(&invalid_encoded);
    io::writeInt<TerminalStyleId>(&invalid_encoded, encoded_style_id);
    invalid_encoded.push_back((char)3);
    invalid_encoded.append(style::len - 1, '\0');
    invalid_encoded += grid_bytes;

    Reader invalid_reader = Reader::fixed(invalid_encoded.data(), invalid_encoded.size());
    PageHolder inv;
    ASSERT_TRUE(sp::decodePayload(&invalid_reader, talloc(), &inv.p) == Error::none);
    inv.ok = true;
    ASSERT_EQ(0, (long)inv.p.getRowAndCell(0, 0).cell->style_id());
}

TEST(snapshot_page, decode_releases_unused_style_table_references) {
    PageHolder empty_page(cap(1, 1));
    ASSERT_TRUE(empty_page.ok);
    std::string grid_bytes;
    ASSERT_TRUE(grid::encode(&empty_page.p, &grid_bytes) == grid::EncodeError::none);

    std::string encoded;
    header(1, 1, 1, 0, 8, 0, 0, 0).encode(&encoded);
    io::writeInt<TerminalStyleId>(&encoded, 7);
    style::encode(boldStyle(), &encoded);
    encoded += grid_bytes;

    Reader reader = Reader::fixed(encoded.data(), encoded.size());
    PageHolder decoded;
    ASSERT_TRUE(sp::decodePayload(&reader, talloc(), &decoded.p) == Error::none);
    decoded.ok = true;
    ASSERT_EQ(0, (long)decoded.p.styles.count());

    /* The dead table-only entry is absent from the very first re-encode. */
    std::string canonical;
    ASSERT_TRUE(sp::encodePayload(&decoded.p, &canonical) == Error::none);
    Reader canonical_reader = Reader::fixed(canonical.data(), canonical.size());
    Header canonical_header;
    ReadError rerr = ReadError::none;
    ASSERT_TRUE(Header::decode(&canonical_reader, &canonical_header, &rerr));
    ASSERT_EQ(0, (long)canonical_header.style_count);
}

TEST(snapshot_page, decode_reuses_duplicate_hyperlinks) {
    const TerminalHyperlink duplicate = explicitLink("id", "uri");

    /* As with styles above, use a sparse native ID to make grid.encode
     * produce the cell reference whose table value this test deliberately
     * duplicates. */
    tp::Capacity gcap = cap(1, 1);
    gcap.hyperlink_bytes = 512;
    gcap.string_bytes = 64;
    PageHolder grid_holder(gcap);
    ASSERT_TRUE(grid_holder.ok);
    tp::Page &grid_page = grid_holder.p;
    const TerminalHyperlinkId released_a = insertLink(&grid_page, implicitLink(1, "one"));
    const TerminalHyperlinkId released_b = insertLink(&grid_page, implicitLink(2, "two"));
    const TerminalHyperlinkId encoded_hyperlink_id = insertLink(&grid_page, implicitLink(3, "three"));
    ASSERT_TRUE(released_a && released_b && encoded_hyperlink_id);
    grid_page.hyperlink_set.release((const void *)grid_page.memory, released_a);
    grid_page.hyperlink_set.release((const void *)grid_page.memory, released_b);
    tp::Page::RowAndCell grid_cell = grid_page.getRowAndCell(0, 0);
    *grid_cell.cell = TerminalCell::init('A');
    ASSERT_TRUE(grid_page.setHyperlink(grid_cell.row, grid_cell.cell, encoded_hyperlink_id) ==
                tp::PageError::none);

    std::string grid_bytes;
    ASSERT_TRUE(grid::encode(&grid_page, &grid_bytes) == grid::EncodeError::none);

    std::string encoded;
    header(1, 1, 0, 2, 0, 512, 0, 16).encode(&encoded);
    io::writeInt<TerminalHyperlinkId>(&encoded, 1);
    ASSERT_TRUE(hyperlink::encode(duplicate, &encoded) == hyperlink::EncodeError::none);
    io::writeInt<TerminalHyperlinkId>(&encoded, encoded_hyperlink_id);
    ASSERT_TRUE(hyperlink::encode(duplicate, &encoded) == hyperlink::EncodeError::none);
    encoded += grid_bytes;

    Reader reader = Reader::fixed(encoded.data(), encoded.size());
    PageHolder holder;
    ASSERT_TRUE(sp::decodePayload(&reader, talloc(), &holder.p) == Error::none);
    holder.ok = true;
    tp::Page &decoded = holder.p;
    const void *base = decoded.memory;
    ASSERT_EQ(1, (long)decoded.hyperlink_set.count());
    const TerminalCell *cell = decoded.getRowAndCell(0, 0).cell;
    ASSERT_TRUE(cell->hyperlink());
    TerminalHyperlinkId id = 0;
    ASSERT_TRUE(lookupLink(decoded, cell, &id));
    const th::PageEntry *entry = decoded.hyperlink_set.get(base, id);
    ASSERT_EQ(1, (long)decoded.hyperlink_set.refCount(base, id));
    ASSERT_TRUE(entry->uri.len == 3 && memcmp(entry->uri.slice(base), "uri", 3) == 0);

    /* Overwriting the sole linked cell must make the deduplicated entry dead.
     * Any surviving reference belongs to the decode-time wire table, not the
     * native page. */
    decoded.clearCells(decoded.getRow(0), 0, 1);
    ASSERT_EQ(0, (long)decoded.hyperlink_set.count());
}

TEST(snapshot_page, discard_consumes_exactly_one_PAGE_record) {
    /* Discard validates framing only, so an arbitrary payload keeps this test
     * focused on record consumption rather than page structure. */
    std::string encoded;
    record::Writer stream;
    stream.init(&encoded);
    stream.begin(record::Tag::page)->append("undecodable page payload");
    ASSERT_TRUE(stream.finish() == record::Writer::FinishError::none);
    const size_t record_len = encoded.size();
    encoded += "next";

    Reader source = Reader::fixed(encoded.data(), encoded.size());
    ASSERT_TRUE(sp::discard(&source) == Error::none);
    ReadError rerr = ReadError::none;
    const uint8_t *next = source.take(4, &rerr);
    ASSERT_TRUE(next != nullptr && memcmp(next, "next", 4) == 0);

    /* Only PAGE records may be discarded; the tag remains strict. */
    std::string wrong_tag;
    record::Writer wrong_tag_stream;
    wrong_tag_stream.init(&wrong_tag);
    wrong_tag_stream.begin(record::Tag::history)->append("payload");
    ASSERT_TRUE(wrong_tag_stream.finish() == record::Writer::FinishError::none);
    Reader wrong_tag_source = Reader::fixed(wrong_tag.data(), wrong_tag.size());
    ASSERT_TRUE(sp::discard(&wrong_tag_source) == Error::UnexpectedRecordTag);

    /* Discarded bytes are still covered by the record CRC. */
    std::string corrupted = encoded.substr(0, record_len);
    corrupted[corrupted.size() - 1] ^= 1;
    Reader corrupted_source = Reader::fixed(corrupted.data(), corrupted.size());
    ASSERT_TRUE(sp::discard(&corrupted_source) == Error::InvalidChecksum);

    /* A truncated payload is detected before `finish`. */
    Reader truncated_source = Reader::fixed(encoded.data(), record_len - 1);
    ASSERT_TRUE(sp::discard(&truncated_source) == Error::EndOfStream);
}

TEST(snapshot_page, decode_ignores_empty_hyperlink_strings) {
    struct Fx {
        const char *bytes;
        size_t len;
    };
    const Fx fixtures[] = {
        /* Empty implicit URI. */
        { "\x01\x01\x00\x00\x00\x00\x00\x00\x00", 9 },
        /* Empty explicit ID. */
        { "\x02\x00\x00\x00\x00\x03\x00\x00\x00" "uri", 12 },
        /* Empty explicit URI. */
        { "\x02\x02\x00\x00\x00" "id" "\x00\x00\x00\x00", 11 },
        /* Valid contents that cannot fit the zero-capacity native set. */
        { "\x01\x01\x00\x00\x00\x03\x00\x00\x00" "uri", 12 },
    };

    /* Only the invalid hyperlink entry itself is hand-authored above.
     * Generate its cell reference through the native grid encoder. */
    tp::Capacity gcap = cap(1, 1);
    gcap.hyperlink_bytes = 256;
    gcap.string_bytes = 16;
    PageHolder grid_holder(gcap);
    ASSERT_TRUE(grid_holder.ok);
    tp::Page &grid_page = grid_holder.p;
    const TerminalHyperlinkId encoded_hyperlink_id = insertLink(&grid_page, implicitLink(1, "uri"));
    ASSERT_TRUE(encoded_hyperlink_id != 0);
    tp::Page::RowAndCell grid_cell = grid_page.getRowAndCell(0, 0);
    *grid_cell.cell = TerminalCell::init('A');
    ASSERT_TRUE(grid_page.setHyperlink(grid_cell.row, grid_cell.cell, encoded_hyperlink_id) ==
                tp::PageError::none);
    std::string grid_bytes;
    ASSERT_TRUE(grid::encode(&grid_page, &grid_bytes) == grid::EncodeError::none);

    for (size_t i = 0; i < 4; i++) {
        std::string encoded;
        header(1, 1, 0, 1, 0, 0, 0, 16).encode(&encoded);
        io::writeInt<TerminalHyperlinkId>(&encoded, encoded_hyperlink_id);
        encoded.append(fixtures[i].bytes, fixtures[i].len);

        /* The cell refers to the ignored table entry. Its absent native
         * remapping must degrade to no hyperlink while preserving the cell. */
        encoded += grid_bytes;

        Reader reader = Reader::fixed(encoded.data(), encoded.size());
        PageHolder decoded;
        ASSERT_TRUE(sp::decodePayload(&reader, talloc(), &decoded.p) == Error::none);
        decoded.ok = true;

        ASSERT_EQ(0, (long)decoded.p.hyperlink_set.count());
        const TerminalCell *cell = decoded.p.getRowAndCell(0, 0).cell;
        ASSERT_EQ('A', (long)cell->codepoint());
        ASSERT_FALSE(cell->hyperlink());
        TerminalHyperlinkId link = 0;
        ASSERT_FALSE(lookupLink(decoded.p, cell, &link));
    }
}

TEST(snapshot_page, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
