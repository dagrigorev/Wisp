/* Transliterated from the test blocks in Zig 0.16.0 lib/std/hash/wyhash.zig
 * and lib/std/Random/Xoshiro256.zig
 * Copyright (c) Zig contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests, except "smhasher" (needs std.hash verify.zig), the
 * comptime variants, and the Xoshiro256 jump() half of "sequence" (jump is
 * not ported).
 */

#include "test_helpers.h"
#include "../zigstd/wyhash.hpp"
#include "../zigstd/random.hpp"

#include <string>

using namespace wisp::zigstd;

TEST(wyhash, test_vectors) {
    struct V { uint64_t expected; uint64_t seed; const char *input; };
    /* Run https://github.com/wangyi-fudan/wyhash/blob/77e50f267fbc7b8e2d09f2d455219adb70ad4749/test_vector.cpp directly. */
    const V vectors[] = {
        { 0x409638ee2bde459ull, 0, "" },
        { 0xa8412d091b5fe0a9ull, 1, "a" },
        { 0x32dd92e4b2915153ull, 2, "abc" },
        { 0x8619124089a3a16bull, 3, "message digest" },
        { 0x7a43afb61d7f5f40ull, 4, "abcdefghijklmnopqrstuvwxyz" },
        { 0xff42329b90e50d58ull, 5, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789" },
        { 0xc39cab13b115aad3ull, 6, "12345678901234567890123456789012345678901234567890123456789012345678901234567890" },
    };
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
        ASSERT_TRUE(vectors[i].expected == Wyhash::hash(vectors[i].seed, vectors[i].input, strlen(vectors[i].input)));
    }
}

TEST(wyhash, iterative_maintains_last_sixteen) {
    const std::string input = std::string(48, 'Z') + "01234567890abcdefg";
    const uint64_t seed = 0;

    for (size_t i = 0; i < 17; i++) {
        const size_t len = input.size() - i;
        const uint64_t non_iterative_hash = Wyhash::hash(seed, input.data(), len);

        Wyhash wh = Wyhash::init(seed);
        wh.update((const uint8_t *)input.data(), len);
        const uint64_t iterative_hash = wh.final_();

        ASSERT_TRUE(non_iterative_hash == iterative_hash);
    }
}

TEST(Xoshiro256, sequence) {
    Xoshiro256 r = Xoshiro256::init(0);

    const uint64_t seq1[] = {
        0x53175d61490b23dfull,
        0x61da6f3dc380d507ull,
        0x5c0fdf91ec9a7bfcull,
        0x02eebf8c3bbe5e1aull,
        0x7eca04ebaf4a5eeaull,
        0x0543c37757f08d9aull,
    };

    for (size_t i = 0; i < 6; i++) ASSERT_TRUE(seq1[i] == r.next());
}

TEST(Xoshiro256, fill) {
    Xoshiro256 r = Xoshiro256::init(0);

    const uint64_t seq[] = {
        0x53175d61490b23dfull,
        0x61da6f3dc380d507ull,
        0x5c0fdf91ec9a7bfcull,
        0x02eebf8c3bbe5e1aull,
        0x7eca04ebaf4a5eeaull,
        0x0543c37757f08d9aull,
    };

    for (size_t i = 0; i < 6; i++) {
        uint8_t buf0[8];
        uint8_t buf1[7];
        for (int k = 0; k < 8; k++) buf0[k] = (uint8_t)(seq[i] >> (8 * k));
        r.fill(buf1, 7);
        ASSERT_TRUE(memcmp(buf0, buf1, 7) == 0);
    }
}
