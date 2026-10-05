/* Transliterated from Ghostty src/simd/base64.zig and base64_scalar.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Wisp, differences in shape rather than behavior:
 *   - SIMD paths are their scalar equivalents, as elsewhere in src/simd;
 *     this is upstream's `options.simd == false` build.
 *   - `error{Base64Invalid}![]const u8` is a bool return (true on success)
 *     plus the decoded slice through out parameters.
 *   - log.warn has no sink yet and is a comment.
 */

#pragma once
#ifndef WISP_SIMD_BASE64_HPP
#define WISP_SIMD_BASE64_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace wisp {
namespace simd {
namespace base64 {

/* ─── base64_scalar.zig ────────────────────────────────────────────────── */

/* Copied from Zig 0.14.1 stdlib and commented out the invalid padding
 * scenarios, because Kitty Graphics requires a decoder that doesn't care
 * about invalid padding scenarios. */
struct Base64Decoder {
    static const uint8_t invalid_char = 0xff;
    static const uint32_t invalid_char_tst = 0xff000000u;

    enum class Error : uint8_t {
        none,
        InvalidCharacter,
        InvalidPadding,
        NoSpaceLeft,
    };

    /* e.g. 'A' => 0.
     * `invalid_char` for any value not in the 64 alphabet chars. */
    uint8_t char_to_index[256];
    uint32_t fast_char_to_index[4][256];
    bool has_pad_char;
    uint8_t pad_char;

    static const char *standardAlphabetChars() {
        return "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    }

    static Base64Decoder init(const char *alphabet_chars, bool has_pad, uint8_t pad) {
        Base64Decoder result;
        memset(result.char_to_index, invalid_char, sizeof result.char_to_index);
        for (size_t k = 0; k < 4; k++) {
            for (size_t j = 0; j < 256; j++) result.fast_char_to_index[k][j] = invalid_char_tst;
        }
        result.has_pad_char = has_pad;
        result.pad_char = pad;

        for (size_t i = 0; i < 64; i++) {
            const uint8_t c = (uint8_t)alphabet_chars[i];
            const uint32_t ci = (uint32_t)i;
            result.fast_char_to_index[0][c] = ci << 2;
            result.fast_char_to_index[1][c] = (ci >> 4) | ((ci & 0x0f) << 12);
            result.fast_char_to_index[2][c] = ((ci & 0x3) << 22) | ((ci & 0x3c) << 6);
            result.fast_char_to_index[3][c] = ci << 16;

            result.char_to_index[c] = (uint8_t)i;
        }
        return result;
    }

    /* Return the maximum possible decoded size for a given input length - The
     * actual length may be less if the input includes padding.
     * `InvalidPadding` is returned if the input length is not valid. */
    Error calcSizeUpperBound(size_t source_len, size_t *out) const {
        size_t result = source_len / 4 * 3;
        const size_t leftover = source_len % 4;
        if (has_pad_char) {
            if (leftover % 4 != 0) return Error::InvalidPadding;
        } else {
            if (leftover % 4 == 1) return Error::InvalidPadding;
            result += leftover * 3 / 4;
        }
        *out = result;
        return Error::none;
    }

    /* Return the exact decoded size for a slice.
     * `InvalidPadding` is returned if the input length is not valid. */
    Error calcSizeForSlice(const uint8_t *source, size_t source_len, size_t *out) const {
        size_t result;
        const Error err = calcSizeUpperBound(source_len, &result);
        if (err != Error::none) return err;
        if (has_pad_char) {
            if (source_len >= 1 && source[source_len - 1] == pad_char) result -= 1;
            if (source_len >= 2 && source[source_len - 2] == pad_char) result -= 1;
        }
        *out = result;
        return Error::none;
    }

    /* dest.len must be what you get from ::calcSize.
     * Invalid characters result in `error.InvalidCharacter`.
     * Invalid padding results in `error.InvalidPadding`. */
    Error decode(uint8_t *dest, size_t dest_len, const uint8_t *source, size_t source_len) const {
        if (has_pad_char && source_len % 4 != 0) return Error::InvalidPadding;
        size_t dest_idx = 0;
        size_t fast_src_idx = 0;
        uint16_t acc = 0; /* Wisp: u12 */
        uint8_t acc_len = 0;
        bool have_leftover = false;
        size_t leftover_idx = 0;

        /* Wisp: the u128 bulk path is the u32 path run four times; the
         * observable result is identical. */
        while (fast_src_idx + 4 < source_len && dest_idx + 3 < dest_len) {
            uint32_t bits = fast_char_to_index[0][source[fast_src_idx]];
            bits |= fast_char_to_index[1][source[fast_src_idx + 1]];
            bits |= fast_char_to_index[2][source[fast_src_idx + 2]];
            bits |= fast_char_to_index[3][source[fast_src_idx + 3]];
            if ((bits & invalid_char_tst) != 0) return Error::InvalidCharacter;
            dest[dest_idx + 0] = (uint8_t)(bits);
            dest[dest_idx + 1] = (uint8_t)(bits >> 8);
            dest[dest_idx + 2] = (uint8_t)(bits >> 16);
            dest[dest_idx + 3] = (uint8_t)(bits >> 24);
            fast_src_idx += 4;
            dest_idx += 3;
        }
        for (size_t src_idx = fast_src_idx; src_idx < source_len; src_idx++) {
            const uint8_t c = source[src_idx];
            const uint8_t d = char_to_index[c];
            if (d == invalid_char) {
                if (!has_pad_char || c != pad_char) return Error::InvalidCharacter;
                have_leftover = true;
                leftover_idx = src_idx;
                break;
            }
            acc = (uint16_t)(((acc << 6) + d) & 0xFFF);
            acc_len += 6;
            if (acc_len >= 8) {
                acc_len -= 8;
                dest[dest_idx] = (uint8_t)(acc >> acc_len);
                dest_idx += 1;
            }
        }
        /* if (acc_len > 4 or (acc & (@as(u12, 1) << acc_len) - 1) != 0) {
         *     return error.InvalidPadding;
         * } */
        if (!have_leftover) return Error::none;
        if (has_pad_char) {
            const size_t padding_len = acc_len / 2;
            size_t padding_chars = 0;
            for (size_t i = leftover_idx; i < source_len; i++) {
                const uint8_t c = source[i];
                if (c != pad_char) {
                    return c == invalid_char ? Error::InvalidCharacter : Error::InvalidPadding;
                }
                padding_chars += 1;
            }
            if (padding_chars != padding_len) return Error::InvalidPadding;
        }
        return Error::none;
    }
};

inline const Base64Decoder &scalar_decoder() {
    static const Base64Decoder d = Base64Decoder::init(Base64Decoder::standardAlphabetChars(), false, 0);
    return d;
}

/* Whether c is one of the 64 standard base64 alphabet characters
 * (padding is not part of the alphabet). */
inline bool isAlphabetChar(uint8_t c) {
    return scalar_decoder().char_to_index[c] != Base64Decoder::invalid_char;
}

/* ─── base64.zig ───────────────────────────────────────────────────────── */

/* For non-SIMD enabled builds, we trim the padding from the end of the
 * base64 input in order to get identical output with the SIMD version. */
inline size_t scalarInputLen(const uint8_t *input, size_t len) {
    size_t end = len;
    while (end > 0 && input[end - 1] == '=') end -= 1;
    return end;
}

inline size_t maxLenScalar(const uint8_t *input, size_t len) {
    size_t out;
    if (scalar_decoder().calcSizeForSlice(input, len, &out) != Base64Decoder::Error::none) {
        /* log.warn("failed to calculate base64 size for payload: {}") */
        return 0;
    }
    return out;
}

inline size_t maxLen(const uint8_t *input, size_t len) {
    return maxLenScalar(input, scalarInputLen(input, len));
}

inline bool decodeScalar(const uint8_t *input_raw, size_t input_raw_len, uint8_t *output, size_t output_len,
                         size_t *out_len) {
    const size_t input_len = scalarInputLen(input_raw, input_raw_len);
    const size_t size = maxLenScalar(input_raw, input_len);
    if (size == 0) {
        *out_len = 0;
        return true;
    }
    /* assert(output.len >= size) */
    (void)output_len;
    if (scalar_decoder().decode(output, size, input_raw, input_len) != Base64Decoder::Error::none) return false;
    *out_len = size;
    return true;
}

inline bool decode(const uint8_t *input, size_t len, uint8_t *output, size_t output_len, size_t *out_len) {
    return decodeScalar(input, len, output, output_len, out_len);
}

/* Whether strict decoding requires the input to be padded to a
 * multiple of four bytes (RFC 4648 section 3.2). The Kitty clipboard
 * protocol requires padding; the legacy OSC 52 protocol tolerates a
 * missing-padding tail because it has no way to report errors to the
 * client. */
enum class Padding : uint8_t { required, optional };

/* Decode strict RFC 4648 standard-alphabet base64: characters outside
 * the alphabet (including whitespace) and misplaced padding are
 * errors rather than being skipped, and padding is validated per the
 * given requirement. This is the decoding the Kitty clipboard
 * protocol specifies:
 * https://sw.kovidgoyal.net/kitty/clipboard/#encoding-of-payloads
 *
 * The output must be at least maxLen(input) bytes. */
inline bool decodeStrict(const uint8_t *input, size_t len, uint8_t *output, size_t output_len, Padding padding,
                         size_t *out_len) {
    /* Padding can only be a suffix of at most two bytes; any '='
     * elsewhere is rejected by the underlying decode. */
    size_t pad = 0;
    if (len > 0 && input[len - 1] == '=') pad += 1;
    if (len > 1 && input[len - 2] == '=') pad += 1;

    switch (padding) {
    case Padding::required:
        if (len % 4 != 0) return false;
        break;
    /* Present padding must still complete a four byte group; only
     * fully absent padding is tolerated. A single leftover byte
     * can never carry a decodable value. */
    case Padding::optional:
        if (pad > 0) {
            if (len % 4 != 0) return false;
        } else if (len % 4 == 1) {
            return false;
        }
        break;
    }

    /* The permissive decode already rejects every invalid character
     * except whitespace, which simdutf silently skips. Skipped
     * characters make the decoded length fall short of the exact
     * length the input length implies, so comparing the two rejects
     * whitespace without a separate validation pass over the input. */
    size_t decoded_len = 0;
    if (!decode(input, len, output, output_len, &decoded_len)) return false;
    size_t expected;
    switch (len % 4) {
    case 0: expected = len / 4 * 3 - pad; break;
    case 2: expected = len / 4 * 3 + 1; break;
    case 3: expected = len / 4 * 3 + 2; break;
    default: return false; /* unreachable, rejected above */
    }
    if (decoded_len != expected) return false;
    *out_len = decoded_len;
    return true;
}

/* A streaming strict base64 decoder for one logical payload split
 * across multiple chunks at arbitrary byte boundaries (e.g. the Kitty
 * clipboard protocol's wdata packets): the concatenation of the fed
 * chunks must be valid RFC 4648 standard-alphabet base64.
 *
 * Padding is terminal within a feed: a padded group followed by more
 * data in the same feed is an error. A feed that ends exactly at
 * terminal padding resets the decoder so the next feed starts a
 * fresh stream, exactly like the kitty reference implementation
 * (which resets its aklomp streaming decoder on EOF); this keeps
 * clients that pad each chunk independently working. */
struct Streaming {
    /* Partial group carried between feeds; a group only decodes once
     * all four of its characters have arrived. */
    uint8_t carry[4];
    uint8_t carry_len; /* = 0 */

    Streaming() : carry_len(0) { carry[0] = carry[1] = carry[2] = carry[3] = 0; }

    /* Maximum decoded bytes one feed of input can produce. */
    size_t maxLen(size_t input_len) const { return ((size_t)carry_len + input_len) / 4 * 3; }

    /* Whether c is valid at the given position of a partial group
     * still waiting for its remaining characters: the first two
     * positions must be alphabet characters and only the last two
     * may open the padding suffix. */
    static bool validPartialChar(size_t pos, uint8_t c) {
        return isAlphabetChar(c) || (pos >= 2 && c == '=');
    }

    /* Decode one complete four character group, the only place
     * padding is legal: the last two characters may be '=' ('=' in
     * the second-to-last position requires it in the last). Returns
     * the decoded length and whether the group was padded. */
    static bool group(const uint8_t *g, uint8_t *output, size_t output_len, size_t *out_decoded, bool *out_padded) {
        const bool padded = g[3] == '=';
        size_t decoded_len = 0;
        if (!decodeStrict(g, 4, output, output_len, Padding::required, &decoded_len)) return false;
        *out_decoded = decoded_len;
        *out_padded = padded;
        return true;
    }

    /* Decode the complete groups of input (with any carried bytes
     * prepended) into output, which must be at least maxLen(input)
     * bytes, and carry the remainder for the next feed. */
    bool feed(const uint8_t *input, size_t input_len, uint8_t *output, size_t output_len, size_t *out_len) {
        /* assert(output.len >= self.maxLen(input)) */
        if (input_len == 0) {
            *out_len = 0;
            return true;
        }

        const uint8_t *rem = input;
        size_t rem_len = input_len;
        size_t written = 0;

        /* Complete a carried partial group first. */
        if (carry_len > 0) {
            const size_t want = 4 - (size_t)carry_len;
            const size_t take = want < rem_len ? want : rem_len;
            for (size_t i = 0; i < take; i++) {
                if (!validPartialChar((size_t)carry_len + i, rem[i])) return false;
            }
            memcpy(carry + carry_len, rem, take);
            carry_len = (uint8_t)(carry_len + take);
            rem += take;
            rem_len -= take;
            if (carry_len < 4) {
                *out_len = 0;
                return true;
            }
            carry_len = 0;
            size_t decoded = 0;
            bool padded = false;
            if (!group(carry, output, output_len, &decoded, &padded)) return false;
            written += decoded;
            if (padded) {
                if (rem_len > 0) return false;
                *out_len = written;
                return true;
            }
        }

        /* Bulk-decode all complete groups with the strict single-shot
         * decode, which also enforces that padding only appears as a
         * terminal suffix. Terminal padding must then end the feed. */
        const size_t bulk_len = rem_len - (rem_len % 4);
        size_t bulk_decoded = 0;
        if (!decodeStrict(rem, bulk_len, output + written, output_len - written, Padding::required, &bulk_decoded))
            return false;
        written += bulk_decoded;
        if (bulk_len > 0 && rem[bulk_len - 1] == '=') {
            if (bulk_len != rem_len) return false;
            *out_len = written;
            return true;
        }

        /* Carry the trailing partial group, validated eagerly so
         * garbage is reported on the feed that contains it. */
        const uint8_t *tail = rem + bulk_len;
        const size_t tail_len = rem_len - bulk_len;
        for (size_t pos = 0; pos < tail_len; pos++) {
            if (!validPartialChar(pos, tail[pos])) return false;
        }
        memcpy(carry, tail, tail_len);
        carry_len = (uint8_t)tail_len;
        *out_len = written;
        return true;
    }

    /* Finish the stream: the concatenation of the fed chunks must
     * have formed complete groups, so a carried partial group is an
     * error (the stream was not correctly padded). The decoder is
     * ready for a fresh stream afterwards either way. */
    bool finish() {
        const bool ok = carry_len == 0;
        *this = Streaming();
        return ok;
    }
};

} /* namespace base64 */
} /* namespace simd */
} /* namespace wisp */

#endif /* WISP_SIMD_BASE64_HPP */
