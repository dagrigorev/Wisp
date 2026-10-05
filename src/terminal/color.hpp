/* Transliterated from Ghostty src/terminal/color.zig, src/terminal/
 * fraction.zig and src/terminal/x11_color.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * The X11 color names come from rgb.txt, embedded through
 * res/x11_rgb_data.inc (tools/gen_x11_rgb.py), under the MIT/X11 license.
 *
 * TRANSLITERATION, see parser.hpp for the general Zig-to-C++ mapping.
 * Specific to this file:
 *
 *   error{...}!T          a bool (or ColorError) return with an out parameter
 *   ?T                    a bool return with an out parameter
 *   comptime tables       built once on first use by the same logic
 *   *const Palette        a pointer, compared by identity as upstream does
 *   Allocator             new/delete; the tests that exercise allocation
 *                         failure are not portable and say so
 *   @Vector shuffle in    the scalar loop only. Upstream's vector path is a
 *   paletteCvalSlice      speed-up that must produce the same bytes, and its
 *                         test checks exactly that.
 *
 * Comments are upstream's unless marked "Wisp:".
 */

#pragma once
#ifndef WISP_TERMINAL_COLOR_HPP
#define WISP_TERMINAL_COLOR_HPP

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace wisp {
namespace terminal {

/* ─── fraction.zig ───────────────────────────────────────────────────────── */

/* Parsing of decimal fractions in the unit interval [0, 1].
 *
 * Escape sequences that carry fractional parameters (XTerm `rgbi:`
 * color specifications, glyph protocol padding) only ever need plain
 * decimal values such as "0.5". Using `std.fmt.parseFloat` for these
 * pulls the full correctly-rounded float parser and its lookup tables
 * into the binary (~26KB). This implements only the restricted subset
 * we need. */
namespace fraction {

/* Parse a decimal fraction in the range [0, 1] inclusive.
 *
 * Accepts an optional leading `+` or `-` sign followed by decimal
 * digits with an optional decimal point ("0.5", ".5", "1.", "1"); at
 * least one digit is required. Exponent and hexadecimal float syntax
 * are not supported. Returns null for invalid syntax and for any value
 * outside [0, 1] (so negative values other than -0 are rejected).
 *
 * Results are correctly rounded for inputs of up to 15 fractional
 * digits; further digits are validated but ignored, which can move the
 * result by less than 1e-15.
 *
 * Wisp: ?f64 is the bool return plus *out. */
inline bool parse(const char *value, size_t value_len, double *out) {
    const char *v = value;
    size_t v_len = value_len;
    bool negative = false;
    if (v_len > 0) {
        switch (v[0]) {
            case '+':
                v++;
                v_len--;
                break;
            case '-':
                negative = true;
                v++;
                v_len--;
                break;
            default:
                break;
        }
    }

    size_t i = 0;
    double int_part = 0;
    while (i < v_len && v[i] != '.') {
        const char d = v[i];
        if (d < '0' || d > '9') return false;
        int_part = int_part * 10 + (double)(d - '0');
        i += 1;
    }

    size_t digits = i;
    uint64_t frac = 0;
    uint64_t scale = 1;
    if (i < v_len) {
        /* assert(v[i] == '.') */
        i += 1;
        while (i < v_len) {
            const char d = v[i];
            if (d < '0' || d > '9') return false;
            /* Stop accumulating after 15 digits so that frac and scale
             * both stay exactly representable in f64 (and cannot
             * overflow), making the division below round only once. */
            if (scale < 1000000000000000ULL) {
                frac = frac * 10 + (uint64_t)(d - '0');
                scale *= 10;
            }
            digits += 1;
            i += 1;
        }
    }
    if (digits == 0) return false;

    /* frac and scale are both exact in f64 (below 2^53 and a power of
     * ten no greater than 1e15), so this division rounds only once. */
    const double magnitude = int_part + (double)frac / (double)scale;
    const double result = negative ? -magnitude : magnitude;

    /* Range check. Written so that -0 passes and any out-of-range or
     * non-finite value fails. */
    if (!(result >= 0 && result <= 1)) return false;
    *out = result;
    return true;
}

inline bool parse(const char *value, double *out) {
    return parse(value, strlen(value), out);
}

} /* namespace fraction */

/* ─── color.zig: RGB ─────────────────────────────────────────────────────── */

/* Wisp: error{InvalidFormat, Overflow}. */
enum class ColorError : uint8_t { none = 0, InvalidFormat, Overflow };

/* RGB */
struct RGB {
    uint8_t r;
    uint8_t g;
    uint8_t b;

    RGB() : r(0), g(0), b(0) {}
    RGB(uint8_t r_, uint8_t g_, uint8_t b_) : r(r_), g(g_), b(b_) {}

    struct C {
        uint8_t r;
        uint8_t g;
        uint8_t b;
    };

    static RGB fromC(C c) { return RGB(c.r, c.g, c.b); }

    C cval() const {
        C c;
        c.r = r;
        c.g = g;
        c.b = b;
        return c;
    }

    bool eql(const RGB &other) const {
        return r == other.r && g == other.g && b == other.b;
    }

    bool operator==(const RGB &o) const { return eql(o); }
    bool operator!=(const RGB &o) const { return !eql(o); }

    /* Wisp: writer.print into a buffer; returns bytes written. */
    size_t encodeRgb8(char *buf, size_t cap) const {
        const int n = snprintf(buf, cap, "rgb:%02x/%02x/%02x", r, g, b);
        return n > 0 ? (size_t)n : 0;
    }

    size_t encodeRgb16(char *buf, size_t cap) const {
        const int n = snprintf(buf, cap, "rgb:%04x/%04x/%04x",
                               (unsigned)((uint16_t)r * 257),
                               (unsigned)((uint16_t)g * 257),
                               (unsigned)((uint16_t)b * 257));
        return n > 0 ? (size_t)n : 0;
    }

    /* Calculates the contrast ratio between two colors. The contrast
     * ration is a value between 1 and 21 where 1 is the lowest contrast
     * and 21 is the highest contrast.
     *
     * https://www.w3.org/TR/WCAG20/#contrast-ratiodef */
    double contrast(const RGB &other) const {
        /* pair[0] = lighter, pair[1] = darker */
        double pair[2];
        const double self_lum = luminance();
        const double other_lum = other.luminance();
        if (self_lum > other_lum) {
            pair[0] = self_lum;
            pair[1] = other_lum;
        } else {
            pair[0] = other_lum;
            pair[1] = self_lum;
        }

        return (pair[0] + 0.05) / (pair[1] + 0.05);
    }

    /* Calculates luminance based on the W3C formula. This returns a
     * normalized value between 0 and 1 where 0 is black and 1 is white.
     *
     * https://www.w3.org/TR/WCAG20/#relativeluminancedef */
    double luminance() const {
        const double r_lum = componentLuminance(r);
        const double g_lum = componentLuminance(g);
        const double b_lum = componentLuminance(b);
        return 0.2126 * r_lum + 0.7152 * g_lum + 0.0722 * b_lum;
    }

    /* Calculates "perceived luminance" which is better for determining
     * light vs dark.
     *
     * Source: https://www.w3.org/TR/AERT/#color-contrast */
    double perceivedLuminance() const {
        const double r_f64 = (double)r;
        const double g_f64 = (double)g;
        const double b_f64 = (double)b;
        return 0.299 * (r_f64 / 255) + 0.587 * (g_f64 / 255) +
               0.114 * (b_f64 / 255);
    }

    /* Parse a color specification. See the definition below. */
    static ColorError parse(const char *value, size_t len, RGB *out);
    static ColorError parse(const char *value, RGB *out) {
        return parse(value, strlen(value), out);
    }

    /* Parse a color from a floating point intensity value.
     *
     * The value should be between 0.0 and 1.0, inclusive. */
    static ColorError fromIntensity(const char *value, size_t len, uint8_t *out) {
        double i;
        if (!fraction::parse(value, len, &i)) return ColorError::InvalidFormat;
        *out = (uint8_t)(i * 255);
        return ColorError::none;
    }

    /* Parse a color from a string of hexadecimal digits
     *
     * The string can contain 1, 2, 3, or 4 characters and represents the
     * color value scaled in 4, 8, 12, or 16 bits, respectively. */
    static ColorError fromHex(const char *value, size_t len, uint8_t *out) {
        if (len == 0 || len > 4) return ColorError::InvalidFormat;

        /* std.fmt.parseUnsigned(u16, value, 16) */
        uint32_t color = 0;
        for (size_t i = 0; i < len; i++) {
            const char c = value[i];
            uint32_t d;
            if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
            else return ColorError::InvalidFormat;
            color = color * 16 + d;
        }

        size_t divisor = 0;
        switch (len) {
            case 1: divisor = 0xF; break;
            case 2: divisor = 0xFF; break;
            case 3: divisor = 0xFFF; break;
            case 4: divisor = 0xFFFF; break;
        }

        *out = (uint8_t)((size_t)color * 255 / divisor);
        return ColorError::none;
    }

private:
    /* Calculates single-component luminance based on the W3C formula.
     *
     * Expects sRGB color space which at the time of writing we don't
     * generally use but it's a good enough approximation until we fix that.
     * https://www.w3.org/TR/WCAG20/#relativeluminancedef */
    static double componentLuminance(uint8_t c) {
        const double c_f64 = (double)c;
        const double normalized = c_f64 / 255;
        if (normalized <= 0.03928) return normalized / 12.92;
        return pow((normalized + 0.055) / 1.055, 2.4);
    }
};

/* ─── x11_color.zig ──────────────────────────────────────────────────────── */

namespace x11_color {

/* A single X11 color entry. */
struct Entry {
    /* Color name. Null-terminated so it can be exposed through the C API
     * without runtime allocation. */
    const char *name;
    size_t      name_len;
    RGB         color;
};

/* This is the rgb.txt file from the X11 project. This was last sourced
 * from this location: https://gitlab.freedesktop.org/xorg/app/rgb
 * This data is licensed under the MIT/X11 license while this Zig file is
 * licensed under the same license as Ghostty. */
static const char data[] =
#include "res/x11_rgb_data.inc"
    ;

/* Wisp: entriesArray, run once on first use rather than at compile time. */
struct Table {
    Entry entries[1024];
    char  names[sizeof(data)];
    size_t len;
};

inline uint8_t parse_u8_trimmed(const char *s, size_t len) {
    /* std.fmt.parseInt(u8, std.mem.trim(u8, line[a..b], " "), 10) */
    size_t a = 0, b = len;
    while (a < b && s[a] == ' ') a++;
    while (b > a && s[b - 1] == ' ') b--;
    unsigned v = 0;
    for (size_t i = a; i < b; i++) v = v * 10 + (unsigned)(s[i] - '0');
    return (uint8_t)v;
}

inline const Table &table() {
    static Table t;
    static bool built = false;
    if (built) return t;

    /* Parse the line. This is not very robust parsing, because we expect
     * a very exact format for rgb.txt. However, this is all done at comptime
     * so if our data is bad, we should hopefully get an error here or one
     * of our unit tests will catch it. */
    size_t names_used = 0;
    t.len = 0;
    const char *p = data;
    const char *end = data + sizeof(data) - 1;
    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        const char *raw_end = nl ? nl : end;
        size_t line_len = (size_t)(raw_end - p);

        /* Trim \r so this works with both LF and CRLF line endings,
         * since git may convert rgb.txt to CRLF on Windows checkouts. */
        if (line_len > 0 && p[line_len - 1] == '\r') line_len--;

        if (line_len > 0) {
            Entry &e = t.entries[t.len];
            const uint8_t r = parse_u8_trimmed(p + 0, 3);
            const uint8_t g = parse_u8_trimmed(p + 4, 3);
            const uint8_t b = parse_u8_trimmed(p + 8, 3);

            /* std.mem.trim(u8, line[12..], " \t") */
            size_t na = 12, nb = line_len;
            while (na < nb && (p[na] == ' ' || p[na] == '\t')) na++;
            while (nb > na && (p[nb - 1] == ' ' || p[nb - 1] == '\t')) nb--;

            char *name = t.names + names_used;
            memcpy(name, p + na, nb - na);
            name[nb - na] = '\0';
            names_used += nb - na + 1;

            e.name = name;
            e.name_len = nb - na;
            e.color = RGB(r, g, b);
            t.len += 1;
        }

        p = nl ? nl + 1 : end;
    }

    built = true;
    return t;
}

/* All X11 colors in rgb.txt file order. */
inline const Entry *entries() { return table().entries; }
inline size_t entries_len() { return table().len; }

/* The map of all available X11 colors.
 *
 * Wisp: std.StaticStringMapWithEql(RGB, eqlAsciiIgnoreCase). Lookup is a
 * case-insensitive comparison over the entries; the first match wins, which
 * is the only observable behaviour since equal names have equal colors. */
inline bool map_get(const char *key, size_t key_len, RGB *out) {
    const Table &t = table();
    for (size_t i = 0; i < t.len; i++) {
        const Entry &e = t.entries[i];
        if (e.name_len != key_len) continue;
        bool same = true;
        for (size_t k = 0; k < key_len; k++) {
            char a = e.name[k], b = key[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) {
                same = false;
                break;
            }
        }
        if (same) {
            *out = e.color;
            return true;
        }
    }
    return false;
}

inline bool map_get(const char *key, RGB *out) {
    return map_get(key, strlen(key), out);
}

} /* namespace x11_color */

/* Parse a color specification.
 *
 * Leading and trailing spaces and tabs are ignored.
 *
 * Any of the following forms are accepted:
 *
 * 1. rgb:<red>/<green>/<blue>
 *
 *    <red>, <green>, <blue> := h | hh | hhh | hhhh
 *
 *    where `h` is a single hexadecimal digit.
 *
 * 2. rgbi:<red>/<green>/<blue>
 *
 *    where <red>, <green>, and <blue> are floating point values between
 *    0.0 and 1.0 (inclusive).
 *
 * 3. #rgb, #rrggbb, rgb, rrggbb, #rrrgggbbb, #rrrrggggbbbb
 *
 *    where `r`, `g`, and `b` are hexadecimal digits. The forms with
 *    a leading # specify a color with 4, 8, 12, and 16 bits of
 *    precision per color channel. The forms without a leading # are
 *    accepted for compatibility with Ghostty config/theme color values.
 *
 * 4. X11 color names */
inline ColorError RGB::parse(const char *value, size_t value_len, RGB *out) {
    /* std.mem.trim(u8, value, " \t") */
    size_t a = 0, z = value_len;
    while (a < z && (value[a] == ' ' || value[a] == '\t')) a++;
    while (z > a && (value[z - 1] == ' ' || value[z - 1] == '\t')) z--;
    const char *input = value + a;
    const size_t len = z - a;

    if (len == 0) return ColorError::InvalidFormat;

#define WISP_HEX(dst, from, to)                                               \
    do {                                                                     \
        const ColorError e_ = RGB::fromHex(input + (from), (to) - (from), &dst); \
        if (e_ != ColorError::none) return e_;                               \
    } while (0)

    if (input[0] == '#') {
        RGB c;
        switch (len) {
            case 4:
                WISP_HEX(c.r, 1, 2); WISP_HEX(c.g, 2, 3); WISP_HEX(c.b, 3, 4);
                break;
            case 7:
                WISP_HEX(c.r, 1, 3); WISP_HEX(c.g, 3, 5); WISP_HEX(c.b, 5, 7);
                break;
            case 10:
                WISP_HEX(c.r, 1, 4); WISP_HEX(c.g, 4, 7); WISP_HEX(c.b, 7, 10);
                break;
            case 13:
                WISP_HEX(c.r, 1, 5); WISP_HEX(c.g, 5, 9); WISP_HEX(c.b, 9, 13);
                break;
            default:
                return ColorError::InvalidFormat;
        }
        *out = c;
        return ColorError::none;
    }

    /* Check for X11 named colors. We allow whitespace around the edges. */
    {
        RGB named;
        if (x11_color::map_get(input, len, &named)) {
            *out = named;
            return ColorError::none;
        }
    }

    switch (len) {
        case 3: {
            RGB c;
            WISP_HEX(c.r, 0, 1); WISP_HEX(c.g, 1, 2); WISP_HEX(c.b, 2, 3);
            *out = c;
            return ColorError::none;
        }
        case 6: {
            RGB c;
            WISP_HEX(c.r, 0, 2); WISP_HEX(c.g, 2, 4); WISP_HEX(c.b, 4, 6);
            *out = c;
            return ColorError::none;
        }
        default:
            break;
    }
#undef WISP_HEX

    if (len < sizeof("rgb:a/a/a") - 1 || memcmp(input, "rgb", 3) != 0) {
        return ColorError::InvalidFormat;
    }

    size_t i = 3;

    bool use_intensity = false;
    if (input[i] == 'i') {
        i += 1;
        use_intensity = true;
    }

    if (input[i] != ':') return ColorError::InvalidFormat;

    i += 1;

    uint8_t channels[3];
    for (int k = 0; k < 2; k++) {
        /* std.mem.indexOfScalarPos(u8, input, i, '/') */
        size_t end = (size_t)-1;
        for (size_t j = i; j < len; j++) {
            if (input[j] == '/') {
                end = j;
                break;
            }
        }
        if (end == (size_t)-1) return ColorError::InvalidFormat;

        const size_t slice_len = end - i;
        const ColorError e = use_intensity
                                 ? RGB::fromIntensity(input + i, slice_len, &channels[k])
                                 : RGB::fromHex(input + i, slice_len, &channels[k]);
        if (e != ColorError::none) return e;
        i += slice_len + 1;
    }

    {
        const ColorError e = use_intensity
                                 ? RGB::fromIntensity(input + i, len - i, &channels[2])
                                 : RGB::fromHex(input + i, len - i, &channels[2]);
        if (e != ColorError::none) return e;
    }

    *out = RGB(channels[0], channels[1], channels[2]);
    return ColorError::none;
}

/* ─── color.zig: Name, Special, Dynamic ──────────────────────────────────── */

/* Color names in the standard 8 or 16 color palette.
 *
 * Wisp: enum(u8) with `_`, so any u8 is a valid value. */
enum class Name : uint8_t {
    black = 0,
    red = 1,
    green = 2,
    yellow = 3,
    blue = 4,
    magenta = 5,
    cyan = 6,
    white = 7,

    bright_black = 8,
    bright_red = 9,
    bright_green = 10,
    bright_yellow = 11,
    bright_blue = 12,
    bright_magenta = 13,
    bright_cyan = 14,
    bright_white = 15,

    /* Remainders are valid unnamed values in the 256 color palette. */
};

/* Default colors for tagged values.
 *
 * Wisp: error{NoDefaultValue}!RGB is the bool return plus *out. */
inline bool name_default(Name self, RGB *out) {
    switch (self) {
        case Name::black: *out = RGB(0x1D, 0x1F, 0x21); return true;
        case Name::red: *out = RGB(0xCC, 0x66, 0x66); return true;
        case Name::green: *out = RGB(0xB5, 0xBD, 0x68); return true;
        case Name::yellow: *out = RGB(0xF0, 0xC6, 0x74); return true;
        case Name::blue: *out = RGB(0x81, 0xA2, 0xBE); return true;
        case Name::magenta: *out = RGB(0xB2, 0x94, 0xBB); return true;
        case Name::cyan: *out = RGB(0x8A, 0xBE, 0xB7); return true;
        case Name::white: *out = RGB(0xC5, 0xC8, 0xC6); return true;

        case Name::bright_black: *out = RGB(0x66, 0x66, 0x66); return true;
        case Name::bright_red: *out = RGB(0xD5, 0x4E, 0x53); return true;
        case Name::bright_green: *out = RGB(0xB9, 0xCA, 0x4A); return true;
        case Name::bright_yellow: *out = RGB(0xE7, 0xC5, 0x47); return true;
        case Name::bright_blue: *out = RGB(0x7A, 0xA6, 0xDA); return true;
        case Name::bright_magenta: *out = RGB(0xC3, 0x97, 0xD8); return true;
        case Name::bright_cyan: *out = RGB(0x70, 0xC0, 0xB1); return true;
        case Name::bright_white: *out = RGB(0xEA, 0xEA, 0xEA); return true;

        default: return false;
    }
}

/* The "special colors" as denoted by xterm. These can be set via
 * OSC 5 or via OSC 4 by adding the palette length to it.
 *
 * https://invisible-island.net/xterm/ctlseqs/ctlseqs.html */
enum class Special : uint8_t {
    bold = 0,
    underline = 1,
    blink = 2,
    reverse = 3,
    italic = 4,
};

inline uint16_t special_osc4(Special self) {
    /* "The special colors can also be set by adding the maximum
     * number of colors (e.g., 88 or 256) to these codes in an
     * OSC 4  control" - xterm ctlseqs */
    const uint16_t max = 256;
    return (uint16_t)((uint16_t)self + max);
}

/* The "dynamic colors" as denoted by xterm. These can be set via
 * OSC 10 through 19. */
enum class Dynamic : uint8_t {
    foreground = 10,
    background = 11,
    cursor = 12,
    pointer_foreground = 13,
    pointer_background = 14,
    tektronix_foreground = 15,
    tektronix_background = 16,
    highlight_background = 17,
    tektronix_cursor = 18,
    highlight_foreground = 19,
};

/* The next dynamic color sequentially. This is required because
 * specifying colors sequentially without their index will automatically
 * use the next dynamic color.
 *
 * "Each successive parameter changes the next color in the list.  The
 * value of Ps tells the starting point in the list."
 *
 * Wisp: ?Dynamic is the bool return plus *out. */
inline bool dynamic_next(Dynamic self, Dynamic *out) {
    const int n = (int)self + 1;
    if (n < 10 || n > 19) return false;
    *out = (Dynamic)n;
    return true;
}

/* ─── color.zig: palettes ────────────────────────────────────────────────── */

/* Palette is the 256 color palette. */
struct Palette {
    RGB colors[256];

    RGB &operator[](size_t i) { return colors[i]; }
    const RGB &operator[](size_t i) const { return colors[i]; }

    bool eql(const Palette &o) const {
        for (size_t i = 0; i < 256; i++) {
            if (!colors[i].eql(o.colors[i])) return false;
        }
        return true;
    }
};

static const size_t PALETTE_SIZE = 256;

/* The default palette. Wisp: `default` is a keyword, so this is a function
 * returning the one shared instance — its address is the identity upstream's
 * DynamicPalette compares against. */
inline const Palette &default_palette() {
    static Palette result;
    static bool built = false;
    if (built) return result;

    /* Named values */
    uint8_t i = 0;
    while (i < 16) {
        name_default((Name)i, &result[i]);
        i += 1;
    }

    /* Cube */
    /* assert(i == 16) */
    for (uint8_t r = 0; r < 6; r++) {
        for (uint8_t g = 0; g < 6; g++) {
            for (uint8_t b = 0; b < 6; b++) {
                result[i] = RGB((uint8_t)(r == 0 ? 0 : (r * 40 + 55)),
                                (uint8_t)(g == 0 ? 0 : (g * 40 + 55)),
                                (uint8_t)(b == 0 ? 0 : (b * 40 + 55)));
                i += 1;
            }
        }
    }

    /* Gray ramp */
    /* assert(i == 232) */
    while (i > 0) {
        const uint8_t value = (uint8_t)(((i - 232) * 10) + 8);
        result[i] = RGB(value, value, value);
        i = (uint8_t)(i + 1);   /* +%= */
    }

    built = true;
    return result;
}

/* A parsed palette entry from Ghostty's config "N=COLOR" syntax. */
struct PaletteEntry {
    uint8_t index;
    RGB     color;
};

/* Wisp: std.fmt.parseInt(u8, s, 0). Base 0 reads a 0x, 0o or 0b prefix
 * (either case) and otherwise decimal. An optional leading '+' is accepted.
 * Zig also accepts '_' digit separators and a '-' sign on "-0"; neither
 * appears in upstream's tests and neither is reproduced here. */
inline ColorError parse_int_u8_base0(const char *s, size_t len, uint8_t *out) {
    size_t i = 0;
    if (i < len && s[i] == '+') i++;
    if (i >= len) return ColorError::InvalidFormat;

    unsigned base = 10;
    if (len - i >= 2 && s[i] == '0') {
        const char p = s[i + 1];
        if (p == 'x' || p == 'X') { base = 16; i += 2; }
        else if (p == 'o' || p == 'O') { base = 8; i += 2; }
        else if (p == 'b' || p == 'B') { base = 2; i += 2; }
    }
    if (i >= len) return ColorError::InvalidFormat;

    unsigned v = 0;
    for (; i < len; i++) {
        const char c = s[i];
        unsigned d;
        if (c >= '0' && c <= '9') d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'z') d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'Z') d = (unsigned)(c - 'A' + 10);
        else return ColorError::InvalidFormat;
        if (d >= base) return ColorError::InvalidFormat;
        v = v * base + d;
        if (v > 255) return ColorError::Overflow;
    }
    *out = (uint8_t)v;
    return ColorError::none;
}

/* Parse a palette entry in Ghostty config syntax: "N=COLOR" where N is
 * a palette index 0-255 (decimal, or 0x/0o/0b-prefixed per Zig's
 * parseInt base-0 rules) and COLOR is anything RGB.parse accepts.
 * Whitespace (spaces/tabs) around N and COLOR is ignored. */
inline ColorError parsePaletteEntry(const char *value, size_t len,
                                    PaletteEntry *out) {
    const char *eq = (const char *)memchr(value, '=', len);
    if (!eq) return ColorError::InvalidFormat;
    const size_t eql_idx = (size_t)(eq - value);

    /* std.mem.trim(u8, value[0..eql_idx], " \t") */
    size_t a = 0, b = eql_idx;
    while (a < b && (value[a] == ' ' || value[a] == '\t')) a++;
    while (b > a && (value[b - 1] == ' ' || value[b - 1] == '\t')) b--;

    uint8_t index;
    const ColorError ie = parse_int_u8_base0(value + a, b - a, &index);
    if (ie != ColorError::none) return ie;

    RGB rgb;
    const ColorError ce = RGB::parse(value + eql_idx + 1, len - eql_idx - 1, &rgb);
    if (ce != ColorError::none) return ce;

    out->index = index;
    out->color = rgb;
    return ColorError::none;
}

inline ColorError parsePaletteEntry(const char *value, PaletteEntry *out) {
    return parsePaletteEntry(value, strlen(value), out);
}

/* C-compatible palette type using the extern RGB struct. */
struct PaletteC {
    RGB::C colors[256];
};

/* Convert a slice of palette entries to their C representation.
 * Asserts that both slices are the same length.
 *
 * Wisp: the scalar loop only; see the header. */
inline void paletteCvalSlice(const RGB *src, RGB::C *dst, size_t len) {
    for (size_t i = 0; i < len; i++) dst[i] = src[i].cval();
}

/* Convert a Palette to a PaletteC. */
inline PaletteC paletteCval(const Palette &palette) {
    PaletteC result;
    paletteCvalSlice(palette.colors, result.colors, 256);
    return result;
}

/* Convert a PaletteC to a Palette. */
inline Palette paletteZval(const PaletteC &palette) {
    Palette result;
    for (size_t i = 0; i < 256; i++) result[i] = RGB::fromC(palette.colors[i]);
    return result;
}

/* Mask that can be used to set which palette indexes were set.
 *
 * Wisp: std.StaticBitSet(256). */
struct PaletteMask {
    uint64_t words[4];

    PaletteMask() { words[0] = words[1] = words[2] = words[3] = 0; }
    static PaletteMask initEmpty() { return PaletteMask(); }

    void set(size_t i) { words[i >> 6] |= (uint64_t)1 << (i & 63); }
    void unset(size_t i) { words[i >> 6] &= ~((uint64_t)1 << (i & 63)); }
    bool isSet(size_t i) const { return (words[i >> 6] >> (i & 63)) & 1; }

    size_t count() const {
        size_t n = 0;
        for (int w = 0; w < 4; w++) {
            for (uint64_t m = words[w]; m; m &= m - 1) n++;
        }
        return n;
    }
};

/* ─── color.zig: LAB ─────────────────────────────────────────────────────── */

/* LAB color space */
struct LAB {
    float l;
    float a;
    float b;

    /* RGB to LAB */
    static LAB fromRgb(RGB rgb) {
        /* Step 1: Normalize sRGB channels from [0, 255] to [0.0, 1.0]. */
        float r = (float)rgb.r / 255.0f;
        float g = (float)rgb.g / 255.0f;
        float b = (float)rgb.b / 255.0f;

        /* Step 2: Apply the inverse sRGB companding (gamma correction) to
         * convert from sRGB to linear RGB. The sRGB transfer function has
         * two segments: a linear portion for small values and a power curve
         * for the rest. */
        r = r > 0.04045f ? powf((r + 0.055f) / 1.055f, 2.4f) : r / 12.92f;
        g = g > 0.04045f ? powf((g + 0.055f) / 1.055f, 2.4f) : g / 12.92f;
        b = b > 0.04045f ? powf((b + 0.055f) / 1.055f, 2.4f) : b / 12.92f;

        /* Step 3: Convert linear RGB to CIE XYZ using the sRGB to XYZ
         * transformation matrix (D65 illuminant). The X and Z values are
         * normalized by the D65 white point reference values (Xn=0.95047,
         * Zn=1.08883; Yn=1.0 is implicit). */
        float x = (r * 0.4124564f + g * 0.3575761f + b * 0.1804375f) / 0.95047f;
        float y = r * 0.2126729f + g * 0.7151522f + b * 0.0721750f;
        float z = (r * 0.0193339f + g * 0.1191920f + b * 0.9503041f) / 1.08883f;

        /* Step 4: Apply the CIE f(t) nonlinear transform to each XYZ
         * component. Above the threshold (epsilon ≈ 0.008856) the cube
         * root is used; below it, a linear approximation avoids numerical
         * instability near zero. */
        x = x > 0.008856f ? cbrtf(x) : 7.787f * x + 16.0f / 116.0f;
        y = y > 0.008856f ? cbrtf(y) : 7.787f * y + 16.0f / 116.0f;
        z = z > 0.008856f ? cbrtf(z) : 7.787f * z + 16.0f / 116.0f;

        /* Step 5: Compute the final CIELAB values from the transformed XYZ.
         * L* is lightness (0–100), a* is green–red, b* is blue–yellow. */
        LAB out;
        out.l = 116.0f * y - 16.0f;
        out.a = 500.0f * (x - y);
        out.b = 200.0f * (y - z);
        return out;
    }

    /* LAB to RGB */
    RGB toRgb() const {
        /* Step 1: Recover the intermediate f(Y), f(X), f(Z) values from
         * L*a*b* by inverting the CIELAB formulas. */
        const float y = (l + 16.0f) / 116.0f;
        const float x = a / 500.0f + y;
        const float z = y - b / 200.0f;

        /* Step 2: Apply the inverse CIE f(t) transform to get back to
         * XYZ. Above epsilon (≈0.008856) the cube is used; below it the
         * linear segment is inverted. Results are then scaled by the D65
         * white point reference values (Xn=0.95047, Zn=1.08883; Yn=1.0). */
        const float x3 = x * x * x;
        const float y3 = y * y * y;
        const float z3 = z * z * z;
        const float xf = (x3 > 0.008856f ? x3 : (x - 16.0f / 116.0f) / 7.787f) * 0.95047f;
        const float yf = y3 > 0.008856f ? y3 : (y - 16.0f / 116.0f) / 7.787f;
        const float zf = (z3 > 0.008856f ? z3 : (z - 16.0f / 116.0f) / 7.787f) * 1.08883f;

        /* Step 3: Convert CIE XYZ back to linear RGB using the XYZ to sRGB
         * matrix (inverse of the sRGB to XYZ matrix, D65 illuminant). */
        float r = xf * 3.2404542f - yf * 1.5371385f - zf * 0.4985314f;
        float g = -xf * 0.9692660f + yf * 1.8760108f + zf * 0.0415560f;
        float bb = xf * 0.0556434f - yf * 0.2040259f + zf * 1.0572252f;

        /* Step 4: Apply sRGB companding (gamma correction) to convert from
         * linear RGB back to sRGB. This is the forward sRGB transfer
         * function with the same two-segment split as the inverse. */
        r = r > 0.0031308f ? 1.055f * powf(r, 1.0f / 2.4f) - 0.055f : 12.92f * r;
        g = g > 0.0031308f ? 1.055f * powf(g, 1.0f / 2.4f) - 0.055f : 12.92f * g;
        bb = bb > 0.0031308f ? 1.055f * powf(bb, 1.0f / 2.4f) - 0.055f : 12.92f * bb;

        /* Step 5: Clamp to [0.0, 1.0], scale to [0, 255], and round to
         * the nearest integer to produce the final 8-bit sRGB values. */
        const auto clamp01 = [](float v) {
            return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        };
        return RGB((uint8_t)(clamp01(r) * 255.0f + 0.5f),
                   (uint8_t)(clamp01(g) * 255.0f + 0.5f),
                   (uint8_t)(clamp01(bb) * 255.0f + 0.5f));
    }

    /* Linearly interpolate between two LAB colors component-wise.
     * `t` is the interpolation factor in [0, 1]: t=0 returns `a`,
     * t=1 returns `b`, and values in between blend proportionally. */
    static LAB lerp(float t, LAB a_, LAB b_) {
        LAB out;
        out.l = a_.l + t * (b_.l - a_.l);
        out.a = a_.a + t * (b_.a - a_.a);
        out.b = a_.b + t * (b_.b - a_.b);
        return out;
    }
};

/* Generate the 256-color palette from the user's base16 theme colors,
 * terminal background, and terminal foreground.
 *
 * Motivation: The default 256-color palette uses fixed, fully-saturated
 * colors that clash with custom base16 themes, have poor readability in
 * dark shades (the first non-black shade jumps to 37% intensity instead
 * of the expected 20%), and exhibit inconsistent perceived brightness
 * across hues of the same shade (e.g., blue appears darker than green).
 * By generating the extended palette from the user's chosen colors,
 * programs can use the richer 256-color range without requiring their
 * own theme configuration, and light/dark switching works automatically.
 *
 * The 216-color cube (indices 16–231) is built via trilinear
 * interpolation in CIELAB space over the 8 base colors. The base16
 * palette maps to the 8 corners of a 6×6×6 RGB cube as follows:
 *
 *   R=0 edge: bg      → base[1] (red)
 *   R=5 edge: base[6] → fg
 *   G=0 edge: bg/base[6] (via R) → base[2]/base[4] (green/blue via R)
 *   G=5 edge: base[1]/fg (via R) → base[3]/base[5] (yellow/magenta via R)
 *
 * For each R slice, four corner colors (c0–c3) are interpolated along
 * the R axis, then for each G row two edge colors (c4–c5) are
 * interpolated along G, and finally each B cell is interpolated along B
 * to produce the final color. CIELAB interpolation ensures perceptually
 * uniform brightness transitions across different hues.
 *
 * The 24-step grayscale ramp (indices 232–255) is a simple linear
 * interpolation in CIELAB from the background to the foreground,
 * excluding pure black and white (available in the cube at (0,0,0)
 * and (5,5,5)). The interpolation parameter runs from 1/25 to 24/25.
 *
 * Fill `skip` with user-defined color indexes to avoid replacing them.
 *
 * Reference: https://gist.github.com/jake-stewart/0a8ea46159a7da2c808e5be2177e1783 */
inline Palette generate256Color(const Palette &base, const PaletteMask &skip,
                                RGB bg, RGB fg, bool harmonious) {
    /* Convert the background, foreground, and 8 base theme colors into
     * CIELAB space so that all interpolation is perceptually uniform. */
    LAB base8_lab[8] = {
        LAB::fromRgb(bg),
        LAB::fromRgb(base[1]),
        LAB::fromRgb(base[2]),
        LAB::fromRgb(base[3]),
        LAB::fromRgb(base[4]),
        LAB::fromRgb(base[5]),
        LAB::fromRgb(base[6]),
        LAB::fromRgb(fg),
    };

    /* For light themes (where the foreground is darker than the
     * background), the cube's dark-to-light orientation is inverted
     * relative to the base color mapping. When `harmonious` is false,
     * swap bg and fg so the cube still runs from black (16) to
     * white (231). */
    const bool is_light_theme = base8_lab[7].l < base8_lab[0].l;
    const bool invert = is_light_theme && !harmonious;
    if (invert) {
        const LAB tmp = base8_lab[0];
        base8_lab[0] = base8_lab[7];
        base8_lab[7] = tmp;
    }

    /* Start from the base palette so indices 0–15 are preserved as-is. */
    Palette result = base;

    /* Build the 216-color cube (indices 16–231) via trilinear interpolation
     * in CIELAB. The three nested loops correspond to the R, G, and B axes
     * of a 6×6×6 cube. For each R slice, four corner colors (c0–c3) are
     * interpolated along R from the 8 base colors, mapping the cube corners
     * to theme-aware anchors (see doc comment for the mapping). Then for
     * each G row, two edge colors (c4–c5) blend along G, and finally each
     * B cell interpolates along B to produce the final color. */
    size_t idx = 16;
    for (int ri = 0; ri < 6; ri++) {
        /* R-axis corners: blend base colors along the red dimension. */
        const float tr = (float)ri / 5.0f;
        const LAB c0 = LAB::lerp(tr, base8_lab[0], base8_lab[1]);
        const LAB c1 = LAB::lerp(tr, base8_lab[2], base8_lab[3]);
        const LAB c2 = LAB::lerp(tr, base8_lab[4], base8_lab[5]);
        const LAB c3 = LAB::lerp(tr, base8_lab[6], base8_lab[7]);
        for (int gi = 0; gi < 6; gi++) {
            /* G-axis edges: blend the R-interpolated corners along green. */
            const float tg = (float)gi / 5.0f;
            const LAB c4 = LAB::lerp(tg, c0, c1);
            const LAB c5 = LAB::lerp(tg, c2, c3);
            for (int bi = 0; bi < 6; bi++) {
                /* B-axis: final interpolation along blue, then convert back
                 * to RGB. */
                if (!skip.isSet(idx)) {
                    const LAB c6 = LAB::lerp((float)bi / 5.0f, c4, c5);
                    result[idx] = c6.toRgb();
                }

                idx += 1;
            }
        }
    }

    /* Build the 24-step grayscale ramp (indices 232–255) by linearly
     * interpolating in CIELAB from background to foreground. The parameter
     * runs from 1/25 to 24/25, excluding the endpoints which are already
     * available in the cube at (0,0,0) and (5,5,5). */
    for (int i = 0; i < 24; i++) {
        const float t = (float)(i + 1) / 25.0f;
        if (!skip.isSet(idx)) {
            const LAB c = LAB::lerp(t, base8_lab[0], base8_lab[7]);
            result[idx] = c.toRgb();
        }
        idx += 1;
    }

    return result;
}

/* ─── color.zig: DynamicPalette, DynamicRGB ──────────────────────────────── */

/* A palette that can have its colors changed and reset. Purposely built
 * for terminal color operations. */
struct DynamicPalette {
    /* The current palette including any user modifications. */
    Palette current;

    /* The original/default palette values. This points at the shared
     * built-in default palette unless a different default was given
     * (see `changeDefault`), in which case it points at an
     * allocator-owned copy that `deinit` frees. Sharing the built-in
     * default saves a full palette copy for every terminal. */
    const Palette *original;

    /* A bitset where each bit represents whether the corresponding
     * palette index has been modified from its default value. */
    PaletteMask mask;

    /* A dynamic palette using the built-in default palette. This owns
     * no memory, but `deinit` is still safe to call on it. */
    static DynamicPalette default_() {
        DynamicPalette p;
        p.current = default_palette();
        p.original = &default_palette();
        p.mask = PaletteMask::initEmpty();
        return p;
    }

    /* Initialize a dynamic palette with a default palette. The result
     * must be released with `deinit`. No memory is allocated if `def`
     * is the built-in default palette. */
    static DynamicPalette init(const Palette &def) {
        DynamicPalette result = default_();
        result.changeDefault(def);
        return result;
    }

    void deinit() {
        if (Palette *owned = ownedOriginal()) delete owned;
        original = &default_palette();
    }

    /* Set a custom color at the given palette index. */
    void set(uint8_t idx, RGB color) {
        current[idx] = color;
        mask.set(idx);
    }

    /* Reset the color at the given palette index to its original value. */
    void reset(uint8_t idx) {
        current[idx] = (*original)[idx];
        mask.unset(idx);
    }

    /* Reset all colors to their original values. */
    void resetAll() {
        current = *original;
        mask = PaletteMask::initEmpty();
    }

    /* Change the default palette, but preserve the changed values.
     *
     * The built-in default palette is shared rather than copied, so
     * changing back to it releases any owned copy (see `resetDefault`).
     * Any other default is copied into memory owned by this palette,
     * allocated on the first change and reused after that. On allocation
     * failure nothing changes. */
    void changeDefault(const Palette &def) {
        if (def.eql(default_palette())) {
            resetDefault();
            return;
        }

        Palette *owned = ownedOriginal();
        if (!owned) owned = new Palette;
        *owned = def;
        original = owned;
        applyDefault(def);
    }

    /* Change the default palette back to the built-in default palette,
     * preserving the changed values. This releases any owned copy and
     * never allocates, so it is safe to use as the fallback when
     * `changeDefault` fails. */
    void resetDefault() {
        if (Palette *owned = ownedOriginal()) delete owned;
        original = &default_palette();
        applyDefault(default_palette());
    }

private:
    /* Rebuild the current palette from a new default, preserving the
     * changed values. */
    void applyDefault(const Palette &def) {
        /* Fast path, the palette is usually not changed. */
        if (mask.count() == 0) {
            current = def;
            return;
        }

        /* There are usually less set than unset, so iterate over the changed
         * values and override them. */
        Palette next = def;
        for (size_t idx = 0; idx < 256; idx++) {
            if (mask.isSet(idx)) next[idx] = current[idx];
        }
        current = next;
    }

    /* The allocator-owned copy of the original palette, or null if the
     * original is the shared built-in default. */
    Palette *ownedOriginal() const {
        if (original == &default_palette()) return nullptr;

        /* Only the shared built-in default is ever const; anything else
         * is a copy that we allocated ourselves. */
        return const_cast<Palette *>(original);
    }
};

/* RGB value that can be changed and reset. This can also be totally unset
 * in every way, in which case the caller can determine their own ultimate
 * default.
 *
 * Wisp: ?RGB fields are a flag plus the value. */
struct DynamicRGB {
    bool has_override;
    RGB  override_;
    bool has_default;
    RGB  default_;

    static DynamicRGB unset() {
        DynamicRGB d;
        d.has_override = false;
        d.has_default = false;
        return d;
    }

    static DynamicRGB init(RGB def) {
        DynamicRGB d;
        d.has_override = false;
        d.has_default = true;
        d.default_ = def;
        return d;
    }

    bool get(RGB *out) const {
        if (has_override) {
            *out = override_;
            return true;
        }
        if (has_default) {
            *out = default_;
            return true;
        }
        return false;
    }

    void set(RGB color) {
        has_override = true;
        override_ = color;
    }

    void reset() { has_override = false; }
};

} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_COLOR_HPP */
