/* Transliterated from Zig 0.16.0 lib/std/hash.zig (`int`)
 * Copyright (c) Zig contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Only the 16, 32 and 64 bit widths, which have dedicated functions.
 */

#pragma once
#ifndef WISP_ZIGSTD_HASH_INT_HPP
#define WISP_ZIGSTD_HASH_INT_HPP

#include <stdint.h>

namespace wisp {
namespace zigstd {
namespace hash {

/* Source: https://github.com/skeeto/hash-prospector */
inline uint16_t uint16(uint16_t input) {
    uint16_t x = input;
    x = (uint16_t)((x ^ (x >> 7)) * 0x2993u);
    x = (uint16_t)((x ^ (x >> 5)) * 0xe877u);
    x = (uint16_t)((x ^ (x >> 9)) * 0x0235u);
    x = (uint16_t)(x ^ (x >> 10));
    return x;
}

/* Source: https://github.com/skeeto/hash-prospector */
inline uint32_t uint32(uint32_t input) {
    uint32_t x = input;
    x = (x ^ (x >> 17)) * 0xed5ad4bbu;
    x = (x ^ (x >> 11)) * 0xac4c1b51u;
    x = (x ^ (x >> 15)) * 0x31848babu;
    x = x ^ (x >> 14);
    return x;
}

/* Source: https://github.com/jonmaiga/mx3 */
inline uint64_t uint64(uint64_t input) {
    uint64_t x = input;
    const uint64_t c = 0xbea225f9eb34556dull;
    x = (x ^ (x >> 32)) * c;
    x = (x ^ (x >> 29)) * c;
    x = (x ^ (x >> 32)) * c;
    x = x ^ (x >> 29);
    return x;
}

/* Wisp: std.hash.int for the widths that have a dedicated function. */
inline uint16_t int_(uint16_t v) { return uint16(v); }
inline int16_t int_(int16_t v) { return (int16_t)uint16((uint16_t)v); }
inline uint32_t int_(uint32_t v) { return uint32(v); }
inline int32_t int_(int32_t v) { return (int32_t)uint32((uint32_t)v); }
inline uint64_t int_(uint64_t v) { return uint64(v); }
inline int64_t int_(int64_t v) { return (int64_t)uint64((uint64_t)v); }

} /* namespace hash */
} /* namespace zigstd */
} /* namespace wisp */

#endif /* WISP_ZIGSTD_HASH_INT_HPP */
