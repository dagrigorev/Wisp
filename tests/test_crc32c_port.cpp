/* Transliterated from the test blocks in Ghostty src/crc32c.zig @ b32f20f3e
 * Copyright (c) Mitchell Hashimoto
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * All five upstream tests, with upstream's names given above each one.
 */

#include "test_helpers.h"
#include "../zigstd/crc32c.hpp"
#include "../zigstd/random.hpp"

#include <vector>

using namespace wisp::zigstd;

/* Wisp: upstream's reference is `std.hash.crc.Crc32Iscsi`
 * (`const Reference = std.hash.crc.Crc32Iscsi;`). The port has no stdlib
 * CRC type, so this stands in for it: a deliberately naive bit-at-a-time
 * CRC32C over the reflected polynomial 0x82F63B78, initial and final XOR
 * 0xFFFFFFFF. It is written for obviousness, not speed, and it shares no
 * code at all with the implementation under test — no slicing tables, no
 * GF(2) matrices — so the comparison is not vacuous. */
struct Reference {
    uint32_t crc;

    void update(const uint8_t *bytes, size_t bytes_len) {
        for (size_t i = 0; i < bytes_len; i++) {
            crc ^= bytes[i];
            for (int bit = 0; bit < 8; bit++) {
                if (crc & 1u) {
                    crc = (crc >> 1) ^ 0x82F63B78u;
                } else {
                    crc = crc >> 1;
                }
            }
        }
    }

    static uint32_t hash(const uint8_t *bytes, size_t bytes_len) {
        Reference r;
        r.crc = 0xFFFFFFFFu;
        r.update(bytes, bytes_len);
        return r.crc ^ 0xFFFFFFFFu;
    }
};

/* Wisp: upstream seeds `std.Random.DefaultPrng.init(seed)` and draws its
 * input bytes with `prng.random().bytes(&bytes)`. src/zigstd/random.hpp is
 * a transliteration of that same Xoshiro256++ and its `fill`, seeded the
 * same way, so the byte sequences here should match upstream's; nothing in
 * these tests depends on that, since what is under test is a property
 * ("equals the reference at every length, alignment and split point") that
 * holds for any input bytes. */
static void fillRandom(uint8_t *buf, size_t len, uint64_t seed) {
    Xoshiro256 prng = DefaultPrng::init(seed);
    Random rand(&prng);
    rand.bytes(buf, len);
}

/* upstream: test "software slicing matches the standard library" */
TEST(crc32c, software_slicing_matches_the_standard_library) {
    /* The selected backend may be hardware, so cover the sliced software
     * path directly: every length around the sixteen-byte boundary, several
     * alignments, and continuation across arbitrary split points. */
    uint8_t bytes[512 + 19];
    fillRandom(bytes, sizeof(bytes), 0x511C);

    for (size_t len = 0; len < 64 + 1; len++) {
        for (size_t offset = 0; offset < 4; offset++) {
            const uint8_t *input = bytes + offset;
            Reference reference;
            reference.crc = 0xFFFFFFFFu;
            reference.update(input, len);
            ASSERT_TRUE(reference.crc == Software::update(0xFFFFFFFFu, input, len));
        }
    }

    const uint8_t *long_ = bytes;
    const size_t long_len = 512;
    Reference reference;
    reference.crc = 0xFFFFFFFFu;
    reference.update(long_, long_len);
    const size_t splits[] = { 0, 1, 15, 16, 17, 100, 511, 512 };
    for (size_t s = 0; s < sizeof(splits) / sizeof(splits[0]); s++) {
        const size_t split = splits[s];
        const uint32_t first = Software::update(0xFFFFFFFFu, long_, split);
        ASSERT_TRUE(reference.crc == Software::update(first, long_ + split, long_len - split));
    }
}

/* upstream: test "software multi-stream matches the standard library" */
TEST(crc32c, software_multi_stream_matches_the_standard_library) {
    /* Lengths around and far above the interleaving threshold, plus odd
     * remainders, so all three streams and both combine steps are covered. */
    std::vector<uint8_t> bytes(96 * 1024);
    fillRandom(bytes.data(), bytes.size(), 0x35171A3Bull);

    const size_t lens[] = {
        Software::multi_stream_threshold - 1,
        Software::multi_stream_threshold,
        Software::multi_stream_threshold + 1,
        Software::multi_stream_threshold + 97,
        12 * 1024,
        64 * 1024 + 31,
        96 * 1024, /* bytes.len */
    };
    for (size_t li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
        const size_t len = lens[li];
        const uint8_t *input = bytes.data();
        Reference reference;
        reference.crc = 0xFFFFFFFFu;
        reference.update(input, len);
        ASSERT_TRUE(reference.crc == Software::update(0xFFFFFFFFu, input, len));

        /* Continuation across a split inside the multi-stream range. */
        const uint32_t first = Software::update(0xFFFFFFFFu, input, len / 2);
        ASSERT_TRUE(reference.crc == Software::update(first, input + len / 2, len - len / 2));
    }
}

/* upstream: test "matches the check value" */
TEST(crc32c, matches_the_check_value) {
    /* The catalog check value for CRC-32/ISCSI. */
    ASSERT_TRUE(0xE3069283u == Crc32c::hash((const uint8_t *)"123456789", 9));
}

/* upstream: test "matches the standard library at every length and split" */
TEST(crc32c, matches_the_standard_library_at_every_length_and_split) {
    uint8_t bytes[259];
    fillRandom(bytes, sizeof(bytes), 0xC5C32Cull);

    for (size_t len = 0; len < sizeof(bytes) + 1; len++) {
        const uint8_t *input = bytes;
        ASSERT_TRUE(Reference::hash(input, len) == Crc32c::hash(input, len));

        /* Streaming across arbitrary split points must not change the
         * result: word batching may not leak state between updates. */
        Crc32c split = Crc32c::init();
        split.update(input, len / 3);
        split.update(input + len / 3, (len - len / 3) - len / 3);
        split.update(input + (len - len / 3), len - (len - len / 3));
        ASSERT_TRUE(Reference::hash(input, len) == split.final_());
    }
}

/* upstream: test "matches the standard library at every alignment" */
TEST(crc32c, matches_the_standard_library_at_every_alignment) {
    uint8_t bytes[64 + 16];
    fillRandom(bytes, sizeof(bytes), 0xA11Cull);

    for (size_t offset = 0; offset < 16; offset++) {
        const uint8_t *input = bytes + offset;
        ASSERT_TRUE(Reference::hash(input, 64) == Crc32c::hash(input, 64));
    }
}
