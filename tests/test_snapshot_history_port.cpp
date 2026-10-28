/* Transliterated from the test blocks in Ghostty
 * src/terminal/snapshot/history.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include <string.h>

#include <string>

#include "test_helpers.h"
#include "snapshot_fixture.hpp"
#include "../src/terminal/snapshot/history.hpp"
#include "../src/terminal/snapshot/screen.hpp"
#include "../src/zigstd/allocator.hpp"
#include "../src/zigstd/reader.hpp"

using namespace wisp;

namespace sh = wisp::terminal::snapshot::history;
namespace ss = wisp::terminal::snapshot::screen;
namespace sp = wisp::terminal::snapshot::page;
namespace record = wisp::terminal::snapshot::record;
namespace fixture = wisp::test_fixture;
namespace tp = wisp::vt::page;

typedef zigstd::Reader Reader;
typedef zigstd::ReadError ReadError;
typedef sh::Header Header;
typedef sh::Error Error;
typedef sh::TerminalScreen TerminalScreen;
typedef sh::TerminalScreenKey Key;
typedef wisp::vt::PageList PageList;
typedef wisp::vt::point::Tag PointTag;
typedef tp::Cell TerminalCell;

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

static TerminalScreen::Options opts(uint16_t cols, uint16_t rows, wisp::vt::Maybe<size_t> msb) {
    return TerminalScreen::Options(cols, rows, msb);
}

struct ScreenHolder {
    TerminalScreen s;
    bool ok = false;
    explicit ScreenHolder(const TerminalScreen::Options &o) { ok = TerminalScreen::init(talloc(), o, &s); }
    ~ScreenHolder() {
        if (ok) s.deinit();
    }
};

struct DecodedHolder {
    ss::Decoded d;
    bool ok = false;
    ~DecodedHolder() {
        if (ok) d.deinit();
    }
};

static bool atEnd(Reader *r) {
    uint8_t b = 0;
    ReadError rerr = ReadError::none;
    return !r->takeByte(&b, &rerr) && rerr == ReadError::EndOfStream;
}

TEST(snapshot_history, HISTORY_header_golden_encoding_and_decoding) {
    Header expected;
    expected.key = Key::alternate;
    expected.page_count = 0x01020304;
    std::string encoded;
    expected.encode(&encoded);

    const std::string path = fixturePath("history-header-v1.hex");
    std::string text, golden;
    ASSERT_TRUE(fixture::readFile(path.c_str(), &text));
    ASSERT_TRUE(fixture::parse(text.data(), text.size(), &golden));
    ASSERT_TRUE(fixture::expectEqual(fixture::Kind::bytes, path.c_str(),
                                     "snapshot_fixture-history-header-v1.hex", golden, encoded));
    ASSERT_EQ((long)Header::len, (long)golden.size());

    Reader reader = Reader::fixed(golden.data(), golden.size());
    Header decoded;
    ASSERT_TRUE(Header::decode(&reader, &decoded) == Error::none);
    ASSERT_TRUE(expected == decoded);
    for (size_t fixture_len = 0; fixture_len < Header::len; fixture_len++) {
        Reader truncated = Reader::fixed(golden.data(), fixture_len);
        Header out;
        ASSERT_TRUE(Header::decode(&truncated, &out) == Error::EndOfStream);
    }

    std::string invalid_key = golden;
    invalid_key[0] = 2;
    Reader invalid_key_reader = Reader::fixed(invalid_key.data(), invalid_key.size());
    Header out;
    ASSERT_TRUE(Header::decode(&invalid_key_reader, &out) == Error::InvalidKey);
}

TEST(snapshot_history, HISTORY_encodes_newest_first_and_restores_complete_history) {
    /* Choose an active height which spans two native pages, then grow until
     * exactly two additional complete pages precede the active boundary. */
    size_t page_rows = 0;
    {
        ScreenHolder probe(opts(80, 1, wisp::vt::Maybe<size_t>((size_t)0)));
        ASSERT_TRUE(probe.ok);
        page_rows = probe.s.pages.getTopLeft(PointTag::screen).node->capacity().rows;
    }
    const uint16_t screen_rows = (uint16_t)(page_rows + 1);

    ScreenHolder holder(opts(80, screen_rows, wisp::vt::Maybe<size_t>()));
    ASSERT_TRUE(holder.ok);
    TerminalScreen &source_screen = holder.s;
    source_screen.cursorAbsolute(0, (uint16_t)(screen_rows - 1));
    while (source_screen.pages.totalPages() < 4) ASSERT_TRUE(source_screen.testWriteString("\n"));

    const PageList::Pin active_top = source_screen.pages.getTopLeft(PointTag::active);
    PageList::Node *newest_history = active_top.node->prev;
    ASSERT_TRUE(newest_history != nullptr);
    PageList::Node *oldest_history = newest_history->prev;
    ASSERT_TRUE(oldest_history != nullptr);
    ASSERT_TRUE(source_screen.pages.getTopLeft(PointTag::screen).node == oldest_history);
    ASSERT_TRUE(oldest_history->prev == nullptr);

    /* Mark the two complete historical pages so the wire sequence and final
     * native order are visible. The oldest prompt also verifies derived
     * state is updated only when that page is restored. */
    *oldest_history->pageAssumeResident()->getRowAndCell(0, 0).cell = TerminalCell::init('A');
    oldest_history->pageAssumeResident()->getRowAndCell(0, 0).cell->setSemanticContent(
        TerminalCell::SemanticContent::prompt);
    *newest_history->pageAssumeResident()->getRowAndCell(0, 0).cell = TerminalCell::init('B');

    /* Encode SCREEN before history compression, then compress eligible
     * history and require HISTORY encoding to preserve each source storage
     * state. */
    std::string destination;
    record::Writer stream;
    stream.init(&destination);
    ASSERT_TRUE(ss::encode(&source_screen, Key::primary, &stream) == ss::Error::none);
    const size_t history_offset = destination.size();

    (void)source_screen.pages.compress(PageList::CompressMode::full);
    const PageList::Node::Storage oldest_storage = oldest_history->storage();
    const PageList::Node::Storage newest_storage = newest_history->storage();
    ASSERT_TRUE(sh::encode(&source_screen, Key::primary, &stream) == Error::none);
    ASSERT_TRUE(oldest_storage == oldest_history->storage());
    ASSERT_TRUE(newest_storage == newest_history->storage());

    /* Inspect the manifest and PAGE records independently. Newest history is
     * sent first even though native PageList order is oldest-to-newest. */
    Reader history_source = Reader::fixed(destination.data() + history_offset, destination.size() - history_offset);
    record::Reader history_record;
    record::Reader::InitError ierr = record::Reader::InitError::none;
    ASSERT_TRUE(history_record.init(&history_source, &ierr));
    ASSERT_TRUE(record::Tag::history == history_record.header.tag);
    Header header;
    ASSERT_TRUE(Header::decode(history_record.payloadReader(), &header) == Error::none);
    ASSERT_TRUE(history_record.finish() == record::Reader::FinishError::none);
    ASSERT_TRUE(Key::primary == header.key);
    ASSERT_EQ(2, (long)header.page_count);

    tp::Page decoded_newest;
    ASSERT_TRUE(sp::decode(&history_source, talloc(), &decoded_newest) == sp::Error::none);
    const long newest_cp = (long)decoded_newest.getRowAndCell(0, 0).cell->codepoint();
    decoded_newest.deinit();
    ASSERT_EQ('B', newest_cp);
    tp::Page decoded_oldest;
    ASSERT_TRUE(sp::decode(&history_source, talloc(), &decoded_oldest) == sp::Error::none);
    const long oldest_cp = (long)decoded_oldest.getRowAndCell(0, 0).cell->codepoint();
    decoded_oldest.deinit();
    ASSERT_EQ('A', oldest_cp);
    ASSERT_TRUE(atEnd(&history_source));

    /* Restore SCREEN first, then prepend HISTORY directly into that
     * PageList. */
    Reader restore_source = Reader::fixed(destination.data(), destination.size());
    DecodedHolder decoded_screen;
    ASSERT_TRUE(ss::decode(&restore_source, talloc(), opts(80, screen_rows, wisp::vt::Maybe<size_t>()),
                           &decoded_screen.d) == ss::Error::none);
    decoded_screen.ok = true;
    ASSERT_TRUE(Key::primary == decoded_screen.d.key);
    ASSERT_TRUE((uint64_t)(source_screen.pages.total_rows - screen_rows) == decoded_screen.d.history_rows);
    TerminalScreen *restored = &decoded_screen.d.screen;
    ASSERT_FALSE(restored->semantic_prompt.seen);
    ASSERT_TRUE(sh::decode(&restore_source, talloc(), Key::primary, restored) == Error::none);

    ASSERT_EQ((long)source_screen.pages.totalPages(), (long)restored->pages.totalPages());
    PageList::Node *restored_oldest = restored->pages.getTopLeft(PointTag::screen).node;
    ASSERT_EQ('A', (long)restored_oldest->pageAssumeResident()->getRowAndCell(0, 0).cell->codepoint());
    ASSERT_TRUE(restored_oldest->next != nullptr);
    ASSERT_EQ('B', (long)restored_oldest->next->pageAssumeResident()->getRowAndCell(0, 0).cell->codepoint());
    ASSERT_TRUE(restored->semantic_prompt.seen);
    ASSERT_TRUE(atEnd(&restore_source));

    /* Reuse a writable copy of the sequence for failure-path fixtures. */
    std::string encoded = destination;
    const size_t first_page_offset = history_offset + record::Header::len + Header::len;
    const uint32_t first_payload_len = (uint32_t)(uint8_t)encoded[first_page_offset + 2] |
                                       ((uint32_t)(uint8_t)encoded[first_page_offset + 3] << 8) |
                                       ((uint32_t)(uint8_t)encoded[first_page_offset + 4] << 16) |
                                       ((uint32_t)(uint8_t)encoded[first_page_offset + 5] << 24);
    const size_t second_page_offset = first_page_offset + record::Header::len + first_payload_len;

    /* Truncate the first PAGE only after its header has exposed a capacity.
     * Its detached allocation is discarded and the live SCREEN list remains
     * completely unchanged. */
    {
        Reader truncated_source = Reader::fixed(encoded.data(), second_page_offset - 1);
        DecodedHolder decoded_truncated;
        ASSERT_TRUE(ss::decode(&truncated_source, talloc(), opts(80, screen_rows, wisp::vt::Maybe<size_t>()),
                               &decoded_truncated.d) == ss::Error::none);
        decoded_truncated.ok = true;
        TerminalScreen *truncated = &decoded_truncated.d.screen;
        PageList::Node *truncated_screen_first = truncated->pages.getTopLeft(PointTag::screen).node;
        const size_t truncated_screen_page_count = truncated->pages.totalPages();
        ASSERT_TRUE(sh::decode(&truncated_source, talloc(), Key::primary, truncated) == Error::EndOfStream);
        ASSERT_EQ((long)truncated_screen_page_count, (long)truncated->pages.totalPages());
        ASSERT_TRUE(truncated_screen_first == truncated->pages.getTopLeft(PointTag::screen).node);
        truncated->pages.assertIntegrity();
        truncated->assertIntegrity();
    }

    /* Corrupt only the older PAGE tag. A failure in a later PAGE keeps only
     * the successfully prepended newer pages, contiguous with SCREEN. */
    encoded[second_page_offset] = (char)(uint8_t)record::Tag::screen;
    encoded[second_page_offset + 1] = 0;

    Reader partial_source = Reader::fixed(encoded.data(), encoded.size());
    DecodedHolder decoded_partial;
    ASSERT_TRUE(ss::decode(&partial_source, talloc(), opts(80, screen_rows, wisp::vt::Maybe<size_t>()),
                           &decoded_partial.d) == ss::Error::none);
    decoded_partial.ok = true;
    TerminalScreen *partial = &decoded_partial.d.screen;
    const size_t screen_page_count = partial->pages.totalPages();
    ASSERT_TRUE(sh::decode(&partial_source, talloc(), Key::primary, partial) == Error::UnexpectedRecordTag);
    ASSERT_EQ((long)(screen_page_count + 1), (long)partial->pages.totalPages());
    ASSERT_EQ('B', (long)partial->pages.getTopLeft(PointTag::screen).node->pageAssumeResident()
                       ->getRowAndCell(0, 0).cell->codepoint());
    ASSERT_FALSE(partial->semantic_prompt.seen);
    partial->pages.assertIntegrity();
    partial->assertIntegrity();
}

TEST(snapshot_history, HISTORY_encodes_and_restores_an_empty_sequence) {
    ScreenHolder holder(opts(2, 2, wisp::vt::Maybe<size_t>()));
    ASSERT_TRUE(holder.ok);
    TerminalScreen &terminal_screen = holder.s;

    std::string destination;
    record::Writer stream;
    stream.init(&destination);
    ASSERT_TRUE(sh::encode(&terminal_screen, Key::primary, &stream) == Error::none);

    Reader inspect_source = Reader::fixed(destination.data(), destination.size());
    record::Reader history_record;
    record::Reader::InitError ierr = record::Reader::InitError::none;
    ASSERT_TRUE(history_record.init(&inspect_source, &ierr));
    Header header;
    ASSERT_TRUE(Header::decode(history_record.payloadReader(), &header) == Error::none);
    ASSERT_TRUE(history_record.finish() == record::Reader::FinishError::none);
    ASSERT_EQ(0, (long)header.page_count);
    ASSERT_TRUE(atEnd(&inspect_source));

    Reader decode_source = Reader::fixed(destination.data(), destination.size());
    PageList::Node *initial_first = terminal_screen.pages.getTopLeft(PointTag::screen).node;
    ASSERT_TRUE(sh::decode(&decode_source, talloc(), Key::primary, &terminal_screen) == Error::none);
    ASSERT_TRUE(initial_first == terminal_screen.pages.getTopLeft(PointTag::screen).node);
    ASSERT_TRUE(atEnd(&decode_source));
}

TEST(snapshot_history, HISTORY_rejects_invalid_routing_and_incomplete_sequences) {
    ScreenHolder holder(opts(2, 2, wisp::vt::Maybe<size_t>()));
    ASSERT_TRUE(holder.ok);
    TerminalScreen &terminal_screen = holder.s;

    /* Routing and sequence length still determine which native screen is
     * mutated and where the following record begins, so they remain
     * strict. */
    struct Case {
        Key key;
        uint32_t page_count;
        Error expected;
    };
    const Case cases[] = {
        { Key::alternate, 0, Error::UnexpectedScreenKey },
        { Key::primary, 1, Error::EndOfStream },
    };
    for (size_t i = 0; i < 2; i++) {
        std::string destination;
        record::Writer stream;
        stream.init(&destination);
        Header h;
        h.key = cases[i].key;
        h.page_count = cases[i].page_count;
        h.encode(stream.begin(record::Tag::history));
        ASSERT_TRUE(stream.finish() == record::Writer::FinishError::none);

        Reader source = Reader::fixed(destination.data(), destination.size());
        ASSERT_TRUE(sh::decode(&source, talloc(), Key::primary, &terminal_screen) == cases[i].expected);
        terminal_screen.pages.assertIntegrity();
        terminal_screen.assertIntegrity();
    }
}

TEST(snapshot_history, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
