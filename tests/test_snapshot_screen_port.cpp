/* Transliterated from the test blocks in Ghostty
 * src/terminal/snapshot/screen.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include <string.h>

#include <string>
#include <vector>

#include "test_helpers.h"
#include "snapshot_fixture.hpp"
#include "../src/terminal/snapshot/screen.hpp"
#include "../src/vt/compress/page.hpp"
#include "../src/zigstd/allocator.hpp"
#include "../src/zigstd/reader.hpp"

using namespace wisp;

namespace ss = wisp::terminal::snapshot::screen;
namespace sp = wisp::terminal::snapshot::page;
namespace grid = wisp::terminal::snapshot::grid;
namespace record = wisp::terminal::snapshot::record;
namespace io = wisp::terminal::snapshot::io;
namespace fixture = wisp::test_fixture;
namespace tp = wisp::vt::page;

typedef zigstd::Reader Reader;
typedef zigstd::LimitedReader LimitedReader;
typedef zigstd::ReadError ReadError;
typedef ss::Header Header;
typedef ss::SavedCursor SavedCursor;
typedef ss::Error Error;
typedef ss::TerminalScreen TerminalScreen;
typedef ss::TerminalScreenKey Key;
typedef ss::CharsetState CharsetState;
typedef ss::SemanticClick SemanticClick;
typedef ss::Slots Slots;
typedef ss::Charset Charset;
typedef ss::ProtectedMode ProtectedMode;
typedef ss::TerminalStyle TerminalStyle;
typedef tp::Cell TerminalCell;
typedef wisp::vt::PageList PageList;
typedef wisp::vt::point::Point Point;
typedef wisp::vt::point::Tag PointTag;

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

static TerminalScreen::Options opts(uint16_t cols, uint16_t rows, wisp::vt::Maybe<size_t> max_scrollback_bytes) {
    return TerminalScreen::Options(cols, rows, max_scrollback_bytes);
}

static wisp::vt::Maybe<size_t> noScrollback() { return wisp::vt::Maybe<size_t>((size_t)0); }
static wisp::vt::Maybe<size_t> unlimited() { return wisp::vt::Maybe<size_t>(); }

/* Wisp: `var screen = try TerminalScreen.init(...); defer screen.deinit();` */
struct ScreenHolder {
    TerminalScreen s;
    bool ok = false;
    explicit ScreenHolder(const TerminalScreen::Options &o) { ok = TerminalScreen::init(talloc(), o, &s); }
    ~ScreenHolder() {
        if (ok) s.deinit();
    }
};

/* Wisp: `var decoded = try decode(...); defer decoded.deinit();` */
struct DecodedHolder {
    ss::Decoded d;
    bool ok = false;
    ~DecodedHolder() {
        if (ok) d.deinit();
    }
};

static SemanticClick clickCl(wisp::terminal::osc::semantic_prompt::Click c) {
    SemanticClick s = ss::semanticClickNone();
    s.tag = SemanticClick::Kind::cl;
    s.cl = c;
    return s;
}

static CharsetState testCharsetState() {
    CharsetState result;
    result.gl = Slots::G1;
    result.gr = Slots::G3;
    result.single_shift = wisp::vt::Maybe<Slots>(Slots::G2);
    result.charsets.set(Slots::G0, Charset::utf8);
    result.charsets.set(Slots::G1, Charset::ascii);
    result.charsets.set(Slots::G2, Charset::british);
    result.charsets.set(Slots::G3, Charset::dec_special);
    return result;
}

static TerminalStyle testPen() {
    TerminalStyle pen;
    pen.fg_color = TerminalStyle::Color::none();
    pen.bg_color = TerminalStyle::Color::makePalette(0x7f);
    wisp::terminal::RGB rgb;
    rgb.r = 0x12;
    rgb.g = 0x34;
    rgb.b = 0x56;
    pen.underline_color = TerminalStyle::Color::makeRgb(rgb);
    pen.flags.bold = true;
    pen.flags.italic = true;
    pen.flags.faint = true;
    pen.flags.blink = true;
    pen.flags.inverse = true;
    pen.flags.invisible = true;
    pen.flags.strikethrough = true;
    pen.flags.overline = true;
    pen.flags.underline = wisp::terminal::sgr::Attribute::Underline::curly;
    return pen;
}

static ss::KittyKeyboard testKitty() {
    ss::KittyKeyboard k;
    k.index = 7;
    k.flags[0].disambiguate = true;
    k.flags[1].report_events = true;
    k.flags[2].report_alternates = true;
    k.flags[3].report_all = true;
    k.flags[4].report_associated = true;
    k.flags[5].disambiguate = true;
    k.flags[5].report_events = true;
    k.flags[5].report_alternates = true;
    k.flags[5].report_all = true;
    k.flags[5].report_associated = true;
    k.flags[7].disambiguate = true;
    k.flags[7].report_associated = true;
    return k;
}

static Header testHeader() {
    Header h;
    h.key = Key::alternate;
    h.page_count = 0x0203;
    h.history_rows = 0x0102030405060708ull;
    h.cursor_x = 0x0405;
    h.cursor_y = 0x0607;
    h.cursor_style = TerminalScreen::CursorStyle::block_hollow;
    h.cursor_flags.pending_wrap = true;
    h.cursor_flags.semantic_content = TerminalCell::SemanticContent::prompt;
    h.cursor_flags.semantic_content_clear_eol = true;
    h.cursor_pen = testPen();
    h.hyperlink_implicit_id = 0x0a0b0c0d;
    h.charset = testCharsetState();
    h.protected_mode = ProtectedMode::dec;
    h.kitty_keyboard = testKitty();
    h.semantic_click = clickCl(wisp::terminal::osc::semantic_prompt::Click::smart_vertical);
    h.saved_cursor_present = true;
    return h;
}

static SavedCursor testSavedCursor() {
    SavedCursor s;
    s.x = 0x0102;
    s.y = 0x0304;
    s.pen = TerminalStyle();
    s.flags.protected_ = true;
    s.flags.pending_wrap = true;
    s.flags.origin = true;
    s.charset = testCharsetState();
    return s;
}

/* Wisp: std.testing.expectEqualDeep over the snapshot structs. */
static bool charsetEql(const CharsetState &a, const CharsetState &b) {
    for (int i = 0; i < 4; i++) {
        if (a.charsets.get((Slots)i) != b.charsets.get((Slots)i)) return false;
    }
    if (a.gl != b.gl || a.gr != b.gr) return false;
    if (a.single_shift.has != b.single_shift.has) return false;
    return !a.single_shift.has || a.single_shift.value == b.single_shift.value;
}

static bool headerEql(const Header &a, const Header &b) {
    if (a.key != b.key || a.page_count != b.page_count || a.history_rows != b.history_rows) return false;
    if (a.cursor_x != b.cursor_x || a.cursor_y != b.cursor_y || a.cursor_style != b.cursor_style) return false;
    if (a.cursor_flags.bits() != b.cursor_flags.bits()) return false;
    if (!a.cursor_pen.eql(b.cursor_pen)) return false;
    if (a.hyperlink_implicit_id != b.hyperlink_implicit_id) return false;
    if (!charsetEql(a.charset, b.charset)) return false;
    if (a.protected_mode != b.protected_mode) return false;
    if (a.kitty_keyboard.index != b.kitty_keyboard.index) return false;
    for (size_t i = 0; i < 8; i++) {
        if (a.kitty_keyboard.flags[i].bits() != b.kitty_keyboard.flags[i].bits()) return false;
    }
    if (!ss::semanticClickEql(a.semantic_click, b.semantic_click)) return false;
    return a.saved_cursor_present == b.saved_cursor_present;
}

static bool savedCursorEql(const SavedCursor &a, const SavedCursor &b) {
    return a.x == b.x && a.y == b.y && a.pen.eql(b.pen) && a.flags.bits() == b.flags.bits() &&
           charsetEql(a.charset, b.charset);
}

static const tp::Page *activePage(const TerminalScreen &screen) {
    return screen.pages.getTopLeft(PointTag::active).node->pageAssumeResident();
}

/* A SCREEN record with the given header and an empty cursor hyperlink. */
static void beginScreenRecord(record::Writer *stream, const Header &header, const SavedCursor *saved) {
    std::string *payload = stream->begin(record::Tag::screen);
    header.encode(payload);
    if (saved != nullptr) saved->encode(payload);
    payload->push_back(0);
    stream->finish();
}

TEST(snapshot_screen, SCREEN_header_golden_encoding_and_decoding) {
    std::string encoded;
    ASSERT_TRUE(testHeader().encode(&encoded) == Error::none);

    std::string golden;
    ASSERT_TRUE(loadFixture("screen-header-v1.hex", &golden));
    const std::string path = fixturePath("screen-header-v1.hex");
    ASSERT_TRUE(fixture::expectEqual(fixture::Kind::bytes, path.c_str(),
                                     "snapshot_fixture-screen-header-v1.hex", golden, encoded));
    ASSERT_EQ((long)Header::len, (long)golden.size());

    Reader source = Reader::fixed(golden.data(), golden.size());
    uint8_t buffer[1];
    LimitedReader limited = LimitedReader::limited(&source, buffer, sizeof buffer);
    Header decoded;
    ASSERT_TRUE(Header::decode(&limited, &decoded) == Error::none);
    ASSERT_TRUE(headerEql(testHeader(), decoded));
}

TEST(snapshot_screen, native_enum_values_used_by_the_SCREEN_format) {
    ASSERT_EQ(0, (long)Key::primary);
    ASSERT_EQ(1, (long)Key::alternate);

    ASSERT_EQ(0, (long)TerminalScreen::CursorStyle::bar);
    ASSERT_EQ(1, (long)TerminalScreen::CursorStyle::block);
    ASSERT_EQ(2, (long)TerminalScreen::CursorStyle::underline);
    ASSERT_EQ(3, (long)TerminalScreen::CursorStyle::block_hollow);

    ASSERT_EQ(0, (long)TerminalCell::SemanticContent::output);
    ASSERT_EQ(1, (long)TerminalCell::SemanticContent::input);
    ASSERT_EQ(2, (long)TerminalCell::SemanticContent::prompt);

    ASSERT_EQ(0, (long)Charset::utf8);
    ASSERT_EQ(1, (long)Charset::ascii);
    ASSERT_EQ(2, (long)Charset::british);
    ASSERT_EQ(3, (long)Charset::dec_special);

    ASSERT_EQ(0, (long)Slots::G0);
    ASSERT_EQ(1, (long)Slots::G1);
    ASSERT_EQ(2, (long)Slots::G2);
    ASSERT_EQ(3, (long)Slots::G3);

    ASSERT_EQ(0, (long)ProtectedMode::off);
    ASSERT_EQ(1, (long)ProtectedMode::iso);
    ASSERT_EQ(2, (long)ProtectedMode::dec);

    ASSERT_EQ(0, (long)SemanticClick::Kind::none);
    ASSERT_EQ(1, (long)SemanticClick::Kind::click_events);
    ASSERT_EQ(2, (long)SemanticClick::Kind::cl);

    ASSERT_EQ(0, (long)wisp::terminal::osc::semantic_prompt::ClickEvents::absolute);
    ASSERT_EQ(1, (long)wisp::terminal::osc::semantic_prompt::ClickEvents::relative);

    ASSERT_EQ(0, (long)wisp::terminal::osc::semantic_prompt::Click::line);
    ASSERT_EQ(1, (long)wisp::terminal::osc::semantic_prompt::Click::multiple);
    ASSERT_EQ(2, (long)wisp::terminal::osc::semantic_prompt::Click::conservative_vertical);
    ASSERT_EQ(3, (long)wisp::terminal::osc::semantic_prompt::Click::smart_vertical);
}

TEST(snapshot_screen, cursor_Kitty_and_saved_cursor_flag_bit_layouts) {
    ss::CursorFlags c;
    c = ss::CursorFlags(); c.pending_wrap = true; ASSERT_EQ(1 << 0, (long)c.bits());
    c = ss::CursorFlags(); c.protected_ = true; ASSERT_EQ(1 << 1, (long)c.bits());
    c = ss::CursorFlags(); c.semantic_content = TerminalCell::SemanticContent::input; ASSERT_EQ(1 << 2, (long)c.bits());
    c = ss::CursorFlags(); c.semantic_content = TerminalCell::SemanticContent::prompt; ASSERT_EQ(2 << 2, (long)c.bits());
    c = ss::CursorFlags(); c.semantic_content_clear_eol = true; ASSERT_EQ(1 << 4, (long)c.bits());

    ss::KittyKeyboard::Flags k;
    k = ss::KittyKeyboard::Flags(); k.disambiguate = true; ASSERT_EQ(1 << 0, (long)k.bits());
    k = ss::KittyKeyboard::Flags(); k.report_events = true; ASSERT_EQ(1 << 1, (long)k.bits());
    k = ss::KittyKeyboard::Flags(); k.report_alternates = true; ASSERT_EQ(1 << 2, (long)k.bits());
    k = ss::KittyKeyboard::Flags(); k.report_all = true; ASSERT_EQ(1 << 3, (long)k.bits());
    k = ss::KittyKeyboard::Flags(); k.report_associated = true; ASSERT_EQ(1 << 4, (long)k.bits());

    SavedCursor::Flags s;
    s = SavedCursor::Flags(); s.protected_ = true; ASSERT_EQ(1 << 0, (long)s.bits());
    s = SavedCursor::Flags(); s.pending_wrap = true; ASSERT_EQ(1 << 1, (long)s.bits());
    s = SavedCursor::Flags(); s.origin = true; ASSERT_EQ(1 << 2, (long)s.bits());
}

TEST(snapshot_screen, header_encoding_rejects_invalid_state) {
    std::string encoded;

    Header invalid_cursor_flags = testHeader();
    invalid_cursor_flags.cursor_flags.padding_ = 1;
    ASSERT_TRUE(invalid_cursor_flags.encode(&encoded) == Error::InvalidCursorFlags);

    Header invalid_kitty_index = testHeader();
    invalid_kitty_index.kitty_keyboard.index = 8;
    ASSERT_TRUE(invalid_kitty_index.encode(&encoded) == Error::InvalidKittyKeyboardIndex);

    Header invalid_kitty_flags = testHeader();
    invalid_kitty_flags.kitty_keyboard.flags[3].padding_ = 1;
    ASSERT_TRUE(invalid_kitty_flags.encode(&encoded) == Error::InvalidKittyKeyboardFlags);
    ASSERT_EQ(0, (long)encoded.size());
}

TEST(snapshot_screen, header_decoding_rejects_structural_values) {
    uint8_t invalid_key[Header::len] = { 0 };
    invalid_key[0] = 2;
    Reader key_reader = Reader::fixed(invalid_key, sizeof invalid_key);
    Header out;
    ASSERT_TRUE(Header::decode(&key_reader, &out) == Error::InvalidKey);
}

TEST(snapshot_screen, header_decoding_normalizes_unknown_semantic_values) {
    uint8_t fx[Header::len] = { 0 };
    fx[16] = 4;    /* Unknown cursor style. */
    fx[17] = 0xFF; /* Known flags, unknown semantic value, reserved bits. */
    fx[18] = 3;    /* Unknown cursor foreground color kind. */
    fx[39] = 0xD0; /* Invalid single shift plus a reserved bit. */
    fx[40] = 3;    /* Unknown protected mode. */
    fx[41] = 8;    /* Out-of-range Kitty keyboard index. */
    memset(&fx[42], 0xFF, 8); /* Known flags plus reserved bits. */
    fx[50] = 3;    /* Unknown semantic-click kind. */
    fx[51] = 0xFF;
    fx[52] = 2;    /* Noncanonical true. */

    Reader reader = Reader::fixed(fx, sizeof fx);
    Header decoded;
    ASSERT_TRUE(Header::decode(&reader, &decoded) == Error::none);

    ASSERT_TRUE(TerminalScreen::CursorStyle::block == decoded.cursor_style);
    ASSERT_TRUE(decoded.cursor_pen.default_());
    ASSERT_TRUE(decoded.cursor_flags.pending_wrap);
    ASSERT_TRUE(decoded.cursor_flags.protected_);
    ASSERT_TRUE(TerminalCell::SemanticContent::output == decoded.cursor_flags.semantic_content);
    ASSERT_TRUE(decoded.cursor_flags.semantic_content_clear_eol);
    ASSERT_EQ(0, (long)decoded.cursor_flags.padding_);

    ASSERT_FALSE(decoded.charset.single_shift.has);
    ASSERT_TRUE(Slots::G0 == decoded.charset.gr);
    ASSERT_TRUE(ProtectedMode::off == decoded.protected_mode);
    ASSERT_EQ(0, (long)decoded.kitty_keyboard.index);
    for (size_t i = 0; i < 8; i++) {
        const ss::KittyKeyboard::Flags &f = decoded.kitty_keyboard.flags[i];
        ASSERT_TRUE(f.disambiguate && f.report_events && f.report_alternates && f.report_all &&
                    f.report_associated);
        ASSERT_EQ(0, (long)f.padding_);
    }
    ASSERT_TRUE(ss::semanticClickEql(ss::semanticClickNone(), decoded.semantic_click));
    ASSERT_TRUE(decoded.saved_cursor_present);

    /* Known semantic-click kinds with unknown or noncanonical values also
     * degrade to disabled while consuming both fixed bytes. */
    const uint8_t invalid_semantic_clicks[3][2] = { { 0, 1 }, { 1, 2 }, { 2, 4 } };
    for (size_t i = 0; i < 3; i++) {
        uint8_t semantic_fixture[Header::len] = { 0 };
        semantic_fixture[50] = invalid_semantic_clicks[i][0];
        semantic_fixture[51] = invalid_semantic_clicks[i][1];
        Reader semantic_reader = Reader::fixed(semantic_fixture, sizeof semantic_fixture);
        Header semantic_decoded;
        ASSERT_TRUE(Header::decode(&semantic_reader, &semantic_decoded) == Error::none);
        ASSERT_TRUE(ss::semanticClickEql(ss::semanticClickNone(), semantic_decoded.semantic_click));
    }
}

TEST(snapshot_screen, header_decoding_rejects_every_truncation) {
    std::string golden;
    ASSERT_TRUE(loadFixture("screen-header-v1.hex", &golden));
    for (size_t fixture_len = 0; fixture_len < Header::len; fixture_len++) {
        Reader reader = Reader::fixed(golden.data(), fixture_len);
        Header out;
        ASSERT_TRUE(Header::decode(&reader, &out) == Error::EndOfStream);
    }
}

TEST(snapshot_screen, native_SCREEN_payload_omits_absent_optional_state) {
    ScreenHolder screen(opts(2, 2, noScrollback()));
    ASSERT_TRUE(screen.ok);

    std::string encoded;
    ASSERT_TRUE(ss::encodePayload(&screen.s, Key::primary, 1, &encoded) == Error::none);
    ASSERT_EQ((long)(Header::len + 1), (long)encoded.size());

    Reader reader = Reader::fixed(encoded.data(), encoded.size());
    Header header;
    ASSERT_TRUE(Header::decode(&reader, &header) == Error::none);
    ASSERT_TRUE(Key::primary == header.key);
    ASSERT_EQ(1, (long)header.page_count);
    ASSERT_TRUE(0 == header.history_rows);
    ASSERT_FALSE(header.saved_cursor_present);
    ss::TerminalHyperlink link;
    bool has = true;
    ASSERT_TRUE(ss::decodeCursorHyperlink(&reader, talloc(), &link, &has) == Error::none);
    ASSERT_FALSE(has);
}

TEST(snapshot_screen, saved_cursor_golden_encoding_and_decoding) {
    std::string encoded;
    ASSERT_TRUE(testSavedCursor().encode(&encoded) == Error::none);

    std::string golden;
    ASSERT_TRUE(loadFixture("screen-saved-cursor-v1.hex", &golden));
    const std::string path = fixturePath("screen-saved-cursor-v1.hex");
    ASSERT_TRUE(fixture::expectEqual(fixture::Kind::bytes, path.c_str(),
                                     "snapshot_fixture-screen-saved-cursor-v1.hex", golden, encoded));
    ASSERT_EQ((long)SavedCursor::len, (long)golden.size());

    Reader source = Reader::fixed(golden.data(), golden.size());
    uint8_t buffer[1];
    LimitedReader limited = LimitedReader::limited(&source, buffer, sizeof buffer);
    SavedCursor decoded;
    ASSERT_TRUE(SavedCursor::decode(&limited, &decoded) == Error::none);
    ASSERT_TRUE(savedCursorEql(testSavedCursor(), decoded));
}

TEST(snapshot_screen, saved_cursor_encoding_rejects_and_decoding_normalizes_invalid_state) {
    std::string encoded;
    SavedCursor invalid_flags = testSavedCursor();
    invalid_flags.flags.padding_ = 1;
    ASSERT_TRUE(invalid_flags.encode(&encoded) == Error::InvalidSavedCursorFlags);

    uint8_t invalid[SavedCursor::len] = { 0 };
    invalid[4] = 3;     /* Unknown saved-cursor foreground color kind. */
    invalid[20] = 0xF9; /* Protected plus reserved bits. */
    invalid[22] = 0xD0; /* Invalid single shift plus a reserved bit. */
    Reader reader = Reader::fixed(invalid, sizeof invalid);
    SavedCursor decoded;
    ASSERT_TRUE(SavedCursor::decode(&reader, &decoded) == Error::none);

    ASSERT_TRUE(decoded.pen.default_());
    ASSERT_TRUE(decoded.flags.protected_);
    ASSERT_FALSE(decoded.flags.pending_wrap);
    ASSERT_FALSE(decoded.flags.origin);
    ASSERT_EQ(0, (long)decoded.flags.padding_);
    ASSERT_FALSE(decoded.charset.single_shift.has);
    ASSERT_TRUE(Slots::G0 == decoded.charset.gr);
}

TEST(snapshot_screen, saved_cursor_decoding_rejects_every_truncation) {
    std::string golden;
    ASSERT_TRUE(loadFixture("screen-saved-cursor-v1.hex", &golden));
    for (size_t fixture_len = 0; fixture_len < SavedCursor::len; fixture_len++) {
        Reader reader = Reader::fixed(golden.data(), fixture_len);
        SavedCursor out;
        ASSERT_TRUE(SavedCursor::decode(&reader, &out) == Error::EndOfStream);
    }
}

TEST(snapshot_screen, cursor_hyperlink_null_encoding_and_decoding) {
    std::string none_encoded;
    ASSERT_TRUE(ss::encodeCursorHyperlink(nullptr, &none_encoded) == Error::none);
    ASSERT_TRUE(none_encoded == std::string("\x00", 1));

    Reader none_reader = Reader::fixed(none_encoded.data(), none_encoded.size());
    ss::TerminalHyperlink link;
    bool has = true;
    ASSERT_TRUE(ss::decodeCursorHyperlink(&none_reader, talloc(), &link, &has) == Error::none);
    ASSERT_FALSE(has);
}

TEST(snapshot_screen, framed_native_SCREEN_and_PAGE_sequence) {
    ScreenHolder holder(opts(8, 8, noScrollback()));
    ASSERT_TRUE(holder.ok);
    TerminalScreen &screen = holder.s;

    /* Configure the live cursor state represented by the fixed header. */
    screen.cursorAbsolute(7, 6);
    screen.cursor.cursor_style = TerminalScreen::CursorStyle::block_hollow;
    screen.cursor.pending_wrap = true;
    screen.cursor.protected_ = false;
    screen.cursor.semantic_content = TerminalCell::SemanticContent::prompt;
    screen.cursor.semantic_content_clear_eol = true;
    screen.cursor.style = testHeader().cursor_pen;
    screen.cursor.hyperlink_implicit_id = 0x0a0b0c0d;

    /* Configure the screen-wide modes and packed charset state. */
    const CharsetState charset = testCharsetState();
    screen.charset = charset;
    screen.protected_mode = ProtectedMode::dec;
    screen.kitty_keyboard = testKitty().terminal();
    screen.semantic_prompt.seen = true;
    screen.semantic_prompt.click = clickCl(wisp::terminal::osc::semantic_prompt::Click::smart_vertical);

    /* Configure the optional cursor state encoded after the fixed header. */
    TerminalScreen::SavedCursor saved;
    saved.x = 0x0102;
    saved.y = 0x0304;
    saved.style = TerminalStyle();
    saved.protected_ = true;
    saved.pending_wrap = true;
    saved.origin = true;
    saved.charset = charset;
    screen.saved_cursor = wisp::vt::Maybe<TerminalScreen::SavedCursor>(saved);
    const char *uri = "cursor-uri";
    ASSERT_TRUE(screen.startHyperlink((const uint8_t *)uri, strlen(uri), nullptr, 0) ==
                PageList::IncreaseCapacityError::none);

    std::string destination = "prefix";
    record::Writer stream;
    stream.init(&destination);
    ASSERT_TRUE(ss::encode(&screen, Key::alternate, &stream) == Error::none);
    ASSERT_TRUE(destination.compare(0, 6, "prefix") == 0);

    /* The complete sequence restores directly into native Screen/PageList
     * state, including page-local cursor references. */
    Reader restore_source = Reader::fixed(destination.data() + 6, destination.size() - 6);
    DecodedHolder dh;
    ASSERT_TRUE(ss::decode(&restore_source, talloc(), opts(8, 8, noScrollback()), &dh.d) == Error::none);
    dh.ok = true;
    ASSERT_TRUE(Key::alternate == dh.d.key);
    TerminalScreen *restored = &dh.d.screen;

    ASSERT_TRUE(restored->no_scrollback);
    ASSERT_EQ(7, (long)restored->cursor.x);
    ASSERT_EQ(6, (long)restored->cursor.y);
    ASSERT_TRUE(TerminalScreen::CursorStyle::block_hollow == restored->cursor.cursor_style);
    ASSERT_TRUE(restored->cursor.pending_wrap);
    ASSERT_TRUE(TerminalCell::SemanticContent::prompt == restored->cursor.semantic_content);
    ASSERT_TRUE(restored->cursor.style.eql(testHeader().cursor_pen));
    ASSERT_TRUE(restored->cursor.style_id != wisp::vt::style::default_id);
    ASSERT_TRUE(0x0a0b0c0eu == restored->cursor.hyperlink_implicit_id);
    ASSERT_TRUE(restored->cursor.hyperlink != nullptr);
    const ss::TerminalHyperlink &restored_hyperlink = *restored->cursor.hyperlink;
    ASSERT_TRUE(restored_hyperlink.uri_len == strlen(uri) && memcmp(restored_hyperlink.uri, uri, strlen(uri)) == 0);
    ASSERT_TRUE(restored_hyperlink.id.tag == ss::TerminalHyperlink::Id::Tag::implicit);
    ASSERT_TRUE(0x0a0b0c0du == restored_hyperlink.id.implicit);

    ASSERT_TRUE(restored->saved_cursor.has);
    const TerminalScreen::SavedCursor &restored_saved = restored->saved_cursor.value;
    ASSERT_EQ(7, (long)restored_saved.x);
    ASSERT_EQ(7, (long)restored_saved.y);
    ASSERT_TRUE(restored_saved.protected_);
    ASSERT_TRUE(restored_saved.pending_wrap);
    ASSERT_TRUE(restored_saved.origin);
    ASSERT_TRUE(Charset::ascii == restored->charset.charsets.get(Slots::G1));
    ASSERT_TRUE(ProtectedMode::dec == restored->protected_mode);
    ASSERT_EQ(7, (long)restored->kitty_keyboard.idx);
    ASSERT_TRUE(restored->semantic_prompt.seen);
    ASSERT_TRUE(ss::semanticClickEql(clickCl(wisp::terminal::osc::semantic_prompt::Click::smart_vertical),
                                     restored->semantic_prompt.click));

    /* Restored page-local cursor IDs are immediately usable by normal
     * writes. */
    ASSERT_TRUE(restored->testWriteString("Z"));
    const wisp::vt::Maybe<PageList::Cell> written = restored->pages.getCell(Point::active(0, 7));
    ASSERT_TRUE(written.has);
    ASSERT_EQ('Z', (long)written.value.cell->codepoint());
    ASSERT_EQ((long)restored->cursor.style_id, (long)written.value.cell->style_id());
    wisp::vt::hyperlink::Id written_link = 0;
    ASSERT_TRUE(written.value.node->pageAssumeResident()->lookupHyperlink(written.value.cell, &written_link));
    ASSERT_EQ((long)restored->cursor.hyperlink_id, (long)written_link);
    ASSERT_TRUE(TerminalCell::SemanticContent::prompt == written.value.cell->semantic_content());
    uint8_t b = 0;
    ReadError rerr = ReadError::none;
    ASSERT_FALSE(restore_source.takeByte(&b, &rerr));
    ASSERT_TRUE(rerr == ReadError::EndOfStream);
}

TEST(snapshot_screen, SCREEN_encodes_the_minimal_complete_page_active_suffix) {
    size_t page_rows = 0;
    {
        ScreenHolder probe(opts(80, 1, noScrollback()));
        ASSERT_TRUE(probe.ok);
        page_rows = probe.s.pages.getTopLeft(PointTag::screen).node->capacity().rows;
    }
    const uint16_t screen_rows = (uint16_t)(page_rows + 1);

    ScreenHolder holder(opts(80, screen_rows, unlimited()));
    ASSERT_TRUE(holder.ok);
    TerminalScreen &screen = holder.s;

    /* Create one complete historical page followed by a mixed page whose
     * first row is history and whose remaining rows begin the active area. */
    screen.cursorAbsolute(0, (uint16_t)(screen_rows - 1));
    for (size_t i = 0; i < screen_rows; i++) ASSERT_TRUE(screen.testWriteString("\n"));
    ASSERT_EQ(3, (long)screen.pages.totalPages());

    const PageList::Pin active_top = screen.pages.getTopLeft(PointTag::active);
    PageList::Node *historical = screen.pages.getTopLeft(PointTag::screen).node;
    PageList::Node *mixed = historical->next;
    ASSERT_TRUE(mixed != nullptr);
    PageList::Node *newest = mixed->next;
    ASSERT_TRUE(newest != nullptr);
    ASSERT_TRUE(mixed == active_top.node);
    ASSERT_EQ(1, (long)active_top.y);
    ASSERT_TRUE(newest->next == nullptr);

    /* Mark each native page so the encoded sequence proves both omission and
     * oldest-to-newest ordering. The mixed page's marker is in its incidental
     * history prefix and must survive because complete pages are encoded. */
    *historical->pageAssumeResident()->getRowAndCell(0, 0).cell = TerminalCell::init('A');
    *mixed->pageAssumeResident()->getRowAndCell(0, 0).cell = TerminalCell::init('B');
    *newest->pageAssumeResident()->getRowAndCell(0, 0).cell = TerminalCell::init('C');

    std::string destination;
    record::Writer stream;
    stream.init(&destination);
    ASSERT_TRUE(ss::encode(&screen, Key::primary, &stream) == Error::none);

    Reader source = Reader::fixed(destination.data(), destination.size());
    record::Reader screen_record;
    record::Reader::InitError ierr = record::Reader::InitError::none;
    ASSERT_TRUE(screen_record.init(&source, &ierr));
    ASSERT_TRUE(record::Tag::screen == screen_record.header.tag);

    record::PayloadReader *payload_reader = screen_record.payloadReader();
    Header header;
    ASSERT_TRUE(Header::decode(payload_reader, &header) == Error::none);
    ASSERT_TRUE(Key::primary == header.key);
    ASSERT_EQ(2, (long)header.page_count);
    ASSERT_TRUE((uint64_t)(screen.pages.total_rows - screen.pages.rows) == header.history_rows);
    ASSERT_FALSE(header.saved_cursor_present);
    ss::TerminalHyperlink link;
    bool has = true;
    ASSERT_TRUE(ss::decodeCursorHyperlink(payload_reader, talloc(), &link, &has) == Error::none);
    ASSERT_FALSE(has);
    ASSERT_TRUE(screen_record.finish() == record::Reader::FinishError::none);

    tp::Page decoded_mixed;
    ASSERT_TRUE(sp::decode(&source, talloc(), &decoded_mixed) == sp::Error::none);
    const long mixed_cp = (long)decoded_mixed.getRowAndCell(0, 0).cell->codepoint();
    decoded_mixed.deinit();
    ASSERT_EQ('B', mixed_cp);

    tp::Page decoded_newest;
    ASSERT_TRUE(sp::decode(&source, talloc(), &decoded_newest) == sp::Error::none);
    const long newest_cp = (long)decoded_newest.getRowAndCell(0, 0).cell->codepoint();
    decoded_newest.deinit();
    ASSERT_EQ('C', newest_cp);
    uint8_t b = 0;
    ReadError rerr = ReadError::none;
    ASSERT_FALSE(source.takeByte(&b, &rerr));

    /* Native restoration keeps the incidental history prefix and positions
     * the active top within the first decoded page. */
    Reader restore_source = Reader::fixed(destination.data(), destination.size());
    DecodedHolder dh;
    ASSERT_TRUE(ss::decode(&restore_source, talloc(), opts(80, screen_rows, unlimited()), &dh.d) == Error::none);
    dh.ok = true;
    ASSERT_TRUE(Key::primary == dh.d.key);
    ASSERT_TRUE(header.history_rows == dh.d.history_rows);
    TerminalScreen *restored = &dh.d.screen;

    ASSERT_EQ(2, (long)restored->pages.totalPages());
    const PageList::Pin restored_top = restored->pages.getTopLeft(PointTag::active);
    ASSERT_EQ(1, (long)restored_top.y);
    ASSERT_EQ((long)restored_top.y, (long)(restored->pages.total_rows - restored->pages.rows));
    ASSERT_TRUE(dh.d.history_rows > (uint64_t)(restored->pages.total_rows - restored->pages.rows));
    PageList::Node *first_node = restored->pages.getTopLeft(PointTag::screen).node;
    ASSERT_EQ('B', (long)first_node->pageAssumeResident()->getRowAndCell(0, 0).cell->codepoint());
    ASSERT_TRUE(first_node->next != nullptr);
    ASSERT_EQ('C', (long)first_node->next->pageAssumeResident()->getRowAndCell(0, 0).cell->codepoint());

    /* The restored PageList resumes ordinary scrolling and row growth. */
    ASSERT_TRUE(restored->testWriteString("\n"));
    restored->pages.assertIntegrity();
    restored->assertIntegrity();
    ASSERT_FALSE(restore_source.takeByte(&b, &rerr));
}

TEST(snapshot_screen, SCREEN_encoding_preserves_a_compressed_suffix_page) {
    ScreenHolder holder(opts(8, 2, noScrollback()));
    ASSERT_TRUE(holder.ok);
    TerminalScreen &screen = holder.s;
    *screen.pages.getCell(Point::active()).value.cell = TerminalCell::init('A');

    /* Force the active suffix into the representation that current PageList
     * policy normally reserves for history. This models a future policy
     * change and makes pageAssumeResident an invalid tagged-union access. */
    PageList::Node *node = screen.pages.getTopLeft(PointTag::active).node;
    const tp::Page *resident = node->pageAssumeResident();
    size_t required = 0;
    ASSERT_TRUE(wisp::vt::compress::Page::requiredScratch(resident->memory_len, &required) ==
                wisp::vt::lz4::CompressError::none);
    std::vector<uint8_t> scratch(required);
    static wisp::vt::lz4::HashTable table;
    wisp::vt::compress::Page compressed;
    ASSERT_TRUE(wisp::vt::compress::Page::init(talloc(), resident, scratch.data(), required, table, &compressed) ==
                wisp::vt::compress::Page::InitResult::ok);
    node->data.tag = PageList::Node::Data::Tag::compressed;
    node->data.compressed = compressed;
    ASSERT_TRUE(PageList::Node::Storage::compressed == node->storage());

    std::string destination;
    record::Writer stream;
    stream.init(&destination);
    ASSERT_TRUE(ss::encode(&screen, Key::primary, &stream) == Error::none);

    /* Encoding borrows or clones through PreservedPage and never changes the
     * source node's storage representation. */
    ASSERT_TRUE(PageList::Node::Storage::compressed == node->storage());

    Reader source = Reader::fixed(destination.data(), destination.size());
    DecodedHolder dh;
    ASSERT_TRUE(ss::decode(&source, talloc(), opts(8, 2, noScrollback()), &dh.d) == Error::none);
    dh.ok = true;
    ASSERT_EQ('A', (long)dh.d.screen.pages.getCell(Point::active()).value.cell->codepoint());
}

TEST(snapshot_screen, SCREEN_restoration_normalizes_invalid_cursor_positions) {
    ScreenHolder holder(opts(2, 2, noScrollback()));
    ASSERT_TRUE(holder.ok);
    TerminalScreen &screen = holder.s;

    struct Case {
        uint16_t x, y;
        bool pending_wrap;
        uint16_t expected_x, expected_y;
        bool expected_pending_wrap;
    };
    const Case cases[] = {
        /* Out-of-range coordinates clamp to the bottom-right. Pending wrap is
         * valid there and can be retained. */
        { 0xFFFF, 0xFFFF, true, 1, 1, true },
        /* Pending wrap away from the final column would trip native printing
         * assertions, so retain the position and clear only that flag. */
        { 0, 0, true, 0, 0, false },
    };

    for (size_t i = 0; i < 2; i++) {
        std::string destination;
        record::Writer stream;
        stream.init(&destination);

        Header header = Header::init(&screen, Key::primary, 1);
        header.cursor_x = cases[i].x;
        header.cursor_y = cases[i].y;
        header.cursor_flags.pending_wrap = cases[i].pending_wrap;
        header.saved_cursor_present = false;
        beginScreenRecord(&stream, header, nullptr);
        ASSERT_TRUE(sp::encode(activePage(screen), &stream) == sp::Error::none);

        Reader source = Reader::fixed(destination.data(), destination.size());
        DecodedHolder dh;
        ASSERT_TRUE(ss::decode(&source, talloc(), opts(2, 2, noScrollback()), &dh.d) == Error::none);
        dh.ok = true;

        ASSERT_EQ(cases[i].expected_x, (long)dh.d.screen.cursor.x);
        ASSERT_EQ(cases[i].expected_y, (long)dh.d.screen.cursor.y);
        ASSERT_TRUE(cases[i].expected_pending_wrap == dh.d.screen.cursor.pending_wrap);
        dh.d.screen.assertIntegrity();
    }
}

TEST(snapshot_screen, SCREEN_validates_pending_wrap_against_a_mixed_width_cursor_page) {
    ScreenHolder holder(opts(8, 1, noScrollback()));
    ASSERT_TRUE(holder.ok);
    TerminalScreen &screen = holder.s;

    /* Model a lazily reflowed active page that is narrower than the terminal.
     * Column three is its physical final column even though it is not
     * column seven of the current terminal dimensions. */
    tp::Capacity cap;
    cap.cols = 4;
    cap.rows = 1;
    tp::Page narrow_page;
    ASSERT_TRUE(tp::Page::init(cap, &narrow_page));

    std::string destination;
    record::Writer stream;
    stream.init(&destination);

    Header header = Header::init(&screen, Key::primary, 1);
    header.cursor_x = 3;
    header.cursor_y = 0;
    header.cursor_flags.pending_wrap = true;
    header.saved_cursor_present = false;
    beginScreenRecord(&stream, header, nullptr);
    const sp::Error perr = sp::encode(&narrow_page, &stream);
    narrow_page.deinit();
    ASSERT_TRUE(perr == sp::Error::none);

    Reader source = Reader::fixed(destination.data(), destination.size());
    DecodedHolder dh;
    ASSERT_TRUE(ss::decode(&source, talloc(), opts(8, 1, noScrollback()), &dh.d) == Error::none);
    dh.ok = true;

    ASSERT_EQ(4, (long)dh.d.screen.cursor.page_pin->node->cols());
    ASSERT_EQ(3, (long)dh.d.screen.cursor.x);
    ASSERT_TRUE(dh.d.screen.cursor.pending_wrap);
    dh.d.screen.assertIntegrity();
}

TEST(snapshot_screen, SCREEN_clamps_a_decoded_saved_cursor_to_terminal_dimensions) {
    ScreenHolder holder(opts(2, 2, noScrollback()));
    ASSERT_TRUE(holder.ok);
    TerminalScreen &screen = holder.s;

    std::string destination;
    record::Writer stream;
    stream.init(&destination);

    Header header = Header::init(&screen, Key::primary, 1);
    header.saved_cursor_present = true;
    SavedCursor saved;
    saved.x = 0xFFFF;
    saved.y = 0xFFFF;
    saved.flags.pending_wrap = true;
    beginScreenRecord(&stream, header, &saved);
    ASSERT_TRUE(sp::encode(activePage(screen), &stream) == sp::Error::none);

    Reader source = Reader::fixed(destination.data(), destination.size());
    DecodedHolder dh;
    ASSERT_TRUE(ss::decode(&source, talloc(), opts(2, 2, noScrollback()), &dh.d) == Error::none);
    dh.ok = true;

    ASSERT_TRUE(dh.d.screen.saved_cursor.has);
    const TerminalScreen::SavedCursor &s = dh.d.screen.saved_cursor.value;
    ASSERT_EQ(1, (long)s.x);
    ASSERT_EQ(1, (long)s.y);
    ASSERT_TRUE(s.pending_wrap);
}

TEST(snapshot_screen, SCREEN_restoration_rejects_invalid_and_incomplete_sequences) {
    ScreenHolder holder(opts(2, 2, noScrollback()));
    ASSERT_TRUE(holder.ok);
    TerminalScreen &screen = holder.s;

    std::string destination;
    record::Writer stream;
    stream.init(&destination);
    ASSERT_TRUE(ss::encode(&screen, Key::primary, &stream) == Error::none);

    /* Every truncation must fail without leaking a partially restored
     * PageList, cursor pin, or cursor-owned state. */
    for (size_t fixture_len = 0; fixture_len < destination.size(); fixture_len++) {
        Reader source = Reader::fixed(destination.data(), fixture_len);
        DecodedHolder dh;
        const Error err = ss::decode(&source, talloc(), opts(2, 2, noScrollback()), &dh.d);
        dh.ok = err == Error::none;
        ASSERT_TRUE(err != Error::none);
    }

    /* Native PageList construction is transactional on allocation failure. */
    {
        zigstd::FailingAllocator failing(talloc(), 0);
        Reader source = Reader::fixed(destination.data(), destination.size());
        DecodedHolder dh;
        const Error err = ss::decode(&source, failing.allocator(), opts(2, 2, noScrollback()), &dh.d);
        dh.ok = err == Error::none;
        ASSERT_TRUE(err == Error::OutOfMemory);
    }

    /* A syntactically valid SCREEN cannot declare an empty PAGE sequence. */
    std::string empty_sequence;
    record::Writer empty_stream;
    empty_stream.init(&empty_sequence);
    beginScreenRecord(&empty_stream, Header::init(&screen, Key::primary, 0), nullptr);
    Reader empty_source = Reader::fixed(empty_sequence.data(), empty_sequence.size());
    DecodedHolder dh;
    const Error err = ss::decode(&empty_source, talloc(), opts(2, 2, noScrollback()), &dh.d);
    dh.ok = err == Error::none;
    ASSERT_TRUE(err == Error::InvalidPageCount);
}

/* Wisp: upstream backs the record writer's scratch with a failing allocator
 * so `finish` cannot buffer the payload, and asserts WriteFailed with the
 * destination untouched. The port's writer buffers in a std::string, which
 * has no allocator to fail; what remains expressible is the guarantee the
 * test exists for: a failed SCREEN encode emits nothing. That is driven here
 * by invalid header state, which is rejected before any byte is written. */
TEST(snapshot_screen, SCREEN_sequence_failure_preserves_preceding_bytes) {
    ScreenHolder holder(opts(2, 2, noScrollback()));
    ASSERT_TRUE(holder.ok);
    TerminalScreen &screen = holder.s;
    screen.kitty_keyboard.idx = 8;

    std::string destination = "prefix";
    record::Writer stream;
    stream.init(&destination);
    ASSERT_TRUE(ss::encode(&screen, Key::primary, &stream) == Error::InvalidKittyKeyboardIndex);
    ASSERT_TRUE(destination == "prefix");
    screen.kitty_keyboard.idx = 0;
}

TEST(snapshot_screen, SCREEN_decode_ignores_an_invalid_cursor_hyperlink) {
    ScreenHolder holder(opts(1, 1, noScrollback()));
    ASSERT_TRUE(holder.ok);
    TerminalScreen &screen = holder.s;

    std::string destination;
    record::Writer stream;
    stream.init(&destination);

    Header header = testHeader();
    header.key = Key::primary;
    header.page_count = 1;
    header.cursor_x = 0;
    header.cursor_y = 0;
    header.cursor_flags.pending_wrap = false;
    header.saved_cursor_present = false;

    std::string *screen_payload = stream.begin(record::Tag::screen);
    ASSERT_TRUE(header.encode(screen_payload) == Error::none);
    screen_payload->push_back(1); /* Implicit hyperlink. */
    io::writeInt<uint32_t>(screen_payload, 1);
    io::writeInt<uint32_t>(screen_payload, 0); /* Empty URI. */
    ASSERT_TRUE(stream.finish() == record::Writer::FinishError::none);

    ASSERT_TRUE(sp::encode(activePage(screen), &stream) == sp::Error::none);

    Reader source = Reader::fixed(destination.data(), destination.size());
    DecodedHolder dh;
    ASSERT_TRUE(ss::decode(&source, talloc(), opts(1, 1, noScrollback()), &dh.d) == Error::none);
    dh.ok = true;
    ASSERT_TRUE(dh.d.screen.cursor.hyperlink == nullptr);
}

TEST(snapshot_screen, SCREEN_decode_ignores_a_PAGE_with_an_empty_hyperlink_URI) {
    std::string destination;
    record::Writer stream;
    stream.init(&destination);

    Header header = testHeader();
    header.key = Key::primary;
    header.page_count = 1;
    header.cursor_x = 0;
    header.cursor_y = 0;
    header.cursor_flags.pending_wrap = false;
    header.saved_cursor_present = false;
    beginScreenRecord(&stream, header, nullptr);

    sp::Header page_header;
    page_header.columns = 1;
    page_header.rows = 1;
    page_header.style_count = 0;
    page_header.hyperlink_count = 1;
    page_header.style_capacity = 16;
    page_header.hyperlink_capacity_bytes = 512;
    page_header.grapheme_capacity_bytes = 0;
    page_header.string_capacity_bytes = 0;
    std::string *page_payload = stream.begin(record::Tag::page);
    page_header.encode(page_payload);
    io::writeInt<wisp::vt::hyperlink::Id>(page_payload, 1);
    page_payload->push_back(1); /* Implicit hyperlink. */
    io::writeInt<uint32_t>(page_payload, 1);
    io::writeInt<uint32_t>(page_payload, 0); /* Empty URI. */

    /* One narrow codepoint cell refers to the hyperlink table entry above.
     * Since that entry is ignored, the cell must restore without a
     * hyperlink. */
    page_payload->push_back((char)0x30); /* row flags, full cell width */
    io::writeInt<uint16_t>(page_payload, 1);
    grid::Cell c;
    c.content = 'A';
    c.hyperlink = true;
    c.hyperlink_id = 1;
    io::writeInt<uint64_t>(page_payload, c.bits());
    io::writeInt<uint32_t>(page_payload, 0); /* grapheme section */
    ASSERT_TRUE(stream.finish() == record::Writer::FinishError::none);

    Reader source = Reader::fixed(destination.data(), destination.size());
    DecodedHolder dh;
    ASSERT_TRUE(ss::decode(&source, talloc(), TerminalScreen::Options(1, 1), &dh.d) == Error::none);
    dh.ok = true;

    ASSERT_EQ('A', (long)dh.d.screen.cursor.page_cell->codepoint());
    ASSERT_FALSE(dh.d.screen.cursor.page_cell->hyperlink());

    /* Resizing copies the restored cells into a wider page. The ignored
     * hyperlink must not leave a zero-length PageEntry to duplicate later. */
    ASSERT_TRUE(dh.d.screen.resize(TerminalScreen::Resize(2, 1)));
    ASSERT_FALSE(dh.d.screen.cursor.page_cell->hyperlink());
}

TEST(snapshot_screen, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
