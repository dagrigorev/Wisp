/* Transliterated from Ghostty src/terminal/sgr.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * SGR (Select Graphic Rendition) attrinvbute parsing and types.
 *
 * TRANSLITERATION, see parser.hpp for the Zig-to-C++ mapping. Comments are
 * upstream's unless marked "Wisp:".
 *
 * Wisp: union(Tag) is a tag plus one field per payload type. Tag names that
 * start with a digit use upstream's own C ABI renames (256_fg is fg_256,
 * 8_bright_bg is bright_bg_8, ...). The C ABI padding/cval helpers are not
 * carried over; there is no C ABI here.
 */

#pragma once
#ifndef WISP_TERMINAL_SGR_HPP
#define WISP_TERMINAL_SGR_HPP

#include <stddef.h>
#include <stdint.h>

#include "color.hpp"
#include "parser.hpp"

namespace wisp {
namespace terminal {
namespace sgr {

typedef ::wisp::terminal::parser::SepList SepList;

/* Attribute type for SGR */
struct Attribute {
    enum class Tag : uint8_t {
        /* Unset all attributes */
        unset,

        /* Unknown attribute, the raw CSI command parameters are here. */
        unknown,

        /* Bold the text. */
        bold,
        reset_bold,

        /* Italic text. */
        italic,
        reset_italic,

        /* Faint/dim text.
         * Note: reset faint is the same SGR code as reset bold */
        faint,

        /* Underline the text */
        underline,
        underline_color,
        underline_color_256,
        reset_underline_color,

        /* Overline the text */
        overline,
        reset_overline,

        /* Blink the text */
        blink,
        reset_blink,

        /* Invert fg/bg colors. */
        inverse,
        reset_inverse,

        /* Invisible */
        invisible,
        reset_invisible,

        /* Strikethrough the text. */
        strikethrough,
        reset_strikethrough,

        /* Set foreground color as RGB values. */
        direct_color_fg,

        /* Set background color as RGB values. */
        direct_color_bg,

        /* Set the background/foreground as a named color attribute. */
        bg_8,
        fg_8,

        /* Reset the fg/bg to their default values. */
        reset_fg,
        reset_bg,

        /* Set the background/foreground as a named bright color attribute. */
        bright_bg_8,
        bright_fg_8,

        /* Set background color as 256-color palette. */
        bg_256,

        /* Set foreground color as 256-color palette. */
        fg_256,
    };

    struct Unknown {
        /* Full is the full SGR input. */
        const uint16_t *full;
        size_t full_len;

        /* Partial is the remaining, where we got hung up. */
        const uint16_t *partial;
        size_t partial_len;
    };

    enum class Underline : uint8_t {
        none = 0,
        single = 1,
        double_ = 2,
        curly = 3,
        dotted = 4,
        dashed = 5,
    };

    Tag tag;

    /* Wisp: payloads. `unknown` for .unknown; `underline` for .underline;
     * `rgb` for .underline_color, .direct_color_fg and .direct_color_bg;
     * `index` for the 256 variants; `name` for the 8 variants. */
    Unknown unknown;
    Underline underline;
    RGB rgb;
    uint8_t index;
    Name name;

    Attribute()
        : tag(Tag::unset), unknown(), underline(Underline::none), rgb(),
          index(0), name(Name::black) {}

    static Attribute make(Tag t) {
        Attribute a;
        a.tag = t;
        return a;
    }
    static Attribute makeUnderline(Underline u) {
        Attribute a = make(Tag::underline);
        a.underline = u;
        return a;
    }
    static Attribute makeRgb(Tag t, uint8_t r, uint8_t g, uint8_t b) {
        Attribute a = make(t);
        a.rgb = RGB(r, g, b);
        return a;
    }
    static Attribute makeIndex(Tag t, uint8_t idx) {
        Attribute a = make(t);
        a.index = idx;
        return a;
    }
    static Attribute makeName(Tag t, uint16_t v) {
        Attribute a = make(t);
        a.name = (Name)v;
        return a;
    }
    static Attribute makeUnknown(const uint16_t *full, size_t full_len,
                                 const uint16_t *partial, size_t partial_len) {
        Attribute a = make(Tag::unknown);
        a.unknown.full = full;
        a.unknown.full_len = full_len;
        a.unknown.partial = partial;
        a.unknown.partial_len = partial_len;
        return a;
    }
};

/* Parser parses the attributes from a list of SGR parameters. */
struct Parser {
    const uint16_t *params; /* = &.{} */
    size_t params_len;
    SepList params_sep;     /* = .initEmpty() */
    size_t idx;             /* = 0 */

    /* Empty state parser. */
    Parser() : params(nullptr), params_len(0), params_sep(), idx(0) {}

    Parser(const uint16_t *p, size_t len, SepList sep = SepList())
        : params(p), params_len(len), params_sep(sep), idx(0) {}

    /* Next returns the next attribute or null if there are no more attributes.
     * Wisp: ?Attribute is the bool return plus *out. */
    bool next(Attribute *out) {
        typedef Attribute::Tag T;
        if (idx >= params_len) {
            /* We're more likely to not be done than to be done. */

            /* Add one to ensure we don't loop on unset */
            const bool first = idx == 0;
            idx += 1;

            /* If we're at index zero it means we must have an empty list
             * and an empty list implicitly means unset, otherwise we're
             * done and return null. */
            if (first) {
                *out = Attribute::make(T::unset);
                return true;
            }
            return false;
        }

        const uint16_t *slice = params + idx;
        const size_t slice_len = params_len - idx;
        /* Call inlined for performance reasons. */
        const bool colon = params_sep.isSet(idx);
        idx += 1;

        /* Our last one will have an idx be the last value. */
        if (slice_len == 0) return false;

        /* If we have a colon separator then we need to ensure we're
         * parsing a value that allows it. */
        if (colon) {
            /* Colons are fairly rare in the wild. */
            switch (slice[0]) {
                case 4: case 38: case 48: case 58: break;

                default: {
                    /* In real world use it's very rare
                     * that we receive an invalid sequence. */

                    /* Consume all the colon separated
                     * values and return them as unknown. */
                    const size_t start = idx;
                    while (params_sep.isSet(idx)) idx += 1;
                    idx += 1;
                    const size_t n = idx - start + 1;
                    *out = Attribute::makeUnknown(params, params_len, slice,
                                                  n < slice_len ? n : slice_len);
                    return true;
                }
            }
        }

        switch (slice[0]) {
            case 0: *out = Attribute::make(T::unset); return true;

            case 1: *out = Attribute::make(T::bold); return true;

            case 2: *out = Attribute::make(T::faint); return true;

            case 3: *out = Attribute::make(T::italic); return true;

            case 4: {
                if (colon) {
                    /* Colons are fairly rare in the wild. */

                    /* A trailing colon with no following sub-param
                     * (e.g. "ESC[58:4:m") leaves the colon separator
                     * bit set on the last param without adding another
                     * entry, so we can see param 4 with a colon but
                     * nothing after it. */
                    if (slice_len < 2) break;

                    if (isColon()) {
                        /* Invalid/unknown SGRs are just not very likely. */
                        consumeUnknownColon();
                        break;
                    }

                    idx += 1;
                    Attribute::Underline u;
                    switch (slice[1]) {
                        case 0: u = Attribute::Underline::none; break;
                        case 1: u = Attribute::Underline::single; break;
                        case 2: u = Attribute::Underline::double_; break;
                        case 3: u = Attribute::Underline::curly; break;
                        case 4: u = Attribute::Underline::dotted; break;
                        case 5: u = Attribute::Underline::dashed; break;

                        /* For unknown underline styles,
                         * just render a single underline. */
                        default: u = Attribute::Underline::single; break;
                    }
                    *out = Attribute::makeUnderline(u);
                    return true;
                }

                *out = Attribute::makeUnderline(Attribute::Underline::single);
                return true;
            }

            case 5: *out = Attribute::make(T::blink); return true;

            case 6: *out = Attribute::make(T::blink); return true;

            case 7: *out = Attribute::make(T::inverse); return true;

            case 8: *out = Attribute::make(T::invisible); return true;

            case 9: *out = Attribute::make(T::strikethrough); return true;

            case 21: *out = Attribute::makeUnderline(Attribute::Underline::double_); return true;

            case 22: *out = Attribute::make(T::reset_bold); return true;

            case 23: *out = Attribute::make(T::reset_italic); return true;

            case 24: *out = Attribute::makeUnderline(Attribute::Underline::none); return true;

            case 25: *out = Attribute::make(T::reset_blink); return true;

            case 27: *out = Attribute::make(T::reset_inverse); return true;

            case 28: *out = Attribute::make(T::reset_invisible); return true;

            case 29: *out = Attribute::make(T::reset_strikethrough); return true;

            case 30: case 31: case 32: case 33:
            case 34: case 35: case 36: case 37:
                *out = Attribute::makeName(T::fg_8, (uint16_t)(slice[0] - 30));
                return true;

            case 38:
                if (slice_len >= 2) {
                    /* We are very likely to have enough parameters. */
                    switch (slice[1]) {
                        /* `2` indicates direct-color (r, g, b).
                         * We need at least 3 more params for this to make sense. */
                        case 2:
                            if (parseDirectColor(T::direct_color_fg, slice, slice_len, colon, out)) return true;
                            break;

                        /* `5` indicates indexed color. */
                        case 5:
                            if (slice_len >= 3) {
                                idx += 2;
                                *out = Attribute::makeIndex(T::fg_256, (uint8_t)slice[2]);
                                return true;
                            }
                            break;

                        default: break;
                    }
                }
                break;

            case 39: *out = Attribute::make(T::reset_fg); return true;

            case 40: case 41: case 42: case 43:
            case 44: case 45: case 46: case 47:
                *out = Attribute::makeName(T::bg_8, (uint16_t)(slice[0] - 40));
                return true;

            case 48:
                if (slice_len >= 2) {
                    switch (slice[1]) {
                        /* `2` indicates direct-color (r, g, b).
                         * We need at least 3 more params for this to make sense. */
                        case 2:
                            if (parseDirectColor(T::direct_color_bg, slice, slice_len, colon, out)) return true;
                            break;

                        /* `5` indicates indexed color. */
                        case 5:
                            if (slice_len >= 3) {
                                idx += 2;
                                *out = Attribute::makeIndex(T::bg_256, (uint8_t)slice[2]);
                                return true;
                            }
                            break;

                        default: break;
                    }
                }
                break;

            case 49: *out = Attribute::make(T::reset_bg); return true;

            case 53: *out = Attribute::make(T::overline); return true;
            case 55: *out = Attribute::make(T::reset_overline); return true;

            case 58:
                if (slice_len >= 2) {
                    switch (slice[1]) {
                        /* `2` indicates direct-color (r, g, b).
                         * We need at least 3 more params for this to make sense. */
                        case 2:
                            if (parseDirectColor(T::underline_color, slice, slice_len, colon, out)) return true;
                            break;

                        /* `5` indicates indexed color. */
                        case 5:
                            if (slice_len >= 3) {
                                idx += 2;
                                *out = Attribute::makeIndex(T::underline_color_256, (uint8_t)slice[2]);
                                return true;
                            }
                            break;
                        default: break;
                    }
                }
                break;

            case 59: *out = Attribute::make(T::reset_underline_color); return true;

            case 90: case 91: case 92: case 93:
            case 94: case 95: case 96: case 97:
                /* 82 instead of 90 to offset to "bright" colors */
                *out = Attribute::makeName(T::bright_fg_8, (uint16_t)(slice[0] - 82));
                return true;

            case 100: case 101: case 102: case 103:
            case 104: case 105: case 106: case 107:
                *out = Attribute::makeName(T::bright_bg_8, (uint16_t)(slice[0] - 92));
                return true;

            default: break;
        }

        *out = Attribute::makeUnknown(params, params_len, slice, slice_len);
        return true;
    }

    /* Wisp: ?Attribute is the bool return plus *out. */
    bool parseDirectColor(Attribute::Tag tag, const uint16_t *slice, size_t slice_len,
                          bool colon, Attribute *out) {
        /* Any direct color style must have at least 5 values. */
        if (slice_len < 5) return false;

        /* Only used for direct color sets (38, 48, 58) and subparam 2.
         * assert(slice[1] == 2) */

        /* Note: We use @truncate because the value should be 0 to 255. If
         * it isn't, the behavior is undefined so we just... truncate it. */

        /* If we don't have a colon, then we expect exactly 3 semicolon
         * separated values. */
        if (!colon) {
            /* Semicolons are much more common than colons. */
            idx += 4;
            *out = Attribute::makeRgb(tag, (uint8_t)slice[2], (uint8_t)slice[3], (uint8_t)slice[4]);
            return true;
        }

        /* We have a colon, we might have either 5 or 6 values depending
         * on if the colorspace is present. */
        const size_t count = countColon();
        switch (count) {
            case 3:
                /* This is the much more common case in the wild. */
                idx += 4;
                *out = Attribute::makeRgb(tag, (uint8_t)slice[2], (uint8_t)slice[3], (uint8_t)slice[4]);
                return true;

            case 4:
                idx += 5;
                *out = Attribute::makeRgb(tag, (uint8_t)slice[3], (uint8_t)slice[4], (uint8_t)slice[5]);
                return true;

            default:
                /* Invalid/unknown SGRs just don't happen very often at all. */
                consumeUnknownColon();
                return false;
        }
    }

    /* Returns true if the present position has a colon separator.
     * This always returns false for the last value since it has no
     * separator. */
    bool isColon() const { return params_sep.isSet(idx); }

    size_t countColon() const {
        size_t count = 0;
        size_t i = idx;
        while (i < params_len - 1 && params_sep.isSet(i)) {
            count += 1;
            i += 1;
        }
        return count;
    }

    /* Consumes all the remaining parameters separated by a colon and
     * returns an unknown attribute. */
    void consumeUnknownColon() {
        const size_t count = countColon();
        idx += count + 1;
    }
};

} /* namespace sgr */

/* Wisp adapter, not upstream: style.hpp is not transliterated yet and names
 * the underline style unqualified. */
typedef sgr::Attribute::Underline Underline;

} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_SGR_HPP */
