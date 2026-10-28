/* Transliterated from Ghostty src/terminal/snapshot/terminal.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * TERMINAL record payload encoding.
 *
 * One TERMINAL record contains terminal-wide state shared by every screen. It
 * is the first record in a snapshot and declares which SCREEN sequences follow
 * it. Screen contents, cursors, and history are encoded by `screen.zig` and
 * `history.zig`.
 *
 * Snapshot version 1 supports the primary screen and an optional alternate
 * screen. `screen_count` is therefore one or two. SCREEN records identify
 * their destination by key and may appear in either order. Canonical encoders
 * write primary first, then alternate when present. `active_screen_key` must
 * name one of those declared screens.
 *
 * All integers are unsigned and little-endian.
 *
 * ## Binary Format
 *
 * The payload begins with a fixed header followed by terminal-wide variable
 * state. The enclosing record supplies the payload boundary.
 *
 *   0 +--------------------------+
 *     | Header                   |
 * 103 +--------------------------+
 *     | Tab stops                |
 *     | ceil(columns / 8) bytes  |
 *     +--------------------------+
 *     | Original palette         |
 *     | 256 * RGB                |
 *     +--------------------------+
 *     | Palette override mask    |
 *     | 32 bytes                 |
 *     +--------------------------+
 *     | Palette overrides        |
 *     | 3 * popcount(mask) bytes |
 *     +--------------------------+
 *     | PWD length (u32)         |
 *     +--------------------------+
 *     | PWD bytes                |
 *     +--------------------------+
 *     | Title length (u32)       |
 *     +--------------------------+
 *     | Title bytes              |
 * end +--------------------------+
 *
 * ### Header
 *
 *   0 +-----------------------------------+
 *     | Columns (u16)                     |
 *   2 +-----------------------------------+
 *     | Rows (u16)                        |
 *   4 +-----------------------------------+
 *     | Pixel width (u32)                 |
 *   8 +-----------------------------------+
 *     | Pixel height (u32)                |
 *  12 +-----------------------------------+
 *     | Scrolling-region top (u16)        |
 *  14 +-----------------------------------+
 *     | Scrolling-region bottom (u16)     |
 *  16 +-----------------------------------+
 *     | Scrolling-region left (u16)       |
 *  18 +-----------------------------------+
 *     | Scrolling-region right (u16)      |
 *  20 +-----------------------------------+
 *     | Status display (u8)               |
 *  21 +-----------------------------------+
 *     | Active screen key (u16)           |
 *  23 +-----------------------------------+
 *     | Screen count (u16)                |
 *  25 +-----------------------------------+
 *     | Previous codepoint (u32)          |
 *  29 +-----------------------------------+
 *     | Cursor is-default (u8)            |
 *  30 +-----------------------------------+
 *     | Cursor default visual style (u8)  |
 *  31 +-----------------------------------+
 *     | Cursor default blink policy (u8)  |
 *  32 +-----------------------------------+
 *     | Shell-redraw policy (u8)          |
 *  33 +-----------------------------------+
 *     | Modify-other-keys level 2 (u8)    |
 *  34 +-----------------------------------+
 *     | Mouse event (u8)                  |
 *  35 +-----------------------------------+
 *     | Mouse format (u8)                 |
 *  36 +-----------------------------------+
 *     | Mouse shift-capture policy (u8)   |
 *  37 +-----------------------------------+
 *     | Mouse shape (u8)                  |
 *  38 +-----------------------------------+
 *     | Password-input flag (u8)          |
 *  39 +-----------------------------------+
 *     | Current modes (ModePacked)        |
 *  47 +-----------------------------------+
 *     | Saved modes (ModePacked)          |
 *  55 +-----------------------------------+
 *     | Default modes (ModePacked)        |
 *  63 +-----------------------------------+
 *     | Background color (DynamicRGB)     |
 *  71 +-----------------------------------+
 *     | Foreground color (DynamicRGB)     |
 *  79 +-----------------------------------+
 *     | Cursor color (DynamicRGB)         |
 *  87 +-----------------------------------+
 *     | Maximum scrollback bytes (u64)    |
 *  95 +-----------------------------------+
 *     | Maximum scrollback rows (u64)     |
 * 103 +-----------------------------------+
 *
 * The previous codepoint is a Unicode scalar value or `0xffffffff` when there
 * is no previous character. Boolean fields are zero or one.
 * Cursor blink and mouse shift-capture policies encode null as zero, false as
 * one, and true as two.
 * Canonical encoders use only the documented enum and boolean values.
 * Decoders replace unknown semantic values with neutral native defaults,
 * discard invalid previous codepoints, and reset each invalid scrolling axis
 * to the terminal's full extent. Dimensions and screen count remain
 * structural.
 *
 * A scrollback limit of `0xffffffffffffffff` means unlimited. Every other
 * value is finite, including zero. The values are the source primary
 * PageList's explicit policies, not its dimension-adjusted effective limits.
 * Alternate-screen no-scrollback behavior is derived from the screen key.
 *
 * ### Tab stops
 *
 * One bit is encoded for each column, least-significant bit first within each
 * byte. Bit zero of the first byte is column zero. Unused high bits in the last
 * byte are canonically zero and ignored by decoders.
 *
 * ### Dynamic RGB
 *
 * The three terminal dynamic colors share this layout:
 *
 * 0 +----------------------------+
 *   | Default present (u8)       |
 * 1 +----------------------------+
 *   | Default RGB                |
 * 4 +----------------------------+
 *   | Override present (u8)      |
 * 5 +----------------------------+
 *   | Override RGB               |
 * 8 +----------------------------+
 *
 * Presence values are zero or one. The corresponding RGB bytes must be zero
 * when a value is absent. Decoders treat every other presence value as absent
 * and ignore RGB bytes belonging to an absent value.
 *
 * ### Palette
 *
 * The complete original 256-color palette is encoded first in index order.
 * Each mask bit then states whether the current palette entry overrides that
 * original value. Bit zero of the first mask byte is palette index zero.
 * Override RGB values follow in increasing index order with no index or count
 * fields.
 *
 * ### Modes
 *
 * Current, saved, and default modes use the same stable bit registry:
 *
 * bit  0  disable_keyboard
 * bit  1  insert
 * bit  2  send_receive_mode
 * bit  3  linefeed
 * bit  4  cursor_keys
 * bit  5  132_column
 * bit  6  slow_scroll
 * bit  7  reverse_colors
 * bit  8  origin
 * bit  9  wraparound
 * bit 10  autorepeat
 * bit 11  mouse_event_x10
 * bit 12  cursor_blinking
 * bit 13  cursor_visible
 * bit 14  enable_mode_3
 * bit 15  reverse_wrap
 * bit 16  alt_screen_legacy
 * bit 17  keypad_keys
 * bit 18  backarrow_key_mode
 * bit 19  enable_left_and_right_margin
 * bit 20  mouse_event_normal
 * bit 21  mouse_event_button
 * bit 22  mouse_event_any
 * bit 23  focus_event
 * bit 24  mouse_format_utf8
 * bit 25  mouse_format_sgr
 * bit 26  mouse_alternate_scroll
 * bit 27  mouse_format_urxvt
 * bit 28  mouse_format_sgr_pixels
 * bit 29  ignore_keypad_with_numlock
 * bit 30  alt_esc_prefix
 * bit 31  alt_sends_escape
 * bit 32  reverse_wrap_extended
 * bit 33  alt_screen
 * bit 34  save_cursor
 * bit 35  alt_screen_save_cursor_clear_enter
 * bit 36  bracketed_paste
 * bit 37  synchronized_output
 * bit 38  grapheme_cluster
 * bit 39  report_color_scheme
 * bit 40  report_visibility
 * bit 41  in_band_size_reports
 * bit 42  kitty_paste_events
 * bits 43-63  reserved, zero
 *
 * This is the packed field order of native `ModePacked`. Its layout is
 * well-defined and is the snapshot registry. Moving or adding a native mode
 * therefore requires a snapshot version bump. Decoders ignore reserved bits.
 *
 * ## Field classification
 *
 * The TERMINAL payload encodes terminal dimensions, pixel dimensions,
 * scrolling region, status display, tab stops, PWD, title, color state,
 * previous character, all three mode sets, cursor defaults, mouse behavior,
 * password-input state, source scrollback policies, the active screen key, and
 * the set of declared screens.
 *
 * SCREEN and HISTORY sequences encode each Screen's pages, cursor, saved
 * cursor, charset, protected mode, Kitty keyboard stack, semantic-click state,
 * and complete history. The semantic-prompt `seen` bit is derived from
 * restored resident content.
 *
 * Native allocators, I/O implementations, pools, pointers, page IDs, byte
 * accounting, tracked pins, and ScreenSet generations are reconstructed.
 * PageList row totals and the active viewport are derived from decoded pages.
 * The destination surface supplies its own focus state. Selections, scrolled
 * viewports, selection-scroll activity, dirty flags, search flags, hyperlink
 * hover state, and incremental compression state are presentation or cache
 * state and reset during restore.
 *
 * Kitty image state and glyph glossary registrations are unsupported by this
 * snapshot version and are ignored during capture. Unicode virtual placeholder
 * cells are preserved as grid content, but their image and placement state is
 * not restored. Build-time terminal behavior, Unicode width policy, parser
 * continuation, and external callbacks are version-level or caller-local
 * requirements.
 *
 * Native enum declaration indices and mode bit positions used by this format
 * are snapshot-version registries. Changing any of them requires a snapshot
 * version bump.
 *
 * Wisp, differences in shape rather than behavior:
 *   - The namespace is `terminal_record`: a nested `snapshot::terminal`
 *     would shadow `wisp::terminal` for every sibling snapshot header.
 *   - Error sets flatten into one enum, `Error`.
 *   - `?T` fields are `Maybe<T>`.
 *   - `Header.len` is a constant; upstream computes it at comptime by
 *     encoding a value and asserts it is 103. The golden test checks the
 *     encoded length.
 *   - `std.Io` is not threaded through: the native Terminal takes none.
 *   - DynamicPalette owns its original palette with new/delete rather than
 *     the allocator (see color.hpp), so `DynamicPalette.init` cannot fail.
 */
#pragma once
#ifndef WISP_TERMINAL_SNAPSHOT_TERMINAL_HPP
#define WISP_TERMINAL_SNAPSHOT_TERMINAL_HPP

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "../../vt/screen.hpp"
#include "../../vt/screen_set.hpp"
#include "../../vt/tabstops.hpp"
#include "../../vt/terminal.hpp"
#include "../../zigstd/allocator.hpp"
#include "../../zigstd/reader.hpp"
#include "../color.hpp"
#include "../modes.hpp"
#include "io.hpp"
#include "record.hpp"

namespace wisp {
namespace terminal {
namespace snapshot {
namespace terminal_record {

typedef ::wisp::vt::Terminal Terminal;
typedef ::wisp::vt::Screen TerminalScreen;
typedef ::wisp::vt::ScreenSet::Key TerminalScreenKey;
typedef ::wisp::vt::Tabstops TerminalTabstops;
typedef ::wisp::terminal::modes::ModePacked ModePacked;
typedef ::wisp::terminal::ansi::StatusDisplay StatusDisplay;
typedef ::wisp::terminal::osc::semantic_prompt::Redraw Redraw;
typedef ::wisp::terminal::mouse::Event MouseEvent;
typedef ::wisp::terminal::mouse::Format MouseFormat;
typedef ::wisp::terminal::mouse::Shape MouseShape;
typedef ::wisp::zigstd::ReadError ReadError;
typedef ::wisp::zigstd::Allocator Allocator;
template <typename T>
using Maybe = ::wisp::vt::Maybe<T>;

/* Reuse the terminal's semantic RGB value without adopting its memory layout
 * as a wire encoding. */
typedef ::wisp::terminal::RGB RGB;

/* Reuse the terminal's semantic dynamic color without adopting its memory
 * layout as a wire encoding. */
typedef ::wisp::terminal::DynamicRGB DynamicRGB;

typedef ::wisp::terminal::DynamicPalette DynamicPalette;
typedef ::wisp::terminal::Palette Palette;

/* Wisp: @typeInfo(ModePacked).backing_integer is u43. */
static const unsigned mode_bit_count = 43;
static const uint64_t mode_bits_mask = ((uint64_t)1 << mode_bit_count) - 1;

enum class Error : uint8_t {
    none = 0,

    /* std.Io.Reader.Error / std.Io.Writer.Error / Allocator.Error */
    EndOfStream,
    ReadFailed,
    WriteFailed,
    OutOfMemory,

    /* ValidationError */
    InvalidDimensions,
    InvalidScrollingRegion,
    InvalidScreenCount,
    InvalidActiveScreenKey,
    InvalidPreviousCodepoint,
    InvalidScrollbackLimit,

    /* HeaderInitError */
    ScrollbackLimitOverflow,

    /* PayloadEncodeError */
    InvalidTabStops,
    InvalidPalette,
    StringTooLong,

    /* record.Writer.FinishError / record.Reader.InitError / FinishError */
    PayloadTooLarge,
    InvalidTag,
    InvalidChecksum,
    PayloadNotExhausted,

    /* The next record is valid but is not a TERMINAL. */
    UnexpectedRecordTag,
};

inline Error fromRead(ReadError e) {
    switch (e) {
        case ReadError::none: return Error::none;
        case ReadError::EndOfStream: return Error::EndOfStream;
        case ReadError::ReadFailed: return Error::ReadFailed;
    }
    return Error::EndOfStream;
}

/* Wisp: `enumFromInt(T, raw)` for the contiguous zero-based native enums
 * this format uses; `count` is the number of declared tags. */
template <typename T>
inline bool enumFromInt(uint64_t raw, uint64_t count, T *out) {
    if (raw >= count) return false;
    *out = (T)raw;
    return true;
}

inline uint8_t encodeOptionalBool(Maybe<bool> value) {
    return value.has ? (value.value ? 2 : 1) : 0;
}

inline void encodeRGB(RGB value, std::string *writer) {
    writer->push_back((char)value.r);
    writer->push_back((char)value.g);
    writer->push_back((char)value.b);
}

template <typename ReaderT>
inline bool decodeRGB(ReaderT *reader, RGB *out, ReadError *err) {
    RGB result;
    if (!reader->takeByte(&result.r, err)) return false;
    if (!reader->takeByte(&result.g, err)) return false;
    if (!reader->takeByte(&result.b, err)) return false;
    *out = result;
    return true;
}

inline void encodeDynamicRGB(const DynamicRGB &value, std::string *writer) {
    if (value.has_default) {
        writer->push_back((char)1);
        encodeRGB(value.default_, writer);
    } else {
        writer->push_back((char)0);
        writer->append(3, (char)0);
    }

    if (value.has_override) {
        writer->push_back((char)1);
        encodeRGB(value.override_, writer);
    } else {
        writer->push_back((char)0);
        writer->append(3, (char)0);
    }
}

template <typename ReaderT>
inline bool decodeDynamicRGB(ReaderT *reader, DynamicRGB *out, ReadError *err) {
    uint8_t default_present = 0;
    if (!reader->takeByte(&default_present, err)) return false;
    RGB default_rgb;
    if (!decodeRGB(reader, &default_rgb, err)) return false;

    uint8_t override_present = 0;
    if (!reader->takeByte(&override_present, err)) return false;
    RGB override_rgb;
    if (!decodeRGB(reader, &override_rgb, err)) return false;

    DynamicRGB result = DynamicRGB::unset();
    if (default_present == 1) {
        result.has_default = true;
        result.default_ = default_rgb;
    }
    if (override_present == 1) {
        result.has_override = true;
        result.override_ = override_rgb;
    }
    *out = result;
    return true;
}

inline Maybe<uint64_t> decodeOptionalLimit(uint64_t raw) {
    return raw == UINT64_MAX ? Maybe<uint64_t>::none() : Maybe<uint64_t>(raw);
}

inline Error encodeScrollbackLimit(size_t value, Maybe<uint64_t> *out) {
    if (value == SIZE_MAX) {
        *out = Maybe<uint64_t>::none();
        return Error::none;
    }
    /* Wisp: std.math.cast(u64, value) cannot fail; usize is at most 64 bits. */
    const uint64_t result = (uint64_t)value;
    if (result == UINT64_MAX) return Error::ScrollbackLimitOverflow;
    *out = Maybe<uint64_t>(result);
    return Error::none;
}

inline Maybe<size_t> nativeScrollbackLimit(Maybe<uint64_t> value) {
    if (!value.has) return Maybe<size_t>::none();
    const uint64_t cap = (uint64_t)(SIZE_MAX - 1);
    return Maybe<size_t>((size_t)(value.value < cap ? value.value : cap));
}

/* The fixed fields at the start of every TERMINAL payload. */
struct Header {
    /* Number of bytes written by `encode`. */
    static const size_t len = 103;

    /* Terminal geometry and its current scrolling region. */
    uint16_t columns = 1;
    uint16_t rows = 1;
    uint32_t width_px = 0;
    uint32_t height_px = 0;

    uint16_t scrolling_region_top = 0;
    uint16_t scrolling_region_bottom = 0;
    uint16_t scrolling_region_left = 0;
    uint16_t scrolling_region_right = 0;

    /* Status display, screen routing, and character context. */
    StatusDisplay status_display = StatusDisplay::main;
    TerminalScreenKey active_screen_key = TerminalScreenKey::primary;
    uint16_t screen_count = 1;
    Maybe<uint32_t> previous_codepoint;

    /* Cursor presentation defaults shared by the screens. */
    bool cursor_is_default = true;
    TerminalScreen::CursorStyle cursor_default_style = TerminalScreen::CursorStyle::block;
    Maybe<bool> cursor_default_blink;

    /* Terminal input, semantic redraw, and pointer behavior. */
    Redraw shell_redraw = Redraw::true_;
    bool modify_other_keys_2 = false;
    MouseEvent mouse_event = MouseEvent::none;
    MouseFormat mouse_format = MouseFormat::x10;
    Maybe<bool> mouse_shift_capture;
    MouseShape mouse_shape = MouseShape::text;
    bool password_input = false;

    /* Runtime, saved, and reset mode sets. */
    ModePacked current_modes;
    ModePacked saved_modes;
    ModePacked default_modes;

    /* Terminal-wide dynamic color state. */
    DynamicRGB background = DynamicRGB::unset();
    DynamicRGB foreground = DynamicRGB::unset();
    DynamicRGB cursor_color = DynamicRGB::unset();

    /* Explicit primary-screen scrollback policies. */
    Maybe<uint64_t> max_scrollback_bytes;
    Maybe<uint64_t> max_scrollback_rows;

    /* Capture the terminal-wide native state represented by this header. */
    static Error init(const Terminal *terminal, Header *out) {
        const TerminalScreen *primary = terminal->screens.get(TerminalScreenKey::primary);

        Header h;
        /* Terminal geometry and its current scrolling region. */
        h.columns = terminal->cols;
        h.rows = terminal->rows;
        h.width_px = terminal->width_px;
        h.height_px = terminal->height_px;
        h.scrolling_region_top = terminal->scrolling_region.top;
        h.scrolling_region_bottom = terminal->scrolling_region.bottom;
        h.scrolling_region_left = terminal->scrolling_region.left;
        h.scrolling_region_right = terminal->scrolling_region.right;

        /* Status display, screen routing, and character context. */
        h.status_display = terminal->status_display;
        h.active_screen_key = terminal->screens.active_key;
        h.screen_count = terminal->screens.get(TerminalScreenKey::alternate) == nullptr ? 1 : 2;
        h.previous_codepoint = terminal->previous_char;

        /* Cursor presentation defaults shared by the screens. */
        h.cursor_is_default = terminal->cursor.is_default;
        h.cursor_default_style = terminal->cursor.default_style;
        h.cursor_default_blink = terminal->cursor.default_blink;

        /* Terminal input, semantic redraw, and pointer behavior. */
        h.shell_redraw = terminal->flags.shell_redraws_prompt;
        h.modify_other_keys_2 = terminal->flags.modify_other_keys_2;
        h.mouse_event = terminal->flags.mouse_event;
        h.mouse_format = terminal->flags.mouse_format;
        switch (terminal->flags.mouse_shift_capture) {
            case Terminal::Flags::MouseShiftCapture::null_: h.mouse_shift_capture = Maybe<bool>::none(); break;
            case Terminal::Flags::MouseShiftCapture::false_: h.mouse_shift_capture = Maybe<bool>(false); break;
            case Terminal::Flags::MouseShiftCapture::true_: h.mouse_shift_capture = Maybe<bool>(true); break;
        }
        h.mouse_shape = terminal->mouse_shape;
        h.password_input = terminal->flags.password_input;

        /* Runtime, saved, and reset mode sets. */
        h.current_modes = terminal->modes.values;
        h.saved_modes = terminal->modes.saved;
        h.default_modes = terminal->modes.default_;

        /* Terminal-wide dynamic color state. */
        h.background = terminal->colors.background;
        h.foreground = terminal->colors.foreground;
        h.cursor_color = terminal->colors.cursor;

        /* Explicit primary-screen scrollback policies. */
        Error err = encodeScrollbackLimit(primary->pages.limits.bytes.explicit_, &h.max_scrollback_bytes);
        if (err != Error::none) return err;
        err = encodeScrollbackLimit(primary->pages.limits.lines.explicit_, &h.max_scrollback_rows);
        if (err != Error::none) return err;

        *out = h;
        return Error::none;
    }

    /* Encode the fixed TERMINAL payload header field by field. */
    Error encode(std::string *writer) const {
        const Error err = validate();
        if (err != Error::none) return err;

        /* Terminal geometry and its current scrolling region. */
        io::writeInt<uint16_t>(writer, columns);
        io::writeInt<uint16_t>(writer, rows);
        io::writeInt<uint32_t>(writer, width_px);
        io::writeInt<uint32_t>(writer, height_px);
        io::writeInt<uint16_t>(writer, scrolling_region_top);
        io::writeInt<uint16_t>(writer, scrolling_region_bottom);
        io::writeInt<uint16_t>(writer, scrolling_region_left);
        io::writeInt<uint16_t>(writer, scrolling_region_right);

        /* Status display, screen routing, and character context. */
        writer->push_back((char)(uint8_t)status_display);
        io::writeInt<uint16_t>(writer, (uint16_t)active_screen_key);
        io::writeInt<uint16_t>(writer, screen_count);
        io::writeInt<uint32_t>(writer, previous_codepoint.has ? previous_codepoint.value : UINT32_MAX);

        /* Cursor presentation defaults shared by the screens. */
        writer->push_back((char)(cursor_is_default ? 1 : 0));
        writer->push_back((char)(uint8_t)cursor_default_style);
        writer->push_back((char)encodeOptionalBool(cursor_default_blink));

        /* Terminal input, semantic redraw, and pointer behavior. */
        writer->push_back((char)(uint8_t)shell_redraw);
        writer->push_back((char)(modify_other_keys_2 ? 1 : 0));
        writer->push_back((char)(uint8_t)mouse_event);
        writer->push_back((char)(uint8_t)mouse_format);
        writer->push_back((char)encodeOptionalBool(mouse_shift_capture));
        writer->push_back((char)(uint8_t)mouse_shape);
        writer->push_back((char)(password_input ? 1 : 0));

        /* Runtime, saved, and reset mode sets. ModePacked occupies 43 bits;
         * its eight-byte wire slots zero-extend the native packed value. */
        const ModePacked mode_values[3] = { current_modes, saved_modes, default_modes };
        for (size_t i = 0; i < 3; i++) {
            io::writeInt<uint64_t>(writer, mode_values[i].bits & mode_bits_mask);
        }

        /* Terminal-wide dynamic color state. */
        encodeDynamicRGB(background, writer);
        encodeDynamicRGB(foreground, writer);
        encodeDynamicRGB(cursor_color, writer);

        /* Explicit primary-screen scrollback policies. */
        io::writeInt<uint64_t>(writer, max_scrollback_bytes.orelse(UINT64_MAX));
        io::writeInt<uint64_t>(writer, max_scrollback_rows.orelse(UINT64_MAX));
        return Error::none;
    }

    /* Decode the fixed TERMINAL payload header, normalizing semantic values. */
    template <typename ReaderT>
    static Error decode(ReaderT *reader, Header *out) {
        ReadError rerr = ReadError::none;
#define WISP_TRY_READ(expr) \
    if (!(expr)) return fromRead(rerr)

        /* Terminal geometry and its current scrolling region. */
        uint16_t columns = 0, rows = 0;
        uint32_t width_px = 0, height_px = 0;
        uint16_t scrolling_region_top = 0, scrolling_region_bottom = 0;
        uint16_t scrolling_region_left = 0, scrolling_region_right = 0;
        WISP_TRY_READ(io::readInt<uint16_t>(reader, &columns, &rerr));
        WISP_TRY_READ(io::readInt<uint16_t>(reader, &rows, &rerr));
        WISP_TRY_READ(io::readInt<uint32_t>(reader, &width_px, &rerr));
        WISP_TRY_READ(io::readInt<uint32_t>(reader, &height_px, &rerr));
        WISP_TRY_READ(io::readInt<uint16_t>(reader, &scrolling_region_top, &rerr));
        WISP_TRY_READ(io::readInt<uint16_t>(reader, &scrolling_region_bottom, &rerr));
        WISP_TRY_READ(io::readInt<uint16_t>(reader, &scrolling_region_left, &rerr));
        WISP_TRY_READ(io::readInt<uint16_t>(reader, &scrolling_region_right, &rerr));

        /* Status display, screen routing, and character context. */
        uint8_t byte = 0;
        WISP_TRY_READ(reader->takeByte(&byte, &rerr));
        StatusDisplay status_display = StatusDisplay::main;
        if (!enumFromInt(byte, 2, &status_display)) status_display = StatusDisplay::main;
        uint16_t active_screen_key_raw = 0, screen_count = 0;
        uint32_t previous_raw = 0;
        WISP_TRY_READ(io::readInt<uint16_t>(reader, &active_screen_key_raw, &rerr));
        WISP_TRY_READ(io::readInt<uint16_t>(reader, &screen_count, &rerr));
        WISP_TRY_READ(io::readInt<uint32_t>(reader, &previous_raw, &rerr));
        Maybe<uint32_t> previous_codepoint;
        do {
            if (previous_raw == UINT32_MAX) break;
            /* Wisp: std.math.cast(u21, previous_raw) */
            if (previous_raw > 0x1FFFFF) break;
            const uint32_t value = previous_raw;
            if (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) break;
            previous_codepoint = Maybe<uint32_t>(value);
        } while (false);

        /* Cursor presentation defaults shared by the screens. */
        WISP_TRY_READ(reader->takeByte(&byte, &rerr));
        bool cursor_is_default;
        switch (byte) {
            case 0: cursor_is_default = false; break;
            case 1: cursor_is_default = true; break;
            default: cursor_is_default = true; break;
        }
        WISP_TRY_READ(reader->takeByte(&byte, &rerr));
        TerminalScreen::CursorStyle cursor_default_style = TerminalScreen::CursorStyle::block;
        if (!enumFromInt(byte, 4, &cursor_default_style)) cursor_default_style = TerminalScreen::CursorStyle::block;
        WISP_TRY_READ(reader->takeByte(&byte, &rerr));
        Maybe<bool> cursor_default_blink;
        switch (byte) {
            case 0: cursor_default_blink = Maybe<bool>::none(); break;
            case 1: cursor_default_blink = Maybe<bool>(false); break;
            case 2: cursor_default_blink = Maybe<bool>(true); break;
            default: cursor_default_blink = Maybe<bool>::none(); break;
        }

        /* Terminal input, semantic redraw, and pointer behavior. */
        WISP_TRY_READ(reader->takeByte(&byte, &rerr));
        Redraw shell_redraw = Redraw::true_;
        if (!enumFromInt(byte, 3, &shell_redraw)) shell_redraw = Redraw::true_;
        WISP_TRY_READ(reader->takeByte(&byte, &rerr));
        bool modify_other_keys_2;
        switch (byte) {
            case 0: modify_other_keys_2 = false; break;
            case 1: modify_other_keys_2 = true; break;
            default: modify_other_keys_2 = false; break;
        }
        WISP_TRY_READ(reader->takeByte(&byte, &rerr));
        MouseEvent mouse_event = MouseEvent::none;
        if (!enumFromInt(byte, 5, &mouse_event)) mouse_event = MouseEvent::none;
        WISP_TRY_READ(reader->takeByte(&byte, &rerr));
        MouseFormat mouse_format = MouseFormat::x10;
        if (!enumFromInt(byte, 5, &mouse_format)) mouse_format = MouseFormat::x10;
        WISP_TRY_READ(reader->takeByte(&byte, &rerr));
        Maybe<bool> mouse_shift_capture;
        switch (byte) {
            case 0: mouse_shift_capture = Maybe<bool>::none(); break;
            case 1: mouse_shift_capture = Maybe<bool>(false); break;
            case 2: mouse_shift_capture = Maybe<bool>(true); break;
            default: mouse_shift_capture = Maybe<bool>::none(); break;
        }
        WISP_TRY_READ(reader->takeByte(&byte, &rerr));
        MouseShape mouse_shape = MouseShape::text;
        if (!enumFromInt(byte, 34, &mouse_shape)) mouse_shape = MouseShape::text;
        WISP_TRY_READ(reader->takeByte(&byte, &rerr));
        bool password_input;
        switch (byte) {
            case 0: password_input = false; break;
            case 1: password_input = true; break;
            default: password_input = false; break;
        }

        /* Runtime, saved, and reset mode sets. */
        ModePacked mode_values[3];
        for (size_t i = 0; i < 3; i++) {
            uint64_t raw = 0;
            WISP_TRY_READ(io::readInt<uint64_t>(reader, &raw, &rerr));
            mode_values[i].bits = raw & mode_bits_mask;
        }

        /* Terminal-wide dynamic color state. */
        DynamicRGB background, foreground, cursor_color;
        WISP_TRY_READ(decodeDynamicRGB(reader, &background, &rerr));
        WISP_TRY_READ(decodeDynamicRGB(reader, &foreground, &rerr));
        WISP_TRY_READ(decodeDynamicRGB(reader, &cursor_color, &rerr));

        /* Explicit primary-screen scrollback policies. */
        uint64_t raw_limit = 0;
        WISP_TRY_READ(io::readInt<uint64_t>(reader, &raw_limit, &rerr));
        const Maybe<uint64_t> max_scrollback_bytes = decodeOptionalLimit(raw_limit);
        WISP_TRY_READ(io::readInt<uint64_t>(reader, &raw_limit, &rerr));
        const Maybe<uint64_t> max_scrollback_rows = decodeOptionalLimit(raw_limit);
#undef WISP_TRY_READ

        /* Dimensions and sequence count are the only header fields that define
         * following allocation or record boundaries. */
        if (columns == 0 || rows == 0) return Error::InvalidDimensions;
        if (screen_count != 1 && screen_count != 2) {
            return Error::InvalidScreenCount;
        }

        const bool scrolling_region_vertical_valid =
            scrolling_region_top <= scrolling_region_bottom && scrolling_region_bottom < rows;
        const bool scrolling_region_horizontal_valid =
            scrolling_region_left <= scrolling_region_right && scrolling_region_right < columns;
        TerminalScreenKey active_screen_key = TerminalScreenKey::primary;
        do {
            TerminalScreenKey decoded = TerminalScreenKey::primary;
            if (!enumFromInt(active_screen_key_raw, 2, &decoded)) break;
            if (decoded == TerminalScreenKey::alternate && screen_count != 2) break;
            active_screen_key = decoded;
        } while (false);

        Header result;
        result.columns = columns;
        result.rows = rows;
        result.width_px = width_px;
        result.height_px = height_px;

        result.scrolling_region_top = scrolling_region_vertical_valid ? scrolling_region_top : 0;
        result.scrolling_region_bottom =
            scrolling_region_vertical_valid ? scrolling_region_bottom : (uint16_t)(rows - 1);
        result.scrolling_region_left = scrolling_region_horizontal_valid ? scrolling_region_left : 0;
        result.scrolling_region_right =
            scrolling_region_horizontal_valid ? scrolling_region_right : (uint16_t)(columns - 1);

        result.status_display = status_display;
        result.active_screen_key = active_screen_key;
        result.screen_count = screen_count;
        result.previous_codepoint = previous_codepoint;

        result.cursor_is_default = cursor_is_default;
        result.cursor_default_style = cursor_default_style;
        result.cursor_default_blink = cursor_default_blink;

        result.shell_redraw = shell_redraw;
        result.modify_other_keys_2 = modify_other_keys_2;
        result.mouse_event = mouse_event;
        result.mouse_format = mouse_format;
        result.mouse_shift_capture = mouse_shift_capture;
        result.mouse_shape = mouse_shape;
        result.password_input = password_input;

        result.current_modes = mode_values[0];
        result.saved_modes = mode_values[1];
        result.default_modes = mode_values[2];

        result.background = background;
        result.foreground = foreground;
        result.cursor_color = cursor_color;

        result.max_scrollback_bytes = max_scrollback_bytes;
        result.max_scrollback_rows = max_scrollback_rows;
        *out = result;
        return Error::none;
    }

    Error validate() const {
        if (columns == 0 || rows == 0) {
            return Error::InvalidDimensions;
        }
        if (scrolling_region_top > scrolling_region_bottom || scrolling_region_bottom >= rows ||
            scrolling_region_left > scrolling_region_right || scrolling_region_right >= columns) {
            return Error::InvalidScrollingRegion;
        }

        if (screen_count != 1 && screen_count != 2) {
            return Error::InvalidScreenCount;
        }
        if (active_screen_key == TerminalScreenKey::alternate && screen_count != 2) {
            return Error::InvalidActiveScreenKey;
        }

        if (previous_codepoint.has) {
            const uint32_t value = previous_codepoint.value;
            if (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
                return Error::InvalidPreviousCodepoint;
            }
        }

        if ((max_scrollback_bytes.has && max_scrollback_bytes.value == UINT64_MAX) ||
            (max_scrollback_rows.has && max_scrollback_rows.value == UINT64_MAX)) {
            return Error::InvalidScrollbackLimit;
        }
        return Error::none;
    }
};

/* Allocator-owned semantic state decoded from one TERMINAL payload.
 *
 * The fixed header is a value. `tabstops`, `palette`, `pwd`, and `title`
 * own allocations and are released by `deinit`. */
struct DecodedPayload {
    Header header;
    TerminalTabstops tabstops;
    DynamicPalette palette = DynamicPalette::default_();
    uint8_t *pwd = nullptr;
    size_t pwd_len = 0;
    uint8_t *title = nullptr;
    size_t title_len = 0;

    void deinit(Allocator alloc) {
        tabstops.deinit(alloc);
        palette.deinit();
        if (pwd_len > 0) alloc.free(pwd, pwd_len);
        if (title_len > 0) alloc.free(title, title_len);
        pwd = nullptr;
        pwd_len = 0;
        title = nullptr;
        title_len = 0;
    }
};

/* Encode one complete TERMINAL payload without record framing. */
inline Error encodePayload(const Header &header, const TerminalTabstops *tabstops, const DynamicPalette *palette,
                           const uint8_t *pwd, size_t pwd_len, const uint8_t *title, size_t title_len,
                           std::string *writer) {
    /* Validate the complete semantic value before writing any bytes. */
    Error err = header.validate();
    if (err != Error::none) return err;
    if (tabstops->cols != (size_t)header.columns) {
        return Error::InvalidTabStops;
    }
    if (pwd_len > UINT32_MAX || title_len > UINT32_MAX) {
        return Error::StringTooLong;
    }
    for (size_t index = 0; index < 256; index++) {
        if (!palette->mask.isSet(index) && !(*palette->original)[index].eql(palette->current[index])) {
            return Error::InvalidPalette;
        }
    }

    err = header.encode(writer);
    if (err != Error::none) return err;

    /* Tab stops are packed least-significant bit first. */
    const size_t tabstop_len = ((size_t)header.columns + 7) / 8;
    for (size_t byte_index = 0; byte_index < tabstop_len; byte_index++) {
        uint8_t raw = 0;
        for (size_t bit = 0; bit < 8; bit++) {
            const size_t column = byte_index * 8 + bit;
            if (column >= header.columns) break;
            if (tabstops->get(column)) {
                raw |= (uint8_t)(1u << bit);
            }
        }
        writer->push_back((char)raw);
    }

    /* Encode the original palette, then a compact mask and only the active
     * overrides in increasing palette-index order. */
    for (size_t index = 0; index < 256; index++) encodeRGB((*palette->original)[index], writer);
    for (size_t byte_index = 0; byte_index < 32; byte_index++) {
        uint8_t raw = 0;
        for (size_t bit = 0; bit < 8; bit++) {
            if (palette->mask.isSet(byte_index * 8 + bit)) {
                raw |= (uint8_t)(1u << bit);
            }
        }
        writer->push_back((char)raw);
    }
    for (size_t index = 0; index < 256; index++) {
        if (palette->mask.isSet(index)) encodeRGB(palette->current[index], writer);
    }

    /* Length-prefixed terminal strings end the payload. */
    io::writeInt<uint32_t>(writer, (uint32_t)pwd_len);
    writer->append((const char *)pwd, pwd_len);
    io::writeInt<uint32_t>(writer, (uint32_t)title_len);
    writer->append((const char *)title, title_len);
    return Error::none;
}

/* Decode one complete allocator-owned TERMINAL payload.
 *
 * The enclosing record reader remains responsible for verifying exact payload
 * exhaustion. On success, the caller owns the result and must call `deinit`. */
template <typename ReaderT>
inline Error decodePayload(ReaderT *reader, Allocator alloc, DecodedPayload *out) {
    ReadError rerr = ReadError::none;
    Header header;
    Error err = Header::decode(reader, &header);
    if (err != Error::none) return err;

    /* Restore the native tab-stop representation from its packed wire bits. */
    TerminalTabstops tabstops;
    if (TerminalTabstops::init(alloc, header.columns, 0, &tabstops) != TerminalTabstops::Error::none) {
        return Error::OutOfMemory;
    }
    struct TabstopsGuard {
        TerminalTabstops *t;
        Allocator a;
        bool armed;
        ~TabstopsGuard() {
            if (armed) t->deinit(a);
        }
    } tabstops_guard = { &tabstops, alloc, true };
    const size_t tabstop_len = ((size_t)header.columns + 7) / 8;
    for (size_t byte_index = 0; byte_index < tabstop_len; byte_index++) {
        uint8_t raw = 0;
        if (!reader->takeByte(&raw, &rerr)) return fromRead(rerr);
        for (size_t bit = 0; bit < 8; bit++) {
            const uint8_t mask = (uint8_t)(1u << bit);
            const size_t column = byte_index * 8 + bit;
            if (column < header.columns && (raw & mask) != 0) {
                tabstops.set(column);
            }
        }
    }

    /* Reconstruct the dynamic palette from its original values and sparse
     * current-value overrides. */
    Palette original;
    for (size_t index = 0; index < 256; index++) {
        if (!decodeRGB(reader, &original[index], &rerr)) return fromRead(rerr);
    }
    uint8_t override_mask[32];
    if (!reader->readSliceAll(override_mask, sizeof(override_mask), &rerr)) return fromRead(rerr);

    DynamicPalette palette = DynamicPalette::init(original);
    struct PaletteGuard {
        DynamicPalette *p;
        bool armed;
        ~PaletteGuard() {
            if (armed) p->deinit();
        }
    } palette_guard = { &palette, true };
    for (size_t index = 0; index < 256; index++) {
        const uint8_t mask = (uint8_t)(1u << (index % 8));
        if ((override_mask[index / 8] & mask) != 0) {
            RGB value;
            if (!decodeRGB(reader, &value, &rerr)) return fromRead(rerr);
            palette.set((uint8_t)index, value);
        }
    }

    /* Read the two allocator-owned terminal strings last so all earlier
     * fixed-size validation happens before allocating them. */
    struct BytesGuard {
        Allocator a;
        uint8_t *p;
        size_t n;
        ~BytesGuard() {
            if (n > 0) a.free(p, n);
        }
    };
    uint32_t pwd_len = 0;
    if (!io::readInt<uint32_t>(reader, &pwd_len, &rerr)) return fromRead(rerr);
    BytesGuard pwd_guard = { alloc, nullptr, 0 };
    if (pwd_len > 0) {
        pwd_guard.p = alloc.alloc(pwd_len);
        if (!pwd_guard.p) return Error::OutOfMemory;
        pwd_guard.n = pwd_len;
        if (!reader->readSliceAll(pwd_guard.p, pwd_len, &rerr)) return fromRead(rerr);
    }

    uint32_t title_len = 0;
    if (!io::readInt<uint32_t>(reader, &title_len, &rerr)) return fromRead(rerr);
    BytesGuard title_guard = { alloc, nullptr, 0 };
    if (title_len > 0) {
        title_guard.p = alloc.alloc(title_len);
        if (!title_guard.p) return Error::OutOfMemory;
        title_guard.n = title_len;
        if (!reader->readSliceAll(title_guard.p, title_len, &rerr)) return fromRead(rerr);
    }

    out->header = header;
    out->tabstops = tabstops;
    out->palette = palette;
    out->pwd = pwd_guard.p;
    out->pwd_len = pwd_guard.n;
    out->title = title_guard.p;
    out->title_len = title_guard.n;
    tabstops_guard.armed = false;
    palette_guard.armed = false;
    pwd_guard.n = 0;
    title_guard.n = 0;
    return Error::none;
}

/* Encode terminal-wide native state as one framed TERMINAL record.
 *
 * State not represented by this snapshot version is ignored. Payload failures
 * emit no part of the TERMINAL record. */
inline Error encode(const Terminal *terminal, record::Writer *destination) {
    Header header;
    Error err = Header::init(terminal, &header);
    if (err != Error::none) return err;

    std::string *payload = destination->begin(record::Tag::terminal);
    /* Wisp: getPwd()/getTitle() orelse "" — the native lists carry a
     * trailing NUL sentinel that is not part of the value. */
    const size_t pwd_len = terminal->pwd.len > 0 ? terminal->pwd.len - 1 : 0;
    const size_t title_len = terminal->title.len > 0 ? terminal->title.len - 1 : 0;
    err = encodePayload(header, &terminal->tabstops, &terminal->colors.palette, terminal->pwd.items, pwd_len,
                        terminal->title.items, title_len, payload);
    if (err != Error::none) {
        destination->cancel();
        return err;
    }
    switch (destination->finish()) {
        case record::Writer::FinishError::none: break;
        case record::Writer::FinishError::WriteFailed: return Error::WriteFailed;
        case record::Writer::FinishError::PayloadTooLarge: return Error::PayloadTooLarge;
    }
    return Error::none;
}

/* Decode one framed TERMINAL record directly into native terminal state.
 *
 * The returned terminal owns every decoded allocation and contains empty
 * primary and optional alternate screens with the declared routing. Later
 * SCREEN records replace those screens in place. On failure, all decoded and
 * partially initialized native state is released.
 *
 * Wisp: the result is written to `*out` only on success. */
template <typename SourceT>
inline Error decode(SourceT *source, Allocator alloc, Terminal *out) {
    /* Finish the complete record before constructing native terminal state. */
    DecodedPayload payload;
    {
        record::Reader record_reader;
        record::Reader::InitError ierr = record::Reader::InitError::none;
        if (!record_reader.init(source, &ierr)) {
            return ierr == record::Reader::InitError::InvalidTag ? Error::InvalidTag : Error::EndOfStream;
        }
        if (record_reader.header.tag != record::Tag::terminal) {
            return Error::UnexpectedRecordTag;
        }

        const Error err = decodePayload(record_reader.payloadReader(), alloc, &payload);
        if (err != Error::none) return err;
        Error finish_err = Error::none;
        switch (record_reader.finish()) {
            case record::Reader::FinishError::none: break;
            case record::Reader::FinishError::InvalidChecksum: finish_err = Error::InvalidChecksum; break;
            case record::Reader::FinishError::PayloadNotExhausted: finish_err = Error::PayloadNotExhausted; break;
        }
        if (finish_err != Error::none) {
            payload.deinit(alloc);
            return finish_err;
        }
    }
    struct PayloadGuard {
        DecodedPayload *p;
        Allocator a;
        ~PayloadGuard() { p->deinit(a); }
    } payload_guard = { &payload, alloc };

    const Header header = payload.header;
    const Maybe<size_t> max_scrollback_bytes = nativeScrollbackLimit(header.max_scrollback_bytes);
    const Maybe<size_t> max_scrollback_lines = nativeScrollbackLimit(header.max_scrollback_rows);

    /* Terminal.init establishes native pools, pins, defaults, and ownership. */
    Terminal::Options opts(header.columns, header.rows);
    opts.max_scrollback_bytes = max_scrollback_bytes;
    opts.max_scrollback_lines = max_scrollback_lines;
    opts.colors.background = header.background;
    opts.colors.foreground = header.foreground;
    opts.colors.cursor = header.cursor_color;
    opts.colors.palette = payload.palette;
    opts.default_modes = header.default_modes;
    opts.default_cursor_style = header.cursor_default_style;
    opts.default_cursor_blink = header.cursor_default_blink;
    Terminal result;
    if (!Terminal::init(alloc, opts, &result)) return Error::OutOfMemory;
    struct ResultGuard {
        Terminal *t;
        Allocator a;
        bool armed;
        ~ResultGuard() {
            if (armed) t->deinit(a);
        }
    } result_guard = { &result, alloc, true };

    /* The terminal owns the decoded palette now, so the payload must not
     * release it. */
    payload.palette = DynamicPalette::default_();

    /* Recreate the optional screen now so ScreenSet routing is complete before
     * its empty screens are replaced by the following SCREEN records. */
    if (header.screen_count == 2) {
        TerminalScreen::Options screen_opts(header.columns, header.rows, Maybe<size_t>((size_t)0));
        if (result.screens.getInit(alloc, TerminalScreenKey::alternate, screen_opts) == nullptr) {
            return Error::OutOfMemory;
        }
    }
    result.screens.switchTo(header.active_screen_key);

    /* Restore terminal-wide runtime state that is not an initialization
     * policy. Presentation-only flags retain Terminal.init defaults. */
    result.width_px = header.width_px;
    result.height_px = header.height_px;
    result.scrolling_region.top = header.scrolling_region_top;
    result.scrolling_region.bottom = header.scrolling_region_bottom;
    result.scrolling_region.left = header.scrolling_region_left;
    result.scrolling_region.right = header.scrolling_region_right;
    result.status_display = header.status_display;
    result.previous_char = header.previous_codepoint;
    result.cursor.is_default = header.cursor_is_default;
    result.cursor.default_style = header.cursor_default_style;
    result.cursor.default_blink = header.cursor_default_blink;
    result.modes.values = header.current_modes;
    result.modes.saved = header.saved_modes;
    result.modes.default_ = header.default_modes;
    result.flags.shell_redraws_prompt = header.shell_redraw;
    result.flags.modify_other_keys_2 = header.modify_other_keys_2;
    result.flags.mouse_event = header.mouse_event;
    result.flags.mouse_format = header.mouse_format;
    result.flags.mouse_shift_capture =
        header.mouse_shift_capture.has
            ? (header.mouse_shift_capture.value ? Terminal::Flags::MouseShiftCapture::true_
                                                : Terminal::Flags::MouseShiftCapture::false_)
            : Terminal::Flags::MouseShiftCapture::null_;
    result.flags.password_input = header.password_input;
    result.mouse_shape = header.mouse_shape;

    /* Native strings retain trailing NUL sentinels. Setters construct those
     * without exposing that detail in the wire format. */
    if (!result.setPwd((const char *)payload.pwd, payload.pwd_len)) return Error::OutOfMemory;
    if (!result.setTitle((const char *)payload.title, payload.title_len)) return Error::OutOfMemory;

    /* Transfer decoded tab-stop ownership last. The deferred payload cleanup
     * releases the table originally allocated by Terminal.init. */
    const TerminalTabstops initialized_tabstops = result.tabstops;
    result.tabstops = payload.tabstops;
    payload.tabstops = initialized_tabstops;

    result.screens.active->pages.assertIntegrity();
    result_guard.armed = false;
    *out = result;
    return Error::none;
}

} /* namespace terminal_record */
} /* namespace snapshot */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_SNAPSHOT_TERMINAL_HPP */
