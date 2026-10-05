/* Transliterated from Ghostty src/fastprint.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Fastprint has fast printing routines that are significantly
 * faster than going through std.fmt.
 */

#pragma once
#ifndef WISP_VT_FASTPRINT_HPP
#define WISP_VT_FASTPRINT_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace wisp {
namespace fastprint {

/* Print a decimal type T. The buffer is expected to be large enough so if
 * necessary use comptime with a buffer-too-large to determine your
 * max size needed. Returns the length written.
 *
 * Wisp: the u8 case. */
inline size_t printDecimalU8(char *buf, uint8_t v) {
    if (v >= 100) {
        buf[0] = (char)('0' + v / 100);
        buf[1] = (char)('0' + (v % 100) / 10);
        buf[2] = (char)('0' + v % 10);
        return 3;
    }
    if (v >= 10) {
        buf[0] = (char)('0' + v / 10);
        buf[1] = (char)('0' + v % 10);
        return 2;
    }
    buf[0] = (char)('0' + v);
    return 1;
}

/* Wisp: the u21 case. Maximum u21 is 2097151: at most 7 digits. */
inline size_t printDecimalU21(char *buf, uint32_t v) {
    char tmp[7];
    uint32_t val = v;
    size_t i = sizeof(tmp);
    while (true) {
        i -= 1;
        tmp[i] = (char)('0' + (val % 10));
        val /= 10;
        if (val == 0) break;
    }
    const size_t n = sizeof(tmp) - i;
    memcpy(buf, tmp + i, n);
    return n;
}

} /* namespace fastprint */
} /* namespace wisp */

#endif /* WISP_VT_FASTPRINT_HPP */
