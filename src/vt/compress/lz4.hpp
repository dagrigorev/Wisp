/* Transliterated from Ghostty src/terminal/compress/lz4.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * An allocation-free implementation of the raw LZ4 block format.
 *
 * LZ4 has two relevant layers: the block format describes the compressed
 * bytes, while the frame format adds headers, sizes, checksums, and support
 * for a stream of blocks. Terminal pages already have their own ownership
 * and metadata, so this implements only blocks. In particular, an encoded
 * block does not contain its decompressed size. The caller must store that
 * separately and provide an exactly sized buffer when decoding.
 *
 * A block is a series of sequences. Each non-final sequence has this shape:
 *
 *     token | literal length extensions | literals | offset | match length extensions
 *
 * The token's high nibble contains the literal length and its low nibble
 * contains the match length minus four. A nibble value of 15 means that the
 * length continues in extension bytes at the corresponding point in the
 * sequence. Each extension byte adds to the length; a value of 255 means
 * another byte follows. The literal bytes are copied directly. The two-byte
 * little-endian offset then points backwards in the already decompressed
 * output to the match bytes.
 *
 * The last sequence is special: it contains literals only and ends directly
 * after them. The reference format also requires the last five input bytes to
 * be literals and the final match to begin at least twelve bytes before the
 * end of the input. The compressor observes these restrictions so its output
 * can be consumed by optimized LZ4 decoders which copy in larger units.
 *
 * Compression uses the fast LZ4 strategy: hash each four-byte input sequence
 * and test recent positions as match candidates. Two 16-bit positions fit in
 * each hash-table entry because the format cannot refer further than 64 KiB
 * backwards. Retaining the displaced position recovers useful matches after
 * hash collisions without increasing the fixed 16 KiB workspace. Long runs
 * without matches gradually skip input positions, while matching runs are
 * extended a machine word at a time. The implementation allocates nothing;
 * all input, output, and scratch memory is supplied by the caller.
 *
 * Format reference:
 * https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 * Wisp: error unions return the error enum (none = success) with the value
 * in an out parameter. Little-endian target assumed (x86-64/ARM64 Windows).
 */

#pragma once
#ifndef WISP_VT_COMPRESS_LZ4_HPP
#define WISP_VT_COMPRESS_LZ4_HPP

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace wisp {
namespace vt {
namespace lz4 {

/* Maximum input accepted by the reference LZ4 block API. Keeping the same
 * limit means `compressBound` fits in the integer sizes used by LZ4 callers
 * and gives us the same compatibility boundary as other implementations. */
static const size_t max_input_size = 0x7E000000;

/* Every LZ4 match represents at least four bytes. The token stores the number
 * of bytes beyond this minimum rather than the full match length. */
static const size_t min_match = 4;

/* Number of bytes at the end of a conforming block which must remain literals. */
static const size_t last_literals = 5;

/* A match may not begin in the final 12 bytes. This leaves enough room for the
 * minimum match and the required five trailing literals. */
static const size_t match_find_limit = 12;

/* We retain one input position for each 12-bit hash. LZ4 refers to this as
 * memory usage 14 because the 4096 entries are four bytes each (16 KiB). */
static const unsigned hash_log = 12;

/* Multiplicative hash used by the reference LZ4 fast compressor. The high
 * `hash_log` bits provide the table index. */
static const uint32_t hash_multiplier = 2654435761u;

/* Scratch memory used while compressing one block. Each entry packs the low
 * 16 bits of the two most recent input positions for its hash. All-ones marks
 * an empty half; LZ4's 16-bit offset is enough to reconstruct the only useful
 * preceding address from the current position.
 * The table is reset by every call to `compress` and can be reused afterwards. */
typedef uint32_t HashTable[1 << hash_log];

/* Errors which can occur while encoding a block. */
enum class CompressError {
    none,

    /* The input exceeds the maximum size supported by the block compressor. */
    InputTooLarge,

    /* The provided output buffer cannot hold the encoded block. */
    OutputTooSmall,
};

/* Errors which can occur while decoding a block. */
enum class DecompressError {
    none,

    /* The encoded block ended in the middle of a sequence. */
    TruncatedInput,

    /* A match offset was zero or pointed before the produced output. */
    InvalidOffset,

    /* A sequence would write beyond the provided output buffer. */
    OutputTooSmall,

    /* The block ended before filling the exact-size output buffer. */
    OutputSizeMismatch,
};

namespace detail {

/* Read an unaligned little-endian integer at `position`. */
template <typename Int>
inline Int readIntAt(const uint8_t *input, size_t position) {
    Int v;
    memcpy(&v, input + position, sizeof(Int));
    return v;
}

/* Write an unaligned little-endian integer at `position`. */
template <typename Int>
inline void writeIntAt(uint8_t *output, size_t position, Int value) {
    memcpy(output + position, &value, sizeof(Int));
}

/* Copy one fixed-size integer between non-overlapping byte ranges.
 * Wisp: N bytes stand in for the integer type (u128 is 16). */
template <size_t N>
inline void copyIntAt(uint8_t *output, size_t output_position, const uint8_t *input, size_t input_position) {
    uint8_t tmp[N];
    memcpy(tmp, input + input_position, N);
    memcpy(output + output_position, tmp, N);
}

inline unsigned clz64(uint64_t x) {
#ifdef _MSC_VER
    unsigned long idx;
    _BitScanReverse64(&idx, x);
    return 63u - (unsigned)idx;
#else
    return (unsigned)__builtin_clzll(x);
#endif
}

inline unsigned ctz64(uint64_t x) {
#ifdef _MSC_VER
    unsigned long idx;
    _BitScanForward64(&idx, x);
    return (unsigned)idx;
#else
    return (unsigned)__builtin_ctzll(x);
#endif
}

/* Read the four-byte sequence used for match finding. Callers only use this
 * where at least four input bytes remain. */
inline uint32_t readU32(const uint8_t *input, size_t pos) { return readIntAt<uint32_t>(input, pos); }

/* Map a four-byte input sequence to its scratch-table slot. */
inline size_t hashSequence(uint32_t sequence) {
    return (size_t)((uint32_t)(sequence * hash_multiplier) >> (32 - hash_log));
}

/* Add a position to one hash slot and shift the previous newest position into
 * the fallback half. A position whose low bits are 0xFFFF conflicts with the
 * sentinel and is simply not stored. */
inline void rememberPosition(HashTable &table, size_t hash, size_t position) {
    const uint16_t low = (uint16_t)position;
    if (low == 0xFFFF) return;
    table[hash] = ((uint32_t)(uint16_t)table[hash] << 16) | low;
}

/* Recover the nearest preceding position from a stored low half. Modular
 * subtraction directly produces the LZ4 offset within the current window.
 * Wisp: ?usize is bool + out. */
inline bool candidatePosition(size_t ip, uint16_t stored, size_t *out) {
    if (stored == 0xFFFF) return false;
    const uint16_t distance = (uint16_t)((uint16_t)ip - stored);
    if (distance == 0) return false;
    *out = ip - distance;
    return true;
}

/* Advance through a literal run. The first KiB inspects every byte without
 * maintaining a counter, which keeps ordinary terminal-page searches cheap.
 * Longer runs enable gradually increasing steps for incompressible data. */
inline void advanceSearch(size_t *ip, size_t anchor, size_t *attempts) {
    if (*attempts == 0) {
        *ip += 1;
        if (*ip - anchor == 1024) *attempts = 1;
        return;
    }

    *attempts += 1;
    *ip += 1 + *attempts / 64;
}

struct MatchBegin {
    size_t position;
    size_t candidate;
};

/* Extend a match backwards without crossing the current literal anchor or the
 * beginning of the candidate. The returned positions preserve their offset. */
inline MatchBegin matchBegin(const uint8_t *input, size_t position_, size_t candidate_, size_t anchor) {
    size_t position = position_;
    size_t candidate = candidate_;

    while ((position - anchor < candidate ? position - anchor : candidate) >= sizeof(uint64_t)) {
        const size_t position_word = position - sizeof(uint64_t);
        const size_t candidate_word = candidate - sizeof(uint64_t);
        const uint64_t difference = readIntAt<uint64_t>(input, position_word) ^
                                    readIntAt<uint64_t>(input, candidate_word);
        if (difference != 0) {
            const size_t equal_bytes = clz64(difference) / 8;
            position -= equal_bytes;
            candidate -= equal_bytes;
            MatchBegin r = {position, candidate};
            return r;
        }

        position = position_word;
        candidate = candidate_word;
    }

    while (position > anchor && candidate > 0 && input[position - 1] == input[candidate - 1]) {
        position -= 1;
        candidate -= 1;
    }
    MatchBegin r = {position, candidate};
    return r;
}

/* Return the first input position where two matching runs differ, or `limit`
 * when they remain equal. Both positions are known to have matched through
 * `min_match` before this is called. */
inline size_t matchEnd(const uint8_t *input, size_t position_, size_t candidate_, size_t limit) {
    size_t position = position_;
    size_t candidate = candidate_;

    /* Reading as little endian makes the least-significant differing bit map
     * to the earliest byte in memory on every target. Unaligned reads are
     * lowered appropriately by Zig and require no target-specific intrinsics. */
    while (limit - position >= sizeof(uint64_t)) {
        const uint64_t difference = readIntAt<uint64_t>(input, position) ^
                                    readIntAt<uint64_t>(input, candidate);
        if (difference != 0) {
            const size_t equal_bytes = ctz64(difference) / 8;
            return position + equal_bytes;
        }

        position += sizeof(uint64_t);
        candidate += sizeof(uint64_t);
    }

    while (position < limit && input[position] == input[candidate]) {
        position += 1;
        candidate += 1;
    }
    return position;
}

/* Return the number of extension bytes needed when a length is represented by
 * a token nibble plus zero or more bytes. An extended length always ends with
 * a byte below 255, so an exact multiple of 255 requires a final zero byte. */
inline size_t encodedLengthBytes(size_t encoded_len) {
    if (encoded_len < 15) return 0;
    return (encoded_len - 15) / 255 + 1;
}

/* Write the portion of a length which did not fit in the token nibble.
 *
 * Each 255 byte means "add 255 and continue". The final byte is always less
 * than 255 and may be zero. */
inline void writeLength(uint8_t *output, size_t *op, size_t length_) {
    size_t length = length_;
    while (length >= 255) {
        output[*op] = 255;
        *op += 1;
        length -= 255;
    }
    output[*op] = (uint8_t)length;
    *op += 1;
}

/* Decode a length from its token nibble and any following extension bytes.
 * `ip` is advanced past every consumed extension byte. */
inline DecompressError decodeLength(const uint8_t *input, size_t input_len, size_t *ip, size_t nibble, size_t *out) {
    size_t length = nibble;
    if (nibble != 15) {
        *out = length;
        return DecompressError::none;
    }

    for (;;) {
        if (*ip >= input_len) return DecompressError::TruncatedInput;
        const uint8_t value = input[*ip];
        *ip += 1;
        if (length > SIZE_MAX - value) return DecompressError::TruncatedInput;
        length += value;
        if (value != 255) {
            *out = length;
            return DecompressError::none;
        }
    }
}

/* Emit one non-final sequence.
 *
 * A sequence starts with a token, followed by optional literal length bytes,
 * the literals themselves, the two-byte offset, and optional match length
 * bytes. This function computes the complete size first so `OutputTooSmall`
 * is reported without partially writing a sequence. */
inline CompressError emitSequence(uint8_t *output, size_t output_len, size_t *op, const uint8_t *literals,
                                  size_t literals_len, uint16_t offset, size_t match_len) {
    assert(match_len >= min_match);
    assert(offset > 0);

    const size_t encoded_match_len = match_len - min_match;

    /* One byte is always needed for the token and two for the offset. Each
     * length may additionally need extension bytes after its token nibble. */
    const size_t required = 1 + encodedLengthBytes(literals_len) + literals_len + 2 +
                            encodedLengthBytes(encoded_match_len);
    if (required > output_len - *op) return CompressError::OutputTooSmall;

    const size_t token_pos = *op;
    *op += 1;

    /* Lengths below 15 fit directly in their nibble. Larger values put 15 in
     * the nibble and encode the remainder immediately after the token. */
    output[token_pos] = (uint8_t)(((uint8_t)(literals_len < 15 ? literals_len : 15) << 4) |
                                  (uint8_t)(encoded_match_len < 15 ? encoded_match_len : 15));

    /* Literal length extensions precede the literals they describe. */
    if (literals_len >= 15) writeLength(output, op, literals_len - 15);
    if (literals_len) memcpy(output + *op, literals, literals_len);
    *op += literals_len;

    /* Match length extensions follow the offset because this is where the
     * decoder expects them in an LZ4 sequence. */
    writeIntAt<uint16_t>(output, *op, offset);
    *op += 2;
    if (encoded_match_len >= 15) writeLength(output, op, encoded_match_len - 15);
    return CompressError::none;
}

/* Emit the literal-only sequence which terminates every block.
 *
 * There is no offset or match length after these bytes. As with
 * `emitSequence`, capacity is checked before modifying the output. */
inline CompressError emitLastLiterals(uint8_t *output, size_t output_len, size_t *op, const uint8_t *literals,
                                      size_t literals_len) {
    const size_t required = 1 + encodedLengthBytes(literals_len) + literals_len;
    if (required > output_len - *op) return CompressError::OutputTooSmall;

    output[*op] = (uint8_t)((uint8_t)(literals_len < 15 ? literals_len : 15) << 4);
    *op += 1;
    if (literals_len >= 15) writeLength(output, op, literals_len - 15);
    if (literals_len) memcpy(output + *op, literals, literals_len);
    *op += literals_len;
    return CompressError::none;
}

/* Copy one decoded match from `offset` bytes behind `op`.
 *
 * The caller has validated that the match fits: `op + match_len` never
 * exceeds `output.len`. Wide copies may write a few scratch bytes past the
 * match's logical end; as described in `decompress`, that is safe anywhere
 * the write stays inside the output buffer. Every path therefore bounds its
 * wide stores by both the match end and the buffer end, and the bytewise
 * loop at the bottom finishes whatever remains. */
inline void copyMatch(uint8_t *output, size_t output_len, size_t op_, size_t offset, size_t match_len) {
    size_t op = op_;
    const size_t end = op_ + match_len;

    /* A source which ends behind the copy can never overlap it. Long
     * distant matches are common in structured pages (repeated rows and
     * whole blank regions), and one exact memcpy moves them in cache-line
     * units. Shorter matches are not worth the call overhead. */
    if (offset >= match_len && match_len >= 64) {
        memcpy(output + op, output + op - offset, match_len);
        return;
    }

    /* Wisp: `-|` is saturating subtraction. */
    const size_t sat7 = output_len > 7 ? output_len - 7 : 0;
    const size_t sat15 = output_len > 15 ? output_len - 15 : 0;

    switch (offset) {
    /* A period which divides the word size expands into one repeated
     * pattern word. Long runs (blank lines, repeated cells) then become
     * independent stores, with no load waiting on a preceding store.
     * Stores advance by whole words from `op`, which preserves the
     * pattern's phase. */
    case 1:
    case 2:
    case 4:
    case 8: {
        uint64_t pattern;
        switch (offset) {
        case 1: pattern = (uint64_t)output[op - 1] * 0x0101010101010101ull; break;
        case 2: pattern = (uint64_t)readIntAt<uint16_t>(output, op - 2) * 0x0001000100010001ull; break;
        case 4: pattern = (uint64_t)readIntAt<uint32_t>(output, op - 4) * 0x0000000100000001ull; break;
        default: pattern = readIntAt<uint64_t>(output, op - 8); break;
        }

        const size_t limit = end < sat7 ? end : sat7;
        while (op < limit) {
            writeIntAt<uint64_t>(output, op, pattern);
            op += 8;
        }
        break;
    }

    /* Wide copies are overlap-safe for the remaining offsets of at
     * least a copy unit: each load lies a full unit behind the store
     * which could observe it. Offsets 3, 5, 6, and 7 fall through to
     * the bytewise loop; they are rare in real data and word tricks
     * for them cost more in complexity than they return. */
    default:
        if (offset >= 16) {
            const size_t limit = end < sat15 ? end : sat15;
            while (op < limit) {
                copyIntAt<16>(output, op, output, op - offset);
                op += 16;
            }
        } else if (offset >= 8) {
            const size_t limit = end < sat7 ? end : sat7;
            while (op < limit) {
                copyIntAt<8>(output, op, output, op - offset);
                op += 8;
            }
        }
        break;
    }

    while (op < end) {
        output[op] = output[op - offset];
        op += 1;
    }
}

} /* namespace detail */

/* Return the maximum number of bytes needed to encode `input_len` bytes.
 *
 * Incompressible input is represented as one literal run. Every 255 literal
 * bytes can require one extension byte. The additional 16-byte margin covers
 * the token and the format's fixed overhead. Callers can allocate this amount
 * once and reuse it for any block no larger than `input_len`. */
inline CompressError compressBound(size_t input_len, size_t *out) {
    if (input_len > max_input_size) return CompressError::InputTooLarge;
    *out = input_len + input_len / 255 + 16;
    return CompressError::none;
}

/* Compress `input` into a raw LZ4 block in `output`.
 *
 * Returns the initialized length of `output`. The input and output buffers
 * must not overlap. `table` is scratch space and does not need to be
 * initialized by the caller; it is reset before use. */
inline CompressError compress(const uint8_t *input, size_t input_len, uint8_t *output, size_t output_len,
                              HashTable &table, size_t *out) {
    using namespace detail;
    if (input_len > max_input_size) return CompressError::InputTooLarge;

    /* All-ones in either packed half means "no previous position". */
    memset(table, 0xFF, sizeof(HashTable));

    /* `ip` is the current input position, `anchor` is the first literal not yet
     * emitted, and `op` is the next output position. A successful match emits
     * input[anchor..ip] as literals followed by the match, then moves both input
     * positions to the end of that match. */
    size_t op = 0;
    size_t anchor = 0;

    /* LZ4's format leaves the final five input bytes as literals and starts
     * the final match at least twelve bytes before the end. This is not
     * required by our safe decoder, but makes blocks compatible with fast
     * decoders that rely on the standard format restrictions. Inputs too
     * short for any match are emitted below as one literal-only sequence. */
    if (input_len >= match_find_limit) {
        const size_t search_end = input_len - match_find_limit;
        const size_t match_end_limit = input_len - last_literals;
        size_t ip = 0;
        size_t search_attempts = 0;

        while (ip <= search_end) {
            /* Hash the next four bytes and replace the table entry
             * immediately. Hash collisions are expected, so equality is
             * checked below before accepting a saved position as a match. */
            const uint32_t sequence = readU32(input, ip);
            const size_t hash = hashSequence(sequence);
            const uint32_t candidates = table[hash];
            rememberPosition(table, hash, ip);

            size_t match_pos;
            if (!candidatePosition(ip, (uint16_t)candidates, &match_pos) &&
                !candidatePosition(ip, (uint16_t)(candidates >> 16), &match_pos)) {
                advanceSearch(&ip, anchor, &search_attempts);
                continue;
            }

            if (readU32(input, match_pos) != sequence) {
                /* The nearest candidate collided. Fall back to the older
                 * one, which must additionally match one byte beyond the
                 * minimum so collision-prone minimum-length matches from
                 * the stale half are not emitted. */
                size_t older;
                if (candidatePosition(ip, (uint16_t)(candidates >> 16), &older) &&
                    readU32(input, older) == sequence && input[older + min_match] == input[ip + min_match]) {
                    match_pos = older;
                } else {
                    advanceSearch(&ip, anchor, &search_attempts);
                    continue;
                }
            }

            /* Pull the match backwards into the current literal run. This is
             * particularly helpful around aligned cell records. As with
             * forward extension, compare words before locating the first
             * differing byte. */
            const MatchBegin match_begin = matchBegin(input, ip, match_pos, anchor);
            ip = match_begin.position;
            match_pos = match_begin.candidate;

            /* We already compared the first four bytes. Continue up to the
             * point where the required last five literals begin. `matchEnd`
             * compares a machine word at a time before locating the first
             * differing byte. */
            const size_t match_end = matchEnd(input, ip + min_match, match_pos + min_match, match_end_limit);

            const CompressError err = emitSequence(output, output_len, &op, input + anchor, ip - anchor,
                                                   (uint16_t)(ip - match_pos), match_end - ip);
            if (err != CompressError::none) return err;

            /* The main loop jumps over the matched bytes rather than hashing
             * every position within them. Seed one position near the end so
             * an adjacent repeated record can still refer back into this
             * match. The next loop iteration will then seed `match_end`
             * normally. */
            if (match_end >= 2 && match_end - 2 + min_match <= input_len) {
                const size_t seed = match_end - 2;
                rememberPosition(table, hashSequence(readU32(input, seed)), seed);
            }

            ip = match_end;
            anchor = ip;
            search_attempts = 0;
        }
    }

    /* Whatever remains after the last match is the terminal literal-only
     * sequence. For short inputs this is also the only sequence in the block. */
    const CompressError err = emitLastLiterals(output, output_len, &op, input + anchor, input_len - anchor);
    if (err != CompressError::none) return err;
    *out = op;
    return CompressError::none;
}

/* Decompress a raw LZ4 block into an exact-size output buffer.
 *
 * Returns `output.len` on success. Both consuming all input and filling all
 * output are required. Raw LZ4 blocks do not carry their decoded size, so this
 * exact-size contract validates the size metadata maintained by the caller.
 * The input and output buffers must not overlap. */
inline DecompressError decompress(const uint8_t *input, size_t input_len, uint8_t *output, size_t output_len,
                                  size_t *out) {
    using namespace detail;
    /* `ip` and `op` always identify the next unread input byte and the next
     * unwritten output byte respectively.
     *
     * The decoder is written around one observation: almost every sequence
     * in real blocks has a short literal run and a short match. Both fast
     * paths below copy a fixed number of bytes blindly and let the length
     * arithmetic sort out how many of them were meaningful. Writing past a
     * run's logical end is safe within the output buffer because decoding
     * is strictly in order: every byte past `op` is either rewritten by a
     * later copy before anything can read it, or lies beyond the block's
     * final length and is never part of the result. The margin conditions
     * on the fast paths also subsume the exact bounds checks they replace,
     * which keeps decoding of malformed blocks memory-safe. */
    size_t ip = 0;
    size_t op = 0;

    for (;;) {
        /* A normal block ends after the literal bytes of its final sequence.
         * This also accepts the empty block produced by our compressor, which
         * consists of a zero token and no literals. */
        if (ip == input_len) {
            if (op != output_len) return DecompressError::OutputSizeMismatch;
            *out = op;
            return DecompressError::none;
        }

        const uint8_t token = input[ip];
        ip += 1;

        /* The high nibble and any extension bytes describe the literal run.
         * The literals are copied as a side effect of computing the length. */
        size_t literal_len;
        {
            const size_t nibble = token >> 4;
            const size_t in_left = input_len - ip;
            const size_t out_left = output_len - op;
            if (nibble != 15 && (in_left < out_left ? in_left : out_left) >= 16) {
                /* A run below the extension threshold is at most 14 bytes,
                 * so with a 16-byte margin on both buffers one wide copy
                 * covers it. */
                copyIntAt<16>(output, op, input, ip);
                literal_len = nibble;
            } else {
                /* Extended or margin-poor runs take the checked path. Bounds are
                 * verified before copying so malformed blocks never cause a
                 * partial read or write. */
                size_t len;
                const DecompressError err = decodeLength(input, input_len, &ip, nibble, &len);
                if (err != DecompressError::none) return err;
                if (len > input_len - ip) return DecompressError::TruncatedInput;
                if (len > output_len - op) return DecompressError::OutputTooSmall;
                if (len) memcpy(output + op, input + ip, len);
                literal_len = len;
            }
        }
        ip += literal_len;
        op += literal_len;

        /* Ending immediately after the literals marks the final sequence. Any
         * non-final sequence must continue with an offset and match length. */
        if (ip == input_len) {
            if (op != output_len) return DecompressError::OutputSizeMismatch;
            *out = op;
            return DecompressError::none;
        }

        if (input_len - ip < 2) return DecompressError::TruncatedInput;
        const size_t offset = readIntAt<uint16_t>(input, ip);
        ip += 2;
        if (offset == 0 || offset > op) return DecompressError::InvalidOffset;

        /* The token stores the match length minus the four-byte minimum. As
         * with literals, a low nibble of 15 is extended by following bytes. */
        const size_t match_nibble = token & 0x0F;

        /* A match whose length fits its nibble spans at most 18 bytes, so
         * three blind copies always cover it. They are overlap-safe when the
         * offset is at least a word: each load lies a full word behind the
         * store which could observe it, so repeating patterns propagate
         * correctly. */
        if (match_nibble != 15 && offset >= 8 && output_len - op >= 18) {
            const size_t match = op - offset;
            copyIntAt<8>(output, op, output, match);
            copyIntAt<8>(output, op + 8, output, match + 8);
            copyIntAt<2>(output, op + 16, output, match + 16);
            op += match_nibble + min_match;
            continue;
        }

        size_t encoded_match_len;
        const DecompressError err = decodeLength(input, input_len, &ip, match_nibble, &encoded_match_len);
        if (err != DecompressError::none) return err;
        if (encoded_match_len > SIZE_MAX - min_match) return DecompressError::OutputTooSmall;
        const size_t match_len = encoded_match_len + min_match;
        if (match_len > output_len - op) return DecompressError::OutputTooSmall;

        /* Match copies may overlap, so this cannot always be one memcpy.
         * `copyMatch` expands the common small repeating periods into wide
         * stores and uses word copies for larger offsets. */
        copyMatch(output, output_len, op, offset, match_len);
        op += match_len;
    }
}

} /* namespace lz4 */
} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_COMPRESS_LZ4_HPP */
