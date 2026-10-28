/* Transliterated from Ghostty src/crc32c.zig @ b32f20f3e
 * Copyright (c) Mitchell Hashimoto
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION; comments are upstream's unless marked "Wisp:".
 */

/* CRC32C with hardware acceleration.
 *
 * The Zig standard library implementation processes one byte per table
 * lookup (as of Zig 0.16), which is more than an order of magnitude slower
 * than the dedicated CRC32C instructions available on aarch64 (CRC
 * extension) and x86_64 (SSE4.2). This module selects the best backend at
 * compile time.
 *
 * Targets without a dedicated instruction, such as WebAssembly, use a
 * custom implementation that is faster than Zig's stdlib.
 *
 * The resulting value is identical across all backends: this is the
 * iSCSI CRC32C parameter set (reflected, initial and final XOR
 * `0xFFFFFFFF`), matching `std.hash.crc.Crc32Iscsi`.
 */

#pragma once
#ifndef WISP_ZIGSTD_CRC32C_HPP
#define WISP_ZIGSTD_CRC32C_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef _MSC_VER
#include <intrin.h>
#include <nmmintrin.h>
#endif

namespace wisp {
namespace zigstd {

enum class Backend {
    /* Wisp: upstream's `aarch64_crc` backend is dropped. Its body is four
     * inline-asm `crc32c*` forms, Wisp targets Windows on x64 only, and
     * MSVC x64 supports no inline asm at all, so there is nothing to
     * transliterate it to and nothing that could select it. */
    x86_64_sse42,
    software,
};

/* Wisp: upstream picks the backend at COMPILE time from
 * `builtin.cpu.features`. A Wisp binary is built once and shipped to
 * machines whose CPU is not known at compile time, so baking SSE4.2 in
 * would crash with an illegal instruction on an older host, and compiling
 * it out would lose the acceleration everywhere. The choice is therefore
 * made at RUNTIME, once, from CPUID leaf 1 ECX bit 20 (SSE4.2), and cached
 * in a function-local static (thread-safe initialisation, no
 * static-initialisation-order hazard). Everything downstream of the choice
 * — both backends, and the value they produce — is upstream's. */
inline Backend backend() {
    static const Backend selected = []() -> Backend {
#ifdef _MSC_VER
        int regs[4] = { 0, 0, 0, 0 };
        __cpuid(regs, 0);
        if (regs[0] >= 1) {
            __cpuid(regs, 1);
            if ((regs[2] & (1 << 20)) != 0) return Backend::x86_64_sse42;
        }
#endif
        return Backend::software;
    }();
    return selected;
}

/* One CRC32C instruction folding `value` into the running CRC.
 *
 * Wisp: upstream's `step` is a comptime switch over the backend and the
 * integer type holding inline asm. MSVC x64 has no inline asm, so each arm
 * becomes the matching SSE4.2 intrinsic from <nmmintrin.h>, which emits the
 * same `crc32` instruction form. The `u64` arm truncates exactly as
 * upstream's `@truncate` does. These are only ever called once the SSE4.2
 * backend has been selected, so upstream's `.software => comptime
 * unreachable` arm has no analogue here. */
inline uint32_t stepU8(uint32_t crc, uint8_t value) {
    return (uint32_t)_mm_crc32_u8((unsigned int)crc, value);
}
inline uint32_t stepU32(uint32_t crc, uint32_t value) {
    return (uint32_t)_mm_crc32_u32((unsigned int)crc, value);
}
inline uint32_t stepU64(uint32_t crc, uint64_t value) {
    return (uint32_t)_mm_crc32_u64((unsigned __int64)crc, (unsigned __int64)value);
}

/* Wisp: std.mem.readInt(T, .., .little). x64 is little-endian, but go
 * through memcpy rather than a cast so an unaligned read stays defined. */
inline uint32_t readIntLittleU32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}
inline uint64_t readIntLittleU64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

/* One update pass using the dedicated CRC32C instructions. Both supported
 * architectures handle unaligned loads efficiently, so the loop reads
 * little-endian words directly from the input. */
inline uint32_t updateHardware(uint32_t initial, const uint8_t *bytes, size_t bytes_len) {
    uint32_t crc = initial;
    const uint8_t *remaining = bytes;
    size_t remaining_len = bytes_len;

    while (remaining_len >= 8) {
        crc = stepU64(crc, readIntLittleU64(remaining));
        remaining += 8;
        remaining_len -= 8;
    }
    if (remaining_len >= 4) {
        crc = stepU32(crc, readIntLittleU32(remaining));
        remaining += 4;
        remaining_len -= 4;
    }
    for (size_t i = 0; i < remaining_len; i++) crc = stepU8(crc, remaining[i]);
    return crc;
}

/* The portable software backend, used by targets without a dedicated
 * CRC32C instruction. */
struct Software {
    /* The reflected CRC32C (Castagnoli) polynomial. */
    static const uint32_t reflected_poly = 0x82F63B78u;

    /* Number of slicing tables, which is also the bytes folded per
     * iteration. */
    static const size_t slices = 16;

    /* Inputs below this length use the single-stream pass: the
     * stream-combine matrix work would not pay for itself. */
    static const size_t multi_stream_threshold = 4096;

    /* Slicing tables: `tables[i][b]` is the CRC of byte `b` followed by
     * `i` zero bytes. Table zero is the classic one-byte-per-step table;
     * the higher tables let one iteration fold a whole block with
     * independent lookups instead of a byte-by-byte dependency chain.
     *
     * Wisp: upstream builds this at comptime. C++14 constexpr cannot write
     * a [16][256] array into a plain `constexpr` variable without wrapping
     * it in a literal type and an awkward constructor, so it is built once
     * on first use inside a function-local static instead: the generating
     * loops stay a line-by-line copy of upstream's, initialisation is
     * thread-safe and ordered (no static-initialisation-order hazard, and
     * `zeroShiftMatrices()` below can simply call this), and the values are
     * identical. */
    typedef uint32_t Tables[slices][256];
    static const Tables &tables() {
        struct Holder {
            Tables t;
            Holder() {
                for (size_t n = 0; n < 256; n++) {
                    uint32_t crc = (uint32_t)n;
                    for (size_t j = 0; j < 8; j++) {
                        crc = (crc >> 1) ^ (reflected_poly * (crc & 1));
                    }
                    t[0][n] = crc;
                }
                for (size_t i = 1; i < slices; i++) {
                    for (size_t n = 0; n < 256; n++) {
                        const uint32_t prev = t[i - 1][n];
                        t[i][n] = (prev >> 8) ^ t[0][prev & 0xFF];
                    }
                }
            }
        };
        static const Holder holder;
        return holder.t;
    }

    static uint32_t update(uint32_t initial, const uint8_t *bytes, size_t bytes_len) {
        if (bytes_len >= multi_stream_threshold) return updateMulti(initial, bytes, bytes_len);

        return updateSingle(initial, bytes, bytes_len);
    }

    /* One single-stream update pass using slicing. */
    static uint32_t updateSingle(uint32_t initial, const uint8_t *bytes, size_t bytes_len) {
        const Tables &t = tables();
        uint32_t crc = initial;
        size_t i = 0;
        while (i + slices <= bytes_len) {
            crc = foldChunk(bytes, i, crc);
            i += slices;
        }
        for (; i < bytes_len; i++) {
            const uint8_t byte = bytes[i];
            crc = (crc >> 8) ^ t[0][(crc ^ byte) & 0xFF];
        }
        return crc;
    }

    /* One update pass as three independent interleaved streams. Faster
     * for large enough inputs. */
    static uint32_t updateMulti(uint32_t initial, const uint8_t *bytes, size_t bytes_len) {
        /* Both leading parts are block multiples so the interleaved loop
         * needs no tail handling; the third part absorbs the remainder. */
        const size_t part = (bytes_len / 3) & ~(size_t)(slices - 1);
        const uint8_t *p0 = bytes;
        const size_t p0_len = part;
        const uint8_t *p1 = bytes + part;
        const size_t p1_len = part;
        const uint8_t *p2 = bytes + 2 * part;
        const size_t p2_len = bytes_len - 2 * part;
        (void)p0_len;

        uint32_t s0 = initial;
        uint32_t s1 = 0;
        uint32_t s2 = 0;
        size_t i = 0;
        while (i + slices <= part) {
            s0 = foldChunk(p0, i, s0);
            s1 = foldChunk(p1, i, s1);
            s2 = foldChunk(p2, i, s2);
            i += slices;
        }
        s2 = updateSingle(s2, p2 + part, p2_len - part);

        const uint32_t s01 = s1 ^ zeroShift(s0, p1_len);
        return s2 ^ zeroShift(s01, p2_len);
    }

    /* Fold one aligned block through the per-position slicing tables.
     * The running CRC must already be XORed into the block's first word. */
    static uint32_t foldBlock(size_t len, const uint32_t *words) {
        const Tables &t = tables();
        uint32_t crc = 0;
        for (size_t w = 0; w < len / 4; w++) {
            const uint32_t word = words[w];
            const size_t base = len - 1 - w * 4;
            crc ^= t[base][word & 0xFF] ^
                   t[base - 1][(word >> 8) & 0xFF] ^
                   t[base - 2][(word >> 16) & 0xFF] ^
                   t[base - 3][word >> 24];
        }
        return crc;
    }

    /* Fold the block starting at `offset`, chaining the running CRC state. */
    static uint32_t foldChunk(const uint8_t *bytes, size_t offset, uint32_t crc) {
        uint32_t words[slices / 4];
        for (size_t w = 0; w < slices / 4; w++) {
            words[w] = readIntLittleU32(bytes + offset + w * 4);
        }
        words[0] ^= crc;
        return foldBlock(slices, words);
    }

    /* Advance a CRC state as if `len` zero bytes had been processed. */
    static uint32_t zeroShift(uint32_t state, size_t len) {
        uint32_t s = state;
        size_t remaining = len;
        size_t k = 0;
        while (remaining != 0) {
            if ((remaining & 1) != 0) {
                const uint32_t *mat = zeroShiftMatrices()[k / 2];
                s = matTimesVec(mat, s);
                if (k % 2 != 0) s = matTimesVec(mat, s);
            }
            remaining >>= 1;
            k += 1;
        }
        return s;
    }

    /* Multiply the GF(2) matrix by a CRC state column vector. */
    static uint32_t matTimesVec(const uint32_t *mat, uint32_t vec) {
        uint32_t sum = 0;
        uint32_t v = vec;
        size_t i = 0;
        while (v != 0) {
            if ((v & 1) != 0) sum ^= mat[i];
            v >>= 1;
            i += 1;
        }
        return sum;
    }

    /* Wisp: comptime in upstream; built once on first use for the same
     * reasons as `tables()` above, and it reads `tables()` directly so the
     * dependency between the two is explicit rather than ordering-dependent. */
    typedef uint32_t Matrices[32][32];
    static const Matrices &zeroShiftMatrices() {
        struct Holder {
            Matrices m;
            Holder() {
                const Tables &t = tables();
                uint32_t previous[32];
                for (size_t i = 0; i < 32; i++) {
                    const uint32_t unit = (uint32_t)1 << i;
                    previous[i] = (unit >> 8) ^ t[0][unit & 0xFF];
                }
                for (size_t i = 0; i < 32; i++) m[0][i] = previous[i];
                for (size_t k = 1; k < 64; k++) {
                    uint32_t squared[32];
                    for (size_t i = 0; i < 32; i++) {
                        squared[i] = matTimesVec(previous, previous[i]);
                    }
                    for (size_t i = 0; i < 32; i++) previous[i] = squared[i];
                    if (k % 2 == 0) {
                        for (size_t i = 0; i < 32; i++) m[k / 2][i] = squared[i];
                    }
                }
            }
        };
        static const Holder holder;
        return holder.m;
    }
};

/* Streaming CRC32C with the same interface shape as `std.hash.crc` types.
 *
 * Wisp: declared after `Software` rather than before it, as upstream has
 * it: C++ needs `Software` complete before `update` can call it. */
struct Crc32c {
    uint32_t crc;

    static Crc32c init() {
        Crc32c c;
        c.crc = 0xFFFFFFFFu;
        return c;
    }

    void update(const uint8_t *bytes, size_t bytes_len) {
        switch (backend()) {
            case Backend::x86_64_sse42:
                crc = updateHardware(crc, bytes, bytes_len);
                break;
            case Backend::software:
                crc = Software::update(crc, bytes, bytes_len);
                break;
        }
    }

    uint32_t final_() const { return crc ^ 0xFFFFFFFFu; }

    static uint32_t hash(const uint8_t *bytes, size_t bytes_len) {
        Crc32c c = Crc32c::init();
        c.update(bytes, bytes_len);
        return c.final_();
    }
};

}  // namespace zigstd
}  // namespace wisp

#endif  // WISP_ZIGSTD_CRC32C_HPP
