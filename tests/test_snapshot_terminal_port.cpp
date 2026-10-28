/* Transliterated from the test blocks in Ghostty
 * src/terminal/snapshot/terminal.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include <string.h>

#include <string>

#include "test_helpers.h"
#include "snapshot_fixture.hpp"
#include "../src/terminal/snapshot/terminal.hpp"
#include "../src/zigstd/allocator.hpp"
#include "../src/zigstd/reader.hpp"

using namespace wisp;

namespace st = wisp::terminal::snapshot::terminal_record;
namespace record = wisp::terminal::snapshot::record;
namespace fixture = wisp::test_fixture;
namespace modes = wisp::terminal::modes;

typedef zigstd::Reader Reader;
typedef zigstd::LimitedReader LimitedReader;
typedef zigstd::ReadError ReadError;
typedef st::Header Header;
typedef st::Error Error;
typedef st::Terminal Terminal;
typedef st::TerminalScreen TerminalScreen;
typedef st::TerminalScreenKey Key;
typedef st::TerminalTabstops TerminalTabstops;
typedef st::DynamicPalette DynamicPalette;
typedef st::DynamicRGB DynamicRGB;
typedef st::RGB RGB;
typedef st::ModePacked ModePacked;
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

/* Wisp: std.mem.zeroes(ModePacked) */
static ModePacked zeroModes() {
    ModePacked m;
    m.bits = 0;
    return m;
}

static bool rgbEq(RGB a, RGB b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

template <typename T>
static bool maybeEq(const Maybe<T> &a, const Maybe<T> &b) {
    return a.has == b.has && (!a.has || a.value == b.value);
}

static bool dynEq(const DynamicRGB &a, const DynamicRGB &b) {
    return a.has_default == b.has_default && (!a.has_default || rgbEq(a.default_, b.default_)) &&
           a.has_override == b.has_override && (!a.has_override || rgbEq(a.override_, b.override_));
}

/* Wisp: expectEqualDeep(DynamicPalette) follows `original`. */
static bool paletteEq(const DynamicPalette &a, const DynamicPalette &b) {
    if (!a.current.eql(b.current)) return false;
    if (!a.original->eql(*b.original)) return false;
    return memcmp(a.mask.words, b.mask.words, sizeof(a.mask.words)) == 0;
}

static bool modeStateEq(const modes::ModeState &a, const modes::ModeState &b) {
    return a.values.bits == b.values.bits && a.saved.bits == b.saved.bits && a.default_.bits == b.default_.bits;
}

static bool colorsEq(const Terminal::Colors &a, const Terminal::Colors &b) {
    return dynEq(a.background, b.background) && dynEq(a.foreground, b.foreground) && dynEq(a.cursor, b.cursor) &&
           paletteEq(a.palette, b.palette);
}

static bool headerEql(const Header &a, const Header &b) {
    return a.columns == b.columns && a.rows == b.rows && a.width_px == b.width_px && a.height_px == b.height_px &&
           a.scrolling_region_top == b.scrolling_region_top &&
           a.scrolling_region_bottom == b.scrolling_region_bottom &&
           a.scrolling_region_left == b.scrolling_region_left &&
           a.scrolling_region_right == b.scrolling_region_right && a.status_display == b.status_display &&
           a.active_screen_key == b.active_screen_key && a.screen_count == b.screen_count &&
           maybeEq(a.previous_codepoint, b.previous_codepoint) && a.cursor_is_default == b.cursor_is_default &&
           a.cursor_default_style == b.cursor_default_style &&
           maybeEq(a.cursor_default_blink, b.cursor_default_blink) && a.shell_redraw == b.shell_redraw &&
           a.modify_other_keys_2 == b.modify_other_keys_2 && a.mouse_event == b.mouse_event &&
           a.mouse_format == b.mouse_format && maybeEq(a.mouse_shift_capture, b.mouse_shift_capture) &&
           a.mouse_shape == b.mouse_shape && a.password_input == b.password_input &&
           a.current_modes.bits == b.current_modes.bits && a.saved_modes.bits == b.saved_modes.bits &&
           a.default_modes.bits == b.default_modes.bits && dynEq(a.background, b.background) &&
           dynEq(a.foreground, b.foreground) && dynEq(a.cursor_color, b.cursor_color) &&
           maybeEq(a.max_scrollback_bytes, b.max_scrollback_bytes) &&
           maybeEq(a.max_scrollback_rows, b.max_scrollback_rows);
}

static DynamicRGB dyn(bool has_default, RGB d, bool has_override, RGB o) {
    DynamicRGB v = DynamicRGB::unset();
    v.has_default = has_default;
    if (has_default) v.default_ = d;
    v.has_override = has_override;
    if (has_override) v.override_ = o;
    return v;
}

static Header testHeader() {
    ModePacked current_modes = zeroModes();
    current_modes.setField(modes::Mode::disable_keyboard, true);
    ModePacked saved_modes = zeroModes();
    saved_modes.setField(modes::Mode::in_band_size_reports, true);
    ModePacked default_modes = current_modes;
    default_modes.setField(modes::Mode::in_band_size_reports, true);

    Header h;
    h.columns = 0x0102;
    h.rows = 0x0304;
    h.width_px = 0x05060708;
    h.height_px = 0x090a0b0c;

    h.scrolling_region_top = 1;
    h.scrolling_region_bottom = 2;
    h.scrolling_region_left = 3;
    h.scrolling_region_right = 4;

    h.status_display = wisp::terminal::ansi::StatusDisplay::status_line;
    h.active_screen_key = Key::alternate;
    h.screen_count = 2;
    h.previous_codepoint = Maybe<uint32_t>((uint32_t)'A');

    h.cursor_is_default = true;
    h.cursor_default_style = TerminalScreen::CursorStyle::block_hollow;
    h.cursor_default_blink = Maybe<bool>(true);

    h.shell_redraw = st::Redraw::last;
    h.modify_other_keys_2 = true;
    h.mouse_event = st::MouseEvent::any;
    h.mouse_format = st::MouseFormat::sgr_pixels;
    h.mouse_shift_capture = Maybe<bool>(false);
    h.mouse_shape = st::MouseShape::zoom_out;
    h.password_input = true;

    h.current_modes = current_modes;
    h.saved_modes = saved_modes;
    h.default_modes = default_modes;

    h.background = dyn(true, RGB(1, 2, 3), false, RGB());
    h.foreground = dyn(false, RGB(), true, RGB(4, 5, 6));
    h.cursor_color = dyn(true, RGB(7, 8, 9), true, RGB(10, 11, 12));

    h.max_scrollback_bytes = Maybe<uint64_t>::none();
    h.max_scrollback_rows = Maybe<uint64_t>((uint64_t)0x0102030405060708ull);
    return h;
}

struct TermHolder {
    Terminal t;
    zigstd::Allocator alloc;
    bool ok = false;
    TermHolder(zigstd::Allocator a, const Terminal::Options &o) : alloc(a) { ok = Terminal::init(alloc, o, &t); }
    ~TermHolder() {
        if (ok) t.deinit(alloc);
    }
};

struct PayloadHolder {
    st::DecodedPayload d;
    bool ok = false;
    ~PayloadHolder() {
        if (ok) d.deinit(talloc());
    }
};

struct TabstopsHolder {
    TerminalTabstops t;
    bool ok = false;
    explicit TabstopsHolder(size_t cols) { ok = TerminalTabstops::init(talloc(), cols, 0, &t) == TerminalTabstops::Error::none; }
    ~TabstopsHolder() {
        if (ok) t.deinit(talloc());
    }
};

static Terminal::Options termOpts(uint16_t cols, uint16_t rows) { return Terminal::Options(cols, rows); }

TEST(snapshot_terminal, TERMINAL_mode_bit_layout) {
    ASSERT_EQ((long)43, (long)modes::entries_len);

    ModePacked first = zeroModes();
    first.setField(modes::Mode::disable_keyboard, true);
    ASSERT_TRUE(((uint64_t)1 << 0) == first.bits);

    ModePacked visibility = zeroModes();
    visibility.setField(modes::Mode::report_visibility, true);
    ASSERT_TRUE(((uint64_t)1 << 40) == visibility.bits);

    ModePacked size_reports = zeroModes();
    size_reports.setField(modes::Mode::in_band_size_reports, true);
    ASSERT_TRUE(((uint64_t)1 << 41) == size_reports.bits);

    ModePacked last = zeroModes();
    last.setField(modes::Mode::kitty_paste_events, true);
    ASSERT_TRUE(((uint64_t)1 << 42) == last.bits);
}

TEST(snapshot_terminal, TERMINAL_header_golden_encoding_and_decoding) {
    std::string encoded;
    ASSERT_TRUE(testHeader().encode(&encoded) == Error::none);
    std::string golden;
    ASSERT_TRUE(loadFixture("terminal-header-v1.hex", &golden));
    const std::string path = fixturePath("terminal-header-v1.hex");
    ASSERT_TRUE(fixture::expectEqual(fixture::Kind::bytes, path.c_str(), "snapshot_fixture-terminal-header-v1.hex",
                                     golden, encoded));
    ASSERT_EQ((long)Header::len, (long)golden.size());

    /* Exercise the streaming path with less buffered data than every integer. */
    Reader source = Reader::fixed(golden.data(), golden.size());
    uint8_t buffer[1];
    LimitedReader limited = LimitedReader::limited(&source, buffer, sizeof buffer);
    Header decoded;
    ASSERT_TRUE(Header::decode(&limited, &decoded) == Error::none);
    ASSERT_TRUE(headerEql(testHeader(), decoded));

    for (size_t fixture_len = 0; fixture_len < Header::len; fixture_len++) {
        Reader truncated = Reader::fixed(golden.data(), fixture_len);
        Header h;
        ASSERT_TRUE(Header::decode(&truncated, &h) == Error::EndOfStream);
    }
}

TEST(snapshot_terminal, TERMINAL_header_encoding_rejects_noncanonical_values) {
    /* Semantic values are validated before encoding writes anything. */
    Header invalid_header = testHeader();
    invalid_header.columns = 0;
    std::string writer;
    ASSERT_TRUE(invalid_header.encode(&writer) == Error::InvalidDimensions);
    ASSERT_EQ((long)0, (long)writer.size());

    invalid_header = testHeader();
    invalid_header.max_scrollback_bytes = Maybe<uint64_t>(UINT64_MAX);
    writer.clear();
    ASSERT_TRUE(invalid_header.encode(&writer) == Error::InvalidScrollbackLimit);
    ASSERT_EQ((long)0, (long)writer.size());
}

TEST(snapshot_terminal, TERMINAL_header_decoding_rejects_structural_values) {
    std::string golden;
    ASSERT_TRUE(loadFixture("terminal-header-v1.hex", &golden));

    std::string invalid_dimensions = golden;
    invalid_dimensions[0] = 0;
    invalid_dimensions[1] = 0;
    Reader dimensions_reader = Reader::fixed(invalid_dimensions.data(), invalid_dimensions.size());
    Header h;
    ASSERT_TRUE(Header::decode(&dimensions_reader, &h) == Error::InvalidDimensions);

    std::string invalid_screen_count = golden;
    invalid_screen_count[23] = 3;
    Reader screen_count_reader = Reader::fixed(invalid_screen_count.data(), invalid_screen_count.size());
    ASSERT_TRUE(Header::decode(&screen_count_reader, &h) == Error::InvalidScreenCount);
}

TEST(snapshot_terminal, TERMINAL_header_decoding_normalizes_semantic_values) {
    std::string golden;
    ASSERT_TRUE(loadFixture("terminal-header-v1.hex", &golden));
    const Header test_header = testHeader();

    std::string fx = golden;

    /* Unknown enum and boolean encodings use their neutral native defaults. */
    fx[20] = 2;
    fx[21] = 2;
    fx[22] = 0;
    fx[29] = 2;
    fx[30] = 4;
    fx[31] = 3;
    fx[32] = 3;
    fx[33] = 2;
    fx[34] = 5;
    fx[35] = 5;
    fx[36] = 3;
    fx[37] = 34;
    fx[38] = 2;

    /* Reserved mode bits are ignored while known low bits survive. */
    fx[44] = 4;

    /* Unknown presence and nonzero absent-value bytes both become absent. */
    fx[63] = 2;
    fx[68] = 1;
    fx[72] = static_cast<char>(0xAAu & 0xFFu);

    /* An invalid scrolling region becomes the full terminal region. */
    fx[14] = 0x04;
    fx[15] = 0x03;

    /* Surrogates cannot become native previous-character state. */
    fx[25] = 0x00;
    fx[26] = static_cast<char>(0xD8u & 0xFFu);
    fx[27] = 0x00;
    fx[28] = 0x00;

    Reader reader = Reader::fixed(fx.data(), fx.size());
    Header decoded;
    ASSERT_TRUE(Header::decode(&reader, &decoded) == Error::none);
    ASSERT_TRUE(wisp::terminal::ansi::StatusDisplay::main == decoded.status_display);
    ASSERT_TRUE(Key::primary == decoded.active_screen_key);
    ASSERT_EQ((long)0, (long)decoded.scrolling_region_top);
    ASSERT_EQ((long)(test_header.rows - 1), (long)decoded.scrolling_region_bottom);
    ASSERT_EQ((long)test_header.scrolling_region_left, (long)decoded.scrolling_region_left);
    ASSERT_EQ((long)test_header.scrolling_region_right, (long)decoded.scrolling_region_right);
    ASSERT_FALSE(decoded.previous_codepoint.has);
    ASSERT_TRUE(decoded.cursor_is_default);
    ASSERT_TRUE(TerminalScreen::CursorStyle::block == decoded.cursor_default_style);
    ASSERT_FALSE(decoded.cursor_default_blink.has);
    ASSERT_TRUE(st::Redraw::true_ == decoded.shell_redraw);
    ASSERT_FALSE(decoded.modify_other_keys_2);
    ASSERT_TRUE(st::MouseEvent::none == decoded.mouse_event);
    ASSERT_TRUE(st::MouseFormat::x10 == decoded.mouse_format);
    ASSERT_FALSE(decoded.mouse_shift_capture.has);
    ASSERT_TRUE(st::MouseShape::text == decoded.mouse_shape);
    ASSERT_FALSE(decoded.password_input);
    ASSERT_TRUE(decoded.current_modes.field(modes::Mode::disable_keyboard));
    ASSERT_TRUE(dynEq(DynamicRGB::unset(), decoded.background));
    ASSERT_FALSE(decoded.foreground.has_default);
    ASSERT_TRUE(decoded.foreground.has_override);
    ASSERT_TRUE(rgbEq(RGB(4, 5, 6), decoded.foreground.override_));

    /* Each scrolling axis is normalized independently. */
    std::string invalid_horizontal = golden;
    invalid_horizontal[18] = 0x02;
    invalid_horizontal[19] = 0x01;
    Reader horizontal_reader = Reader::fixed(invalid_horizontal.data(), invalid_horizontal.size());
    Header horizontal;
    ASSERT_TRUE(Header::decode(&horizontal_reader, &horizontal) == Error::none);
    ASSERT_EQ((long)test_header.scrolling_region_top, (long)horizontal.scrolling_region_top);
    ASSERT_EQ((long)test_header.scrolling_region_bottom, (long)horizontal.scrolling_region_bottom);
    ASSERT_EQ((long)0, (long)horizontal.scrolling_region_left);
    ASSERT_EQ((long)(test_header.columns - 1), (long)horizontal.scrolling_region_right);

    /* Alternate cannot be active when only the primary screen is declared. */
    std::string missing_active_screen = golden;
    missing_active_screen[23] = 1;
    Reader active_screen_reader = Reader::fixed(missing_active_screen.data(), missing_active_screen.size());
    Header active;
    ASSERT_TRUE(Header::decode(&active_screen_reader, &active) == Error::none);
    ASSERT_TRUE(Key::primary == active.active_screen_key);

    const uint64_t largest_finite = UINT64_MAX - 1;
    const uint64_t cap = (uint64_t)(SIZE_MAX - 1);
    const Maybe<size_t> native = st::nativeScrollbackLimit(Maybe<uint64_t>(largest_finite));
    ASSERT_TRUE(native.has);
    ASSERT_TRUE((size_t)(largest_finite < cap ? largest_finite : cap) == native.value);
}

TEST(snapshot_terminal, TERMINAL_payload_round_trip) {
    const Header test_header = testHeader();

    /* Include a partial final tab-stop byte so its bit order and padding are
     * both exercised. */
    TabstopsHolder tabstops(test_header.columns);
    ASSERT_TRUE(tabstops.ok);
    tabstops.t.set(0);
    tabstops.t.set(8);
    tabstops.t.set(test_header.columns - 1);

    /* Use overrides at both ends of the palette to make ordering explicit. */
    DynamicPalette palette = DynamicPalette::default_();
    palette.set(0, RGB(1, 2, 3));
    palette.set(255, RGB(4, 5, 6));

    std::string destination;
    const char *pwd = "file:///tmp/example";
    const char *title = "snapshot title";
    ASSERT_TRUE(st::encodePayload(test_header, &tabstops.t, &palette, (const uint8_t *)pwd, strlen(pwd),
                                  (const uint8_t *)title, strlen(title), &destination) == Error::none);

    /* Tab stops and the sparse palette mask both use least-significant-bit
     * first ordering. */
    const std::string &bytes = destination;
    const size_t tabstop_len = ((size_t)test_header.columns + 7) / 8;
    ASSERT_EQ((long)0x01, (long)(uint8_t)bytes[Header::len]);
    ASSERT_EQ((long)0x01, (long)(uint8_t)bytes[Header::len + 1]);
    ASSERT_EQ((long)0x02, (long)(uint8_t)bytes[Header::len + tabstop_len - 1]);
    const size_t palette_mask_offset = Header::len + tabstop_len + 256 * 3;
    ASSERT_EQ((long)0x01, (long)(uint8_t)bytes[palette_mask_offset]);
    ASSERT_EQ((long)0x80, (long)(uint8_t)bytes[palette_mask_offset + 31]);

    /* Decode through a one-byte reader buffer to cover streaming of the large
     * fixed palette and allocator-owned strings. */
    Reader source = Reader::fixed(bytes.data(), bytes.size());
    uint8_t buffer[1];
    LimitedReader limited = LimitedReader::limited(&source, buffer, sizeof buffer);
    PayloadHolder decoded;
    ASSERT_TRUE(st::decodePayload(&limited, talloc(), &decoded.d) == Error::none);
    decoded.ok = true;

    ASSERT_TRUE(headerEql(test_header, decoded.d.header));
    for (size_t column = 0; column < test_header.columns; column++) {
        ASSERT_TRUE(tabstops.t.get(column) == decoded.d.tabstops.get(column));
    }
    ASSERT_TRUE(paletteEq(palette, decoded.d.palette));
    ASSERT_TRUE(std::string((const char *)decoded.d.pwd, decoded.d.pwd_len) == "file:///tmp/example");
    ASSERT_TRUE(std::string((const char *)decoded.d.title, decoded.d.title_len) == "snapshot title");
}

TEST(snapshot_terminal, TERMINAL_payload_rejects_noncanonical_state) {
    const Header test_header = testHeader();

    /* A tab-stop set for different dimensions cannot represent this header. */
    TabstopsHolder tabstops(test_header.columns - 1);
    ASSERT_TRUE(tabstops.ok);
    DynamicPalette palette = DynamicPalette::default_();
    std::string writer;
    ASSERT_TRUE(st::encodePayload(test_header, &tabstops.t, &palette, nullptr, 0, nullptr, 0, &writer) ==
                Error::InvalidTabStops);
    ASSERT_EQ((long)0, (long)writer.size());

    /* Unmasked current colors must remain equal to their originals. */
    tabstops.t.cols = test_header.columns;
    palette.current[7] = RGB(1, 2, 3);
    writer.clear();
    ASSERT_TRUE(st::encodePayload(test_header, &tabstops.t, &palette, nullptr, 0, nullptr, 0, &writer) ==
                Error::InvalidPalette);
    ASSERT_EQ((long)0, (long)writer.size());
}

TEST(snapshot_terminal, TERMINAL_payload_ignores_tab_stop_padding) {
    const Header test_header = testHeader();

    TabstopsHolder tabstops(test_header.columns);
    ASSERT_TRUE(tabstops.ok);
    DynamicPalette palette = DynamicPalette::default_();
    palette.set(1, RGB(1, 2, 3));

    std::string destination;
    ASSERT_TRUE(st::encodePayload(test_header, &tabstops.t, &palette, (const uint8_t *)"pwd", 3,
                                  (const uint8_t *)"title", 5, &destination) == Error::none);

    /* Only two bits in the last byte correspond to terminal columns. */
    const size_t tabstop_len = ((size_t)test_header.columns + 7) / 8;
    const size_t last_tabstop = Header::len + tabstop_len - 1;
    std::string invalid_padding = destination;
    invalid_padding[last_tabstop] = (char)((uint8_t)invalid_padding[last_tabstop] | (1 << 7));
    Reader padding_reader = Reader::fixed(invalid_padding.data(), invalid_padding.size());
    PayloadHolder decoded;
    ASSERT_TRUE(st::decodePayload(&padding_reader, talloc(), &decoded.d) == Error::none);
    decoded.ok = true;
    for (size_t column = 0; column < test_header.columns; column++) {
        ASSERT_TRUE(tabstops.t.get(column) == decoded.d.tabstops.get(column));
    }
}

TEST(snapshot_terminal, TERMINAL_record_encodes_native_terminal_state) {
    Terminal::Options o = termOpts(10, 3);
    o.max_scrollback_bytes = Maybe<size_t>((size_t)12345);
    o.max_scrollback_lines = Maybe<size_t>((size_t)67);
    TermHolder terminal(talloc(), o);
    ASSERT_TRUE(terminal.ok);
    Terminal &t = terminal.t;

    /* Exercise every native header section plus sentinel-backed strings. */
    t.width_px = 800;
    t.height_px = 600;
    t.scrolling_region.top = 1;
    t.scrolling_region.bottom = 2;
    t.scrolling_region.left = 2;
    t.scrolling_region.right = 8;
    t.status_display = wisp::terminal::ansi::StatusDisplay::status_line;
    t.previous_char = Maybe<uint32_t>((uint32_t)'X');
    t.cursor.is_default = false;
    t.cursor.default_style = TerminalScreen::CursorStyle::underline;
    t.cursor.default_blink = Maybe<bool>(true);
    t.flags.shell_redraws_prompt = st::Redraw::last;
    t.flags.modify_other_keys_2 = true;
    t.flags.mouse_event = st::MouseEvent::button;
    t.flags.mouse_format = st::MouseFormat::sgr;
    t.flags.mouse_shift_capture = Terminal::Flags::MouseShiftCapture::true_;
    t.flags.password_input = true;
    t.mouse_shape = st::MouseShape::pointer;
    t.modes.values.setField(modes::Mode::disable_keyboard, true);
    t.modes.saved.setField(modes::Mode::in_band_size_reports, true);
    t.modes.default_.setField(modes::Mode::bracketed_paste, true);
    t.colors.background = dyn(true, RGB(1, 2, 3), true, RGB(4, 5, 6));
    t.colors.palette.set(7, RGB(7, 8, 9));
    t.tabstops.reset(0);
    t.tabstops.set(1);
    t.tabstops.set(9);
    ASSERT_TRUE(t.setPwd("file:///tmp/native"));
    ASSERT_TRUE(t.setTitle("native title"));

    /* Encode and restore through the public record codec. */
    std::string destination;
    record::Writer stream;
    stream.init(&destination);
    ASSERT_TRUE(st::encode(&t, &stream) == Error::none);

    Reader source = Reader::fixed(destination.data(), destination.size());
    Terminal restored;
    ASSERT_TRUE(st::decode(&source, talloc(), &restored) == Error::none);
    struct Guard {
        Terminal *t;
        ~Guard() { t->deinit(talloc()); }
    } guard = { &restored };

    ASSERT_EQ((long)10, (long)restored.cols);
    ASSERT_EQ((long)3, (long)restored.rows);
    ASSERT_EQ((long)800, (long)restored.width_px);
    ASSERT_EQ((long)600, (long)restored.height_px);
    ASSERT_TRUE(t.scrolling_region.top == restored.scrolling_region.top &&
                t.scrolling_region.bottom == restored.scrolling_region.bottom &&
                t.scrolling_region.left == restored.scrolling_region.left &&
                t.scrolling_region.right == restored.scrolling_region.right);
    ASSERT_TRUE(t.status_display == restored.status_display);
    ASSERT_TRUE(Key::primary == restored.screens.active_key);
    ASSERT_TRUE(restored.screens.get(Key::alternate) == nullptr);
    ASSERT_TRUE(maybeEq(Maybe<uint32_t>((uint32_t)'X'), restored.previous_char));
    ASSERT_TRUE(t.cursor.is_default == restored.cursor.is_default &&
                t.cursor.default_style == restored.cursor.default_style &&
                maybeEq(t.cursor.default_blink, restored.cursor.default_blink));
    ASSERT_TRUE(t.flags.shell_redraws_prompt == restored.flags.shell_redraws_prompt);
    ASSERT_TRUE(t.flags.modify_other_keys_2 == restored.flags.modify_other_keys_2);
    ASSERT_TRUE(t.flags.mouse_event == restored.flags.mouse_event);
    ASSERT_TRUE(t.flags.mouse_format == restored.flags.mouse_format);
    ASSERT_TRUE(t.flags.mouse_shift_capture == restored.flags.mouse_shift_capture);
    ASSERT_TRUE(t.flags.password_input == restored.flags.password_input);
    ASSERT_TRUE(t.mouse_shape == restored.mouse_shape);
    ASSERT_TRUE(modeStateEq(t.modes, restored.modes));
    ASSERT_TRUE(colorsEq(t.colors, restored.colors));
    ASSERT_EQ((long)12345, (long)restored.screens.active->pages.limits.bytes.explicit_);
    ASSERT_EQ((long)67, (long)restored.screens.active->pages.limits.lines.explicit_);
    ASSERT_TRUE(restored.tabstops.get(1));
    ASSERT_TRUE(restored.tabstops.get(9));
    ASSERT_TRUE(restored.getPwd() != nullptr);
    ASSERT_TRUE(std::string(restored.getPwd()) == "file:///tmp/native");
    ASSERT_TRUE(restored.getTitle() != nullptr);
    ASSERT_TRUE(std::string(restored.getTitle()) == "native title");
}

TEST(snapshot_terminal, TERMINAL_record_declares_an_initialized_alternate_screen) {
    TermHolder terminal(talloc(), termOpts(10, 3));
    ASSERT_TRUE(terminal.ok);
    bool ok = false;
    terminal.t.switchScreen(Key::alternate, &ok);
    ASSERT_TRUE(ok);

    std::string destination;
    record::Writer stream;
    stream.init(&destination);
    ASSERT_TRUE(st::encode(&terminal.t, &stream) == Error::none);

    Reader source = Reader::fixed(destination.data(), destination.size());
    Terminal restored;
    ASSERT_TRUE(st::decode(&source, talloc(), &restored) == Error::none);
    struct Guard {
        Terminal *t;
        ~Guard() { t->deinit(talloc()); }
    } guard = { &restored };

    const TerminalScreen *primary = restored.screens.get(Key::primary);
    const TerminalScreen *alternate = restored.screens.get(Key::alternate);
    ASSERT_TRUE(primary != nullptr);
    ASSERT_TRUE(alternate != nullptr);
    ASSERT_TRUE(Key::alternate == restored.screens.active_key);
    ASSERT_TRUE(alternate == restored.screens.active);
    ASSERT_TRUE(primary != alternate);
    ASSERT_EQ((long)10, (long)alternate->pages.cols);
    ASSERT_EQ((long)3, (long)alternate->pages.rows);
    ASSERT_EQ((long)0, (long)alternate->pages.limits.bytes.explicit_);
}

TEST(snapshot_terminal, TERMINAL_payload_failure_emits_no_incomplete_record) {
    TermHolder terminal(talloc(), termOpts(10, 3));
    ASSERT_TRUE(terminal.ok);

    std::string destination = "prefix";
    record::Writer stream;
    stream.init(&destination);

    /* Payload validation occurs in the reusable record scratch buffer. The
     * invalid TERMINAL is never emitted to the destination. */
    terminal.t.colors.palette.current[7] = RGB(1, 2, 3);
    ASSERT_TRUE(st::encode(&terminal.t, &stream) == Error::InvalidPalette);
    ASSERT_TRUE(destination == "prefix");
}

TEST(snapshot_terminal, TERMINAL_record_decoding_rejects_malformed_input_transactionally) {
    /* A valid record of another type is not accepted as TERMINAL state. */
    std::string wrong_tag;
    record::Writer wrong_tag_stream;
    wrong_tag_stream.init(&wrong_tag);
    {
        wrong_tag_stream.begin(record::Tag::screen);
        ASSERT_TRUE(wrong_tag_stream.finish() == record::Writer::FinishError::none);
    }
    Reader wrong_tag_source = Reader::fixed(wrong_tag.data(), wrong_tag.size());
    Terminal unused;
    ASSERT_TRUE(st::decode(&wrong_tag_source, talloc(), &unused) == Error::UnexpectedRecordTag);

    /* Build one canonical record for exhaustive truncation and allocation
     * failure checks below. */
    TermHolder terminal(talloc(), termOpts(10, 3));
    ASSERT_TRUE(terminal.ok);
    ASSERT_TRUE(terminal.t.setPwd("pwd"));
    ASSERT_TRUE(terminal.t.setTitle("title"));

    std::string encoded;
    record::Writer encoded_stream;
    encoded_stream.init(&encoded);
    ASSERT_TRUE(st::encode(&terminal.t, &encoded_stream) == Error::none);

    for (size_t fixture_len = 0; fixture_len < encoded.size(); fixture_len++) {
        Reader source = Reader::fixed(encoded.data(), fixture_len);
        Terminal restored;
        if (st::decode(&source, talloc(), &restored) != Error::none) continue;
        restored.deinit(talloc());
        ASSERT_TRUE(false);
    }

    zigstd::FailingAllocator failing(talloc(), 0);
    Reader failing_source = Reader::fixed(encoded.data(), encoded.size());
    Terminal failed;
    ASSERT_TRUE(st::decode(&failing_source, failing.allocator(), &failed) == Error::OutOfMemory);
}

TEST(snapshot_terminal, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
