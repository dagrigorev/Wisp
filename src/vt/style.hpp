/* Transliterated from Ghostty src/terminal/style.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: style.zig and page.zig import each other. Style::bg and
 * Style::bgCell need a complete page::Cell, so they are declared here and
 * defined in page.hpp. color.hpp and sgr.hpp are the transliterations in
 * src/terminal/. The debug `format` for Style and Color (std.fmt `{any}`
 * output) is not carried over; the VT and HTML formatters are. Writers are
 * std::string.
 */

#pragma once
#ifndef WISP_VT_STYLE_HPP
#define WISP_VT_STYLE_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

#include "fastprint.hpp"
#include "ref_counted_set.hpp"
#include "size.hpp"
#include "../terminal/color.hpp"
#include "../terminal/sgr.hpp"
#include "../zigstd/hash_int.hpp"

namespace wisp {
namespace vt {

namespace page { struct Cell; }

namespace style {

typedef ::wisp::terminal::RGB RGB;
typedef ::wisp::terminal::Palette Palette;

/* The unique identifier for a style. This is at most the number of cells
 * that can fit into a terminal page. */
typedef size::StyleCountInt Id;

/* The Id to use for default styling. */
static const Id default_id = 0;

/* The style attributes for a cell. */
struct Style {
    /* Wisp: packed struct(u16); bitfields keep it two bytes as upstream,
     * and bits() gives the exact packed layout. */
    struct Flags {
        bool bold : 1;
        bool italic : 1;
        bool faint : 1;
        bool blink : 1;
        bool inverse : 1;
        bool invisible : 1;
        bool strikethrough : 1;
        bool overline : 1;
        ::wisp::terminal::sgr::Attribute::Underline underline : 3;

        Flags()
            : bold(false), italic(false), faint(false), blink(false), inverse(false),
              invisible(false), strikethrough(false), overline(false),
              underline(::wisp::terminal::sgr::Attribute::Underline::none) {}

        /* Wisp: @bitCast(u16) — bools in bits 0..7, underline (u3) in bits
         * 8..10, padding zero. */
        uint16_t bits() const {
            return (uint16_t)((bold ? 1 : 0) | (italic ? 2 : 0) | (faint ? 4 : 0) |
                              (blink ? 8 : 0) | (inverse ? 16 : 0) | (invisible ? 32 : 0) |
                              (strikethrough ? 64 : 0) | (overline ? 128 : 0) |
                              (((uint16_t)underline & 7) << 8));
        }

        bool operator==(const Flags &o) const { return bits() == o.bits(); }
        bool operator!=(const Flags &o) const { return bits() != o.bits(); }
    };

    /* The color for an SGR attribute. A color can come from multiple
     * sources so we use this to track the source plus color value so that
     * we can properly react to things like palette changes.
     *
     * Wisp: union(Tag) with an RGB packed struct(u24) payload is 8 bytes
     * with 4-byte alignment in Zig; alignas(4) keeps sizeof(Style), and so
     * the page layout, the same. */
    struct alignas(4) Color {
        enum class Tag : uint8_t {
            none,
            palette,
            rgb,
        };

        Tag tag;
        uint8_t palette;
        RGB rgb;

        Color() : tag(Tag::none), palette(0), rgb() {}

        static Color none() { return Color(); }
        static Color makePalette(uint8_t idx) {
            Color c;
            c.tag = Tag::palette;
            c.palette = idx;
            return c;
        }
        static Color makeRgb(RGB v) {
            Color c;
            c.tag = Tag::rgb;
            c.rgb = v;
            return c;
        }

        /* True if the color is equal to another color. */
        bool eql(const Color &other) const {
            if (tag != other.tag) return false;
            switch (tag) {
                case Tag::none: return true;
                case Tag::palette: return palette == other.palette;
                case Tag::rgb: return rgb.r == other.rgb.r && rgb.g == other.rgb.g && rgb.b == other.rgb.b;
            }
            return false;
        }
    };

    /* Various colors, all self-explanatory. */
    Color fg_color;        /* = .none */
    Color bg_color;        /* = .none */
    Color underline_color; /* = .none */

    /* On/off attributes that don't require much bit width so we use
     * a packed struct to make this take up significantly less space. */
    Flags flags; /* = .{} */

    Style() : fg_color(), bg_color(), underline_color(), flags() {}

    /* True if the style is the default style. */
    bool default_() const { return eql(Style()); }

    /* True if the style is equal to another style. */
    bool eql(const Style &other) const {
        return flags == other.flags && fg_color.eql(other.fg_color) &&
               bg_color.eql(other.bg_color) && underline_color.eql(other.underline_color);
    }

    /* Returns the bg color for a cell with this style given the cell
     * that has this style and the palette to use.
     *
     * Note that generally if a cell is a color-only cell, it SHOULD
     * only have the default style, but this is meant to work with the
     * default style as well.
     *
     * Wisp: defined in page.hpp. ?RGB is the bool return plus *out. */
    bool bg(const page::Cell *cell, const Palette *palette, RGB *out) const;

    /* The color to use for bold text. This avoids a dependency on the
     * config module by using terminal-native color types. */
    struct BoldColor {
        enum class Tag : uint8_t { color, bright };
        Tag tag;
        RGB color;
    };

    struct Fg {
        /* The default color to use if the style doesn't specify a
         * foreground color and no configuration options override
         * it. */
        RGB default_;

        /* The current color palette. Required to map palette indices to
         * real color values. */
        const Palette *palette;

        /* If specified, the color to use for bold text. */
        bool has_bold; /* bold: ?BoldColor = null */
        BoldColor bold;

        Fg() : default_(), palette(nullptr), has_bold(false), bold() {}
    };

    /* Returns the fg color for a cell with this style given the palette
     * and various configuration options. */
    RGB fg(const Fg &opts) const {
        /* Note we don't pull the bold check to the top-level here because
         * we don't want to duplicate the conditional multiple times since
         * certain colors require more checks (e.g. `bold_is_bright`). */

        switch (fg_color.tag) {
            case Color::Tag::none:
                if (flags.bold) {
                    if (opts.has_bold) {
                        if (opts.bold.tag == BoldColor::Tag::color) return opts.bold.color;
                    }
                }
                return opts.default_;

            case Color::Tag::palette: {
                const uint8_t idx = fg_color.palette;
                if (flags.bold) {
                    if (opts.has_bold) {
                        const uint8_t bright_offset = (uint8_t)::wisp::terminal::Name::bright_black;
                        if (idx < bright_offset) {
                            return opts.palette->colors[idx + bright_offset];
                        }
                    }
                }
                return opts.palette->colors[idx];
            }

            case Color::Tag::rgb: {
                const RGB rgb = fg_color.rgb;
                if (flags.bold && rgb.eql(opts.default_)) {
                    if (opts.has_bold && opts.bold.tag == BoldColor::Tag::color) return opts.bold.color;
                }
                return rgb;
            }
        }
        return opts.default_;
    }

    /* Returns the underline color for this style. */
    bool underlineColor(const Palette *palette, RGB *out) const {
        switch (underline_color.tag) {
            case Color::Tag::none: return false;
            case Color::Tag::palette: *out = palette->colors[underline_color.palette]; return true;
            case Color::Tag::rgb: *out = underline_color.rgb; return true;
        }
        return false;
    }

    /* Returns a bg-color only cell from this style, if it exists.
     * Wisp: defined in page.hpp. */
    bool bgCell(page::Cell *out) const;

    /* Returns a formatter that renders this style as VT sequences,
     * to be used with `{f}`. This always resets the style first `\x1b[0m`
     * since a style is meant to be fully self-contained.
     *
     * For individual styles, this always emits multiple SGR sequences
     * (i.e. an individual `\x1b[<stuff>m` for each attribute) rather than
     * trying to combine them into a single sequence. We do this because
     * terminals have varying levels of support for combined sequences
     * especially with mixed separators (e.g. `:` vs `;`). */
    struct VTFormatter {
        const Style *style;

        /* If set, palette colors will be emitted as RGB values instead of
         * palette indices. This is useful when you want to capture the
         * exact colors at formatting time rather than relying on the
         * terminal's palette. */
        const Palette *palette; /* = null */

        void format(std::string *writer) const {
            /* Style emission is a hot path when formatting styled terminal
             * contents, so all of the sequences are assembled in a buffer
             * and written in one call rather than going through the
             * (slower) format string machinery with a write per sequence.
             *
             * Worst case: `\x1b[0m` (4) + 7 flags (28) + `\x1b[53m` (5) +
             * `\x1b[4:2m` (6) + 3 RGB colors (19 each = 57) = 100. */
            char buf[128];

            /* Always reset the style. Styles are fully self-contained.
             * Even if this style is empty, then that means we want to go
             * back to the default. */
            memcpy(buf, "\x1b[0m", 4);
            size_t len = 4;

            /* Our flags */
            const Flags &f = style->flags;
            if (f.bold) { memcpy(buf + len, "\x1b[1m", 4); len += 4; }
            if (f.faint) { memcpy(buf + len, "\x1b[2m", 4); len += 4; }
            if (f.italic) { memcpy(buf + len, "\x1b[3m", 4); len += 4; }
            if (f.blink) { memcpy(buf + len, "\x1b[5m", 4); len += 4; }
            if (f.inverse) { memcpy(buf + len, "\x1b[7m", 4); len += 4; }
            if (f.invisible) { memcpy(buf + len, "\x1b[8m", 4); len += 4; }
            if (f.strikethrough) { memcpy(buf + len, "\x1b[9m", 4); len += 4; }
            if (f.overline) { memcpy(buf + len, "\x1b[53m", 5); len += 5; }
            typedef ::wisp::terminal::sgr::Attribute::Underline U;
            switch (f.underline) {
                case U::none: break;
                case U::single: memcpy(buf + len, "\x1b[4m", 4); len += 4; break;
                case U::double_: memcpy(buf + len, "\x1b[4:2m", 6); len += 6; break;
                case U::curly: memcpy(buf + len, "\x1b[4:3m", 6); len += 6; break;
                case U::dotted: memcpy(buf + len, "\x1b[4:4m", 6); len += 6; break;
                case U::dashed: memcpy(buf + len, "\x1b[4:5m", 6); len += 6; break;
            }

            /* Various colors. */
            len += appendColor(buf + len, 38, style->fg_color);
            len += appendColor(buf + len, 48, style->bg_color);
            len += appendColor(buf + len, 58, style->underline_color);

            writer->append(buf, len);
        }

        /* Appends a standalone `\x1b[{prefix};5;{idx}m` or
         * `\x1b[{prefix};2;{r};{g};{b}m` sequence to buf, returning the
         * length written. */
        size_t appendColor(char *buf, uint8_t prefix, const Color &value) const {
            switch (value.tag) {
                case Color::Tag::none: return 0;

                case Color::Tag::palette: {
                    /* Direct RGB: `\x1b[{prefix};2;{r};{g};{b}m` */
                    if (palette) return appendColorRgb(buf, prefix, palette->colors[value.palette]);

                    /* Palette reference: `\x1b[{prefix};5;{idx}m` */
                    memcpy(buf, "\x1b[", 2);
                    size_t len = 2;
                    len += fastprint::printDecimalU8(buf + len, prefix);
                    memcpy(buf + len, ";5;", 3);
                    len += 3;
                    len += fastprint::printDecimalU8(buf + len, value.palette);
                    buf[len] = 'm';
                    len += 1;
                    return len;
                }

                case Color::Tag::rgb: return appendColorRgb(buf, prefix, value.rgb);
            }
            return 0;
        }

        /* Appends `\x1b[{prefix};2;{r};{g};{b}m` to buf, returning the
         * length written. */
        static size_t appendColorRgb(char *buf, uint8_t prefix, RGB rgb) {
            memcpy(buf, "\x1b[", 2);
            size_t len = 2;
            len += fastprint::printDecimalU8(buf + len, prefix);
            memcpy(buf + len, ";2;", 3);
            len += 3;
            len += fastprint::printDecimalU8(buf + len, rgb.r);
            buf[len] = ';';
            len += 1;
            len += fastprint::printDecimalU8(buf + len, rgb.g);
            buf[len] = ';';
            len += 1;
            len += fastprint::printDecimalU8(buf + len, rgb.b);
            buf[len] = 'm';
            len += 1;
            return len;
        }
    };

    struct HtmlFormatter {
        const Style *style;

        /* If set, palette colors will be emitted as RGB values instead of
         * CSS variables. This is useful when you want to capture the exact
         * colors at formatting time rather than relying on CSS variables. */
        const Palette *palette; /* = null */

        void format(std::string *writer) const {
            /* Colors */
            formatColor(writer, "color", style->fg_color);
            formatColor(writer, "background-color", style->bg_color);
            formatColor(writer, "text-decoration-color", style->underline_color);

            typedef ::wisp::terminal::sgr::Attribute::Underline U;
            const Flags &f = style->flags;

            /* Text decoration line */
            const bool has_line = f.underline != U::none || f.strikethrough || f.overline || f.blink;
            if (has_line) {
                writer->append("text-decoration-line:");
                if (f.underline != U::none) writer->append(" underline");
                if (f.strikethrough) writer->append(" line-through");
                if (f.overline) writer->append(" overline");
                if (f.blink) writer->append(" blink");
                writer->append(";");
            }

            /* Text decoration style */
            switch (f.underline) {
                case U::none: break;
                case U::single: writer->append("text-decoration-style: solid;"); break;
                case U::double_: writer->append("text-decoration-style: double;"); break;
                case U::curly: writer->append("text-decoration-style: wavy;"); break;
                case U::dotted: writer->append("text-decoration-style: dotted;"); break;
                case U::dashed: writer->append("text-decoration-style: dashed;"); break;
            }

            if (f.bold) writer->append("font-weight: bold;");
            if (f.italic) writer->append("font-style: italic;");
            if (f.faint) writer->append("opacity: 0.5;");
            if (f.invisible) writer->append("visibility: hidden;");
            if (f.inverse) writer->append("filter: invert(100%);");
        }

        void formatColor(std::string *writer, const char *property, const Color &c) const {
            /* Style emission is a hot path when formatting styled terminal
             * contents, so the values are assembled in a buffer and written
             * in one call rather than going through the (slower) format
             * string machinery. */
            char buf[32];
            size_t len = 0;
            switch (c.tag) {
                case Color::Tag::none: return;

                /* `{property}: rgb({r}, {g}, {b});` */
                case Color::Tag::palette:
                    if (palette) {
                        len = formatColorRgb(buf, palette->colors[c.palette]);
                    } else {
                        /* `{property}: var(--vt-palette-{idx});` */
                        static const char prefix[] = ": var(--vt-palette-";
                        memcpy(buf, prefix, sizeof(prefix) - 1);
                        len = sizeof(prefix) - 1;
                        len += fastprint::printDecimalU8(buf + len, c.palette);
                        memcpy(buf + len, ");", 2);
                        len += 2;
                    }
                    break;

                case Color::Tag::rgb: len = formatColorRgb(buf, c.rgb); break;
            }

            writer->append(property);
            writer->append(buf, len);
        }

        /* Writes `: rgb({r}, {g}, {b});` into buf, returning the length
         * written. */
        static size_t formatColorRgb(char *buf, RGB rgb) {
            memcpy(buf, ": rgb(", 6);
            size_t len = 6;
            len += fastprint::printDecimalU8(buf + len, rgb.r);
            memcpy(buf + len, ", ", 2);
            len += 2;
            len += fastprint::printDecimalU8(buf + len, rgb.g);
            memcpy(buf + len, ", ", 2);
            len += 2;
            len += fastprint::printDecimalU8(buf + len, rgb.b);
            memcpy(buf + len, ");", 2);
            len += 2;
            return len;
        }
    };

    VTFormatter formatterVt() const {
        VTFormatter f;
        f.style = this;
        f.palette = nullptr;
        return f;
    }

    /* Returns a formatter that renders this style as inline CSS properties,
     * to be used with `{f}`. The output is a valid CSS style string suitable
     * for use in a `style` attribute (e.g., "color: rgb(255, 0, 0); font-weight: bold;").
     *
     * Palette colors are emitted as CSS variables like `var(--vt-palette-N)`. */
    HtmlFormatter formatterHtml() const {
        HtmlFormatter f;
        f.style = this;
        f.palette = nullptr;
        return f;
    }

    /* `PackedStyle` represents the same data as `Style` but without padding,
     * which is necessary for hashing via re-interpretation of the underlying
     * bytes.
     *
     * Wisp: the 128 bits as two little-endian u64 words. Bit layout (LSB
     * first): fg/bg/underline tags (8 bits each), fg/bg/underline data
     * (24 bits each: palette idx in the low byte, or r|g<<8|b<<16), flags
     * (16 bits), 16 bits of padding. */
    static uint32_t packedData(const Color &c) {
        switch (c.tag) {
            case Color::Tag::none: return 0;
            case Color::Tag::palette: return c.palette;
            case Color::Tag::rgb: return (uint32_t)c.rgb.r | ((uint32_t)c.rgb.g << 8) | ((uint32_t)c.rgb.b << 16);
        }
        return 0;
    }

    void packed(uint64_t wide[2]) const {
        uint64_t lo = 0, hi = 0;
        lo |= (uint64_t)(uint8_t)fg_color.tag;
        lo |= (uint64_t)(uint8_t)bg_color.tag << 8;
        lo |= (uint64_t)(uint8_t)underline_color.tag << 16;
        lo |= (uint64_t)packedData(fg_color) << 24;
        const uint64_t bgd = packedData(bg_color);
        lo |= bgd << 48;           /* low 16 bits of bg data */
        hi |= bgd >> 16;           /* high 8 bits of bg data at bit 64 */
        hi |= (uint64_t)packedData(underline_color) << (72 - 64);
        hi |= (uint64_t)flags.bits() << (96 - 64);
        wide[0] = lo;
        wide[1] = hi;
    }

    uint64_t hash() const {
        /* We pack the style in to 128 bits, fold it to 64 bits,
         * then use std.hash.int to make it sufficiently uniform. */
        uint64_t wide[2];
        packed(wide);
        return ::wisp::zigstd::hash::uint64(wide[0] ^ wide[1]);
    }
};

struct SetContext {
    uint64_t hash(const Style &style) const { return style.hash(); }
    bool eql(const Style &a, const Style &b) const { return a.eql(b); }
};

typedef ref_counted_set::RefCountedSet<Style, Id, size::CellCountInt, SetContext> Set;

} /* namespace style */
} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_STYLE_HPP */
