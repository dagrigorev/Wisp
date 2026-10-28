/* Transliterated from the test blocks in Ghostty
 * src/terminal/snapshot/snapshot.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Wisp substitutions, each marked again at its use:
 *   - Upstream streams one encode through `Writer.Discarding.hashed` to
 *     check length and CRC without retaining bytes. record::Writer writes a
 *     std::string, so the second encode is retained and measured instead.
 *   - Upstream feeds post-cut PTY bytes through `Terminal.vtStream()` (the
 *     read-only stream). The port has no read-only stream, so the standard
 *     TerminalStream feeds the same plain text and CRLF bytes.
 */

#include <string.h>

#include <string>

#include "test_helpers.h"
#include "snapshot_fixture.hpp"
#include "../src/terminal/kitty/graphics_unicode.hpp"
#include "../src/terminal/snapshot/main.hpp"
#include "../src/terminal/stream_terminal.hpp"
#include "../src/vt/formatter.hpp"
#include "../src/zigstd/allocator.hpp"
#include "../src/zigstd/crc32c.hpp"
#include "../src/zigstd/reader.hpp"

using namespace wisp;

namespace sn = wisp::terminal::snapshot;
namespace record = wisp::terminal::snapshot::record;
namespace fixture = wisp::test_fixture;
namespace tp = wisp::vt::page;

typedef zigstd::Reader Reader;
typedef zigstd::LimitedReader LimitedReader;
typedef zigstd::ReadError ReadError;
typedef sn::Error Error;
typedef sn::Terminal Terminal;
typedef sn::TerminalScreen TerminalScreen;
typedef sn::TerminalScreenKey Key;
typedef sn::TerminalPageList PageList;
typedef sn::Decoded Decoded;
typedef sn::Continuation Continuation;
typedef sn::Decoder<Reader> Decoder;
typedef wisp::terminal::stream::Stream<wisp::terminal::stream_terminal::Handler> TerminalStream;
template <typename T>
using Maybe = wisp::vt::Maybe<T>;

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

static sn::EncodeOptions testEncodeOptions() {
    sn::EncodeOptions o;
    o.continuation = Continuation::makeGround();
    return o;
}

static sn::EncodeOptions bytesOptions(const std::string &bytes) {
    sn::EncodeOptions o;
    o.continuation = Continuation::makeBytes(bytes);
    return o;
}

static sn::DecodeOptions testDecodeOptions() {
    sn::DecodeOptions o;
    o.max_continuation_bytes = 1024;
    return o;
}

static Terminal::Options termOpts(uint16_t cols, uint16_t rows) { return Terminal::Options(cols, rows); }

static Terminal::Options unlimitedOpts(uint16_t cols, uint16_t rows) {
    Terminal::Options o(cols, rows);
    o.max_scrollback_bytes = Maybe<size_t>();
    o.max_scrollback_lines = Maybe<size_t>();
    return o;
}

struct TermHolder {
    Terminal t;
    bool ok = false;
    explicit TermHolder(const Terminal::Options &o) { ok = Terminal::init(talloc(), o, &t); }
    ~TermHolder() {
        if (ok) t.deinit(talloc());
    }
};

/* Wisp: `defer x.deinit(alloc)` for a Decoded. */
struct DecodedGuard {
    Decoded *d;
    zigstd::Allocator a;
    ~DecodedGuard() { d->deinit(a); }
};

/* Wisp: `defer x.deinit(alloc)` for a transferred Terminal. */
struct TerminalGuard {
    Terminal *t;
    ~TerminalGuard() { t->deinit(talloc()); }
};

/* Wisp: TerminalStream.init(.{ .allocator, .handler = .init(t),
 * .continuation_max_bytes = n }). */
static TerminalStream::Options streamOptions(size_t continuation_max_bytes) {
    TerminalStream::Options o;
    o.allocator = true;
    o.continuation_max_bytes = wisp::terminal::stream_continuation::Maybe<size_t>(continuation_max_bytes);
    return o;
}

struct StreamHolder {
    TerminalStream s;
    StreamHolder(Terminal *t, size_t max_bytes)
        : s(wisp::terminal::stream_terminal::Handler::init(t), streamOptions(max_bytes)) {}
    ~StreamHolder() { s.deinit(); }
};

static std::string continuationBytes(const Continuation &c) {
    return std::string(c.bytes_payload.begin(), c.bytes_payload.end());
}

struct PageSpec {
    uint16_t rows;
    uint32_t codepoints[3];
    size_t codepoint_rows;
};

/* Replace the terminal's primary screen with one built from exact pages.
 * Replacing the Screen in place preserves ScreenSet routing. */
static bool replacePrimary(Terminal *t, const PageSpec *specs, size_t spec_count) {
    TerminalScreen *primary = t->screens.get(Key::primary);
    PageList pages;
    {
        PageList::Builder builder;
        if (!PageList::Builder::init(talloc(), PageList::Options(t->cols, t->rows, Maybe<size_t>(), Maybe<size_t>()),
                                     &builder)) {
            return false;
        }
        for (size_t i = 0; i < spec_count; i++) {
            tp::Page *built = builder.allocatePage(tp::Capacity(2, specs[i].rows));
            if (built == nullptr) {
                builder.deinit();
                return false;
            }
            built->size.rows = specs[i].rows;
            for (size_t y = 0; y < specs[i].codepoint_rows; y++) {
                *built->getRowAndCell(0, y).cell = tp::Cell::init(specs[i].codepoints[y]);
            }
        }
        const PageList::Builder::FinishError ferr = builder.finish(&pages);
        builder.deinit();
        if (ferr != PageList::Builder::FinishError::none) return false;
    }

    PageList::Pin *cursor_pin = pages.trackPin(pages.pin(wisp::vt::point::Point::active()).value);
    if (cursor_pin == nullptr) {
        pages.deinit();
        return false;
    }
    const auto cursor_rac = cursor_pin->rowAndCell();
    TerminalScreen replacement;
    replacement.alloc = talloc();
    replacement.pages = pages;
    replacement.cursor.page_pin = cursor_pin;
    replacement.cursor.page_row = cursor_rac.row;
    replacement.cursor.page_cell = cursor_rac.cell;

    primary->deinit();
    *primary = replacement;
    return true;
}

/* Build a 2x3 test terminal whose primary screen carries `history_pages`
 * complete two-row pages above a three-row active page. The top-left cell
 * of each page is marked 'A', 'B', ... from oldest history to active. */
static bool testHistoryTerminal(uint8_t history_pages, TermHolder *holder) {
    if (!holder->ok) return false;
    PageSpec specs[8];
    for (size_t index = 0; index < history_pages; index++) {
        specs[index].rows = 2;
        specs[index].codepoints[0] = (uint32_t)('A' + index);
        specs[index].codepoint_rows = 1;
    }
    specs[history_pages].rows = 3;
    specs[history_pages].codepoints[0] = (uint32_t)('A' + (size_t)history_pages);
    specs[history_pages].codepoint_rows = 1;
    return replacePrimary(&holder->t, specs, (size_t)history_pages + 1);
}

/* The top-left screen cell, which lands in the oldest applied history page. */
static uint32_t testTopLeftCodepoint(const TerminalScreen *terminal_screen) {
    return terminal_screen->pages.getTopLeft(wisp::vt::point::Tag::screen)
        .node->page()
        ->getRowAndCell(0, 0)
        .cell->codepoint();
}

TEST(snapshot, complete_snapshot_round_trip_with_history_and_alternate_screen) {
    TermHolder holder(unlimitedOpts(2, 3));
    ASSERT_TRUE(holder.ok);
    Terminal &t = holder.t;

    /* Exercise terminal-wide state. */
    t.width_px = 800;
    t.height_px = 600;
    t.colors.palette.set(7, wisp::terminal::RGB(1, 2, 3));
    t.modes.values.setField(wisp::terminal::modes::Mode::bracketed_paste, true);
    ASSERT_TRUE(t.setPwd("file:///tmp/snapshot"));
    ASSERT_TRUE(t.setTitle("complete snapshot"));

    TerminalScreen *primary = t.screens.get(Key::primary);

    /* Use small exact capacities so this compound golden remains practical to
     * review while still containing two complete history pages and one active
     * page. Replacing the Screen in place preserves ScreenSet routing. */
    const PageSpec specs[3] = {
        {2, {'A', 0, 0}, 1},
        {2, {'B', 0, 0}, 1},
        {3, {'C', 'D', 'E'}, 3},
    };
    ASSERT_TRUE(replacePrimary(&t, specs, 3));

    ASSERT_TRUE(primary->pages.scrollbar().total > t.rows);

    /* Compression is an internal source representation and must remain
     * unchanged while the complete history is inspected for encoding. */
    primary->pages.compress(PageList::CompressMode::full);
    const PageList::MemoryStats source_memory = primary->pages.memoryStats();

    /* The optional alternate screen participates in both phases and remains
     * the active screen after restoration. */
    bool ok = false;
    t.switchScreen(Key::alternate, &ok);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(t.printString("alternate"));

    std::string encoded;
    ASSERT_TRUE(sn::encode(&encoded, &t, testEncodeOptions()) == Error::none);
    const PageList::MemoryStats after_memory = primary->pages.memoryStats();
    ASSERT_TRUE(memcmp(&source_memory, &after_memory, sizeof(source_memory)) == 0);
    std::string golden;
    ASSERT_TRUE(loadFixture("complete-v1.hex", &golden));
    const std::string path = fixturePath("complete-v1.hex");
    ASSERT_TRUE(fixture::expectEqual(fixture::Kind::snapshot, path.c_str(), "snapshot_fixture-complete-v1.hex",
                                     golden, encoded));

    /* A complete snapshot can stream through a non-allocating destination.
     * Independently hash that output so both its length and complete byte
     * sequence are checked without retaining a second snapshot copy.
     *
     * Wisp: see the header -- the second encode is retained and measured. */
    std::string second;
    ASSERT_TRUE(sn::encode(&second, &t, testEncodeOptions()) == Error::none);
    ASSERT_EQ((long)golden.size(), (long)second.size());
    ASSERT_TRUE(zigstd::Crc32c::hash((const uint8_t *)golden.data(), golden.size()) ==
                zigstd::Crc32c::hash((const uint8_t *)second.data(), second.size()));

    /* Restore the checked-in reference rather than the just-generated bytes. */
    Reader encoded_source = Reader::fixed(golden.data(), golden.size());
    uint8_t source_buffer[1];
    LimitedReader limited = LimitedReader::limited(&encoded_source, source_buffer, sizeof source_buffer);
    Decoded restored;
    ASSERT_TRUE(sn::decode(talloc(), &limited, testDecodeOptions(), &restored) == Error::none);
    DecodedGuard restored_guard = { &restored, talloc() };
    Terminal *restored_terminal = &restored.terminal;

    ASSERT_TRUE(Key::alternate == restored_terminal->screens.active_key);
    ASSERT_TRUE(restored_terminal->screens.get(Key::alternate) == restored_terminal->screens.active);
    ASSERT_TRUE(std::string(restored_terminal->getPwd()) == "file:///tmp/snapshot");
    ASSERT_TRUE(std::string(restored_terminal->getTitle()) == "complete snapshot");
    ASSERT_EQ((long)primary->pages.scrollbar().total,
              (long)restored_terminal->screens.get(Key::primary)->pages.scrollbar().total);

    /* Re-encoding is a compact semantic equality check over all TERMINAL,
     * SCREEN, PAGE, and HISTORY fields and both marker boundaries. */
    std::string reencoded;
    ASSERT_TRUE(sn::encode(&reencoded, restored_terminal, testEncodeOptions()) == Error::none);
    ASSERT_TRUE(golden == reencoded);

    /* SCREEN and HISTORY keys make both sequence groups order independent. */
    std::string reversed;
    record::Writer reversed_stream;
    reversed_stream.init(&reversed);
    sn::envelope::encode(reversed_stream.writer());
    ASSERT_TRUE(sn::terminal_record::encode(&t, &reversed_stream) == sn::terminal_record::Error::none);
    ASSERT_TRUE(sn::screen::encode(t.screens.get(Key::alternate), Key::alternate, &reversed_stream) ==
                sn::screen::Error::none);
    ASSERT_TRUE(sn::screen::encode(primary, Key::primary, &reversed_stream) == sn::screen::Error::none);
    ASSERT_TRUE(sn::continuation::encode(Continuation::makeGround(), &reversed_stream) ==
                sn::continuation::EncodeError::none);
    ASSERT_TRUE(sn::checkpoint::encode(sn::checkpoint::Kind::ready, &reversed_stream) ==
                record::Writer::FinishError::none);
    ASSERT_TRUE(sn::history::encode(t.screens.get(Key::alternate), Key::alternate, &reversed_stream) ==
                sn::history::Error::none);
    ASSERT_TRUE(sn::history::encode(primary, Key::primary, &reversed_stream) == sn::history::Error::none);
    ASSERT_TRUE(sn::checkpoint::encode(sn::checkpoint::Kind::finish, &reversed_stream) ==
                record::Writer::FinishError::none);

    Reader reversed_source = Reader::fixed(reversed.data(), reversed.size());
    Decoded reversed_restored;
    ASSERT_TRUE(sn::decode(talloc(), &reversed_source, testDecodeOptions(), &reversed_restored) == Error::none);
    DecodedGuard reversed_guard = { &reversed_restored, talloc() };
    Terminal *reversed_terminal = &reversed_restored.terminal;
    ASSERT_TRUE(Key::alternate == reversed_terminal->screens.active_key);
    ASSERT_EQ((long)0, (long)reversed_terminal->screens.generation(Key::primary));
    ASSERT_EQ((long)0, (long)reversed_terminal->screens.generation(Key::alternate));
}

TEST(snapshot, complete_snapshot_restores_a_canonical_Stream_continuation) {
    TermHolder source_holder(termOpts(8, 2));
    ASSERT_TRUE(source_holder.ok);
    Terminal &source_terminal = source_holder.t;
    StreamHolder source_stream(&source_terminal, 1024);

    source_stream.s.nextSlice("A\x1b[31");
    std::string exported;
    ASSERT_TRUE(source_stream.s.writeContinuation(&exported) == TerminalStream::ContinuationError::none);
    ASSERT_TRUE(exported == "\x1b[31");

    std::string encoded;
    ASSERT_TRUE(sn::encode(&encoded, &source_terminal, bytesOptions(exported)) == Error::none);

    Reader encoded_source = Reader::fixed(encoded.data(), encoded.size());
    sn::DecodeOptions o;
    o.max_continuation_bytes = 1024;
    Decoded decoded;
    ASSERT_TRUE(sn::decode(talloc(), &encoded_source, o, &decoded) == Error::none);
    DecodedGuard decoded_guard = { &decoded, talloc() };
    ASSERT_TRUE(exported == continuationBytes(decoded.continuation));

    Terminal restored_terminal = decoded.toOwned();
    TerminalGuard restored_guard = { &restored_terminal };
    ASSERT_FALSE(decoded.has_terminal);
    StreamHolder restored_stream(&restored_terminal, 1024);

    const std::string replay = continuationBytes(decoded.continuation);
    restored_stream.s.nextSlice(replay.data(), replay.size());
    std::string reexported;
    ASSERT_TRUE(restored_stream.s.writeContinuation(&reexported) == TerminalStream::ContinuationError::none);
    ASSERT_TRUE(exported == reexported);

    /* The identical post-cut bytes may use different feed chunking without
     * changing terminal semantics. */
    source_stream.s.nextSlice("mB");
    restored_stream.s.next('m');
    restored_stream.s.next('B');
    ASSERT_TRUE(source_terminal.plainString() == restored_terminal.plainString());
    ASSERT_TRUE(source_terminal.screens.active->cursor.style_id == restored_terminal.screens.active->cursor.style_id);
}

TEST(snapshot, complete_snapshot_validates_continuation_before_writing) {
    TermHolder holder(termOpts(2, 1));
    ASSERT_TRUE(holder.ok);

    std::string destination = "prefix";
    ASSERT_TRUE(sn::encode(&destination, &holder.t, bytesOptions("\x1b[31m")) == Error::NoPendingState);
    ASSERT_TRUE(destination == "prefix");

    std::string tail = "\x1b[31";
    std::string encoded;
    ASSERT_TRUE(sn::encode(&encoded, &holder.t, bytesOptions(tail)) == Error::none);
    tail[2] = '4';

    Reader source = Reader::fixed(encoded.data(), encoded.size());
    Decoded decoded;
    ASSERT_TRUE(sn::decode(talloc(), &source, testDecodeOptions(), &decoded) == Error::none);
    DecodedGuard decoded_guard = { &decoded, talloc() };
    ASSERT_TRUE(continuationBytes(decoded.continuation) == "\x1b[31");
}

TEST(snapshot, complete_snapshot_waits_for_continuation_tracking_recovery) {
    TermHolder holder(termOpts(2, 1));
    ASSERT_TRUE(holder.ok);
    StreamHolder stream(&holder.t, 4);

    stream.s.nextSlice("\x1b[123");
    std::string unavailable;
    ASSERT_TRUE(stream.s.writeContinuation(&unavailable) == TerminalStream::ContinuationError::ContinuationUnavailable);

    /* A new replay start replaces the lost suffix and makes a later cut
     * publishable without affecting normal terminal parsing. */
    stream.s.nextSlice("\x1b[");
    std::string recovered;
    ASSERT_TRUE(stream.s.writeContinuation(&recovered) == TerminalStream::ContinuationError::none);
    ASSERT_TRUE(recovered == "\x1b[");

    std::string encoded;
    ASSERT_TRUE(sn::encode(&encoded, &holder.t, bytesOptions(recovered)) == Error::none);
}

TEST(snapshot, complete_snapshot_preserves_every_supported_continuation_cut) {
    const std::string corpora[] = {
        std::string("plain \xF0\x9F\x98\x84 utf8"),
        std::string("bad \xE0\xA0\xF0\x9F\x98\x84 utf8"),
        std::string("\x1b[1\x07;2mstyled\x1b[0m"),
        std::string("\x1b]2;window title\x1b\\text"),
        std::string("\x1bP$qm\x1b\\text"),
        std::string("\x1b_Ga=q;payload\x1b\\text"),
        std::string("\x1b_25a1;s\x1b\\text"),
        std::string("\x1b]2;first\x1b\\\x1b_Gsecond"),
        std::string("\x1b[12\x9D" "2;title\x1b\\text"),
        std::string("\x1b[12\x18text\x1b[1\x1Atext"),
    };

    for (const std::string &corpus : corpora) {
        for (size_t cut = 0; cut < corpus.size() + 1; cut++) {
            TermHolder source_holder(termOpts(20, 4));
            ASSERT_TRUE(source_holder.ok);
            Terminal &source_terminal = source_holder.t;
            StreamHolder source_stream(&source_terminal, 4096);
            source_stream.s.nextSlice(corpus.data(), cut);

            std::string cut_writer;
            ASSERT_TRUE(source_stream.s.writeContinuation(&cut_writer) == TerminalStream::ContinuationError::none);
            sn::EncodeOptions cut_options;
            cut_options.continuation =
                cut_writer.empty() ? Continuation::makeGround() : Continuation::makeBytes(cut_writer);

            std::string snapshot_bytes;
            ASSERT_TRUE(sn::encode(&snapshot_bytes, &source_terminal, cut_options) == Error::none);

            Reader snapshot_source = Reader::fixed(snapshot_bytes.data(), snapshot_bytes.size());
            sn::DecodeOptions o;
            o.max_continuation_bytes = 4096;
            Decoded decoded;
            ASSERT_TRUE(sn::decode(talloc(), &snapshot_source, o, &decoded) == Error::none);
            DecodedGuard decoded_guard = { &decoded, talloc() };
            Terminal restored_terminal = decoded.toOwned();
            TerminalGuard restored_guard = { &restored_terminal };
            StreamHolder restored_stream(&restored_terminal, 4096);
            if (decoded.continuation.tag == Continuation::Tag::bytes) {
                const std::string bytes = continuationBytes(decoded.continuation);
                restored_stream.s.nextSlice(bytes.data(), bytes.size());
            }

            std::string reexport_writer;
            ASSERT_TRUE(restored_stream.s.writeContinuation(&reexport_writer) ==
                        TerminalStream::ContinuationError::none);
            ASSERT_TRUE(cut_writer == reexport_writer);

            source_stream.s.nextSlice(corpus.data() + cut, corpus.size() - cut);
            size_t offset = cut;
            size_t partition = cut + corpus.size() + 1;
            while (offset < corpus.size()) {
                partition = partition * 1664525u + 1013904223u;
                const size_t want = 1 + partition % 7;
                const size_t len = want < corpus.size() - offset ? want : corpus.size() - offset;
                restored_stream.s.nextSlice(corpus.data() + offset, len);
                offset += len;
            }

            std::string source_final_writer;
            ASSERT_TRUE(source_stream.s.writeContinuation(&source_final_writer) ==
                        TerminalStream::ContinuationError::none);
            std::string restored_final_writer;
            ASSERT_TRUE(restored_stream.s.writeContinuation(&restored_final_writer) ==
                        TerminalStream::ContinuationError::none);
            ASSERT_TRUE(source_final_writer == restored_final_writer);

            sn::EncodeOptions final_options;
            final_options.continuation = source_final_writer.empty() ? Continuation::makeGround()
                                                                     : Continuation::makeBytes(source_final_writer);
            std::string source_final_snapshot;
            ASSERT_TRUE(sn::encode(&source_final_snapshot, &source_terminal, final_options) == Error::none);
            std::string restored_final_snapshot;
            ASSERT_TRUE(sn::encode(&restored_final_snapshot, &restored_terminal, final_options) == Error::none);
            ASSERT_TRUE(source_final_snapshot == restored_final_snapshot);
        }
    }
}

TEST(snapshot, complete_snapshot_preserves_Kitty_virtual_placeholders) {
    typedef wisp::terminal::kitty::graphics_storage::ImageStorage::Placement Placement;
    typedef wisp::terminal::kitty::graphics_image::Image Image;

    TermHolder holder(termOpts(2, 1));
    ASSERT_TRUE(holder.ok);
    Terminal &t = holder.t;

    /* Register a real virtual placement, then write its grid representation:
     * U+10EEEE followed by row and column diacritics. The image and placement
     * registry is intentionally omitted, but the grid content must remain
     * decodable. */
    Image image;
    image.id = 1;
    ASSERT_TRUE(t.screens.active->kitty_images.addImage(talloc(), t.screens.active, image));
    Placement placement;
    placement.location = Placement::Location();
    placement.columns = 1;
    placement.rows = 1;
    ASSERT_TRUE(t.screens.active->kitty_images.addPlacement(talloc(), t.screens.active, 1, 0, placement));
    ASSERT_TRUE(t.setAttribute(wisp::terminal::sgr::Attribute::makeIndex(wisp::terminal::sgr::Attribute::Tag::fg_256,
                                                                         1)) == PageList::IncreaseCapacityError::none);
    ASSERT_TRUE(t.printString("\xF4\x8E\xBB\xAE\xCC\x85\xCC\x85"));
    const Maybe<PageList::Cell> source_cell = t.screens.active->pages.getCell(wisp::vt::point::Point::screen());
    ASSERT_TRUE(source_cell.has);
    ASSERT_TRUE(wisp::terminal::kitty::graphics::graphics_unicode::placeholder == source_cell.value.cell->codepoint());
    ASSERT_TRUE(source_cell.value.row->kitty_virtual_placeholder());

    std::string encoded;
    ASSERT_TRUE(sn::encode(&encoded, &t, testEncodeOptions()) == Error::none);

    Reader encoded_source = Reader::fixed(encoded.data(), encoded.size());
    Decoded restored;
    ASSERT_TRUE(sn::decode(talloc(), &encoded_source, testDecodeOptions(), &restored) == Error::none);
    DecodedGuard restored_guard = { &restored, talloc() };
    Terminal *restored_terminal = &restored.terminal;

    const Maybe<PageList::Cell> restored_cell =
        restored_terminal->screens.active->pages.getCell(wisp::vt::point::Point::screen());
    ASSERT_TRUE(restored_cell.has);
    ASSERT_TRUE(wisp::terminal::kitty::graphics::graphics_unicode::placeholder == restored_cell.value.cell->codepoint());
    ASSERT_TRUE(restored_cell.value.cell->hasGrapheme());
    ASSERT_TRUE(restored_cell.value.row->kitty_virtual_placeholder());
    ASSERT_EQ((long)0, (long)restored_terminal->screens.active->kitty_images.images.count());
    ASSERT_EQ((long)0, (long)restored_terminal->screens.active->kitty_images.placements.count());
}

TEST(snapshot, complete_snapshot_encoding_streams_from_the_current_writer_position) {
    TermHolder holder(termOpts(2, 1));
    ASSERT_TRUE(holder.ok);
    Terminal &t = holder.t;

    /* Encoding begins with this call's envelope, independent of bytes that
     * were already present in the destination. */
    std::string nonempty = "prefix";
    const size_t snapshot_offset = nonempty.size();
    ASSERT_TRUE(sn::encode(&nonempty, &t, testEncodeOptions()) == Error::none);
    ASSERT_TRUE(nonempty.substr(0, snapshot_offset) == "prefix");
    Reader appended_source = Reader::fixed(nonempty.data() + snapshot_offset, nonempty.size() - snapshot_offset);
    Decoded appended;
    ASSERT_TRUE(sn::decode(talloc(), &appended_source, testDecodeOptions(), &appended) == Error::none);
    appended.deinit(talloc());

    /* Payload validation happens in the record-local scratch allocation. The
     * already-streamed envelope remains, but no partial TERMINAL is emitted. */
    t.colors.palette.current[7] = wisp::terminal::RGB(1, 2, 3);
    std::string destination = "prefix";
    ASSERT_TRUE(sn::encode(&destination, &t, testEncodeOptions()) == Error::InvalidPalette);
    std::string expected_envelope;
    sn::envelope::encode(&expected_envelope);
    ASSERT_TRUE(expected_envelope == destination.substr(strlen("prefix")));
}

TEST(snapshot, complete_snapshot_rejects_ordering_and_invalid_markers) {
    TermHolder holder(termOpts(2, 1));
    ASSERT_TRUE(holder.ok);
    Terminal &t = holder.t;
    TerminalScreen *primary = t.screens.get(Key::primary);

    auto expectDecodeError = [](const std::string &bytes, Error expected) -> bool {
        Reader source = Reader::fixed(bytes.data(), bytes.size());
        Decoded decoded;
        const Error err = sn::decode(talloc(), &source, testDecodeOptions(), &decoded);
        if (err == Error::none) decoded.deinit(talloc());
        return err == expected;
    };

    /* Old version-1 snapshots proceeded directly from SCREEN to READY. READY
     * is individually valid here, but CONTINUATION is now required first. */
    {
        std::string old_order;
        record::Writer s;
        s.init(&old_order);
        sn::envelope::encode(s.writer());
        sn::terminal_record::encode(&t, &s);
        sn::screen::encode(primary, Key::primary, &s);
        sn::checkpoint::encode(sn::checkpoint::Kind::ready, &s);
        ASSERT_TRUE(expectDecodeError(old_order, Error::UnexpectedRecordTag));
    }

    /* A second CONTINUATION is rejected where READY is required. */
    {
        std::string duplicate_continuation;
        record::Writer s;
        s.init(&duplicate_continuation);
        sn::envelope::encode(s.writer());
        sn::terminal_record::encode(&t, &s);
        sn::screen::encode(primary, Key::primary, &s);
        sn::continuation::encode(Continuation::makeGround(), &s);
        sn::continuation::encode(Continuation::makeGround(), &s);
        ASSERT_TRUE(expectDecodeError(duplicate_continuation, Error::UnexpectedRecordTag));
    }

    /* HISTORY is individually valid here, but the full decoder requires the
     * primary SCREEN before READY. */
    {
        std::string reordered;
        record::Writer s;
        s.init(&reordered);
        sn::envelope::encode(s.writer());
        sn::terminal_record::encode(&t, &s);
        sn::history::encode(primary, Key::primary, &s);
        ASSERT_TRUE(expectDecodeError(reordered, Error::UnexpectedRecordTag));
    }

    /* Construct a correctly framed READY with a nonempty payload so the full
     * driver rejects a marker that does not match the required empty shape. */
    {
        std::string invalid_ready;
        record::Writer s;
        s.init(&invalid_ready);
        sn::envelope::encode(s.writer());
        sn::terminal_record::encode(&t, &s);
        sn::screen::encode(primary, Key::primary, &s);
        sn::continuation::encode(Continuation::makeGround(), &s);
        std::string *ready_payload = s.begin(record::Tag::ready);
        ready_payload->push_back(0);
        ASSERT_TRUE(s.finish() == record::Writer::FinishError::none);
        ASSERT_TRUE(expectDecodeError(invalid_ready, Error::PayloadNotExhausted));
    }

    /* A SCREEN key must name one of the slots declared by TERMINAL. */
    {
        std::string undeclared;
        record::Writer s;
        s.init(&undeclared);
        sn::envelope::encode(s.writer());
        sn::terminal_record::encode(&t, &s);
        sn::screen::encode(primary, Key::alternate, &s);
        ASSERT_TRUE(expectDecodeError(undeclared, Error::UnexpectedScreenKey));
    }

    /* HISTORY sequences are also routed by key, which must name a declared
     * screen even when the sequence contains no PAGE records. */
    {
        std::string undeclared_history;
        record::Writer s;
        s.init(&undeclared_history);
        sn::envelope::encode(s.writer());
        sn::terminal_record::encode(&t, &s);
        sn::screen::encode(primary, Key::primary, &s);
        sn::continuation::encode(Continuation::makeGround(), &s);
        sn::checkpoint::encode(sn::checkpoint::Kind::ready, &s);
        sn::history::encode(primary, Key::alternate, &s);
        ASSERT_TRUE(expectDecodeError(undeclared_history, Error::UnexpectedHistoryKey));
    }

    /* The declared count cannot be satisfied by repeating the same key. */
    bool ok = false;
    t.switchScreen(Key::alternate, &ok);
    ASSERT_TRUE(ok);
    {
        std::string duplicate;
        record::Writer s;
        s.init(&duplicate);
        sn::envelope::encode(s.writer());
        sn::terminal_record::encode(&t, &s);
        sn::screen::encode(primary, Key::primary, &s);
        sn::screen::encode(primary, Key::primary, &s);
        ASSERT_TRUE(expectDecodeError(duplicate, Error::DuplicateScreen));
    }

    /* The declared count cannot be satisfied by repeating one HISTORY key. */
    {
        std::string duplicate_history;
        record::Writer s;
        s.init(&duplicate_history);
        sn::envelope::encode(s.writer());
        sn::terminal_record::encode(&t, &s);
        sn::screen::encode(primary, Key::primary, &s);
        sn::screen::encode(t.screens.get(Key::alternate), Key::alternate, &s);
        sn::continuation::encode(Continuation::makeGround(), &s);
        sn::checkpoint::encode(sn::checkpoint::Kind::ready, &s);
        sn::history::encode(primary, Key::primary, &s);
        sn::history::encode(primary, Key::primary, &s);
        ASSERT_TRUE(expectDecodeError(duplicate_history, Error::DuplicateHistory));
    }
}

TEST(snapshot, complete_snapshot_leaves_continuation_bytes_unread) {
    TermHolder holder(termOpts(2, 1));
    ASSERT_TRUE(holder.ok);

    std::string encoded;
    ASSERT_TRUE(sn::encode(&encoded, &holder.t, testEncodeOptions()) == Error::none);
    const size_t snapshot_len = encoded.size();
    encoded += "pty";

    Reader source = Reader::fixed(encoded.data(), encoded.size());
    Decoded restored;
    ASSERT_TRUE(sn::decode(talloc(), &source, testDecodeOptions(), &restored) == Error::none);
    DecodedGuard restored_guard = { &restored, talloc() };

    char trailing[3];
    ReadError rerr = ReadError::none;
    ASSERT_TRUE(source.readSliceAll(trailing, 3, &rerr));
    ASSERT_TRUE(std::string(trailing, 3) == "pty");

    Reader exact_source = Reader::fixed(encoded.data(), encoded.size());
    Decoded exact;
    ASSERT_TRUE(sn::decodeExact(talloc(), &exact_source, testDecodeOptions(), &exact) == Error::TrailingData);

    Reader bounded_source = Reader::fixed(encoded.data(), snapshot_len);
    Decoded bounded;
    ASSERT_TRUE(sn::decodeExact(talloc(), &bounded_source, testDecodeOptions(), &bounded) == Error::none);
    DecodedGuard bounded_guard = { &bounded, talloc() };
}

TEST(snapshot, complete_snapshots_decode_sequentially_from_one_reader) {
    TermHolder holder(termOpts(2, 1));
    ASSERT_TRUE(holder.ok);

    std::string encoded;
    ASSERT_TRUE(sn::encode(&encoded, &holder.t, testEncodeOptions()) == Error::none);
    ASSERT_TRUE(sn::encode(&encoded, &holder.t, testEncodeOptions()) == Error::none);

    Reader source = Reader::fixed(encoded.data(), encoded.size());
    Decoded first;
    ASSERT_TRUE(sn::decode(talloc(), &source, testDecodeOptions(), &first) == Error::none);
    DecodedGuard first_guard = { &first, talloc() };
    Decoded second;
    ASSERT_TRUE(sn::decode(talloc(), &source, testDecodeOptions(), &second) == Error::none);
    DecodedGuard second_guard = { &second, talloc() };
    uint8_t byte = 0;
    ReadError rerr = ReadError::none;
    ASSERT_FALSE(source.takeByte(&byte, &rerr));
    ASSERT_TRUE(rerr == ReadError::EndOfStream);
}

static void exerciseAllocationFailures(const std::string &bytes) {
    zigstd::FailingAllocator baseline(talloc(), SIZE_MAX);
    Reader baseline_source = Reader::fixed(bytes.data(), bytes.size());
    Decoded baseline_decoded;
    ASSERT_TRUE(sn::decode(baseline.allocator(), &baseline_source, testDecodeOptions(), &baseline_decoded) ==
                Error::none);
    baseline_decoded.deinit(baseline.allocator());
    const size_t allocation_count = baseline.alloc_index;
    ASSERT_TRUE(allocation_count > 0);

    bool saw_out_of_memory = false;
    for (size_t fail_index = 0; fail_index < allocation_count; fail_index++) {
        zigstd::FailingAllocator failing(talloc(), fail_index);
        Reader source = Reader::fixed(bytes.data(), bytes.size());
        Decoded decoded;
        const Error err = sn::decode(failing.allocator(), &source, testDecodeOptions(), &decoded);
        if (err == Error::OutOfMemory) {
            saw_out_of_memory = true;
            continue;
        }
        ASSERT_TRUE(err == Error::none);
        decoded.deinit(failing.allocator());
    }
    ASSERT_TRUE(saw_out_of_memory);
}

TEST(snapshot, complete_snapshot_decode_allocation_failures_are_transactional) {
    /* The complete golden exercises Terminal, both screens, and history. */
    std::string golden;
    ASSERT_TRUE(loadFixture("complete-v1.hex", &golden));
    exerciseAllocationFailures(golden);

    /* A non-ground component additionally exercises owned continuation bytes. */
    TermHolder holder(termOpts(2, 1));
    ASSERT_TRUE(holder.ok);
    std::string encoded;
    ASSERT_TRUE(sn::encode(&encoded, &holder.t, bytesOptions("\x1b[31")) == Error::none);
    exerciseAllocationFailures(encoded);
}

TEST(snapshot, incremental_decode_restores_a_renderable_terminal_at_READY) {
    std::string golden;
    ASSERT_TRUE(loadFixture("complete-v1.hex", &golden));

    /* Trailing transport bytes must remain unread after FINISH. */
    std::string transport = golden;
    transport += "pty";

    Reader source = Reader::fixed(transport.data(), transport.size());
    Decoder decoder = Decoder::init(&source);
    Decoded decoded;
    ASSERT_TRUE(decoder.ready(talloc(), testDecodeOptions(), &decoded) == Error::none);
    DecodedGuard decoded_guard = { &decoded, talloc() };

    /* READY restored the complete interactive state, so the Terminal can
     * move to its final address before any history has been decoded. */
    Terminal restored = decoded.toOwned();
    TerminalGuard restored_guard = { &restored };
    ASSERT_TRUE(Key::alternate == restored.screens.active_key);
    ASSERT_TRUE(std::string(restored.getTitle()) == "complete snapshot");

    /* The advisory extents size scrollback UI immediately even though only
     * the resident SCREEN pages exist. The alternate screen's extent is
     * entirely resident overlap, so it must match its live row count. */
    TerminalScreen *primary = restored.screens.get(Key::primary);
    TerminalScreen *alternate = restored.screens.get(Key::alternate);
    ASSERT_EQ((long)3, (long)primary->pages.scrollbar().total);
    ASSERT_TRUE(decoded.history_rows_present[(size_t)Key::primary]);
    ASSERT_EQ((long)4, (long)decoded.history_rows[(size_t)Key::primary]);
    ASSERT_TRUE(decoded.history_rows_present[(size_t)Key::alternate]);
    ASSERT_EQ((long)(alternate->pages.scrollbar().total - restored.rows),
              (long)decoded.history_rows[(size_t)Key::alternate]);

    /* Each call prepends exactly one history page, newest first. */
    Decoder::Progress progress;
    bool has = false;
    ASSERT_TRUE(decoder.next(talloc(), &restored, &progress, &has) == Error::none);
    ASSERT_TRUE(has);
    ASSERT_TRUE(progress == (Decoder::Progress{Key::primary, 2, 1}));
    ASSERT_EQ((long)5, (long)primary->pages.scrollbar().total);
    ASSERT_EQ((long)'B', (long)testTopLeftCodepoint(primary));

    ASSERT_TRUE(decoder.next(talloc(), &restored, &progress, &has) == Error::none);
    ASSERT_TRUE(has);
    ASSERT_TRUE(progress == (Decoder::Progress{Key::primary, 2, 0}));
    ASSERT_EQ((long)7, (long)primary->pages.scrollbar().total);
    ASSERT_EQ((long)'A', (long)testTopLeftCodepoint(primary));

    /* The alternate screen's empty sequence produces no event: the next
     * call validates FINISH, and further calls remain null. */
    ASSERT_TRUE(decoder.next(talloc(), &restored, &progress, &has) == Error::none);
    ASSERT_FALSE(has);
    ASSERT_TRUE(decoder.next(talloc(), &restored, &progress, &has) == Error::none);
    ASSERT_FALSE(has);
    char trailing[3];
    ReadError rerr = ReadError::none;
    ASSERT_TRUE(source.readSliceAll(trailing, 3, &rerr));
    ASSERT_TRUE(std::string(trailing, 3) == "pty");

    /* The incremental path restores the same complete terminal as the
     * one-shot decoder, byte for byte. */
    std::string reencoded;
    ASSERT_TRUE(sn::encode(&reencoded, &restored, testEncodeOptions()) == Error::none);
    ASSERT_TRUE(golden == reencoded);
}

TEST(snapshot, incremental_next_reports_invalid_decoder_states) {
    Reader source = Reader::fixed("", 0);
    Decoder decoder = Decoder::init(&source);
    TermHolder terminal_value(termOpts(1, 1));
    ASSERT_TRUE(terminal_value.ok);

    Decoder::Progress progress;
    bool has = false;
    ASSERT_TRUE(decoder.next(talloc(), &terminal_value.t, &progress, &has) == Error::DecoderNotReady);
}

TEST(snapshot, incremental_decode_applies_history_below_live_PTY_output) {
    TermHolder holder(unlimitedOpts(2, 3));
    ASSERT_TRUE(testHistoryTerminal(2, &holder));
    std::string encoded;
    ASSERT_TRUE(sn::encode(&encoded, &holder.t, testEncodeOptions()) == Error::none);

    Reader source = Reader::fixed(encoded.data(), encoded.size());
    Decoder decoder = Decoder::init(&source);
    Decoded decoded;
    ASSERT_TRUE(decoder.ready(talloc(), testDecodeOptions(), &decoded) == Error::none);
    DecodedGuard decoded_guard = { &decoded, talloc() };
    Terminal restored = decoded.toOwned();
    TerminalGuard restored_guard = { &restored };

    /* The terminal is interactive at READY: post-cut PTY bytes append to
     * the bottom while snapshot history is still pending.
     *
     * Wisp: see the header -- TerminalStream stands in for vtStream(). */
    {
        StreamHolder live(&restored, 1024);
        live.s.nextSlice("1\r\n2\r\n3\r\n4\r\n5\r\n");
    }
    TerminalScreen *primary = restored.screens.get(Key::primary);
    const size_t live_total = primary->pages.scrollbar().total;
    ASSERT_TRUE(live_total > 3);

    /* History still lands above both the snapshot content and the live
     * output, preserving complete scrollback ordering. */
    while (true) {
        Decoder::Progress progress;
        bool has = false;
        ASSERT_TRUE(decoder.next(talloc(), &restored, &progress, &has) == Error::none);
        if (!has) break;
        ASSERT_TRUE(Key::primary == progress.key);
        ASSERT_EQ((long)2, (long)progress.rows);
    }
    ASSERT_EQ((long)(live_total + 4), (long)primary->pages.scrollbar().total);
    ASSERT_EQ((long)'A', (long)testTopLeftCodepoint(primary));
    PageList::Node *newest_history = primary->pages.getTopLeft(wisp::vt::point::Tag::screen).node->next;
    ASSERT_TRUE(newest_history != nullptr);
    ASSERT_EQ((long)'B', (long)newest_history->page()->getRowAndCell(0, 0).cell->codepoint());

    /* The page below the applied history is the snapshot's active page,
     * whose top-left cell the live output already overwrote. */
    ASSERT_TRUE(newest_history->next != nullptr);
    ASSERT_EQ((long)'1', (long)newest_history->next->page()->getRowAndCell(0, 0).cell->codepoint());
}

TEST(snapshot, incremental_decode_discards_history_for_screens_changed_since_READY) {
    TermHolder holder(termOpts(2, 3));
    ASSERT_TRUE(holder.ok);
    Terminal &t = holder.t;
    bool ok = false;
    t.switchScreen(Key::alternate, &ok);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(t.printString("alt"));
    t.switchScreen(Key::primary, &ok);
    ASSERT_TRUE(ok);

    /* The encoder never produces alternate-screen history, but the format
     * permits it, so the decoder must stay stream-aligned even when the
     * pages can no longer be applied. Order independence lets the discarded
     * sequence come first. */
    std::string crafted;
    record::Writer stream;
    stream.init(&crafted);
    sn::envelope::encode(stream.writer());
    ASSERT_TRUE(sn::terminal_record::encode(&t, &stream) == sn::terminal_record::Error::none);
    ASSERT_TRUE(sn::screen::encode(t.screens.get(Key::primary), Key::primary, &stream) == sn::screen::Error::none);
    ASSERT_TRUE(sn::screen::encode(t.screens.get(Key::alternate), Key::alternate, &stream) ==
                sn::screen::Error::none);
    sn::continuation::encode(Continuation::makeGround(), &stream);
    sn::checkpoint::encode(sn::checkpoint::Kind::ready, &stream);
    {
        std::string *payload = stream.begin(record::Tag::history);
        sn::history::Header h;
        h.key = Key::alternate;
        h.page_count = 1;
        h.encode(payload);
        ASSERT_TRUE(stream.finish() == record::Writer::FinishError::none);
    }
    ASSERT_TRUE(sn::page::encode(t.screens.get(Key::alternate)
                                     ->pages.getTopLeft(wisp::vt::point::Tag::screen)
                                     .node->pageAssumeResident(),
                                 &stream) == sn::page::Error::none);
    ASSERT_TRUE(sn::history::encode(t.screens.get(Key::primary), Key::primary, &stream) == sn::history::Error::none);
    sn::checkpoint::encode(sn::checkpoint::Kind::finish, &stream);

    /* An unchanged declared screen accepts crafted history, establishing
     * the baseline that the following runs degrade from. */
    {
        Reader source = Reader::fixed(crafted.data(), crafted.size());
        Decoder decoder = Decoder::init(&source);
        Decoded decoded;
        ASSERT_TRUE(decoder.ready(talloc(), testDecodeOptions(), &decoded) == Error::none);
        DecodedGuard decoded_guard = { &decoded, talloc() };
        Terminal *restored = &decoded.terminal;

        Decoder::Progress progress;
        bool has = false;
        ASSERT_TRUE(decoder.next(talloc(), restored, &progress, &has) == Error::none);
        ASSERT_TRUE(has);
        ASSERT_TRUE(Key::alternate == progress.key);
        ASSERT_TRUE(progress.rows > 0);
        ASSERT_TRUE(decoder.next(talloc(), restored, &progress, &has) == Error::none);
        ASSERT_FALSE(has);
        ASSERT_EQ((long)2, (long)restored->screens.get(Key::alternate)->pages.totalPages());
    }

    /* A screen removed since READY discards its history. FINISH still
     * validates, proving discarded records leave the stream aligned. */
    {
        Reader source = Reader::fixed(crafted.data(), crafted.size());
        Decoder decoder = Decoder::init(&source);
        Decoded decoded;
        ASSERT_TRUE(decoder.ready(talloc(), testDecodeOptions(), &decoded) == Error::none);
        DecodedGuard decoded_guard = { &decoded, talloc() };
        Terminal *restored = &decoded.terminal;
        restored->screens.remove(talloc(), Key::alternate);

        Decoder::Progress progress;
        bool has = false;
        ASSERT_TRUE(decoder.next(talloc(), restored, &progress, &has) == Error::none);
        ASSERT_TRUE(has);
        ASSERT_TRUE(progress == (Decoder::Progress{Key::alternate, 0, 0}));
        ASSERT_TRUE(decoder.next(talloc(), restored, &progress, &has) == Error::none);
        ASSERT_FALSE(has);
    }

    /* A screen removed and recreated since READY has a new generation, so
     * stale history never mixes into its replacement's content. */
    {
        Reader source = Reader::fixed(crafted.data(), crafted.size());
        Decoder decoder = Decoder::init(&source);
        Decoded decoded;
        ASSERT_TRUE(decoder.ready(talloc(), testDecodeOptions(), &decoded) == Error::none);
        DecodedGuard decoded_guard = { &decoded, talloc() };
        Terminal *restored = &decoded.terminal;
        restored->screens.remove(talloc(), Key::alternate);
        restored->switchScreen(Key::alternate, &ok);
        ASSERT_TRUE(ok);

        Decoder::Progress progress;
        bool has = false;
        ASSERT_TRUE(decoder.next(talloc(), restored, &progress, &has) == Error::none);
        ASSERT_TRUE(has);
        ASSERT_TRUE(Key::alternate == progress.key);
        ASSERT_EQ((long)0, (long)progress.rows);
        ASSERT_TRUE(decoder.next(talloc(), restored, &progress, &has) == Error::none);
        ASSERT_FALSE(has);
        ASSERT_EQ((long)1, (long)restored->screens.get(Key::alternate)->pages.totalPages());
    }
}

TEST(snapshot, incremental_decode_discards_history_after_a_resize) {
    TermHolder holder(unlimitedOpts(2, 3));
    ASSERT_TRUE(testHistoryTerminal(2, &holder));
    std::string encoded;
    ASSERT_TRUE(sn::encode(&encoded, &holder.t, testEncodeOptions()) == Error::none);

    Reader source = Reader::fixed(encoded.data(), encoded.size());
    Decoder decoder = Decoder::init(&source);
    Decoded decoded;
    ASSERT_TRUE(decoder.ready(talloc(), testDecodeOptions(), &decoded) == Error::none);
    DecodedGuard decoded_guard = { &decoded, talloc() };
    Terminal restored = decoded.toOwned();
    TerminalGuard restored_guard = { &restored };

    /* Encoded pages assume the READY width. After a live resize reflows the
     * restored screen, pending history is consumed but never applied. */
    ASSERT_TRUE(restored.resize(talloc(), Terminal::Resize(3, 3)) == Terminal::ResizeError::none);
    TerminalScreen *primary = restored.screens.get(Key::primary);
    const size_t resized_total = primary->pages.scrollbar().total;

    size_t pages = 0;
    while (true) {
        Decoder::Progress progress;
        bool has = false;
        ASSERT_TRUE(decoder.next(talloc(), &restored, &progress, &has) == Error::none);
        if (!has) break;
        ASSERT_TRUE(Key::primary == progress.key);
        ASSERT_EQ((long)0, (long)progress.rows);
        pages += 1;
    }
    ASSERT_EQ((long)2, (long)pages);
    ASSERT_EQ((long)resized_total, (long)primary->pages.scrollbar().total);
}

TEST(snapshot, decode_drops_history_beyond_the_declared_scrollback_limits) {
    /* Declare a byte limit the complete history cannot satisfy. The limit
     * is validated at page granularity on decode, so the snapshot itself
     * remains well formed and fully validated. */
    TermHolder holder(unlimitedOpts(2, 3));
    ASSERT_TRUE(testHistoryTerminal(3, &holder));
    holder.t.screens.get(Key::primary)->pages.limits.set(PageList::Limits::Key::bytes, Maybe<size_t>((size_t)1));
    std::string encoded;
    ASSERT_TRUE(sn::encode(&encoded, &holder.t, testEncodeOptions()) == Error::none);

    /* Incremental: the newest page fits the effective floor, the next page
     * exceeds it, and one drop discards the remainder of the sequence so no
     * ordering gap can form. */
    {
        Reader source = Reader::fixed(encoded.data(), encoded.size());
        Decoder decoder = Decoder::init(&source);
        Decoded decoded;
        ASSERT_TRUE(decoder.ready(talloc(), testDecodeOptions(), &decoded) == Error::none);
        DecodedGuard decoded_guard = { &decoded, talloc() };
        Terminal *restored = &decoded.terminal;

        size_t rows[3] = {99, 99, 99};
        size_t pages = 0;
        while (true) {
            Decoder::Progress progress;
            bool has = false;
            ASSERT_TRUE(decoder.next(talloc(), restored, &progress, &has) == Error::none);
            if (!has) break;
            ASSERT_TRUE(pages < 3);
            rows[pages] = progress.rows;
            pages += 1;
        }
        ASSERT_EQ((long)3, (long)pages);
        ASSERT_EQ((long)2, (long)rows[0]);
        ASSERT_EQ((long)0, (long)rows[1]);
        ASSERT_EQ((long)0, (long)rows[2]);

        TerminalScreen *primary = restored->screens.get(Key::primary);
        ASSERT_EQ((long)2, (long)primary->pages.totalPages());
        ASSERT_EQ((long)'C', (long)testTopLeftCodepoint(primary));
    }

    /* The one-shot decoder degrades identically instead of rejecting the
     * snapshot. */
    {
        Reader source = Reader::fixed(encoded.data(), encoded.size());
        Decoded decoded;
        ASSERT_TRUE(sn::decode(talloc(), &source, testDecodeOptions(), &decoded) == Error::none);
        DecodedGuard decoded_guard = { &decoded, talloc() };
        TerminalScreen *primary = decoded.terminal.screens.get(Key::primary);
        ASSERT_EQ((long)2, (long)primary->pages.totalPages());
        ASSERT_EQ((long)'C', (long)testTopLeftCodepoint(primary));
    }
}

TEST(snapshot, incremental_decode_failure_leaves_applied_history_usable) {
    TermHolder holder(unlimitedOpts(2, 3));
    ASSERT_TRUE(testHistoryTerminal(2, &holder));
    std::string encoded;
    ASSERT_TRUE(sn::encode(&encoded, &holder.t, testEncodeOptions()) == Error::none);

    /* Truncate FINISH. READY and both history pages decode, then the
     * failure surfaces on the call that would have completed the snapshot. */
    Reader source = Reader::fixed(encoded.data(), encoded.size() - 1);
    Decoder decoder = Decoder::init(&source);
    Decoded decoded;
    ASSERT_TRUE(decoder.ready(talloc(), testDecodeOptions(), &decoded) == Error::none);
    DecodedGuard decoded_guard = { &decoded, talloc() };
    Terminal restored = decoded.toOwned();
    TerminalGuard restored_guard = { &restored };

    Decoder::Progress progress;
    bool has = false;
    for (size_t i = 0; i < 2; i++) {
        ASSERT_TRUE(decoder.next(talloc(), &restored, &progress, &has) == Error::none);
        ASSERT_TRUE(has);
        ASSERT_EQ((long)2, (long)progress.rows);
    }
    ASSERT_TRUE(decoder.next(talloc(), &restored, &progress, &has) == Error::EndOfStream);
    ASSERT_TRUE(decoder.next(talloc(), &restored, &progress, &has) == Error::DecoderFailed);

    /* The transferred terminal was never owned by the decoder: it remains
     * valid with the contiguous history prefix that did apply; only the
     * incomplete snapshot stream is lost. */
    TerminalScreen *primary = restored.screens.get(Key::primary);
    ASSERT_EQ((long)3, (long)primary->pages.totalPages());
    ASSERT_EQ((long)'A', (long)testTopLeftCodepoint(primary));
    primary->pages.assertIntegrity();
    primary->assertIntegrity();
}

TEST(snapshot, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
