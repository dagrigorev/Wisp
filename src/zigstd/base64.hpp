/* std.base64.standard.Encoder, the pieces the terminal port uses.
 * Derived from Zig lib/std/base64.zig
 * Copyright (c) Zig contributors — MIT License, see THIRD_PARTY_NOTICES.md
 *
 * Wisp: `encodeWriter` appends to a std::string, which is what the port
 * uses in place of *std.Io.Writer, so it cannot fail.
 */

#pragma once
#ifndef WISP_ZIGSTD_BASE64_HPP
#define WISP_ZIGSTD_BASE64_HPP

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace wisp {
namespace zigstd {
namespace base64 {

static const char standard_alphabet_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* std.base64.Base64Encoder.calcSize */
inline size_t calcSize(size_t source_len) { return ((source_len + 2) / 3) * 4; }

/* std.base64.Base64Encoder.encodeWriter */
inline void encodeWriter(std::string *writer, const uint8_t *source, size_t source_len) {
    const char *alphabet = standard_alphabet_chars;
    size_t i = 0;
    while (i + 3 <= source_len) {
        const uint32_t v = ((uint32_t)source[i] << 16) | ((uint32_t)source[i + 1] << 8) | (uint32_t)source[i + 2];
        writer->push_back(alphabet[(v >> 18) & 63]);
        writer->push_back(alphabet[(v >> 12) & 63]);
        writer->push_back(alphabet[(v >> 6) & 63]);
        writer->push_back(alphabet[v & 63]);
        i += 3;
    }
    const size_t rem = source_len - i;
    if (rem == 1) {
        const uint32_t v = (uint32_t)source[i] << 16;
        writer->push_back(alphabet[(v >> 18) & 63]);
        writer->push_back(alphabet[(v >> 12) & 63]);
        writer->push_back('=');
        writer->push_back('=');
    } else if (rem == 2) {
        const uint32_t v = ((uint32_t)source[i] << 16) | ((uint32_t)source[i + 1] << 8);
        writer->push_back(alphabet[(v >> 18) & 63]);
        writer->push_back(alphabet[(v >> 12) & 63]);
        writer->push_back(alphabet[(v >> 6) & 63]);
        writer->push_back('=');
    }
}

inline void encodeWriter(std::string *writer, const char *source, size_t source_len) {
    encodeWriter(writer, (const uint8_t *)source, source_len);
}

} /* namespace base64 */
} /* namespace zigstd */
} /* namespace wisp */

#endif /* WISP_ZIGSTD_BASE64_HPP */
