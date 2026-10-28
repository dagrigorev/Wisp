/* Transliterated from Ghostty src/terminal/snapshot/style.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Snapshot style entry encoding.
 *
 * Each entry describes one terminal style. A record can use these entries to
 * build a style table and assign indexes according to that record's format.
 * Indexing, ordering, etc. are properties of the containing record.
 *
 * Styles are encoded field by field from the terminal's native `Style` type.
 * The native packed representation is deliberately not part of the snapshot
 * format. This gives flexibility for changing one side or the other.
 *
 * All integers are unsigned and little-endian.
 *
 * | Offset | Size | Field                     |
 * | -----: | ---: | :------------------------ |
 * |      0 |    4 | Foreground color          |
 * |      4 |    4 | Background color          |
 * |      8 |    4 | Underline color           |
 * |     12 |    2 | Style flags (`u16`)       |
 * |     14 |    2 | Reserved, must be zero    |
 *
 * The trailing reserved field is explicit wire padding that rounds each
 * style entry from 14 bytes to 16 bytes. This makes entry offsets and byte
 * counts straightforward.
 *
 * Each color begins with a one-byte kind followed by three data bytes:
 *
 * | Kind | Meaning | Data bytes                         |
 * | ---: | :------ | :--------------------------------- |
 * |    0 | None    | All zero                           |
 * |    1 | Palette | Palette index, then two zero bytes |
 * |    2 | RGB     | Red, green, blue                   |
 *
 * Style flag bits 0 through 7 are bold, italic, faint, blink, inverse,
 * invisible, strikethrough, and overline. Bits 8 through 10 contain the
 * underline kind. Bits 11 through 15 must be zero.
 *
 * | Underline | Meaning |
 * | --------: | :------ |
 * |         0 | None    |
 * |         1 | Single  |
 * |         2 | Double  |
 * |         3 | Curly   |
 * |         4 | Dotted  |
 * |         5 | Dashed  |
 *
 * Underline values 6 and 7 are invalid in snapshot version 1.
 *
 * Wisp, differences in shape rather than behavior:
 *   - `DecodeError` is upstream's `std.Io.Reader.Error || ParseError`
 *     flattened into one enum. `ReadFailed` cannot occur with the port's
 *     readers, so only `EndOfStream` survives from the reader half.
 *   - `packed struct(u16) Flags` becomes a plain struct with `bits()` and
 *     `fromBits()` performing the exact `@bitCast`.
 *   - `?Style` from decodeOrNull/parseOrNull becomes a `bool *has`
 *     out-param beside the Style out-param.
 *   - Upstream declares the private helpers after their callers; C++
 *     needs them declared first, so they are forward-declared below.
 */
#pragma once
#ifndef WISP_TERMINAL_SNAPSHOT_STYLE_HPP
#define WISP_TERMINAL_SNAPSHOT_STYLE_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

#include "../../vt/style.hpp"
#include "../../zigstd/reader.hpp"

namespace wisp {
namespace terminal {
namespace snapshot {
namespace style {

typedef ::wisp::vt::style::Style Style;
typedef ::wisp::terminal::sgr::Attribute::Underline Underline;

/* Number of bytes in one encoded style entry. This size is part of the
 * wire format: the codec reads and writes fixed offsets within an entry
 * of exactly this size, so if the layout changes, the snapshot version
 * and golden fixtures must also change. */
static const size_t len = 16;

struct Flags {
    bool bold = false;
    bool italic = false;
    bool faint = false;
    bool blink = false;
    bool inverse = false;
    bool invisible = false;
    bool strikethrough = false;
    bool overline = false;
    uint8_t underline = 0; /* u3 */
    uint8_t reserved = 0;  /* u5 */

    /* Wisp: @bitCast(u16). */
    uint16_t bits() const {
        return (uint16_t)((bold ? 1u : 0u) | (italic ? 2u : 0u) |
                          (faint ? 4u : 0u) | (blink ? 8u : 0u) |
                          (inverse ? 16u : 0u) | (invisible ? 32u : 0u) |
                          (strikethrough ? 64u : 0u) | (overline ? 128u : 0u) |
                          ((unsigned)(underline & 0x7) << 8) |
                          ((unsigned)(reserved & 0x1F) << 11));
    }

    /* Wisp: @bitCast(Flags, u16). */
    static Flags fromBits(uint16_t v) {
        Flags f;
        f.bold = (v & (1u << 0)) != 0;
        f.italic = (v & (1u << 1)) != 0;
        f.faint = (v & (1u << 2)) != 0;
        f.blink = (v & (1u << 3)) != 0;
        f.inverse = (v & (1u << 4)) != 0;
        f.invisible = (v & (1u << 5)) != 0;
        f.strikethrough = (v & (1u << 6)) != 0;
        f.overline = (v & (1u << 7)) != 0;
        f.underline = (uint8_t)((v >> 8) & 0x7);
        f.reserved = (uint8_t)((v >> 11) & 0x1F);
        return f;
    }
};

enum class ColorKind : uint8_t {
    none = 0,
    palette = 1,
    rgb = 2,
};

/* Errors possible while decoding one style entry. */
enum class DecodeError : uint8_t {
    none = 0,

    /* std.Io.Reader.Error */
    EndOfStream,

    /* A color kind is not defined by snapshot version 1. */
    InvalidColorKind,

    /* Bytes unused by the selected color kind are not zero. */
    InvalidColor,

    /* The encoded underline kind is not defined by snapshot version 1. */
    InvalidUnderline,

    /* One or more reserved style flag bits are set. */
    InvalidFlags,

    /* The trailing reserved field is not zero. */
    InvalidReserved,
};

inline bool parse(const uint8_t *encoded, Style *out, DecodeError *err);
inline void encodeColorBuf(uint8_t *encoded, const Style::Color &value);
inline bool parseColor(const uint8_t *encoded, Style::Color *out,
                       DecodeError *err);

/* Encode one terminal style as a fixed-size snapshot style entry.
 *
 * The entry is assembled in a fixed buffer and written once, so hot
 * encoders perform a single writer call per style. */
inline void encode(const Style &value, std::string *writer) {
    uint8_t encoded[len];
    memset(encoded, 0, sizeof encoded);
    encodeColorBuf(&encoded[0], value.fg_color);
    encodeColorBuf(&encoded[4], value.bg_color);
    encodeColorBuf(&encoded[8], value.underline_color);

    Flags flags;
    flags.bold = value.flags.bold;
    flags.italic = value.flags.italic;
    flags.faint = value.flags.faint;
    flags.blink = value.flags.blink;
    flags.inverse = value.flags.inverse;
    flags.invisible = value.flags.invisible;
    flags.strikethrough = value.flags.strikethrough;
    flags.overline = value.flags.overline;
    flags.underline = (uint8_t)value.flags.underline;
    const uint16_t bits = flags.bits();
    encoded[12] = (uint8_t)(bits & 0xFF);
    encoded[13] = (uint8_t)(bits >> 8);
    writer->append((const char *)encoded, len);
}

/* Decode and validate one fixed-size snapshot style entry. */
template <typename ReaderT>
inline bool decode(ReaderT *reader, Style *out, DecodeError *err) {
    uint8_t encoded[len];
    ::wisp::zigstd::ReadError rerr = ::wisp::zigstd::ReadError::none;
    if (!reader->readSliceAll(encoded, len, &rerr)) {
        *err = DecodeError::EndOfStream;
        return false;
    }
    return parse(encoded, out, err);
}

/* Decode and validate one complete fixed-size style entry buffer. */
inline bool parse(const uint8_t *encoded, Style *out, DecodeError *err) {
    Style::Color fg_color, bg_color, underline_color;
    if (!parseColor(&encoded[0], &fg_color, err)) return false;
    if (!parseColor(&encoded[4], &bg_color, err)) return false;
    if (!parseColor(&encoded[8], &underline_color, err)) return false;

    const Flags flags = Flags::fromBits(
        (uint16_t)(encoded[12] | ((uint16_t)encoded[13] << 8)));
    if (flags.reserved != 0) {
        *err = DecodeError::InvalidFlags;
        return false;
    }

    /* Wisp: std.enums.fromInt(sgr.Attribute.Underline, ...). */
    if (flags.underline > (uint8_t)Underline::dashed) {
        *err = DecodeError::InvalidUnderline;
        return false;
    }
    const Underline underline = (Underline)flags.underline;

    const uint16_t reserved =
        (uint16_t)(encoded[14] | ((uint16_t)encoded[15] << 8));
    if (reserved != 0) {
        *err = DecodeError::InvalidReserved;
        return false;
    }

    Style result;
    result.fg_color = fg_color;
    result.bg_color = bg_color;
    result.underline_color = underline_color;
    result.flags.bold = flags.bold;
    result.flags.italic = flags.italic;
    result.flags.faint = flags.faint;
    result.flags.blink = flags.blink;
    result.flags.inverse = flags.inverse;
    result.flags.invisible = flags.invisible;
    result.flags.strikethrough = flags.strikethrough;
    result.flags.overline = flags.overline;
    result.flags.underline = underline;
    *out = result;
    *err = DecodeError::none;
    return true;
}

/* Decode strictly after consuming one complete fixed-size style entry.
 *
 * Unlike `decode`, a semantic error leaves `reader` at the next entry. This
 * lets an enclosing codec catch the error and choose its own fallback without
 * losing the surrounding payload boundary. */
template <typename ReaderT>
inline bool decodeOrDiscard(ReaderT *reader, Style *out, DecodeError *err) {
    uint8_t encoded[len];
    ::wisp::zigstd::ReadError rerr = ::wisp::zigstd::ReadError::none;
    if (!reader->readSliceAll(encoded, len, &rerr)) {
        *err = DecodeError::EndOfStream;
        return false;
    }
    return parse(encoded, out, err);
}

/* Decode one complete entry, returning null for invalid semantic contents.
 *
 * Reader errors remain structural and are always propagated. This is the
 * lenient entry point for enclosing codecs which can safely replace an
 * invalid fixed-size style without treating truncation as invalid styling.
 *
 * Wisp: returns false only for a reader error; on true, `*has` is the
 * non-null case. */
template <typename ReaderT>
inline bool decodeOrNull(ReaderT *reader, Style *out, bool *has,
                         ::wisp::zigstd::ReadError *rerr) {
    DecodeError err = DecodeError::none;
    if (decodeOrDiscard(reader, out, &err)) {
        *has = true;
        return true;
    }
    switch (err) {
        case DecodeError::EndOfStream:
            *rerr = ::wisp::zigstd::ReadError::EndOfStream;
            return false;

        case DecodeError::InvalidColorKind:
        case DecodeError::InvalidColor:
        case DecodeError::InvalidUnderline:
        case DecodeError::InvalidFlags:
        case DecodeError::InvalidReserved:
        case DecodeError::none:
            break;
    }
    *has = false;
    return true;
}

/* `decodeOrNull` over one already-buffered entry, for enclosing codecs
 * that parse many entries from a flat payload without reader calls.
 *
 * Wisp: returns false for the null case. */
inline bool parseOrNull(const uint8_t *encoded, Style *out) {
    DecodeError err = DecodeError::none;
    return parse(encoded, out, &err);
}

inline void encodeColorBuf(uint8_t *encoded, const Style::Color &value) {
    switch (value.tag) {
        case Style::Color::Tag::none:
            encoded[0] = (uint8_t)ColorKind::none;
            break;
        case Style::Color::Tag::palette:
            encoded[0] = (uint8_t)ColorKind::palette;
            encoded[1] = value.palette;
            break;
        case Style::Color::Tag::rgb:
            encoded[0] = (uint8_t)ColorKind::rgb;
            encoded[1] = value.rgb.r;
            encoded[2] = value.rgb.g;
            encoded[3] = value.rgb.b;
            break;
    }
}

inline bool parseColor(const uint8_t *encoded, Style::Color *out,
                       DecodeError *err) {
    /* Kind must be something we know about. */
    if (encoded[0] > (uint8_t)ColorKind::rgb) {
        *err = DecodeError::InvalidColorKind;
        return false;
    }

    switch ((ColorKind)encoded[0]) {
        case ColorKind::none:
            if (encoded[1] == 0 && encoded[2] == 0 && encoded[3] == 0) {
                *out = Style::Color::none();
                return true;
            }
            *err = DecodeError::InvalidColor;
            return false;
        case ColorKind::palette:
            if (encoded[2] == 0 && encoded[3] == 0) {
                *out = Style::Color::makePalette(encoded[1]);
                return true;
            }
            *err = DecodeError::InvalidColor;
            return false;
        case ColorKind::rgb: {
            ::wisp::terminal::RGB rgb;
            rgb.r = encoded[1];
            rgb.g = encoded[2];
            rgb.b = encoded[3];
            *out = Style::Color::makeRgb(rgb);
            return true;
        }
    }
    *err = DecodeError::InvalidColorKind;
    return false;
}

} /* namespace style */
} /* namespace snapshot */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_SNAPSHOT_STYLE_HPP */
