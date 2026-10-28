/* Transliterated from Ghostty src/terminal/snapshot/screen.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * SCREEN record payload encoding.
 *
 * One SCREEN record represents the live state for one terminal screen. The
 * record is followed immediately by the number of complete PAGE records
 * declared by `page_count`. They are the minimal suffix of native pages
 * needed to cover the active area and are ordered from oldest to newest. A
 * decoder uses the declared count, rather than another record tag, to find
 * the end of the screen's page sequence.
 *
 * The final screen-height rows are the active area. When its boundary falls
 * inside the first encoded page, that page also contains an incidental
 * history prefix. `history_rows` declares the complete logical history
 * extent so a client can size its scrollbar at READY without waiting for
 * HISTORY. The extent is advisory; native row totals remain derived from
 * decoded PAGE records.
 *
 * Screen dimensions are terminal-wide state and are not repeated here. Page
 * capacities, native page IDs and pointers, history availability,
 * selection, viewport, dirty state, and derived semantic-prompt state are
 * also omitted.
 *
 * All integers are unsigned and little-endian.
 *
 * Payload: Header (53 bytes), then a SavedCursor (23 bytes) when the header
 * says one is present, then the cursor hyperlink: a one-byte kind where zero
 * means none, kinds one and two being hyperlink.zig's encodings.
 *
 * Header: key u16, page count u16, history rows u64, cursor x u16, cursor
 * y u16, cursor visual style u8, cursor flags u8, cursor pen (style.zig,
 * 16), hyperlink implicit counter u32, charset state u16, protected mode
 * u8, Kitty keyboard index u8, eight Kitty keyboard flag bytes,
 * semantic-click kind u8, semantic-click value u8, saved-cursor-present u8.
 *
 * Cursor flags: bit 0 pending wrap, 1 protected, 2-3 semantic content,
 * 4 semantic-content-clear-EOL, 5-7 reserved zero.
 *
 * Charset state: G0..G3 charsets in bits 0-7 (two bits each), GL slot 8-9,
 * GR slot 10-11, single shift 12-14 (0 none, n = slot n-1), bit 15 zero.
 *
 * Saved cursor: x u16, y u16, pen (style), flags u8 (0 protected, 1 pending
 * wrap, 2 origin, 3-7 zero), charset state u16.
 *
 * Native enum values used below are stable terminal registries. Changing
 * those values or any packed bit assignment requires a snapshot version
 * bump.
 *
 * Wisp, differences in shape rather than behavior:
 *   - The full upstream format diagrams are abridged above; screen.zig is
 *     the authority.
 *   - Error sets flatten into one enum, `Error`, which carries PAGE's
 *     members so PAGE failures inside a SCREEN sequence keep their names.
 *   - `std.Io` is not threaded through; the port's Screen has no io field.
 *   - `?T` values are out-params with a `has` flag or `Maybe<T>`.
 *   - `hyperlink::DecodeError` has no ReadFailed member (the port's readers
 *     cannot produce one through it), so decodeCursorHyperlink treats every
 *     hyperlink decode failure as the discardable kind.
 */
#pragma once
#ifndef WISP_TERMINAL_SNAPSHOT_SCREEN_HPP
#define WISP_TERMINAL_SNAPSHOT_SCREEN_HPP

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

#include "../../vt/page.hpp"
#include "../../vt/page_list.hpp"
#include "../../vt/screen.hpp"
#include "../../vt/screen_set.hpp"
#include "../../zigstd/allocator.hpp"
#include "../../zigstd/reader.hpp"
#include "hyperlink.hpp"
#include "io.hpp"
#include "page.hpp"
#include "record.hpp"
#include "style.hpp"

namespace wisp {
namespace terminal {
namespace snapshot {
namespace screen {

typedef ::wisp::vt::Screen TerminalScreen;
typedef ::wisp::vt::PageList TerminalPageList;
typedef ::wisp::vt::ScreenSet::Key TerminalScreenKey;
typedef ::wisp::vt::hyperlink::Hyperlink TerminalHyperlink;
typedef ::wisp::vt::style::Style TerminalStyle;
typedef ::wisp::vt::page::Cell TerminalCell;
typedef ::wisp::zigstd::ReadError ReadError;
typedef ::wisp::zigstd::Allocator Allocator;
typedef TerminalScreen::CharsetState CharsetState;
typedef TerminalScreen::SemanticPrompt::SemanticClick SemanticClick;
typedef ::wisp::terminal::charsets::Slots Slots;
typedef ::wisp::terminal::charsets::Charset Charset;
typedef ::wisp::terminal::ansi::ProtectedMode ProtectedMode;
typedef ::wisp::terminal::osc::semantic_prompt::ClickEvents ClickEvents;
typedef ::wisp::terminal::osc::semantic_prompt::Click Click;

enum class Error : uint8_t {
    none = 0,

    /* std.Io.Reader.Error / std.Io.Writer.Error */
    EndOfStream,
    ReadFailed,
    WriteFailed,

    /* PayloadEncodeError */
    InvalidCursorFlags,
    InvalidCharsetState,
    InvalidKittyKeyboardIndex,
    InvalidKittyKeyboardFlags,
    InvalidSavedCursorFlags,
    InvalidUri,
    InvalidExplicitId,
    StringTooLong,

    /* The active area spans more pages than the SCREEN header can declare. */
    PageCountOverflow,

    /* PayloadDecodeError */
    InvalidKey,

    /* page.EncodeError / page.DecodeError */
    InvalidWideCell,
    TooManyGraphemes,
    PayloadTooLarge,
    InvalidDimensions,
    InvalidRowCellCount,
    InvalidKind,
    OutOfMemory,
    InvalidDestinationCapacity,
    InvalidTag,
    InvalidChecksum,
    PayloadNotExhausted,
    IntegrityViolation,

    /* TerminalPageList.Builder.FinishError */
    InvalidPageDimensions,
    NoPages,
    InsufficientRows,

    /* TerminalPageList.IncreaseCapacityError */
    OutOfSpace,

    /* The next record is valid but is not a SCREEN. */
    UnexpectedRecordTag,

    /* A SCREEN must declare at least one PAGE. */
    InvalidPageCount,
};

inline Error fromRead(ReadError e) {
    switch (e) {
        case ReadError::none: return Error::none;
        case ReadError::EndOfStream: return Error::EndOfStream;
        case ReadError::ReadFailed: return Error::ReadFailed;
    }
    return Error::EndOfStream;
}

inline Error fromPage(page::Error e) {
    switch (e) {
        case page::Error::none: return Error::none;
        case page::Error::EndOfStream: return Error::EndOfStream;
        case page::Error::ReadFailed: return Error::ReadFailed;
        case page::Error::WriteFailed: return Error::WriteFailed;
        case page::Error::InvalidUri: return Error::InvalidUri;
        case page::Error::InvalidExplicitId: return Error::InvalidExplicitId;
        case page::Error::StringTooLong: return Error::StringTooLong;
        case page::Error::InvalidWideCell: return Error::InvalidWideCell;
        case page::Error::TooManyGraphemes: return Error::TooManyGraphemes;
        case page::Error::PayloadTooLarge: return Error::PayloadTooLarge;
        case page::Error::InvalidDimensions: return Error::InvalidDimensions;
        case page::Error::InvalidRowCellCount: return Error::InvalidRowCellCount;
        case page::Error::InvalidKind: return Error::InvalidKind;
        case page::Error::OutOfMemory: return Error::OutOfMemory;
        case page::Error::InvalidDestinationCapacity: return Error::InvalidDestinationCapacity;
        case page::Error::InvalidTag: return Error::InvalidTag;
        case page::Error::InvalidChecksum: return Error::InvalidChecksum;
        case page::Error::PayloadNotExhausted: return Error::PayloadNotExhausted;
        case page::Error::IntegrityViolation: return Error::IntegrityViolation;
        case page::Error::UnexpectedRecordTag: return Error::UnexpectedRecordTag;
    }
    return Error::EndOfStream;
}

inline Error fromIncrease(TerminalPageList::IncreaseCapacityError e) {
    switch (e) {
        case TerminalPageList::IncreaseCapacityError::none: return Error::none;
        case TerminalPageList::IncreaseCapacityError::OutOfMemory: return Error::OutOfMemory;
        case TerminalPageList::IncreaseCapacityError::OutOfSpace: return Error::OutOfSpace;
    }
    return Error::OutOfMemory;
}

inline Error fromHyperlinkEncode(hyperlink::EncodeError e) {
    switch (e) {
        case hyperlink::EncodeError::none: return Error::none;
        case hyperlink::EncodeError::InvalidUri: return Error::InvalidUri;
        case hyperlink::EncodeError::InvalidExplicitId: return Error::InvalidExplicitId;
        case hyperlink::EncodeError::StringTooLong: return Error::StringTooLong;
    }
    return Error::none;
}

/* Flags encoded after the cursor's visual shape. */
struct CursorFlags {
    bool pending_wrap = false;
    bool protected_ = false;
    TerminalCell::SemanticContent semantic_content = TerminalCell::SemanticContent::output;
    bool semantic_content_clear_eol = false;
    uint8_t padding_ = 0; /* u3 */

    static const unsigned semantic_content_off = 2;

    /* Wisp: @bitCast(u8). */
    uint8_t bits() const {
        return (uint8_t)((pending_wrap ? 1 : 0) | (protected_ ? 2 : 0) |
                         (((uint8_t)semantic_content & 3) << semantic_content_off) |
                         (semantic_content_clear_eol ? 0x10 : 0) | ((padding_ & 7) << 5));
    }

    static CursorFlags init(const TerminalScreen::Cursor *cursor) {
        CursorFlags f;
        f.pending_wrap = cursor->pending_wrap;
        f.protected_ = cursor->protected_;
        f.semantic_content = cursor->semantic_content;
        f.semantic_content_clear_eol = cursor->semantic_content_clear_eol;
        return f;
    }

    /* Decode the cursor flag registry, normalizing unknown semantic state. */
    template <typename ReaderT>
    static bool decode(ReaderT *reader, CursorFlags *out, ReadError *err) {
        uint8_t raw = 0;
        if (!reader->takeByte(&raw, err)) return false;
        const uint8_t semantic_content_raw = (uint8_t)((raw >> semantic_content_off) & 3);
        CursorFlags f;
        f.pending_wrap = (raw & 1) != 0;
        f.protected_ = (raw & 2) != 0;
        f.semantic_content = semantic_content_raw > (uint8_t)TerminalCell::SemanticContent::prompt
                                 ? TerminalCell::SemanticContent::output
                                 : (TerminalCell::SemanticContent)semantic_content_raw;
        f.semantic_content_clear_eol = (raw & 0x10) != 0;
        *out = f;
        return true;
    }
};

/* The packed wire bits shared by current and saved cursor charset state.
 *
 * Native enum values are stable, but their backing type is `c_int` in C ABI
 * builds. The wire therefore stores only checked integer values at its
 * fixed bit widths. */
struct CharsetBits {
    uint8_t g0 = 0, g1 = 0, g2 = 0, g3 = 0, gl = 0, gr = 2, single_shift = 0;

    uint16_t bits() const {
        return (uint16_t)((g0 & 3) | ((g1 & 3) << 2) | ((g2 & 3) << 4) | ((g3 & 3) << 6) |
                          ((gl & 3) << 8) | ((gr & 3) << 10) | ((single_shift & 7) << 12));
    }

    static CharsetBits fromBits(uint16_t v) {
        CharsetBits b;
        b.g0 = (uint8_t)(v & 3);
        b.g1 = (uint8_t)((v >> 2) & 3);
        b.g2 = (uint8_t)((v >> 4) & 3);
        b.g3 = (uint8_t)((v >> 6) & 3);
        b.gl = (uint8_t)((v >> 8) & 3);
        b.gr = (uint8_t)((v >> 10) & 3);
        b.single_shift = (uint8_t)((v >> 12) & 7);
        return b;
    }
};

/* Wisp: enumFromInt(Charset/Slots, raw). Both enums fill their two bits. */
inline bool charsetFromInt(unsigned raw, Charset *out) {
    if (raw > (unsigned)Charset::dec_special) return false;
    *out = (Charset)raw;
    return true;
}
inline bool slotFromInt(unsigned raw, Slots *out) {
    if (raw > (unsigned)Slots::G3) return false;
    *out = (Slots)raw;
    return true;
}

inline Error encodeCharsetState(const CharsetState &value, uint16_t *out) {
    const unsigned g0 = (unsigned)value.charsets.get(Slots::G0);
    const unsigned g1 = (unsigned)value.charsets.get(Slots::G1);
    const unsigned g2 = (unsigned)value.charsets.get(Slots::G2);
    const unsigned g3 = (unsigned)value.charsets.get(Slots::G3);
    const unsigned gl = (unsigned)value.gl;
    const unsigned gr = (unsigned)value.gr;
    if (g0 > 3 || g1 > 3 || g2 > 3 || g3 > 3 || gl > 3 || gr > 3) return Error::InvalidCharsetState;
    unsigned single_shift = 0;
    if (value.single_shift.has) {
        const unsigned raw = (unsigned)value.single_shift.value;
        if (raw > 3) return Error::InvalidCharsetState;
        single_shift = raw + 1;
    }

    CharsetBits bits;
    bits.g0 = (uint8_t)g0;
    bits.g1 = (uint8_t)g1;
    bits.g2 = (uint8_t)g2;
    bits.g3 = (uint8_t)g3;
    bits.gl = (uint8_t)gl;
    bits.gr = (uint8_t)gr;
    bits.single_shift = (uint8_t)single_shift;
    *out = bits.bits();
    return Error::none;
}

inline CharsetState decodeCharsetState(uint16_t raw) {
    const CharsetBits bits = CharsetBits::fromBits(raw);

    CharsetState result;
    Slots slot;
    if (slotFromInt(bits.gl, &slot)) result.gl = slot;
    if (slotFromInt(bits.gr, &slot)) result.gr = slot;
    if (bits.single_shift == 0) {
        result.single_shift = ::wisp::vt::Maybe<Slots>();
    } else if (bits.single_shift <= 4 && slotFromInt(bits.single_shift - 1u, &slot)) {
        result.single_shift = ::wisp::vt::Maybe<Slots>(slot);
    } else {
        result.single_shift = ::wisp::vt::Maybe<Slots>();
    }
    Charset cs;
    result.charsets.set(Slots::G0, charsetFromInt(bits.g0, &cs) ? cs : Charset::utf8);
    result.charsets.set(Slots::G1, charsetFromInt(bits.g1, &cs) ? cs : Charset::utf8);
    result.charsets.set(Slots::G2, charsetFromInt(bits.g2, &cs) ? cs : Charset::utf8);
    result.charsets.set(Slots::G3, charsetFromInt(bits.g3, &cs) ? cs : Charset::utf8);
    return result;
}

/* The complete fixed-size Kitty keyboard stack. */
struct KittyKeyboard {
    /* One byte in the fixed Kitty keyboard flag stack. */
    struct Flags {
        bool disambiguate = false;
        bool report_events = false;
        bool report_alternates = false;
        bool report_all = false;
        bool report_associated = false;
        uint8_t padding_ = 0; /* u3 */

        uint8_t bits() const {
            return (uint8_t)((disambiguate ? 1 : 0) | (report_events ? 2 : 0) |
                             (report_alternates ? 4 : 0) | (report_all ? 8 : 0) |
                             (report_associated ? 16 : 0) | ((padding_ & 7) << 5));
        }

        static Flags fromBits(uint8_t v) {
            Flags f;
            f.disambiguate = (v & 1) != 0;
            f.report_events = (v & 2) != 0;
            f.report_alternates = (v & 4) != 0;
            f.report_all = (v & 8) != 0;
            f.report_associated = (v & 16) != 0;
            f.padding_ = (uint8_t)((v >> 5) & 7);
            return f;
        }

        /* Decode one Kitty keyboard flag byte, ignoring reserved bits. */
        template <typename ReaderT>
        static bool decode(ReaderT *reader, Flags *out, ReadError *err) {
            uint8_t raw = 0;
            if (!reader->takeByte(&raw, err)) return false;
            Flags result = fromBits(raw);
            result.padding_ = 0;
            *out = result;
            return true;
        }
    };

    uint8_t index = 0;
    Flags flags[8];

    static KittyKeyboard init(const ::wisp::terminal::kitty::KeyFlagStack &value) {
        KittyKeyboard result;
        result.index = value.idx;
        for (size_t i = 0; i < 8; i++) {
            result.flags[i].disambiguate = value.flags[i].disambiguate;
            result.flags[i].report_events = value.flags[i].report_events;
            result.flags[i].report_alternates = value.flags[i].report_alternates;
            result.flags[i].report_all = value.flags[i].report_all;
            result.flags[i].report_associated = value.flags[i].report_associated;
        }
        return result;
    }

    ::wisp::terminal::kitty::KeyFlagStack terminal() const {
        ::wisp::terminal::kitty::KeyFlagStack result;
        result.idx = (uint8_t)index;
        for (size_t i = 0; i < 8; i++) {
            result.flags[i].disambiguate = flags[i].disambiguate;
            result.flags[i].report_events = flags[i].report_events;
            result.flags[i].report_alternates = flags[i].report_alternates;
            result.flags[i].report_all = flags[i].report_all;
            result.flags[i].report_associated = flags[i].report_associated;
        }
        return result;
    }

    /* Decode the complete fixed Kitty keyboard stack. */
    template <typename ReaderT>
    static bool decode(ReaderT *reader, KittyKeyboard *out, ReadError *err) {
        uint8_t index_raw = 0;
        if (!reader->takeByte(&index_raw, err)) return false;
        KittyKeyboard result;
        result.index = index_raw < 8 ? index_raw : 0;
        for (size_t i = 0; i < 8; i++) {
            if (!Flags::decode(reader, &result.flags[i], err)) return false;
        }
        *out = result;
        return true;
    }
};

inline SemanticClick semanticClickNone() {
    return TerminalScreen::SemanticPrompt::disabled().click;
}

inline bool semanticClickEql(const SemanticClick &a, const SemanticClick &b) {
    if (a.tag != b.tag) return false;
    switch (a.tag) {
        case SemanticClick::Kind::none: return true;
        case SemanticClick::Kind::click_events: return a.click_events == b.click_events;
        case SemanticClick::Kind::cl: return a.cl == b.cl;
    }
    return false;
}

/* Decode semantic-click state, falling back to disabled for unknown values. */
template <typename ReaderT>
inline bool decodeSemanticClick(ReaderT *reader, SemanticClick *out, ReadError *err) {
    uint8_t kind = 0, value = 0;
    if (!reader->takeByte(&kind, err)) return false;
    if (!reader->takeByte(&value, err)) return false;

    *out = semanticClickNone();
    switch (kind) {
        case (uint8_t)SemanticClick::Kind::click_events:
            if (value <= (uint8_t)ClickEvents::relative) {
                out->tag = SemanticClick::Kind::click_events;
                out->click_events = (ClickEvents)value;
            }
            break;
        case (uint8_t)SemanticClick::Kind::cl:
            if (value <= (uint8_t)Click::smart_vertical) {
                out->tag = SemanticClick::Kind::cl;
                out->cl = (Click)value;
            }
            break;
        default:
            break;
    }
    return true;
}

/* The optional fixed-size saved cursor state. */
struct SavedCursor {
    /* Number of bytes written by `encode`. */
    static const size_t len = 23;

    /* Flags encoded with an optional saved cursor. */
    struct Flags {
        bool protected_ = false;
        bool pending_wrap = false;
        bool origin = false;
        uint8_t padding_ = 0; /* u5 */

        uint8_t bits() const {
            return (uint8_t)((protected_ ? 1 : 0) | (pending_wrap ? 2 : 0) | (origin ? 4 : 0) |
                             ((padding_ & 0x1F) << 3));
        }

        /* Decode saved cursor flags, ignoring reserved bits. */
        template <typename ReaderT>
        static bool decode(ReaderT *reader, Flags *out, ReadError *err) {
            uint8_t raw = 0;
            if (!reader->takeByte(&raw, err)) return false;
            Flags f;
            f.protected_ = (raw & 1) != 0;
            f.pending_wrap = (raw & 2) != 0;
            f.origin = (raw & 4) != 0;
            *out = f;
            return true;
        }
    };

    uint16_t x = 0;
    uint16_t y = 0;
    TerminalStyle pen;
    Flags flags;
    CharsetState charset;

    static SavedCursor init(const TerminalScreen::SavedCursor &value) {
        SavedCursor s;
        s.x = value.x;
        s.y = value.y;
        s.pen = value.style;
        s.flags.protected_ = value.protected_;
        s.flags.pending_wrap = value.pending_wrap;
        s.flags.origin = value.origin;
        s.charset = value.charset;
        return s;
    }

    TerminalScreen::SavedCursor terminal(uint16_t cols, uint16_t rows) const {
        const uint16_t cx = x < cols - 1 ? x : (uint16_t)(cols - 1);
        TerminalScreen::SavedCursor out;
        out.x = cx;
        out.y = y < rows - 1 ? y : (uint16_t)(rows - 1);
        out.style = pen;
        out.protected_ = flags.protected_;
        out.pending_wrap = flags.pending_wrap && cx == cols - 1;
        out.origin = flags.origin;
        out.charset = charset;
        return out;
    }

    /* Encode one optional saved cursor value. */
    Error encode(std::string *writer) const {
        if (flags.padding_ != 0) return Error::InvalidSavedCursorFlags;
        uint16_t cs = 0;
        const Error err = encodeCharsetState(charset, &cs);
        if (err != Error::none) return err;

        io::writeInt<uint16_t>(writer, x);
        io::writeInt<uint16_t>(writer, y);
        style::encode(pen, writer);
        writer->push_back((char)flags.bits());
        io::writeInt<uint16_t>(writer, cs);
        return Error::none;
    }

    /* Decode and validate one optional saved cursor value. */
    template <typename ReaderT>
    static Error decode(ReaderT *reader, SavedCursor *out) {
        SavedCursor s;
        ReadError rerr = ReadError::none;
        if (!io::readInt<uint16_t>(reader, &s.x, &rerr)) return fromRead(rerr);
        if (!io::readInt<uint16_t>(reader, &s.y, &rerr)) return fromRead(rerr);
        bool has_pen = false;
        if (!style::decodeOrNull(reader, &s.pen, &has_pen, &rerr)) return fromRead(rerr);
        if (!has_pen) s.pen = TerminalStyle();
        if (!Flags::decode(reader, &s.flags, &rerr)) return fromRead(rerr);
        uint16_t cs = 0;
        if (!io::readInt<uint16_t>(reader, &cs, &rerr)) return fromRead(rerr);
        s.charset = decodeCharsetState(cs);
        *out = s;
        return Error::none;
    }
};

/* The fixed fields at the start of every SCREEN payload. */
struct Header {
    /* Number of bytes written by `encode`. */
    static const size_t len = 53;

    TerminalScreenKey key = TerminalScreenKey::primary;
    /* Complete pages in the minimal suffix covering the active area. */
    uint16_t page_count = 0;
    /* Advisory logical rows before the active area, including resident
     * SCREEN overlap. */
    uint64_t history_rows = 0;
    uint16_t cursor_x = 0;
    uint16_t cursor_y = 0;
    TerminalScreen::CursorStyle cursor_style = TerminalScreen::CursorStyle::block;
    CursorFlags cursor_flags;
    TerminalStyle cursor_pen;
    uint32_t hyperlink_implicit_id = 0;
    CharsetState charset;
    ProtectedMode protected_mode = ProtectedMode::off;
    KittyKeyboard kitty_keyboard;
    SemanticClick semantic_click = semanticClickNone();
    bool saved_cursor_present = false;

    /* Initialize the fixed header from one native screen. */
    static Header init(const TerminalScreen *screen, TerminalScreenKey key, uint16_t page_count) {
        Header h;
        h.key = key;
        h.page_count = page_count;
        h.history_rows = (uint64_t)(screen->pages.total_rows - screen->pages.rows);
        h.cursor_x = screen->cursor.x;
        h.cursor_y = screen->cursor.y;
        h.cursor_style = screen->cursor.cursor_style;
        h.cursor_flags = CursorFlags::init(&screen->cursor);
        h.cursor_pen = screen->cursor.style;
        h.hyperlink_implicit_id = screen->cursor.hyperlink_implicit_id;
        h.charset = screen->charset;
        h.protected_mode = screen->protected_mode;
        h.kitty_keyboard = KittyKeyboard::init(screen->kitty_keyboard);
        h.semantic_click = screen->semantic_prompt.click;
        h.saved_cursor_present = screen->saved_cursor.has;
        return h;
    }

    /* Encode the fixed SCREEN payload header field by field. */
    Error encode(std::string *writer) const {
        /* Validate packed cursor state before writing any bytes. */
        if (cursor_flags.padding_ != 0) return Error::InvalidCursorFlags;

        /* Validate and narrow native charset enum values before writing. */
        uint16_t cs = 0;
        const Error cerr = encodeCharsetState(charset, &cs);
        if (cerr != Error::none) return cerr;

        /* Validate the complete Kitty keyboard stack. */
        if (kitty_keyboard.index >= 8) return Error::InvalidKittyKeyboardIndex;
        for (size_t i = 0; i < 8; i++) {
            if (kitty_keyboard.flags[i].padding_ != 0) return Error::InvalidKittyKeyboardFlags;
        }

        /* Screen identity, expected history extent, and cursor position. */
        io::writeInt<uint16_t>(writer, (uint16_t)key);
        io::writeInt<uint16_t>(writer, page_count);
        io::writeInt<uint64_t>(writer, history_rows);
        io::writeInt<uint16_t>(writer, cursor_x);
        io::writeInt<uint16_t>(writer, cursor_y);

        /* Current cursor rendering state. */
        writer->push_back((char)(uint8_t)cursor_style);
        writer->push_back((char)cursor_flags.bits());
        style::encode(cursor_pen, writer);
        io::writeInt<uint32_t>(writer, hyperlink_implicit_id);

        /* Charset and selective-erase state. */
        io::writeInt<uint16_t>(writer, cs);
        writer->push_back((char)(uint8_t)protected_mode);

        /* Keyboard modes and semantic-click state. */
        writer->push_back((char)kitty_keyboard.index);
        for (size_t i = 0; i < 8; i++) writer->push_back((char)kitty_keyboard.flags[i].bits());
        writer->push_back((char)(uint8_t)semantic_click.tag);
        switch (semantic_click.tag) {
            case SemanticClick::Kind::none: writer->push_back(0); break;
            case SemanticClick::Kind::click_events:
                writer->push_back((char)(uint8_t)semantic_click.click_events);
                break;
            case SemanticClick::Kind::cl: writer->push_back((char)(uint8_t)semantic_click.cl); break;
        }

        /* Presence of the optional saved-cursor suffix. */
        writer->push_back(saved_cursor_present ? 1 : 0);
        return Error::none;
    }

    /* Decode the fixed SCREEN payload header.
     *
     * The screen key remains structural because it routes the following PAGE
     * sequence. Unknown semantic fields use native defaults instead. */
    template <typename ReaderT>
    static Error decode(ReaderT *reader, Header *out) {
        Header h;
        ReadError rerr = ReadError::none;

        /* Screen identity, expected history extent, and cursor position. */
        uint16_t key_raw = 0;
        if (!io::readInt<uint16_t>(reader, &key_raw, &rerr)) return fromRead(rerr);
        if (key_raw > (uint16_t)TerminalScreenKey::alternate) return Error::InvalidKey;
        h.key = (TerminalScreenKey)key_raw;
        if (!io::readInt<uint16_t>(reader, &h.page_count, &rerr)) return fromRead(rerr);
        if (!io::readInt<uint64_t>(reader, &h.history_rows, &rerr)) return fromRead(rerr);
        if (!io::readInt<uint16_t>(reader, &h.cursor_x, &rerr)) return fromRead(rerr);
        if (!io::readInt<uint16_t>(reader, &h.cursor_y, &rerr)) return fromRead(rerr);

        /* Current cursor rendering state. */
        uint8_t style_raw = 0;
        if (!reader->takeByte(&style_raw, &rerr)) return fromRead(rerr);
        h.cursor_style = style_raw > (uint8_t)TerminalScreen::CursorStyle::block_hollow
                             ? TerminalScreen::CursorStyle::block
                             : (TerminalScreen::CursorStyle)style_raw;
        if (!CursorFlags::decode(reader, &h.cursor_flags, &rerr)) return fromRead(rerr);
        bool has_pen = false;
        if (!style::decodeOrNull(reader, &h.cursor_pen, &has_pen, &rerr)) return fromRead(rerr);
        if (!has_pen) h.cursor_pen = TerminalStyle();
        if (!io::readInt<uint32_t>(reader, &h.hyperlink_implicit_id, &rerr)) return fromRead(rerr);

        /* Charset and selective-erase state. */
        uint16_t cs = 0;
        if (!io::readInt<uint16_t>(reader, &cs, &rerr)) return fromRead(rerr);
        h.charset = decodeCharsetState(cs);
        uint8_t pm = 0;
        if (!reader->takeByte(&pm, &rerr)) return fromRead(rerr);
        h.protected_mode = pm > (uint8_t)ProtectedMode::dec ? ProtectedMode::off : (ProtectedMode)pm;

        /* Keyboard modes and semantic-click state. */
        if (!KittyKeyboard::decode(reader, &h.kitty_keyboard, &rerr)) return fromRead(rerr);
        if (!decodeSemanticClick(reader, &h.semantic_click, &rerr)) return fromRead(rerr);

        /* Presence of the optional saved-cursor suffix. */
        uint8_t present = 0;
        if (!reader->takeByte(&present, &rerr)) return fromRead(rerr);
        h.saved_cursor_present = present != 0;

        *out = h;
        return Error::none;
    }
};

/* Encode the optional cursor hyperlink at the end of a SCREEN payload. */
inline Error encodeCursorHyperlink(const TerminalHyperlink *value, std::string *writer) {
    if (value != nullptr) return fromHyperlinkEncode(hyperlink::encode(*value, writer));
    writer->push_back(0);
    return Error::none;
}

/* Decode one allocator-owned optional cursor hyperlink.
 *
 * The caller owns a non-null returned value and must call
 * `Hyperlink.deinit`. Wisp: `*has` is the non-null case. */
template <typename ReaderT>
inline Error decodeCursorHyperlink(ReaderT *reader, Allocator alloc, TerminalHyperlink *out, bool *has) {
    *has = false;
    ReadError rerr = ReadError::none;
    uint8_t first = 0;
    if (!reader->peekByte(&first, &rerr)) return fromRead(rerr);
    if (first == 0) {
        reader->takeByte(&first, &rerr);
        return Error::none;
    }

    hyperlink::DecodeError herr = hyperlink::DecodeError::none;
    if (hyperlink::decode(reader, alloc, out, &herr)) {
        *has = true;
        return Error::none;
    }

    /* The cursor hyperlink is the final SCREEN payload field, so its record
     * boundary lets us discard an invalid or unrepresentable value without
     * losing the following PAGE sequence. */
    (void)reader->discardRemaining();
    return Error::none;
}

/* Encode the SCREEN payload, excluding its following PAGE records. */
inline Error encodePayload(const TerminalScreen *screen, TerminalScreenKey key, uint16_t page_count,
                           std::string *writer) {
    Error err = Header::init(screen, key, page_count).encode(writer);
    if (err != Error::none) return err;

    if (screen->saved_cursor.has) {
        err = SavedCursor::init(screen->saved_cursor.value).encode(writer);
        if (err != Error::none) return err;
    }

    return encodeCursorHyperlink(screen->cursor.hyperlink, writer);
}

/* Encode one SCREEN and its minimal suffix of complete native pages.
 *
 * The suffix begins with the page containing the active area's first row
 * and ends with the newest page. Completed records may already be emitted
 * if a later record fails; the missing READY marker makes that prefix
 * invalid. */
inline Error encode(const TerminalScreen *screen, TerminalScreenKey key, record::Writer *destination) {
    /* The active top may fall inside this page, leaving an incidental
     * history prefix. Every earlier complete page is history and is
     * omitted. */
    TerminalPageList::Node *first = screen->pages.getTopLeft(::wisp::vt::point::Tag::active).node;
    size_t page_count = 0;
    for (TerminalPageList::Node *node = first; node != nullptr; node = node->next) page_count += 1;
    if (page_count > 0xFFFF) return Error::PageCountOverflow;

    /* SCREEN declares exactly how many immediately following PAGE records
     * belong to it. */
    {
        std::string *payload = destination->begin(record::Tag::screen);
        const Error err = encodePayload(screen, key, (uint16_t)page_count, payload);
        if (err != Error::none) {
            destination->cancel(); /* errdefer */
            return err;
        }
        switch (destination->finish()) {
            case record::Writer::FinishError::none: break;
            case record::Writer::FinishError::WriteFailed: return Error::WriteFailed;
            case record::Writer::FinishError::PayloadTooLarge: return Error::PayloadTooLarge;
        }
    }

    /* Active pages are resident today, but use the representation-safe
     * access path so a future PageList compression-policy change cannot turn
     * this wire encoder's optimization invariant into undefined behavior. */
    for (TerminalPageList::Node *node = first; node != nullptr; node = node->next) {
        TerminalPageList::Node::PreservedPage preserved;
        if (!node->pagePreservingState(screen->alloc, &preserved)) return Error::OutOfMemory;
        const Error err = fromPage(page::encode(preserved.page(), destination));
        preserved.deinit();
        if (err != Error::none) return err;
    }
    return Error::none;
}

/* One decoded SCREEN sequence and the native screen identified by its
 * header. */
struct Decoded {
    TerminalScreenKey key = TerminalScreenKey::primary;
    /* Expected history extent at READY, including resident SCREEN overlap. */
    uint64_t history_rows = 0;
    TerminalScreen screen;

    /* Release the decoded native screen before ownership is transferred. */
    void deinit() { screen.deinit(); }
};

inline Error fromBuilderFinish(TerminalPageList::Builder::FinishError e) {
    switch (e) {
        case TerminalPageList::Builder::FinishError::none: return Error::none;
        case TerminalPageList::Builder::FinishError::OutOfMemory: return Error::OutOfMemory;
        case TerminalPageList::Builder::FinishError::InvalidDimensions: return Error::InvalidDimensions;
        case TerminalPageList::Builder::FinishError::InvalidPageDimensions: return Error::InvalidPageDimensions;
        case TerminalPageList::Builder::FinishError::NoPages: return Error::NoPages;
        case TerminalPageList::Builder::FinishError::InsufficientRows: return Error::InsufficientRows;
    }
    return Error::OutOfMemory;
}

/* Restore one SCREEN and its declared PAGE records into native terminal
 * state.
 *
 * The caller supplies terminal-wide dimensions and policy. The record
 * sequence is decoded transactionally: on failure, all page, pin, hyperlink,
 * and temporary allocations are released and no partially restored Screen
 * escapes. The returned key selects the ScreenSet slot that owns the
 * decoded screen. */
template <typename SourceT>
inline Error decode(SourceT *source, Allocator alloc, const TerminalScreen::Options &options, Decoded *out) {
    /* Decode and finish the self-contained SCREEN record before consuming any
     * of the PAGE records which follow it. */
    record::Reader record_reader;
    record::Reader::InitError ierr = record::Reader::InitError::none;
    if (!record_reader.init(source, &ierr)) {
        return ierr == record::Reader::InitError::InvalidTag ? Error::InvalidTag : Error::EndOfStream;
    }
    if (record_reader.header.tag != record::Tag::screen) return Error::UnexpectedRecordTag;

    /* The optional saved cursor and cursor hyperlink are both part of the
     * SCREEN payload. Keep ownership of the decoded hyperlink locally until
     * it has been copied into the native Screen. */
    record::PayloadReader *payload_reader = record_reader.payloadReader();
    Header header;
    Error err = Header::decode(payload_reader, &header);
    if (err != Error::none) return err;
    SavedCursor saved_cursor;
    if (header.saved_cursor_present) {
        err = SavedCursor::decode(payload_reader, &saved_cursor);
        if (err != Error::none) return err;
    }
    TerminalHyperlink cursor_hyperlink;
    bool has_cursor_hyperlink = false;
    err = decodeCursorHyperlink(payload_reader, alloc, &cursor_hyperlink, &has_cursor_hyperlink);
    if (err != Error::none) return err;
    struct HyperlinkGuard {
        TerminalHyperlink *h;
        bool *has;
        Allocator a;
        ~HyperlinkGuard() {
            if (*has) h->deinit(a);
        }
    } hyperlink_guard = { &cursor_hyperlink, &has_cursor_hyperlink, alloc };
    switch (record_reader.finish()) {
        case record::Reader::FinishError::none: break;
        case record::Reader::FinishError::InvalidChecksum: return Error::InvalidChecksum;
        case record::Reader::FinishError::PayloadNotExhausted: return Error::PayloadNotExhausted;
    }

    const TerminalScreenKey key = header.key;
    if (header.page_count == 0) return Error::InvalidPageCount;

    /* Dimensions are terminal-wide state supplied by the enclosing snapshot.
     * Validate them, and all cursor invariants which depend on them, before
     * allocating the declared pages. */
    if (options.cols == 0 || options.rows == 0) return Error::InvalidDimensions;

    /* Build the native page list transactionally. Alternate screens never
     * retain scrollback, while primary screens inherit the caller's limits. */
    TerminalPageList pages;
    {
        const bool alternate = key == TerminalScreenKey::alternate;
        TerminalPageList::Builder builder;
        if (!TerminalPageList::Builder::init(
                alloc,
                TerminalPageList::Options(options.cols, options.rows,
                                          alternate ? ::wisp::vt::Maybe<size_t>((size_t)0)
                                                    : options.max_scrollback_bytes,
                                          alternate ? ::wisp::vt::Maybe<size_t>((size_t)0)
                                                    : options.max_scrollback_lines),
                &builder)) {
            return Error::OutOfMemory;
        }
        struct BuilderGuard {
            TerminalPageList::Builder *b;
            ~BuilderGuard() { b->deinit(); }
        } builder_guard = { &builder };

        /* Allocate each PAGE at its declared capacity and decode directly
         * into builder-owned storage, avoiding an intermediate page
         * representation. */
        for (size_t i = 0; i < header.page_count; i++) {
            page::Decoder decoder;
            err = fromPage(decoder.init(source));
            if (err != Error::none) return err;
            ::wisp::vt::page::Page *native_page = builder.allocatePage(decoder.capacity());
            if (native_page == nullptr) return Error::OutOfMemory;
            err = fromPage(decoder.decode(native_page, alloc));
            if (err != Error::none) return err;
        }

        /* Finishing validates that the decoded suffix covers the active area
         * and establishes the PageList's active viewport. */
        err = fromBuilderFinish(builder.finish(&pages));
        if (err != Error::none) return err;
    }

    /* Convert the payload-relative cursor position into the tracked native
     * pin and page-local row/cell pointers required by TerminalScreen. */
    TerminalScreen::Cursor cursor;
    {
        const uint16_t y = header.cursor_y < options.rows - 1 ? header.cursor_y : (uint16_t)(options.rows - 1);

        /* The active area can temporarily contain mixed-width pages after a
         * reflow. Locate the row at column zero, then clamp the encoded x
         * coordinate to the width of that physical page. */
        const ::wisp::vt::Maybe<TerminalPageList::Pin> row_pin_m =
            pages.pin(::wisp::vt::point::Point::active(0, y));
        assert(row_pin_m.has); /* orelse unreachable */
        const TerminalPageList::Pin row_pin = row_pin_m.value;
        const uint16_t node_cols = row_pin.node->cols();
        const uint16_t x = header.cursor_x < node_cols - 1 ? header.cursor_x : (uint16_t)(node_cols - 1);
        TerminalPageList::Pin target = row_pin;
        target.x = x;
        TerminalPageList::Pin *pin = pages.trackPin(target);
        if (pin == nullptr) {
            pages.deinit(); /* errdefer pages.deinit() */
            return Error::OutOfMemory;
        }
        const ::wisp::vt::page::Page::RowAndCell rac = pin->rowAndCell();

        cursor.x = x;
        cursor.y = y;
        cursor.cursor_style = header.cursor_style;
        cursor.pending_wrap = header.cursor_flags.pending_wrap && x == node_cols - 1;
        cursor.protected_ = header.cursor_flags.protected_;
        cursor.style = header.cursor_pen;
        cursor.hyperlink_implicit_id = header.hyperlink_implicit_id;
        cursor.semantic_content = header.cursor_flags.semantic_content;
        cursor.semantic_content_clear_eol = header.cursor_flags.semantic_content_clear_eol;
        cursor.page_pin = pin;
        cursor.page_row = rac.row;
        cursor.page_cell = rac.cell;
    }

    /* Assemble the remaining native state from stable snapshot registries.
     * Derived state and page-local table references are repaired below.
     *
     * Wisp: assembled in place in `out->screen`, as Screen::init does,
     * because later steps (kitty_images.setLimit) may retain the Screen's
     * address. */
    TerminalScreen &result = out->screen;
    result = TerminalScreen();
    result.alloc = alloc;
    result.pages = pages;
    result.no_scrollback = key == TerminalScreenKey::alternate ||
                           (options.max_scrollback_bytes.has && options.max_scrollback_bytes.value == 0);
    result.cursor = cursor;
    if (header.saved_cursor_present) {
        result.saved_cursor = ::wisp::vt::Maybe<TerminalScreen::SavedCursor>(
            saved_cursor.terminal(options.cols, options.rows));
    }
    result.charset = header.charset;
    result.protected_mode = header.protected_mode;
    result.kitty_keyboard = header.kitty_keyboard.terminal();
    result.semantic_prompt.seen = false;
    result.semantic_prompt.click = header.semantic_click;

    struct ResultGuard {
        TerminalScreen *s;
        bool armed;
        ~ResultGuard() {
            if (armed) s->deinit();
        }
    } result_guard = { &result, true };

    /* Reinsert the concrete cursor style into its page-local style table.
     * This existing Screen path grows or splits the page when the decoded
     * table is already full. */
    err = fromIncrease(result.manualStyleUpdate());
    if (err != Error::none) return err;

    /* Reinsert the cursor hyperlink into its page-local hyperlink table. For
     * an implicit link, temporarily seed the counter with the encoded link
     * ID, then restore the independent next-ID counter from the header. */
    if (has_cursor_hyperlink) {
        switch (cursor_hyperlink.id.tag) {
            case TerminalHyperlink::Id::Tag::explicit_:
                err = fromIncrease(result.startHyperlink(cursor_hyperlink.uri, cursor_hyperlink.uri_len,
                                                         cursor_hyperlink.id.explicit_ptr,
                                                         cursor_hyperlink.id.explicit_len));
                if (err != Error::none) return err;
                break;
            case TerminalHyperlink::Id::Tag::implicit:
                result.cursor.hyperlink_implicit_id = cursor_hyperlink.id.implicit;
                err = fromIncrease(
                    result.startHyperlink(cursor_hyperlink.uri, cursor_hyperlink.uri_len, nullptr, 0));
                if (err != Error::none) return err;
                result.cursor.hyperlink_implicit_id = header.hyperlink_implicit_id;
                break;
        }
    }

    /* `seen` is derived state. Only resident prompt metadata participates
     * because omitted history is outside the restored Screen model. */
    result.semantic_prompt.seen = [&]() -> bool {
        if (result.cursor.semantic_content == TerminalCell::SemanticContent::prompt) return true;

        for (TerminalPageList::Node *node = result.pages.getTopLeft(::wisp::vt::point::Tag::screen).node;
             node != nullptr; node = node->next) {
            const ::wisp::vt::page::Page *native_page = node->pageAssumeResident();
            for (size_t y = 0; y < native_page->size.rows; y++) {
                const ::wisp::vt::page::Row *row = native_page->getRow(y);
                if (row->semantic_prompt() != ::wisp::vt::page::Row::SemanticPrompt::none) return true;

                const TerminalCell *cells = native_page->getCells(row);
                for (size_t x = 0; x < native_page->size.cols; x++) {
                    if (cells[x].semantic_content() == TerminalCell::SemanticContent::prompt) return true;
                }
            }
        }
        return false;
    }();

    /* Kitty image payloads are outside this record, but the restored Screen
     * must still receive the caller's terminal-wide storage policy. */
    result.kitty_images.setLimit(alloc, &result, options.kitty_image_storage_limit);
    result.kitty_images.image_limits = options.kitty_image_loading_limits;

    /* All fallible reconstruction is complete; verify the native invariants
     * before transferring ownership to the caller. */
    result.pages.assertIntegrity();
    result.assertIntegrity();
    result_guard.armed = false;
    out->key = key;
    out->history_rows = header.history_rows;
    return Error::none;
}

} /* namespace screen */
} /* namespace snapshot */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_SNAPSHOT_SCREEN_HPP */
