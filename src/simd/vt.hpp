/* Transliterated from Ghostty src/simd/vt.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: upstream calls vt.cpp (Highway SIMD) when built with simd and the
 * scalar function otherwise; upstream tests require the two to agree. Only
 * the scalar path is transliterated, so this is the simd=false build. The
 * vectorised ASCII loop inside the scalar path is a plain loop here.
 */

#pragma once
#ifndef WISP_SIMD_VT_HPP
#define WISP_SIMD_VT_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace wisp {
namespace simd {
namespace vt {

struct DecodeResult {
    size_t consumed;
    size_t decoded;
};

inline DecodeResult utf8DecodeUntilControlSeqScalar(const uint8_t *input, size_t input_len,
                                                    uint32_t *output) {
    /* Find our escape */
    const uint8_t *esc = (const uint8_t *)memchr(input, 0x1B, input_len);
    const size_t idx = esc ? (size_t)(esc - input) : input_len;
    const uint8_t *decode = input;
    const size_t decode_len = idx;

    /* Go through and decode one item at a time, following the W3C/Unicode
     * "U+FFFD Substitution of Maximal Subparts" algorithm for ill-formed
     * subsequences. */
    size_t decode_offset = 0;
    size_t decode_count = 0;
    while (decode_offset < decode_len) {
        const uint8_t b0 = decode[decode_offset];

        /* ASCII fast path. */
        if (b0 < 0x80) {
            while (decode_offset < decode_len) {
                const uint8_t b = decode[decode_offset];
                if (b >= 0x80) break;
                output[decode_count] = b;
                decode_count += 1;
                decode_offset += 1;
            }
            continue;
        }

        /* Continuation byte (80-BF) or invalid byte (C0-C1, F5-FF)
         * as lead: each is its own maximal subpart → one FFFD per byte. */
        if (b0 < 0xC2 || b0 > 0xF4) {
            output[decode_count] = 0xFFFD;
            decode_count += 1;
            decode_offset += 1;
            continue;
        }

        /* Multi-byte sequence. Only the first continuation byte has a
         * lead-dependent valid range per Unicode Table 3-7; later
         * continuation bytes are always 80-BF. Range validity per
         * Table 3-7 excludes overlong, surrogate, and out-of-range
         * encodings, so a fully valid sequence can be decoded by
         * direct bit assembly with no further checks. */
        const size_t seq_len = b0 < 0xE0 ? 2 : (b0 < 0xF0 ? 3 : 4);
        uint8_t cb1_lo = 0x80, cb1_hi = 0xBF;
        switch (b0) {
            case 0xE0: cb1_lo = 0xA0; cb1_hi = 0xBF; break;
            case 0xED: cb1_lo = 0x80; cb1_hi = 0x9F; break;
            case 0xF0: cb1_lo = 0x90; cb1_hi = 0xBF; break;
            case 0xF4: cb1_lo = 0x80; cb1_hi = 0x8F; break;
            default: break;
        }

        /* Check how many continuation bytes form a valid prefix (the
         * maximal subpart), accumulating codepoint bits as we go. The
         * lead byte contributes its low 7-len bits. */
        uint32_t cp = b0 & ((uint32_t)0x7F >> seq_len);
        size_t valid = 1; /* lead byte is valid */
        while (valid < seq_len) {
            if (decode_offset + valid >= decode_len) {
                /* The sequence is cut off by the end of the decode
                 * region. If the region ends at the true end of the
                 * input then it may be completed by future input, so
                 * stop without consuming these bytes. If the region
                 * was bounded by an ESC then the sequence can never
                 * be completed; the valid-so-far prefix is a maximal
                 * subpart which maps to a single U+FFFD below. */
                if (decode_len == input_len) {
                    DecodeResult r;
                    r.consumed = decode_offset;
                    r.decoded = decode_count;
                    return r;
                }
                break;
            }
            const uint8_t cb = decode[decode_offset + valid];
            const uint8_t lo = valid == 1 ? cb1_lo : 0x80;
            const uint8_t hi = valid == 1 ? cb1_hi : 0xBF;
            if (cb < lo || cb > hi) break;
            cp = (cp << 6) | (cb & 0x3F);
            valid += 1;
        }

        if (valid == seq_len) {
            output[decode_count] = cp;
            decode_count += 1;
            decode_offset += seq_len;
        } else {
            /* Incomplete/ill-formed: the maximal subpart (valid bytes)
             * maps to a single FFFD. */
            output[decode_count] = 0xFFFD;
            decode_count += 1;
            decode_offset += valid;
        }
    }

    DecodeResult r;
    r.consumed = decode_offset;
    r.decoded = decode_count;
    return r;
}

/* assert(output.len >= input.len) */
inline DecodeResult utf8DecodeUntilControlSeq(const uint8_t *input, size_t input_len,
                                              uint32_t *output) {
    return utf8DecodeUntilControlSeqScalar(input, input_len, output);
}

} /* namespace vt */
} /* namespace simd */
} /* namespace wisp */

#endif /* WISP_SIMD_VT_HPP */
