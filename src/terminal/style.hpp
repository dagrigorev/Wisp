/* Ported from Ghostty src/terminal/style.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Cell styling, and the interned set that stores it.
 *
 * A cell does not carry its own attributes. It holds a small style ID into a
 * per-page RefCountedSet, so a screenful of identically styled text costs one
 * style record rather than thousands. ID 0 is the default style and is never
 * stored.
 *
 * CYCLE. Upstream's style.zig and page.zig import each other: style needs
 * page.Cell for bg() and bgCell(), and page needs Style and Set. Zig resolves
 * that lazily; a C++ header cannot. Cell is forward declared here and the two
 * methods that need a complete Cell are declared but not defined — page.hpp
 * defines them once Cell is complete. See docs/GHOSTTY_PORT_ORDER.md.
 */

#pragma once
#ifndef WISP_TERMINAL_STYLE_HPP
#define WISP_TERMINAL_STYLE_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "size.hpp"
#include "color.hpp"
#include "sgr.hpp"
#include "ref_counted_set.hpp"

namespace wisp {
namespace terminal {

struct Cell;   /* defined in page.hpp; see CYCLE above */

namespace style {

/* A style ID, at most the number of cells that fit in a page. */
typedef StyleCountInt Id;

/* The ID used for default styling. Never occupies a slot in the set. */
static const Id DEFAULT_ID = 0;

/* ─── color ──────────────────────────────────────────────────────────────── */

/* A color for an SGR attribute. The source is tracked alongside the value so
 * that a palette change can be reacted to correctly — a cell colored by
 * palette index must follow the palette, while a direct RGB cell must not. */
struct StyleColor {
    enum class Tag : uint8_t { none = 0, palette = 1, rgb = 2 };

    Tag     tag;
    uint8_t palette;
    RGB     rgb;

    StyleColor() : tag(Tag::none), palette(0), rgb() {}

    static StyleColor none_color() { return StyleColor(); }

    static StyleColor from_palette(uint8_t idx) {
        StyleColor c;
        c.tag = Tag::palette;
        c.palette = idx;
        return c;
    }

    static StyleColor from_rgb(RGB v) {
        StyleColor c;
        c.tag = Tag::rgb;
        c.rgb = v;
        return c;
    }

    bool eql(const StyleColor &o) const {
        if (tag != o.tag) return false;
        switch (tag) {
            case Tag::none:    return true;
            case Tag::palette: return palette == o.palette;
            case Tag::rgb:     return rgb.eql(o.rgb);
        }
        return false;
    }
};

/* ─── flags ──────────────────────────────────────────────────────────────── */

/* The on/off attributes. Kept narrow deliberately: these are stored per style
 * record, and a page can hold many. */
struct Flags {
    bool bold;
    bool italic;
    bool faint;
    bool blink;
    bool inverse;
    bool invisible;
    bool strikethrough;
    bool overline;
    Underline underline;

    Flags()
        : bold(false), italic(false), faint(false), blink(false),
          inverse(false), invisible(false), strikethrough(false),
          overline(false), underline(Underline::none) {}

    bool eql(const Flags &o) const {
        return bold == o.bold && italic == o.italic && faint == o.faint &&
               blink == o.blink && inverse == o.inverse &&
               invisible == o.invisible && strikethrough == o.strikethrough &&
               overline == o.overline && underline == o.underline;
    }
};

/* ─── style ──────────────────────────────────────────────────────────────── */

/* The canonical form is the byte image a Style hashes and compares as.
 *
 * It must be unique: two styles that are equal have to produce identical
 * bytes, or the interning set would store duplicates and cells that should
 * share an ID would not. That is why it is built by zeroing a buffer and
 * filling only meaningful fields — a color's palette and rgb bytes are left
 * zero unless its tag selects them, so stale values in unused fields cannot
 * leak into the image. */
static const size_t STYLE_CANONICAL_SIZE = 20;

struct Style {
    StyleColor fg_color;
    StyleColor bg_color;
    StyleColor underline_color;
    Flags      flags;

    Style() : fg_color(), bg_color(), underline_color(), flags() {}

    bool eql(const Style &o) const {
        return flags.eql(o.flags) &&
               fg_color.eql(o.fg_color) &&
               bg_color.eql(o.bg_color) &&
               underline_color.eql(o.underline_color);
    }

    /* True if this is the default style, which is never interned. */
    bool is_default() const { return eql(Style()); }

    /* Write the canonical byte image. */
    void canonical(uint8_t out[STYLE_CANONICAL_SIZE]) const {
        memset(out, 0, STYLE_CANONICAL_SIZE);

        write_color(out + 0, fg_color);
        write_color(out + 5, bg_color);
        write_color(out + 10, underline_color);

        uint16_t f = 0;
        if (flags.bold)          f |= 1u << 0;
        if (flags.italic)        f |= 1u << 1;
        if (flags.faint)         f |= 1u << 2;
        if (flags.blink)         f |= 1u << 3;
        if (flags.inverse)       f |= 1u << 4;
        if (flags.invisible)     f |= 1u << 5;
        if (flags.strikethrough) f |= 1u << 6;
        if (flags.overline)      f |= 1u << 7;
        f |= (uint16_t)(((uint16_t)flags.underline & 0x7) << 8);

        out[15] = (uint8_t)(f & 0xFF);
        out[16] = (uint8_t)((f >> 8) & 0xFF);
        /* 17..19 stay zero: padding, kept so the image is a round size and
         * any future field has somewhere to go without changing the hash of
         * existing styles more than necessary. */
    }

    /* FNV-1a over the canonical image.
     *
     * The set only needs a hash that is stable and well distributed; it is not
     * persisted, so it does not have to match upstream's choice of function. */
    uint64_t hash() const {
        uint8_t buf[STYLE_CANONICAL_SIZE];
        canonical(buf);

        uint64_t h = 1469598103934665603ULL;
        for (size_t i = 0; i < STYLE_CANONICAL_SIZE; i++) {
            h ^= (uint64_t)buf[i];
            h *= 1099511628211ULL;
        }
        return h;
    }

    /* Background color for a cell carrying this style.
     *
     * A cell may encode its own background directly, which takes precedence
     * over the style. Declared here, defined in page.hpp where Cell is
     * complete — see CYCLE at the top of this file. */
    bool bg(const Cell *cell, const Palette *palette, RGB *out) const;

    /* A background-color-only cell for this style, if it has one. */
    bool bg_cell(Cell *out) const;

    /* Foreground resolution options. */
    struct FgOptions {
        RGB            def;         /* color when the style specifies none */
        const Palette *palette;
        bool           has_bold;    /* whether a bold override applies */
        bool           bold_bright; /* true = brighten, false = use bold_color */
        RGB            bold_color;

        FgOptions()
            : def(), palette(nullptr), has_bold(false),
              bold_bright(false), bold_color() {}
    };

    /* Foreground color for this style.
     *
     * The bold check is not hoisted out of the switch: what bold means depends
     * on where the color came from. A palette color brightens by index, while
     * an explicit RGB color only changes if it matches the default. */
    RGB fg(const FgOptions &opts) const {
        switch (fg_color.tag) {
            case StyleColor::Tag::none:
                if (flags.bold && opts.has_bold && !opts.bold_bright) {
                    return opts.bold_color;
                }
                return opts.def;

            case StyleColor::Tag::palette: {
                uint8_t idx = fg_color.palette;
                /* Bold promotes the first eight colors to their bright
                 * counterparts; anything already bright stays put. */
                if (flags.bold && opts.has_bold) {
                    const uint8_t bright_offset = 8;
                    if (idx < bright_offset) idx = (uint8_t)(idx + bright_offset);
                }
                return opts.palette ? (*opts.palette)[idx] : RGB();
            }

            case StyleColor::Tag::rgb:
                if (flags.bold && opts.has_bold && !opts.bold_bright &&
                    fg_color.rgb.eql(opts.def)) {
                    return opts.bold_color;
                }
                return fg_color.rgb;
        }
        return opts.def;
    }

    /* Underline color, falling back to nothing when unset. */
    bool underline_rgb(const Palette *palette, RGB *out) const {
        switch (underline_color.tag) {
            case StyleColor::Tag::none:
                return false;
            case StyleColor::Tag::palette:
                if (!palette) return false;
                *out = (*palette)[underline_color.palette];
                return true;
            case StyleColor::Tag::rgb:
                *out = underline_color.rgb;
                return true;
        }
        return false;
    }

private:
    static void write_color(uint8_t *p, const StyleColor &c) {
        p[0] = (uint8_t)c.tag;
        switch (c.tag) {
            case StyleColor::Tag::none:
                break;
            case StyleColor::Tag::palette:
                p[1] = c.palette;
                break;
            case StyleColor::Tag::rgb:
                p[2] = c.rgb.r;
                p[3] = c.rgb.g;
                p[4] = c.rgb.b;
                break;
        }
    }
};

/* ─── the interned set ───────────────────────────────────────────────────── */

/* Context wiring Style into RefCountedSet. Styles own nothing, so the
 * deletion hook is empty. */
struct Context {
    uint64_t hash(const Style &s) const { return s.hash(); }
    bool eql(const Style &a, const Style &b) const { return a.eql(b); }
    void deleted(const Style &) const {}
};

typedef RefCountedSet<Style, Id, uint16_t, Context> Set;

} /* namespace style */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_STYLE_HPP */
