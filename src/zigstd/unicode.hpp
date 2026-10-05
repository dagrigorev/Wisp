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

} /* namespace zigstd */
} /* namespace wisp */

#endif /* WISP_ZIGSTD_UNICODE_HPP */
