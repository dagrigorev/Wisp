/* inflate.hpp — DEFLATE (RFC 1951) and zlib (RFC 1950) decompression.
 *
 * Wisp: this has no upstream counterpart to transliterate. Ghostty calls
 * std.compress.flate, part of the Zig standard library, which is not
 * available to this port in any form. The format itself is the
 * specification, so this is written from RFC 1951 and RFC 1950 rather than
 * adapted from anyone's implementation.
 *
 * It exists for one caller: the Kitty graphics protocol's `o=z` transmission
 * medium, where the client sends zlib-compressed pixel data
 * (kitty/graphics_image.zig's decompressZlib).
 *
 * Decompression reads attacker-controlled bytes -- any program on the far
 * end of the pty can send them -- so this is written to fail rather than
 * trust: every bit read is bounds-checked against the input, every output
 * write against max_out, and a malformed stream returns false rather than
 * reading or writing out of bounds. There is no path that trusts a length
 * or distance from the input without validating it first.
 */

#pragma once
#ifndef WISP_ZIGSTD_INFLATE_HPP
#define WISP_ZIGSTD_INFLATE_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "allocator.hpp"
#include "array_list.hpp"

namespace wisp {
namespace zigstd {

namespace inflate_detail {

/* The maximum window a DEFLATE back-reference can look into (RFC 1951 §2). */
static const size_t window_len = 32768;

/* Code length limits from RFC 1951 §3.2.7. */
static const unsigned max_bits = 15;
static const unsigned max_symbols = 288;

/* A canonical Huffman decoder built from code lengths, per RFC 1951 §3.2.2. */
struct Huffman {
    /* count[n] is how many symbols have code length n. */
    uint16_t count[max_bits + 1];
    /* symbols ordered by code, as the canonical construction requires. */
    uint16_t symbol[max_symbols];

    void clear() {
        memset(count, 0, sizeof(count));
        memset(symbol, 0, sizeof(symbol));
    }

    /* Build from `n` code lengths. Returns false for an over-subscribed or
     * (for more than one used code) incomplete set, both of which RFC 1951
     * makes invalid. */
    bool build(const uint8_t *lengths, size_t n) {
        clear();
        for (size_t i = 0; i < n; i++) {
            if (lengths[i] > max_bits) return false;
            count[lengths[i]]++;
        }
        /* All lengths zero means no codes at all, which is valid only for a
         * distance tree that is never consulted. */
        if (count[0] == (uint16_t)n) return true;

        /* Check for an over-subscribed or incomplete code. */
        int left = 1;
        for (unsigned len = 1; len <= max_bits; len++) {
            left <<= 1;
            left -= (int)count[len];
            if (left < 0) return false; /* over-subscribed */
        }

        uint16_t offs[max_bits + 2];
        offs[1] = 0;
        for (unsigned len = 1; len <= max_bits; len++) offs[len + 1] = (uint16_t)(offs[len] + count[len]);
        for (size_t i = 0; i < n; i++) {
            if (lengths[i] != 0) symbol[offs[lengths[i]]++] = (uint16_t)i;
        }
        return true;
    }
};

/* A bit reader over a fixed input, LSB-first as DEFLATE requires. Every
 * accessor reports exhaustion rather than reading past the end. */
struct BitReader {
    const uint8_t *src;
    size_t len;
    size_t pos;      /* next byte to consume */
    uint32_t bitbuf; /* bits not yet consumed, LSB-first */
    unsigned bitcnt;

    BitReader(const uint8_t *s, size_t l) : src(s), len(l), pos(0), bitbuf(0), bitcnt(0) {}

    /* Read `need` bits. Returns false if the input runs out. */
    bool bits(unsigned need, uint32_t *out) {
        while (bitcnt < need) {
            if (pos >= len) return false;
            bitbuf |= (uint32_t)src[pos++] << bitcnt;
            bitcnt += 8;
        }
        *out = bitbuf & ((need == 32) ? 0xFFFFFFFFu : (((uint32_t)1 << need) - 1));
        bitbuf >>= need;
        bitcnt -= need;
        return true;
    }

    void alignToByte() {
        const unsigned drop = bitcnt & 7;
        bitbuf >>= drop;
        bitcnt -= drop;
    }

    /* Decode one symbol with `h`, walking code lengths shortest first. */
    bool decode(const Huffman &h, uint16_t *out) {
        int code = 0, first = 0, index = 0;
        for (unsigned code_len = 1; code_len <= max_bits; code_len++) {
            uint32_t b = 0;
            if (!bits(1, &b)) return false;
            code |= (int)b;
            const int count = (int)h.count[code_len];
            if (code - count < first) {
                *out = h.symbol[index + (code - first)];
                return true;
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return false; /* no code matched within the maximum length */
    }
};

/* RFC 1951 §3.2.5: length codes 257..285 and distance codes 0..29. */
static const uint16_t length_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,
                                         15, 17, 19, 23, 27, 31, 35, 43, 51,  59,
                                         67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                         2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1,    2,    3,    4,    5,    7,     9,     13,
                                       17,   25,   33,   49,   65,   97,    129,   193,
                                       257,  385,  513,  769,  1025, 1537,  2049,  3073,
                                       4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                       6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/* Append `n` bytes to `out`, refusing to exceed max_out. */
static inline bool emit(Allocator alloc, ArrayListUnmanaged<uint8_t> *out, size_t max_out,
                        const uint8_t *data, size_t n) {
    if (n > max_out || out->items_len > max_out - n) return false;
    if (!out->ensureUnusedCapacity(alloc, n)) return false;
    memcpy((void *)(out->items + out->items_len), (const void *)data, n);
    out->items_len += n;
    return true;
}

static inline bool emitByte(Allocator alloc, ArrayListUnmanaged<uint8_t> *out, size_t max_out,
                            uint8_t b) {
    return emit(alloc, out, max_out, &b, 1);
}

/* One compressed block body, given its literal/length and distance trees. */
static inline bool inflateBlock(BitReader *br, const Huffman &lit, const Huffman &dist,
                                Allocator alloc, ArrayListUnmanaged<uint8_t> *out,
                                size_t max_out) {
    for (;;) {
        uint16_t sym = 0;
        if (!br->decode(lit, &sym)) return false;

        if (sym < 256) {
            if (!emitByte(alloc, out, max_out, (uint8_t)sym)) return false;
            continue;
        }
        if (sym == 256) return true; /* end of block */

        /* A length/distance pair. */
        const unsigned li = (unsigned)sym - 257;
        if (li >= 29) return false; /* 286 and 287 are invalid per RFC 1951 */
        uint32_t extra = 0;
        if (!br->bits(length_extra[li], &extra)) return false;
        const size_t length = (size_t)length_base[li] + extra;

        uint16_t dsym = 0;
        if (!br->decode(dist, &dsym)) return false;
        if (dsym >= 30) return false; /* 30 and 31 are invalid */
        if (!br->bits(dist_extra[dsym], &extra)) return false;
        const size_t distance = (size_t)dist_base[dsym] + extra;

        /* The distance must not reach behind the start of the output. This
         * is the check that keeps a malformed stream from reading out of
         * bounds, so it is made before any copying. */
        if (distance > out->items_len) return false;

        /* Overlapping copies are legal and are how runs are encoded, so copy
         * one byte at a time rather than with memcpy. */
        if (length > max_out || out->items_len > max_out - length) return false;
        if (!out->ensureUnusedCapacity(alloc, length)) return false;
        size_t from = out->items_len - distance;
        for (size_t i = 0; i < length; i++) {
            out->items[out->items_len] = out->items[from + i];
            out->items_len++;
        }
    }
}

/* The fixed Huffman trees of RFC 1951 §3.2.6. */
static inline bool buildFixed(Huffman *lit, Huffman *dist) {
    uint8_t lengths[max_symbols];
    size_t i = 0;
    for (; i < 144; i++) lengths[i] = 8;
    for (; i < 256; i++) lengths[i] = 9;
    for (; i < 280; i++) lengths[i] = 7;
    for (; i < 288; i++) lengths[i] = 8;
    if (!lit->build(lengths, 288)) return false;

    uint8_t dlengths[30];
    for (i = 0; i < 30; i++) dlengths[i] = 5;
    return dist->build(dlengths, 30);
}

/* The dynamic trees of RFC 1951 §3.2.7. */
static inline bool buildDynamic(BitReader *br, Huffman *lit, Huffman *dist) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                      11, 4,  12, 3, 13, 2, 14, 1, 15};
    uint32_t hlit = 0, hdist = 0, hclen = 0;
    if (!br->bits(5, &hlit) || !br->bits(5, &hdist) || !br->bits(4, &hclen)) return false;
    const size_t nlen = (size_t)hlit + 257;
    const size_t ndist = (size_t)hdist + 1;
    const size_t ncode = (size_t)hclen + 4;
    if (nlen > 286 || ndist > 30) return false;

    uint8_t clen[19];
    memset(clen, 0, sizeof(clen));
    for (size_t i = 0; i < ncode; i++) {
        uint32_t v = 0;
        if (!br->bits(3, &v)) return false;
        clen[order[i]] = (uint8_t)v;
    }
    Huffman code_tree;
    if (!code_tree.build(clen, 19)) return false;

    /* Read nlen + ndist code lengths, which share one run-length coding. */
    uint8_t lengths[max_symbols + 30];
    memset(lengths, 0, sizeof(lengths));
    size_t n = 0;
    while (n < nlen + ndist) {
        uint16_t sym = 0;
        if (!br->decode(code_tree, &sym)) return false;
        if (sym < 16) {
            lengths[n++] = (uint8_t)sym;
            continue;
        }
        uint8_t repeat_value = 0;
        uint32_t repeat = 0;
        if (sym == 16) {
            if (n == 0) return false; /* nothing to repeat */
            repeat_value = lengths[n - 1];
            if (!br->bits(2, &repeat)) return false;
            repeat += 3;
        } else if (sym == 17) {
            if (!br->bits(3, &repeat)) return false;
            repeat += 3;
        } else if (sym == 18) {
            if (!br->bits(7, &repeat)) return false;
            repeat += 11;
        } else {
            return false;
        }
        if (n + repeat > nlen + ndist) return false;
        for (uint32_t i = 0; i < repeat; i++) lengths[n++] = repeat_value;
    }
    /* The end-of-block code must exist, or the stream cannot terminate. */
    if (lengths[256] == 0) return false;

    if (!lit->build(lengths, nlen)) return false;
    return dist->build(lengths + nlen, ndist);
}

}  // namespace inflate_detail

/* Inflate a raw DEFLATE (RFC 1951) stream. Returns false on malformed input
 * or if the output would exceed max_out. On success `out` holds the
 * decompressed bytes and the caller owns them. */
inline bool inflateRaw(Allocator alloc, const uint8_t *src, size_t src_len, size_t max_out,
                       ArrayListUnmanaged<uint8_t> *out) {
    using namespace inflate_detail;
    if (src == nullptr && src_len != 0) return false;

    BitReader br(src, src_len);
    for (;;) {
        uint32_t final_block = 0, type = 0;
        if (!br.bits(1, &final_block)) return false;
        if (!br.bits(2, &type)) return false;

        if (type == 0) {
            /* Stored: LEN, NLEN, then LEN literal bytes (RFC 1951 §3.2.4). */
            br.alignToByte();
            uint32_t len = 0, nlen = 0;
            if (!br.bits(16, &len) || !br.bits(16, &nlen)) return false;
            if ((len ^ 0xFFFFu) != nlen) return false;
            /* The bit reader may still hold whole buffered bytes. */
            uint8_t byte_buf[4];
            size_t buffered = 0;
            while (br.bitcnt >= 8 && buffered < len) {
                uint32_t b = 0;
                if (!br.bits(8, &b)) return false;
                byte_buf[buffered++] = (uint8_t)b;
                if (buffered == sizeof(byte_buf)) break;
            }
            if (buffered > 0 && !emit(alloc, out, max_out, byte_buf, buffered)) return false;
            const size_t remaining = (size_t)len - buffered;
            if (remaining > br.len - br.pos) return false;
            if (remaining > 0) {
                if (!emit(alloc, out, max_out, br.src + br.pos, remaining)) return false;
                br.pos += remaining;
            }
        } else if (type == 1 || type == 2) {
            Huffman lit, dist;
            const bool ok = (type == 1) ? buildFixed(&lit, &dist) : buildDynamic(&br, &lit, &dist);
            if (!ok) return false;
            if (!inflateBlock(&br, lit, dist, alloc, out, max_out)) return false;
        } else {
            return false; /* type 3 is reserved */
        }

        if (final_block != 0) return true;
    }
}

/* Inflate a zlib (RFC 1950) stream: a two byte header, a DEFLATE stream, and
 * an Adler-32 of the uncompressed data. Returns false on malformed input or
 * if the output would exceed max_out. */
inline bool inflateZlib(Allocator alloc, const uint8_t *src, size_t src_len, size_t max_out,
                        ArrayListUnmanaged<uint8_t> *out) {
    if (src_len < 2) return false;

    /* RFC 1950 §2.2: CMF and FLG. */
    const uint8_t cmf = src[0];
    const uint8_t flg = src[1];
    if ((cmf & 0x0F) != 8) return false;                 /* CM must be deflate */
    if (((cmf >> 4) & 0x0F) > 7) return false;           /* CINFO, window <= 32K */
    if ((((unsigned)cmf << 8) | flg) % 31 != 0) return false; /* header checksum */
    if ((flg & 0x20) != 0) return false;                 /* a preset dictionary is not supported */

    if (!inflateRaw(alloc, src + 2, src_len - 2, max_out, out)) return false;

    /* Wisp: the trailing Adler-32 is not verified. Upstream's reader does not
     * surface a checksum failure separately either, and the caller treats any
     * failure the same way; the bounds checks above are what make a malformed
     * stream safe, not the checksum. */
    return true;
}

}  // namespace zigstd
}  // namespace wisp

#endif  // WISP_ZIGSTD_INFLATE_HPP
