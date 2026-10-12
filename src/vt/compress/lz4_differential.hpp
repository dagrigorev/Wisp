/* Transliterated from Ghostty src/terminal/compress/lz4_differential.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Differential and property tests for the LZ4 block codec.
 *
 * Every generated input must compress into a block which:
 *
 *   1. fits within `compressBound`,
 *   2. is structurally valid LZ4 with the stricter guarantees our
 *      compressor documents (final five bytes are literals, matches start
 *      at least twelve bytes before the end), verified by an independent
 *      walker which shares no code with the codec,
 *   3. decompresses to exactly the original bytes, and
 *   4. is rejected when decompressed into a buffer of the wrong size.
 *
 * Valid blocks are additionally mutated (bit flips, splices, truncations)
 * and fed to the decompressor, which must fail cleanly or succeed, but
 * never read or write out of bounds. The unit-test build enables runtime
 * safety, so any out-of-bounds slice access fails the test.
 *
 * The light suite below runs as a normal unit test and finishes quickly.
 * The exhaustive suite multiplies the same properties across far more
 * sizes, periods, seeds, and mutations; it is slow and therefore skipped
 * unless the environment variable `GHOSTTY_LZ4_SLOW` is set:
 *
 *     GHOSTTY_LZ4_SLOW=1 zig build test -Dtest-filter="lz4 differential"
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 * Wisp: upstream is a test-only file. The machinery lives here, in the
 * shape of the neighbouring lz4.hpp, and the two upstream `test` blocks
 * are in tests/test_vt_lz4_differential_port.cpp.
 * Wisp: error unions return `bool` (true = success) with the value in an
 * out parameter; `try testing.expect(x)` is `WISP_LZ4D_EXPECT(x)`, which
 * reports file and line and returns false up the chain.
 * Wisp: upstream's mutation suite leans on Zig's runtime slice bounds
 * checks to turn an out-of-bounds decode into a test failure. C++ has no
 * such check, so the mutation run only catches what the codec's own
 * asserts, the CRT heap, or a sanitizer build catches. The mutations and
 * their inputs are ported unchanged so the coverage is there when the
 * suite is built with /RTCs, ASan, or a checked allocator.
 */

#pragma once
#ifndef WISP_VT_COMPRESS_LZ4_DIFFERENTIAL_HPP
#define WISP_VT_COMPRESS_LZ4_DIFFERENTIAL_HPP

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lz4.hpp"
#include "../../zigstd/allocator.hpp"
#include "../../zigstd/random.hpp"

namespace wisp {
namespace vt {
namespace lz4_differential {

/* Wisp: `try testing.expect(cond)` — report and unwind. */
#define WISP_LZ4D_EXPECT(cond)                                                                     \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "  FAIL  %s:%d: lz4_differential: %s\n", __FILE__, __LINE__, #cond);   \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

/* Number of trailing block bytes which must be literals, mirroring the
 * documented guarantee of the compressor. Kept as an independent constant
 * so a codec regression cannot silently weaken the check. */
static const size_t last_literals = 5;

/* A match may not begin in the final twelve bytes. See `last_literals`. */
static const size_t match_find_limit = 12;

/* Sizes around every encoding boundary: token nibble limits (14/15),
 * minimum match and find limits (4/12), length-extension steps (15 + 255k),
 * and power-of-two neighborhoods. */
static const size_t boundary_sizes[] = {
    0,   1,    2,    3,    4,    5,    6,   7,
    8,   9,    11,   12,   13,   14,   15,  16,
    17,  18,   19,   20,   31,   32,   33,  63,
    64,  65,   254,  255,  256,  269,  270, 271,
    272, 1023, 1024, 4095, 4096, 4097,
};
static const size_t boundary_sizes_len = sizeof(boundary_sizes) / sizeof(boundary_sizes[0]);

/* Wisp: std.Random helpers the port's zigstd/random.hpp does not carry.
 * They are transliterated from Zig 0.16.0 lib/std/Random.zig here rather
 * than added to random.hpp, which this task may not modify. */
namespace detail {

/* Wisp: `random.int(u3)` reads one byte and truncates to its low 3 bits. */
inline uint8_t intU3(zigstd::Random &random) { return (uint8_t)(random.int_<uint8_t>() & 0x07u); }

inline unsigned clz64(uint64_t x) {
    /* Wisp: @clz(@as(u64, 0)) is 64; the intrinsics are undefined there. */
    if (x == 0) return 64u;
    return lz4::detail::clz64(x);
}

/* std.Random.float(f64).
 *
 * Use 52 random bits for the mantissa, and the rest for the exponent.
 * If all 12 bits are zero, generate additional random bits, until a
 * set bit is found, or 1022 bits have been generated. */
inline double floatF64(zigstd::Random &random) {
    const uint64_t rand = random.int_<uint64_t>();
    uint64_t rand_lz = clz64(rand);
    if (rand_lz >= 12) {
        rand_lz = 12;
        for (;;) {
            /* It is astronomically unlikely for this loop to execute more than once. */
            const uint64_t addl_rand_lz = clz64(random.int_<uint64_t>());
            rand_lz += addl_rand_lz;
            if (addl_rand_lz != 64) break;
            if (rand_lz >= 1022) {
                rand_lz = 1022;
                break;
            }
        }
    }
    const uint64_t mantissa = rand & 0xFFFFFFFFFFFFFull;
    const uint64_t exponent = (uint64_t)(1022 - rand_lz) << 52;
    const uint64_t bits = mantissa | exponent;
    double out;
    memcpy(&out, &bits, sizeof(out));
    return out;
}

} /* namespace detail */

/* Input generators exercising distinct codec behaviors. Every generator is
 * deterministic for a given random state. */
enum class Generator {
    /* Uniform random bytes; largely incompressible. */
    random_bytes,

    /* Runs of one repeated byte with random lengths; period-one matches. */
    runs,

    /* One repeating pattern; exercises a fixed match period end to end. */
    periodic,

    /* Eight-byte records with random small payloads and zero padding, with
     * some records repeated; resembles terminal cell memory. */
    cells,

    /* Dictionary words with separators; text-like literal/match mix. */
    words,

    /* Mostly zeros with scattered random bytes; long matches with isolated
     * literals. */
    sparse,

    /* Random segments of all other generators; exercises transitions. */
    mixed,
};

/* Wisp: @typeInfo(Generator).@"enum".fields.len. */
static const size_t generator_count = 7;

/* Wisp: `random.enumValue(Generator)` is uintLessThan(usize, fields.len)
 * into the dense field list. */
inline Generator enumValue(zigstd::Random &random) {
    return (Generator)random.uintLessThan<size_t>(generator_count);
}

/* Fill `buf` with one repeating pattern of the given period. */
inline void fillPeriodic(zigstd::Random &random, uint8_t *buf, size_t buf_len, size_t period) {
    if (buf_len == 0) return;
    const size_t head = period < buf_len ? period : buf_len;
    random.bytes(buf, head);
    for (size_t i = head; i < buf_len; i++) buf[i] = buf[i - period];
}

inline void fill(Generator gen, zigstd::Random &random, uint8_t *buf, size_t buf_len) {
    switch (gen) {
    case Generator::random_bytes: random.bytes(buf, buf_len); break;

    case Generator::runs: {
        size_t i = 0;
        while (i < buf_len) {
            const size_t drawn = random.intRangeAtMost<size_t>(1, 300);
            const size_t run = drawn < buf_len - i ? drawn : buf_len - i;
            memset(buf + i, random.int_<uint8_t>(), run);
            i += run;
        }
        break;
    }

    case Generator::periodic:
        fillPeriodic(random, buf, buf_len, random.intRangeAtMost<size_t>(1, 40));
        break;

    case Generator::cells: {
        size_t i = 0;
        for (; i + 8 <= buf_len; i += 8) {
            uint8_t *cell = buf + i;
            /* Wisp: Zig's `and` short-circuits, so the boolean is only
             * drawn when i >= 8. `&&` keeps the draw sequence. */
            if (i >= 8 && random.boolean()) {
                /* Repeat one of the last 256 records. */
                const size_t limit = (i / 8) < 256 ? (i / 8) : 256;
                const size_t back = 8 * random.intRangeAtMost<size_t>(1, limit);
                memcpy(cell, buf + (i - back), 8);
            } else {
                memset(cell, 0, 8);
                /* Wisp: ' ' + uintLessThan(u8, 95) is u8 arithmetic;
                 * 32 + 0..94 stays in range, but the operands are kept
                 * 8-bit so no promotion widens the result. */
                cell[0] = (uint8_t)((uint8_t)' ' + random.uintLessThan<uint8_t>(95));
                cell[1] = random.uintLessThan<uint8_t>(4);
            }
        }
        random.bytes(buf + i, buf_len - i);
        break;
    }

    case Generator::words: {
        static const char *const words[] = {
            "the",   "terminal", "page",   "compress",
            "row",   "cell",     "style",  "zig",
            "lz4",   "block",    "offset", "match",
            "a",     "of",       "and",    "literal",
            "0x00",  "0xFF",     "    ",   "\r\n",
            "-----", "=",        "pub fn", "const",
        };
        static const size_t words_len = sizeof(words) / sizeof(words[0]);
        size_t i = 0;
        while (i < buf_len) {
            const char *word = words[random.uintLessThan<size_t>(words_len)];
            const size_t word_size = strlen(word);
            const size_t n = word_size < buf_len - i ? word_size : buf_len - i;
            if (n) memcpy(buf + i, word, n);
            i += n;
            if (i < buf_len) {
                buf[i] = random.boolean() ? (uint8_t)' ' : (uint8_t)'\n';
                i += 1;
            }
        }
        break;
    }

    case Generator::sparse: {
        memset(buf, 0, buf_len);
        if (buf_len == 0) return;
        for (size_t k = 0; k < buf_len / 32 + 1; k++) {
            const size_t at = random.uintLessThan<size_t>(buf_len);
            buf[at] = random.int_<uint8_t>();
        }
        break;
    }

    case Generator::mixed: {
        size_t i = 0;
        while (i < buf_len) {
            const size_t drawn = random.intRangeAtMost<size_t>(1, 2048);
            const size_t segment = drawn < buf_len - i ? drawn : buf_len - i;
            const Generator sub = enumValue(random);
            if (sub != Generator::mixed) fill(sub, random, buf + i, segment);
            i += segment;
        }
        break;
    }
    }
}

/* Reusable buffers sized for the largest input a suite generates. */
struct Workspace {
    uint8_t *input;
    size_t input_len;
    uint8_t *encoded;
    size_t encoded_len; /* Wisp: the slice length, i.e. compressBound(max_input). */
    uint8_t *decoded;
    size_t decoded_len;
    lz4::HashTable *table;

    /* Wisp: !Workspace is bool + out. */
    static bool init(zigstd::Allocator alloc, size_t max_input, Workspace *out) {
        uint8_t *input = alloc.allocT<uint8_t>(max_input);
        if (!input) return false;
        size_t bound;
        if (lz4::compressBound(max_input, &bound) != lz4::CompressError::none) {
            alloc.freeT(input, max_input);
            return false;
        }
        uint8_t *encoded = alloc.allocT<uint8_t>(bound);
        if (!encoded) {
            alloc.freeT(input, max_input);
            return false;
        }
        /* One extra byte so wrong-size decompression can be tested above
         * the exact length as well as below it. */
        uint8_t *decoded = alloc.allocT<uint8_t>(max_input + 1);
        if (!decoded) {
            alloc.freeT(encoded, bound);
            alloc.freeT(input, max_input);
            return false;
        }
        lz4::HashTable *table = alloc.create<lz4::HashTable>();
        if (!table) {
            alloc.freeT(decoded, max_input + 1);
            alloc.freeT(encoded, bound);
            alloc.freeT(input, max_input);
            return false;
        }
        out->input = input;
        out->input_len = max_input;
        out->encoded = encoded;
        out->encoded_len = bound;
        out->decoded = decoded;
        out->decoded_len = max_input + 1;
        out->table = table;
        return true;
    }

    void deinit(zigstd::Allocator alloc) {
        alloc.freeT(input, input_len);
        alloc.freeT(encoded, encoded_len);
        alloc.freeT(decoded, decoded_len);
        alloc.destroy(table);
        /* Wisp: `ws.* = undefined` — poison rather than leave live pointers. */
        input = nullptr;
        encoded = nullptr;
        decoded = nullptr;
        table = nullptr;
        input_len = 0;
        encoded_len = 0;
        decoded_len = 0;
    }
};

/* Read one length extension: bytes of 255 accumulate until a terminator
 * below 255, which is included in the sum. */
inline bool readExtension(const uint8_t *encoded, size_t encoded_len, size_t *ip, size_t *out) {
    size_t total = 0;
    for (;;) {
        WISP_LZ4D_EXPECT(*ip < encoded_len);
        const uint8_t value = encoded[*ip];
        *ip += 1;
        /* Wisp: upstream adds into a usize with no overflow check; a u8
         * added to a usize cannot wrap for any block this test builds. */
        total += value;
        if (value != 255) {
            *out = total;
            return true;
        }
    }
}

/* Structurally validate one encoded block against the LZ4 block format and
 * the stricter guarantees documented by our compressor. This deliberately
 * reimplements the format rather than reusing codec internals. */
inline bool expectValidBlock(const uint8_t *encoded, size_t encoded_len, size_t raw_len) {
    size_t ip = 0;
    size_t op = 0;
    size_t last_match_end = 0;

    for (;;) {
        /* Every sequence, including the final one, starts with a token. */
        WISP_LZ4D_EXPECT(ip < encoded_len);
        const uint8_t token = encoded[ip];
        ip += 1;

        size_t literal_len = (size_t)(token >> 4);
        if (literal_len == 15) {
            size_t extension;
            if (!readExtension(encoded, encoded_len, &ip, &extension)) return false;
            literal_len += extension;
        }
        WISP_LZ4D_EXPECT(encoded_len - ip >= literal_len);
        ip += literal_len;
        op += literal_len;

        /* The final sequence contains only literals and ends the block. */
        if (ip == encoded_len) {
            WISP_LZ4D_EXPECT(raw_len == op);
            if (last_match_end > 0) WISP_LZ4D_EXPECT(raw_len - last_match_end >= last_literals);
            return true;
        }

        WISP_LZ4D_EXPECT(encoded_len - ip >= 2);
        /* Wisp: readInt(u16, .., .little) — the codec's own unaligned
         * little-endian read, which this walker must not share. */
        uint16_t offset_le;
        memcpy(&offset_le, encoded + ip, 2);
        const size_t offset = offset_le;
        ip += 2;
        WISP_LZ4D_EXPECT(offset >= 1);
        WISP_LZ4D_EXPECT(offset <= op);

        /* Our compressor starts matches at least `match_find_limit` bytes
         * before the end and never lets one run into the final literals. */
        WISP_LZ4D_EXPECT(op + match_find_limit <= raw_len);
        size_t match_len = (size_t)(token & 0x0F) + 4;
        if ((token & 0x0F) == 15) {
            size_t extension;
            if (!readExtension(encoded, encoded_len, &ip, &extension)) return false;
            match_len += extension;
        }
        op += match_len;
        WISP_LZ4D_EXPECT(op + last_literals <= raw_len);
        last_match_end = op;
    }
}

/* Compress one input and verify every property promised by the codec.
 * Returns the encoded length so callers can reuse the encoded block. */
inline bool expectCodecProperties(Workspace *ws, const uint8_t *input, size_t input_len,
                                  size_t *out_encoded_len) {
    size_t encoded_len;
    WISP_LZ4D_EXPECT(lz4::compress(input, input_len, ws->encoded, ws->encoded_len, *ws->table,
                                   &encoded_len) == lz4::CompressError::none);
    size_t bound;
    WISP_LZ4D_EXPECT(lz4::compressBound(input_len, &bound) == lz4::CompressError::none);
    WISP_LZ4D_EXPECT(encoded_len <= bound);
    const uint8_t *encoded = ws->encoded;

    if (!expectValidBlock(encoded, encoded_len, input_len)) return false;

    /* Exact-size decompression must reproduce the input bit for bit. The
     * output is poisoned first so unwritten bytes cannot pass as correct. */
    uint8_t *output = ws->decoded;
    memset(output, 0xAA, input_len);
    size_t produced;
    WISP_LZ4D_EXPECT(lz4::decompress(encoded, encoded_len, output, input_len, &produced) ==
                     lz4::DecompressError::none);
    WISP_LZ4D_EXPECT(input_len == produced);
    WISP_LZ4D_EXPECT(input_len == 0 || memcmp(input, output, input_len) == 0);

    /* The exact-size contract must reject both smaller and larger buffers. */
    if (input_len > 0) {
        size_t n;
        WISP_LZ4D_EXPECT(lz4::decompress(encoded, encoded_len, ws->decoded, input_len - 1, &n) ==
                         lz4::DecompressError::OutputTooSmall);
    }
    {
        size_t n;
        WISP_LZ4D_EXPECT(lz4::decompress(encoded, encoded_len, ws->decoded, input_len + 1, &n) ==
                         lz4::DecompressError::OutputSizeMismatch);
    }

    *out_encoded_len = encoded_len;
    return true;
}

/* Decode deterministic corruptions of a valid block. Any result is
 * acceptable except memory unsafety, which the safety-checked test build
 * turns into a failure. Mutations may still form a valid block, so output
 * contents are intentionally not asserted. */
inline bool expectMutationSafety(Workspace *ws, zigstd::Random &random, size_t encoded_len,
                                 size_t raw_len, size_t mutations) {
    const zigstd::Allocator alloc = zigstd::testing_allocator();
    uint8_t *original = alloc.dupe(ws->encoded, encoded_len);
    WISP_LZ4D_EXPECT(original != nullptr);

    for (size_t m = 0; m < mutations; m++) {
        uint8_t *block = ws->encoded;
        const size_t block_len = encoded_len;
        if (block_len) memcpy(block, original, block_len);

        switch (random.uintLessThan<uint8_t>(4)) {
        /* Flip up to eight random bits. */
        case 0: {
            const size_t flips = random.intRangeAtMost<size_t>(1, 8);
            for (size_t k = 0; k < flips; k++) {
                const size_t at = random.uintLessThan<size_t>(block_len);
                /* Wisp: `@as(u8, 1) << random.int(u3)` — the shift amount
                 * is a 3-bit draw, so the result stays inside one byte. */
                block[at] ^= (uint8_t)((uint8_t)1 << detail::intU3(random));
            }
            break;
        }
        /* Overwrite one random byte. Token and length bytes are the
         * most interesting targets and small blocks are mostly tokens. */
        case 1: block[random.uintLessThan<size_t>(block_len)] = random.int_<uint8_t>(); break;
        /* Splice random garbage over a random span. */
        case 2: {
            const size_t at = random.uintLessThan<size_t>(block_len);
            const size_t drawn = random.intRangeAtMost<size_t>(1, 16);
            const size_t span = drawn < block_len - at ? drawn : block_len - at;
            random.bytes(block + at, span);
            break;
        }
        /* Decode a random prefix of the intact block. */
        default: break;
        }

        const size_t len = random.boolean() ? random.uintAtMost<size_t>(block_len) : block_len;
        size_t n;
        (void)lz4::decompress(block, len, ws->decoded, raw_len, &n);
    }

    alloc.free(original, encoded_len);
    return true;
}

/* Workload knobs shared by the light and exhaustive suites. */
struct Budget {
    /* Upper bound and step of the contiguous small-size sweep applied to
     * every generator. */
    size_t sweep_max;
    size_t sweep_step;

    /* Number and maximum size of random-parameter inputs. */
    size_t random_inputs;
    size_t random_max;

    /* Explicit match periods checked with `fillPeriodic`. */
    const size_t *periods;
    size_t periods_len;
    size_t period_len;

    /* Number of corrupted decode attempts per mutation base block. */
    size_t mutations;

    size_t maxInput() const {
        const size_t a = period_len > boundary_sizes[boundary_sizes_len - 1]
                             ? period_len
                             : boundary_sizes[boundary_sizes_len - 1];
        return random_max > a ? random_max : a;
    }
};

/* One period per copy strategy: byte propagation (3), pattern words
 * (1/2/4/8), word strides (9), wide strides (17), plus the 64 KiB
 * window edge cases. */
static const size_t light_periods[] = {1, 2, 3, 4, 8, 9, 17, 65534, 65535, 65536, 65537};

static const Budget light_budget = {
    /* .sweep_max = */ 96,
    /* .sweep_step = */ 1,
    /* .random_inputs = */ 24,
    /* .random_max = */ 32 * 1024,
    /* .periods = */ light_periods,
    /* .periods_len = */ sizeof(light_periods) / sizeof(light_periods[0]),
    /* .period_len = */ 160 * 1024,
    /* .mutations = */ 64,
};

static const size_t exhaustive_periods[] = {
    1,     2,     3,    4,     5,     6,     7,     8,
    9,     10,    11,   12,    13,    14,    15,    16,
    17,    18,    19,   23,    24,    31,    32,    33,
    48,    63,    64,   65,    127,   128,   255,   256,
    257,   4095,  4096, 32768, 65533, 65534, 65535, 65536,
    65537, 65538,
};

static const Budget exhaustive_budget = {
    /* .sweep_max = */ 2048,
    /* .sweep_step = */ 1,
    /* .random_inputs = */ 512,
    /* .random_max = */ 512 * 1024,
    /* .periods = */ exhaustive_periods,
    /* .periods_len = */ sizeof(exhaustive_periods) / sizeof(exhaustive_periods[0]),
    /* .period_len = */ 320 * 1024,
    /* .mutations = */ 4096,
};

inline bool runSuite(const Budget &budget, uint64_t seed) {
    zigstd::DefaultPrng prng = zigstd::DefaultPrng::init(seed);
    zigstd::Random random(&prng);

    Workspace ws;
    WISP_LZ4D_EXPECT(Workspace::init(zigstd::testing_allocator(), budget.maxInput(), &ws));
    /* Wisp: `defer ws.deinit(...)` — RAII guard. */
    struct Guard {
        Workspace *ws;
        ~Guard() { ws->deinit(zigstd::testing_allocator()); }
    } guard = {&ws};
    (void)guard;

    // Every generator across every boundary size and the small-size sweep.
    // Small inputs hit the literal-only format edges: below the minimum
    // match sizes, around nibble limits, and around extension steps.
    for (size_t field = 0; field < generator_count; field++) {
        const Generator gen = (Generator)field;

        for (size_t b = 0; b < boundary_sizes_len; b++) {
            const size_t size = boundary_sizes[b];
            uint8_t *input = ws.input;
            fill(gen, random, input, size);
            size_t encoded_len;
            if (!expectCodecProperties(&ws, input, size, &encoded_len)) return false;
        }

        size_t size = 0;
        for (; size <= budget.sweep_max; size += budget.sweep_step) {
            uint8_t *input = ws.input;
            fill(gen, random, input, size);
            size_t encoded_len;
            if (!expectCodecProperties(&ws, input, size, &encoded_len)) return false;
        }
    }

    // Random generator and size pairs, biased toward interesting sizes by
    // squaring so both tiny and large inputs appear.
    for (size_t k = 0; k < budget.random_inputs; k++) {
        const Generator gen = enumValue(random);
        const double scale = detail::floatF64(random);
        /* Wisp: @intFromFloat truncates toward zero. scale is in [0, 1),
         * so the product stays below `random_max` and inside the input
         * buffer; the cast is the same truncation Zig performs. */
        const size_t size = (size_t)(scale * scale * (double)budget.random_max);
        uint8_t *input = ws.input;
        fill(gen, random, input, size);
        size_t encoded_len;
        if (!expectCodecProperties(&ws, input, size, &encoded_len)) return false;

        // Reuse a handful of these encodings as corruption bases.
        if (!expectMutationSafety(&ws, random, encoded_len, size,
                                  budget.mutations / budget.random_inputs + 1))
            return false;
    }

    // Directed match periods, including the 64 KiB window edge where the
    // compressor's 16-bit position arithmetic wraps.
    for (size_t p = 0; p < budget.periods_len; p++) {
        const size_t period = budget.periods[p];
        uint8_t *input = ws.input;
        fillPeriodic(random, input, budget.period_len, period);
        size_t encoded_len;
        if (!expectCodecProperties(&ws, input, budget.period_len, &encoded_len)) return false;
    }

    // Dedicated corruption run over a text-like block, plus an exhaustive
    // truncation sweep: every prefix of a valid block must decode cleanly
    // or fail cleanly.
    {
        const size_t input_len = (16 * 1024) < budget.random_max ? (16 * 1024) : budget.random_max;
        uint8_t *input = ws.input;
        fill(Generator::words, random, input, input_len);
        size_t encoded_len;
        if (!expectCodecProperties(&ws, input, input_len, &encoded_len)) return false;
        if (!expectMutationSafety(&ws, random, encoded_len, input_len, budget.mutations))
            return false;

        for (size_t prefix = 0; prefix < encoded_len; prefix++) {
            size_t n;
            (void)lz4::decompress(ws.encoded, prefix, ws.decoded, input_len, &n);
        }
    }

    return true;
}

} /* namespace lz4_differential */
} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_COMPRESS_LZ4_DIFFERENTIAL_HPP */
