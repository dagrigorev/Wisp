/* The pieces of Zig's std.unicode the terminal port needs.
 * Derived from Zig lib/std/unicode.zig
 * Copyright (c) Zig contributors — MIT License, see THIRD_PARTY_NOTICES.md
 */

#pragma once
#ifndef WISP_ZIGSTD_UNICODE_HPP
#define WISP_ZIGSTD_UNICODE_HPP

#include <stddef.h>
#include <stdint.h>

namespace wisp {
namespace zigstd {

/* std.unicode.utf8ValidateSlice — well-formed UTF-8, rejecting
 * overlong forms, surrogates and values above U+10FFFF. */
inline bool utf8ValidateSlice(const char *s, size_t len) {
    const uint8_t *p = (const uint8_t *)s;
    size_t i = 0;
    while (i < len) {
        const uint8_t b0 = p[i];
        if (b0 < 0x80) { i += 1; continue; }
        size_t n;
        uint8_t lo = 0x80, hi = 0xBF;
        if (b0 >= 0xC2 && b0 <= 0xDF) n = 2;
        else if (b0 >= 0xE0 && b0 <= 0xEF) {
            n = 3;
            if (b0 == 0xE0) lo = 0xA0;
            if (b0 == 0xED) hi = 0x9F;
        } else if (b0 >= 0xF0 && b0 <= 0xF4) {
            n = 4;
            if (b0 == 0xF0) lo = 0x90;
            if (b0 == 0xF4) hi = 0x8F;
        } else return false;
        if (i + n > len) return false;
        if (p[i + 1] < lo || p[i + 1] > hi) return false;
        for (size_t k = 2; k < n; k++) {
            if (p[i + k] < 0x80 || p[i + k] > 0xBF) return false;
        }
        i += n;
    }
    return true;
}

/* std.unicode.Utf8View: `init` validates the whole slice (false is
 * error.InvalidUtf8) and `nextCodepoint` decodes one scalar at a time. */
struct Utf8View {
    const uint8_t *bytes;
    size_t len;
    size_t i;

    static bool init(const char *s, size_t n, Utf8View *out) {
        if (!utf8ValidateSlice(s, n)) return false;
        out->bytes = (const uint8_t *)s;
        out->len = n;
        out->i = 0;
        return true;
    }

    /* Wisp: `?u21` is the bool return plus `*cp`. */
    bool nextCodepoint(uint32_t *cp) {
        if (i >= len) return false;
        const uint8_t b0 = bytes[i];
        if (b0 < 0x80) {
            *cp = b0;
            i += 1;
        } else if (b0 < 0xE0) {
            *cp = ((uint32_t)(b0 & 0x1F) << 6) | (bytes[i + 1] & 0x3F);
            i += 2;
        } else if (b0 < 0xF0) {
            *cp = ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(bytes[i + 1] & 0x3F) << 6) | (bytes[i + 2] & 0x3F);
            i += 3;
        } else {
            *cp = ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(bytes[i + 1] & 0x3F) << 12) |
                  ((uint32_t)(bytes[i + 2] & 0x3F) << 6) | (bytes[i + 3] & 0x3F);
            i += 4;
        }
        return true;
    }
};

/* std.unicode.utf8Encode: returns the byte count, or 0 for
 * error.Utf8CannotEncodeSurrogateHalf / error.CodepointTooLarge. */
inline size_t utf8Encode(uint32_t c, uint8_t out[4]) {
    if (c < 0x80) {
        out[0] = (uint8_t)c;
        return 1;
    }
    if (c < 0x800) {
        out[0] = (uint8_t)(0xC0 | (c >> 6));
        out[1] = (uint8_t)(0x80 | (c & 0x3F));
        return 2;
    }
    if (c < 0x10000) {
        if (c >= 0xD800 && c <= 0xDFFF) return 0;
        out[0] = (uint8_t)(0xE0 | (c >> 12));
        out[1] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
        out[2] = (uint8_t)(0x80 | (c & 0x3F));
        return 3;
    }
    if (c < 0x110000) {
        out[0] = (uint8_t)(0xF0 | (c >> 18));
        out[1] = (uint8_t)(0x80 | ((c >> 12) & 0x3F));
        out[2] = (uint8_t)(0x80 | ((c >> 6) & 0x3F));
        out[3] = (uint8_t)(0x80 | (c & 0x3F));
        return 4;
    }
    return 0;
}

} /* namespace zigstd */
} /* namespace wisp */

#endif /* WISP_ZIGSTD_UNICODE_HPP */
