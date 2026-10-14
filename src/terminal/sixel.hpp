/* sixel.hpp — DEC Sixel graphics decoding (the body of a Sixel DCS).
 *
 * Wisp: this has no upstream counterpart to transliterate. Ghostty does not
 * implement Sixel at all: the entire upstream tree at the pinned revision
 * mentions it exactly once, as `sixel = 4` in device_attributes.zig, which is
 * only the capability number reported in a Device Attributes reply. There is
 * therefore nothing to port, and this file is written from the published
 * format description -- DEC STD 070 and the VT330/VT340 Programmer Reference
 * Manual (EK-VT3XX-GP-002) volume 1, chapter 14 "Sixel Graphics" -- in the
 * same spirit as src/zigstd/inflate.hpp was written from RFC 1951/1950,
 * rather than adapted from anyone's implementation.
 *
 * Scope: this decodes the *body* only, the bytes between `DCS P1;P2;P3 q` and
 * ST. The DCS hook that will feed it does not exist yet; integration waits on
 * graphics_storage, which decides where a decoded image lives. P2, which
 * selects whether a zero bit paints the background colour or leaves the pixel
 * alone, is a DCS parameter rather than a body byte, so it arrives through
 * Options::background_select.
 *
 * Every byte here comes from whatever program is running on the far end of
 * the pty, so none of it is trusted:
 *   - Repeat counts, raster attributes and colour register numbers are
 *     attacker-controlled. Parameter accumulation saturates (see param_max),
 *     so `!99999999999999` cannot wrap; the saturated value is then checked
 *     against the size limits like any other number.
 *   - max_width, max_height and max_pixels are enforced *before* every
 *     allocation and re-checked on every growth, including the geometric
 *     overshoot. A body that would exceed them returns Error::TooLarge
 *     instead of allocating. This is the decompression-bomb defence: a
 *     repeat introducer can ask for billions of pixels in six bytes.
 *   - Every pixel write is bounds-checked against the buffer that exists at
 *     that moment, and the size arithmetic is overflow-checked before it
 *     reaches the allocator.
 *   - No read may pass data + len; the parser's only cursor is an index
 *     compared against len before every dereference.
 *   - Unrecognised bytes are ignored rather than fatal, as the format
 *     requires, so a body of random bytes decodes to an empty image.
 */

#pragma once
#ifndef WISP_TERMINAL_SIXEL_HPP
#define WISP_TERMINAL_SIXEL_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../zigstd/allocator.hpp"

namespace wisp {
namespace terminal {
namespace sixel {

/* A decoded image. `pixels` is width*height*4 bytes, RGBA8888 in memory
 * order: pixels[i*4+0] is red, +1 green, +2 blue, +3 alpha. An empty image
 * (a body with no data bytes) has width == height == 0 and a null pointer;
 * deinit is still safe to call on it. */
struct Image {
    uint8_t *pixels;
    size_t width;
    size_t height;

    void deinit(zigstd::Allocator alloc) {
        if (pixels != nullptr) alloc.free(pixels, width * height * 4, 1);
        pixels = nullptr;
        width = 0;
        height = 0;
    }
};

enum class Error {
    none,
    OutOfMemory,
    /* Reserved for malformed input. Nothing currently returns it: the format
     * says unrecognised bytes are ignored, and every structurally bad
     * construct this decoder can meet (a truncated repeat, an empty
     * parameter, a colour register out of range) has a defined
     * ignore-or-clamp behaviour spelled out below. It is kept so callers can
     * distinguish "bad data" from "too big" once the DCS hook lands, and so
     * a future strict mode has somewhere to report. */
    InvalidData,
    TooLarge,
};

/* What a zero bit in a sixel means, which the DCS parameter P2 selects. */
enum class BackgroundSelect {
    /* P2 = 0 or 2: zero bits paint Options::background. (DEC distinguishes
     * these two for the printer; on a display they behave the same, and this
     * decoder treats them the same.) */
    paint,
    /* P2 = 1: zero bits leave the pixel as it was. Untouched pixels are
     * fully transparent, all four bytes zero. */
    transparent,
};

struct Options {
    size_t max_width;
    size_t max_height;
    size_t max_pixels;
    BackgroundSelect background_select;
    /* 0xRRGGBBAA. Used for untouched pixels, and for zero bits when
     * background_select is `paint`. */
    uint32_t background;

    static Options defaults() {
        Options o;
        o.max_width = 10000;
        o.max_height = 10000;
        o.max_pixels = 50 * 1000 * 1000;
        o.background_select = BackgroundSelect::paint;
        o.background = 0x000000FFu; /* opaque black */
        return o;
    }
};

namespace detail {

/* Sixel data bytes are '?' (0x3F) through '~' (0x7E); value = byte - 0x3F. */
static const uint8_t data_min = 0x3F;
static const uint8_t data_max = 0x7E;

/* Colour registers. DEC hardware had 16 (the VT340 has four planes); this
 * decoder supports 256, which is what software implementations settled on
 * and what the colour-register count in a Device Attributes reply commonly
 * advertises. A `#Pc` with Pc >= 256 is ignored rather than fatal. */
static const size_t num_colors = 256;

/* Numeric parameters saturate here rather than wrapping. 65535 is the
 * largest value DEC's parameter parsing accepts, and it is far above any
 * legitimate repeat count or dimension; anything larger is refused by the
 * size limits anyway. */
static const uint32_t param_max = 65535;

static const size_t max_params = 8;

inline uint32_t satMulAdd10(uint32_t v, uint32_t digit) {
    if (v > (param_max - digit) / 10) return param_max;
    const uint32_t r = v * 10 + digit;
    return r > param_max ? param_max : r;
}

inline uint32_t packRGBA(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return (r << 24) | (g << 16) | (b << 8) | a;
}

/* Percent (0..100) to 0..255, rounded to nearest. Values above 100 are
 * clamped: DEC says 0..100, and a hostile body will say 65535. */
inline uint32_t pctTo255(uint32_t pct) {
    if (pct > 100) pct = 100;
    return (pct * 255 + 50) / 100;
}

/* HLS to RGB.
 *
 * Ambiguity, and what this chose: DEC's HLS is *not* the HSL found
 * everywhere else. DEC puts blue at hue 0 and runs the ring the other way,
 * so hue 0 is blue, 120 is red and 240 is green. Ordinary HSL has red at 0.
 * The two agree if DEC's hue is rotated by 240 degrees before the ordinary
 * HSL formula is applied, which is what this does and what libsixel (the de
 * facto reference implementation) does. Some implementations apply no
 * rotation, which swaps red and blue on every HLS-defined colour. The
 * rotation is the one that makes DEC's own default colour map come out as
 * its names say -- register 1 is "blue" at hue 0 -- so the rotation is
 * chosen here.
 *
 * h is taken modulo 360; l and s are 0..100 and are clamped. */
inline void hlsToRGB(uint32_t h, uint32_t l, uint32_t s, uint32_t *r_out, uint32_t *g_out,
                     uint32_t *b_out) {
    if (l > 100) l = 100;
    if (s > 100) s = 100;
    const double ll = (double)l / 100.0;
    const double ss = (double)s / 100.0;

    if (s == 0) {
        const uint32_t v = (uint32_t)(ll * 255.0 + 0.5);
        *r_out = v;
        *g_out = v;
        *b_out = v;
        return;
    }

    const double hh = (double)(((h % 360u) + 240u) % 360u) / 60.0;
    const double chroma = (1.0 - ((2.0 * ll > 1.0) ? (2.0 * ll - 1.0) : (1.0 - 2.0 * ll))) * ss;
    const double hmod2 = hh - 2.0 * (double)((int)(hh / 2.0));
    const double xx = chroma * (1.0 - ((hmod2 < 1.0) ? (1.0 - hmod2) : (hmod2 - 1.0)));
    const double m = ll - chroma / 2.0;

    double r = 0.0, g = 0.0, b = 0.0;
    const int sector = (int)hh; /* 0..5 */
    if (sector == 0) {
        r = chroma;
        g = xx;
    } else if (sector == 1) {
        r = xx;
        g = chroma;
    } else if (sector == 2) {
        g = chroma;
        b = xx;
    } else if (sector == 3) {
        g = xx;
        b = chroma;
    } else if (sector == 4) {
        r = xx;
        b = chroma;
    } else {
        r = chroma;
        b = xx;
    }

    *r_out = (uint32_t)((r + m) * 255.0 + 0.5);
    *g_out = (uint32_t)((g + m) * 255.0 + 0.5);
    *b_out = (uint32_t)((b + m) * 255.0 + 0.5);
}

/* The VT340 default colour map, registers 0..15, as RGB percentages.
 * Source: VT330/VT340 Programmer Reference Manual volume 1, the default
 * colour map table in the sixel chapter -- the same sixteen values libsixel
 * ships. They are kept as percentages so the 0..100 to 0..255 scaling is the
 * single rounding step, exactly as for a `#Pc;2;...` definition. */
struct DefaultColor {
    uint8_t r, g, b;
};
static const DefaultColor default_palette[16] = {
    {0, 0, 0},    /*  0  black          */
    {20, 20, 80}, /*  1  blue           */
    {80, 13, 13}, /*  2  red            */
    {20, 80, 20}, /*  3  green          */
    {80, 20, 80}, /*  4  magenta        */
    {20, 80, 80}, /*  5  cyan           */
    {80, 80, 20}, /*  6  yellow         */
    {53, 53, 53}, /*  7  gray 50%       */
    {26, 26, 26}, /*  8  gray 25%       */
    {33, 33, 60}, /*  9  blue, 50% int. */
    {60, 26, 26}, /* 10  red, 50% int.  */
    {33, 60, 33}, /* 11  green, 50%     */
    {60, 33, 60}, /* 12  magenta, 50%   */
    {33, 60, 60}, /* 13  cyan, 50%      */
    {60, 60, 33}, /* 14  yellow, 50%    */
    {80, 80, 80}, /* 15  gray 75%       */
};

struct Decoder {
    zigstd::Allocator alloc;
    Options opts;

    uint8_t *pix; /* capacity cap_w * cap_h * 4 bytes, or null */
    size_t cap_w;
    size_t cap_h;

    /* The extent actually written to, as a width/height (one past the
     * largest coordinate touched). The image is cropped to this unless
     * raster attributes said otherwise. */
    size_t used_w;
    size_t used_h;

    /* Raster attributes, if a `"` sequence supplied usable ones. */
    bool have_raster;
    size_t raster_w;
    size_t raster_h;

    uint32_t palette[num_colors];
    size_t color; /* the current register; always < num_colors */

    size_t x;   /* pixel column of the cursor */
    size_t row; /* sixel row; the band covers y = row*6 .. row*6+5 */

    uint8_t fill[4]; /* what an untouched pixel looks like */

    void init(zigstd::Allocator a, const Options &o) {
        alloc = a;
        opts = o;
        pix = nullptr;
        cap_w = 0;
        cap_h = 0;
        used_w = 0;
        used_h = 0;
        have_raster = false;
        raster_w = 0;
        raster_h = 0;
        color = 0;
        x = 0;
        row = 0;

        for (size_t i = 0; i < 16; i++) {
            palette[i] = packRGBA(pctTo255(default_palette[i].r), pctTo255(default_palette[i].g),
                                  pctTo255(default_palette[i].b), 255);
        }
        /* Ambiguity: the format says nothing about registers beyond the ones
         * the terminal ships defaults for, and DEC hardware had only sixteen.
         * Opaque black is chosen for 16..255 so that selecting a register
         * that was never defined gives a deterministic result instead of
         * whatever a previous image happened to leave there. */
        for (size_t i = 16; i < num_colors; i++) palette[i] = packRGBA(0, 0, 0, 255);

        const uint32_t bg =
            (opts.background_select == BackgroundSelect::paint) ? opts.background : 0u;
        fill[0] = (uint8_t)(bg >> 24);
        fill[1] = (uint8_t)(bg >> 16);
        fill[2] = (uint8_t)(bg >> 8);
        fill[3] = (uint8_t)bg;
    }

    void deinit() {
        if (pix != nullptr) alloc.free(pix, cap_w * cap_h * 4, 1);
        pix = nullptr;
        cap_w = 0;
        cap_h = 0;
    }

    void fillRange(uint8_t *p, size_t count) {
        for (size_t i = 0; i < count; i++) memcpy(p + i * 4, fill, 4);
    }

    /* Is a w x h image within every limit, including the byte count? Checked
     * before any allocation, and again for the geometric overshoot. */
    bool withinLimits(size_t w, size_t h) const {
        if (w > opts.max_width || h > opts.max_height) return false;
        if (w != 0 && h > opts.max_pixels / w) return false;
        /* Overflow guard on the byte count itself, independent of the
         * caller's max_pixels, which they are free to set absurdly high. */
        if (w != 0 && h > (SIZE_MAX / 4) / w) return false;
        return true;
    }

    /* Make sure the buffer holds at least need_w x need_h pixels. Grows
     * geometrically, but never past the limits, and re-checks the overshoot
     * so that doubling cannot be the thing that blows the budget. */
    Error ensure(size_t need_w, size_t need_h) {
        if (need_w <= cap_w && need_h <= cap_h) return Error::none;
        if (!withinLimits(need_w, need_h)) return Error::TooLarge;

        size_t new_w = cap_w;
        size_t new_h = cap_h;
        while (new_w < need_w) {
            new_w = (new_w == 0) ? 64 : (new_w > SIZE_MAX / 2 ? need_w : new_w * 2);
        }
        while (new_h < need_h) {
            new_h = (new_h == 0) ? 6 : (new_h > SIZE_MAX / 2 ? need_h : new_h * 2);
        }
        if (new_w > opts.max_width) new_w = opts.max_width;
        if (new_h > opts.max_height) new_h = opts.max_height;
        /* The overshoot can sit inside the width and height caps and still
         * be outside max_pixels; fall back to exactly what was asked for,
         * which the check above already proved fits. */
        if (!withinLimits(new_w, new_h)) {
            new_w = need_w;
            new_h = need_h;
        }

        uint8_t *np = alloc.alloc(new_w * new_h * 4);
        if (np == nullptr) return Error::OutOfMemory;
        fillRange(np, new_w * new_h);
        for (size_t yy = 0; yy < cap_h; yy++) {
            memcpy(np + yy * new_w * 4, pix + yy * cap_w * 4, cap_w * 4);
        }
        if (pix != nullptr) alloc.free(pix, cap_w * cap_h * 4, 1);
        pix = np;
        cap_w = new_w;
        cap_h = new_h;
        return Error::none;
    }

    void put(size_t px, size_t py, uint32_t rgba) {
        /* Belt and braces: every caller has called ensure() first, but a
         * pixel write is the one thing that must never be wrong. */
        if (px >= cap_w || py >= cap_h) return;
        uint8_t *p = pix + (py * cap_w + px) * 4;
        p[0] = (uint8_t)(rgba >> 24);
        p[1] = (uint8_t)(rgba >> 16);
        p[2] = (uint8_t)(rgba >> 8);
        p[3] = (uint8_t)rgba;
        if (px + 1 > used_w) used_w = px + 1;
        if (py + 1 > used_h) used_h = py + 1;
    }

    /* Write one sixel value `count` times starting at the cursor, advancing
     * the cursor by `count`. Bit 0 is the top pixel of the band, bit 5 the
     * bottom. */
    Error emit(uint8_t value, size_t count) {
        if (count == 0) return Error::none;
        if (x > SIZE_MAX - count) return Error::TooLarge;
        const size_t need_w = x + count;
        if (row > (SIZE_MAX - 6) / 6) return Error::TooLarge;
        const size_t need_h = row * 6 + 6;
        const Error err = ensure(need_w, need_h);
        if (err != Error::none) return err;

        const uint32_t fg = palette[color];
        const uint32_t bg = packRGBA(fill[0], fill[1], fill[2], fill[3]);
        const bool paint_zero = (opts.background_select == BackgroundSelect::paint);
        for (unsigned bit = 0; bit < 6; bit++) {
            const bool on = (value & (1u << bit)) != 0;
            if (!on && !paint_zero) continue;
            const uint32_t rgba = on ? fg : bg;
            const size_t py = row * 6 + bit;
            for (size_t i = 0; i < count; i++) put(x + i, py, rgba);
        }
        x = need_w;
        return Error::none;
    }

    void selectColor(uint32_t pc) {
        /* Out-of-range register numbers are ignored rather than wrapped.
         * Wrapping would let `#99999` alias a real register, which is a
         * silent surprise; ignoring leaves the previous colour selected. */
        if (pc < num_colors) color = (size_t)pc;
    }

    void defineColor(const uint32_t *p, size_t n) {
        /* p[0] = Pc, p[1] = Pu, p[2..4] = the components. */
        if (n < 5) return;
        const uint32_t pc = p[0];
        if (pc >= num_colors) return; /* ignored, as for selection */
        /* Ambiguity: DEC documents `#Pc;Pu;Px;Py;Pz` as defining a register
         * and `#Pc` as selecting one, without saying whether defining also
         * selects. It does here, because every DEC example and every real
         * encoder defines a register and immediately draws with it without
         * a separate `#Pc`, and because the alternative silently paints with
         * whatever was selected before. The selection happens even when the
         * colour space turns out to be unusable below, so that a malformed
         * definition degrades to the plain selection `#Pc` rather than
         * leaving an unrelated register in force. */
        color = (size_t)pc;
        uint32_t r = 0, g = 0, b = 0;
        if (p[1] == 2) {
            r = pctTo255(p[2]);
            g = pctTo255(p[3]);
            b = pctTo255(p[4]);
        } else if (p[1] == 1) {
            hlsToRGB(p[2], p[3], p[4], &r, &g, &b);
        } else {
            /* A Pu of 0, or anything that is not 1 or 2, is undefined.
             * Ignored: guessing RGB would turn a typo into a wrong colour
             * silently, and the register keeps its previous value. */
            return;
        }
        palette[pc] = packRGBA(r, g, b, 255);
    }
};

/* Read a `;`-separated numeric parameter list starting at `i`. Returns the
 * index of the first byte that is not part of the list, so the caller always
 * makes progress. Values saturate at param_max, missing values are 0, and at
 * most max_params are kept -- any beyond that are still parsed and consumed,
 * just discarded. */
inline size_t parseParams(const char *data, size_t len, size_t i, uint32_t *params,
                          size_t *n_out) {
    size_t n = 0;
    uint32_t cur = 0;
    bool any = false;
    while (i < len) {
        const unsigned char c = (unsigned char)data[i];
        if (c >= '0' && c <= '9') {
            cur = satMulAdd10(cur, (uint32_t)(c - '0'));
            any = true;
            i++;
        } else if (c == ';') {
            if (n < max_params) params[n] = cur;
            n++;
            cur = 0;
            any = true;
            i++;
        } else {
            break;
        }
    }
    if (any) {
        if (n < max_params) params[n] = cur;
        n++;
    }
    *n_out = (n > max_params) ? max_params : n;
    return i;
}

} /* namespace detail */

/* Decode the body of a Sixel DCS -- the bytes between `DCS P1;P2;P3 q` and
 * ST -- into an RGBA8888 image. On success *out owns its pixels and the
 * caller must call out->deinit(alloc). On any failure *out is left zeroed
 * and nothing is leaked. */
inline Error decode(zigstd::Allocator alloc, const char *data, size_t len, const Options &opts,
                    Image *out) {
    out->pixels = nullptr;
    out->width = 0;
    out->height = 0;
    if (data == nullptr) len = 0;

    detail::Decoder d;
    d.init(alloc, opts);

    uint32_t params[detail::max_params];
    size_t nparams = 0;
    Error err = Error::none;

    size_t i = 0;
    while (i < len) {
        const unsigned char c = (unsigned char)data[i];

        if (c >= detail::data_min && c <= detail::data_max) {
            err = d.emit((uint8_t)(c - detail::data_min), 1);
            if (err != Error::none) goto fail;
            i++;
            continue;
        }

        switch (c) {
        case '!': {
            i = detail::parseParams(data, len, i + 1, params, &nparams);
            uint32_t count = (nparams > 0) ? params[0] : 1;
            /* Ambiguity: DEC does not say what `!0` means. One repetition is
             * chosen, which is what libsixel and the VT340 do; dropping the
             * byte entirely would silently shift the rest of the row. */
            if (count == 0) count = 1;
            /* The repeat applies to the next single data byte. If what
             * follows is not a data byte -- including the end of input, the
             * truncated case -- the introducer is dropped and that byte is
             * reprocessed normally by the loop. */
            if (i < len) {
                const unsigned char nc = (unsigned char)data[i];
                if (nc >= detail::data_min && nc <= detail::data_max) {
                    err = d.emit((uint8_t)(nc - detail::data_min), (size_t)count);
                    if (err != Error::none) goto fail;
                    i++;
                }
            }
            break;
        }

        case '#': {
            i = detail::parseParams(data, len, i + 1, params, &nparams);
            if (nparams >= 5) {
                d.defineColor(params, nparams);
            } else if (nparams >= 1) {
                d.selectColor(params[0]);
            }
            /* A bare `#` with no digits selects nothing and is ignored. */
            break;
        }

        case '"': {
            i = detail::parseParams(data, len, i + 1, params, &nparams);
            /* Pan;Pad, params[0] and params[1], are the aspect ratio
             * numerator and denominator. They are parsed and deliberately
             * ignored: this decoder produces square pixels and leaves any
             * aspect correction to the renderer, which is the only thing
             * that knows the cell geometry. */
            if (nparams >= 4) {
                const size_t w = (size_t)params[2];
                const size_t h = (size_t)params[3];
                /* Hostile raster attributes are refused outright rather than
                 * clamped. Clamping would hand back an image whose size the
                 * sender did not ask for and whose contents are a silent
                 * crop; refusing is something a caller can notice and
                 * report. A zero in either field is treated as "no raster",
                 * which is how a sender says it does not know yet. */
                if (w != 0 && h != 0) {
                    if (!d.withinLimits(w, h)) {
                        err = Error::TooLarge;
                        goto fail;
                    }
                    d.have_raster = true;
                    d.raster_w = w;
                    d.raster_h = h;
                }
            }
            break;
        }

        case '$':
            /* Graphics carriage return: back to the left edge of the same
             * sixel row. */
            d.x = 0;
            i++;
            break;

        case '-':
            /* Graphics new line: carriage return and down exactly six
             * pixels, since the band height is fixed. */
            d.x = 0;
            d.row++;
            i++;
            break;

        default:
            /* Everything else -- CR, LF, spaces, stray control bytes, UTF-8
             * debris -- is ignored, not fatal. */
            i++;
            break;
        }
    }

    {
        /* Raster attributes, when present, fix the final size even if the
         * data drew less (pad with the fill colour) or more (crop). Without
         * them the size is whatever was actually drawn. */
        const size_t final_w = d.have_raster ? d.raster_w : d.used_w;
        const size_t final_h = d.have_raster ? d.raster_h : d.used_h;
        if (final_w == 0 || final_h == 0) {
            d.deinit();
            return Error::none;
        }
        if (!d.withinLimits(final_w, final_h)) {
            err = Error::TooLarge;
            goto fail;
        }
        err = d.ensure(final_w, final_h);
        if (err != Error::none) goto fail;

        /* Repack from the capacity stride down to the final stride. The
         * destination row never starts after the source row, so a forward
         * copy is safe; memmove because the two can overlap. */
        if (final_w != d.cap_w) {
            for (size_t yy = 0; yy < final_h; yy++) {
                memmove(d.pix + yy * final_w * 4, d.pix + yy * d.cap_w * 4, final_w * 4);
            }
        }

        const size_t old_bytes = d.cap_w * d.cap_h * 4;
        const size_t new_bytes = final_w * final_h * 4;
        if (new_bytes != old_bytes) {
            /* Shrink so that Image::deinit's width*height*4 is exactly the
             * length the allocator was given. */
            uint8_t *shrunk = d.alloc.realloc<uint8_t>(d.pix, old_bytes, new_bytes);
            if (shrunk == nullptr) {
                err = Error::OutOfMemory;
                goto fail;
            }
            d.pix = shrunk;
        }

        out->pixels = d.pix;
        out->width = final_w;
        out->height = final_h;
        /* Ownership has moved to *out. */
        d.pix = nullptr;
        d.cap_w = 0;
        d.cap_h = 0;
        return Error::none;
    }

fail:
    d.deinit();
    out->pixels = nullptr;
    out->width = 0;
    out->height = 0;
    return err;
}

} /* namespace sixel */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_SIXEL_HPP */
