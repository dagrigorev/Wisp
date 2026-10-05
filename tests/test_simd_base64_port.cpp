/* Transliterated from the test blocks in Ghostty src/simd/base64.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include <string.h>

#include <string>

#include "test_helpers.h"
#include "../simd/base64.hpp"

using namespace wisp;
using namespace wisp::simd;

typedef base64::Streaming Streaming;
typedef base64::Padding Padding;

static const uint8_t *u(const char *s) { return (const uint8_t *)s; }

/* Wisp: try decodeStrict(input, &output, padding) as a std::string, with
 * `ok` reporting whether the decode succeeded. */
static std::string strict(const char *input, size_t input_len, Padding padding, bool *ok) {
    uint8_t output[128];
    size_t out_len = 0;
    *ok = base64::decodeStrict(u(input), input_len, output, sizeof output, padding, &out_len);
    if (!*ok) return std::string();
    return std::string((const char *)output, out_len);
}
static std::string strict(const char *input, Padding padding, bool *ok) {
    return strict(input, strlen(input), padding, ok);
}

TEST(base64, base64_maxLen) {
    const size_t len = base64::maxLen(u("aGVsbG8gd29ybGQ="), 16);
    ASSERT_TRUE(11 == len);
}

TEST(base64, base64_empty_input) {
    uint8_t output[1];
    size_t out_len = 0;

    ASSERT_TRUE(0 == base64::maxLen(u(""), 0));
    ASSERT_TRUE(base64::decode(u(""), 0, output, 0, &out_len));
    ASSERT_TRUE(0 == out_len);
}

TEST(base64, base64_decode) {
    const char *input = "aGVsbG8gd29ybGQ=";
    const size_t input_len = strlen(input);
    const size_t len = base64::maxLen(u(input), input_len);
    uint8_t output[64];
    size_t out_len = 0;
    ASSERT_TRUE(base64::decode(u(input), input_len, output, len, &out_len));
    ASSERT_TRUE(out_len == 11);
    ASSERT_TRUE(memcmp(output, "hello world", 11) == 0);
}

TEST(base64, base64_strict_decode_valid) {
    struct Case {
        const char *input;
        const char *expect;
        size_t expect_len;
    };
    static const char all_alphabet_expect[] =
        "\x00\x10\x83\x10\x51\x87\x20\x92\x8b\x30\xd3\x8f\x41\x14\x93\x51\x55\x97\x61\x96\x9b\x71\xd7\x9f\x82"
        "\x18\xa3\x92\x59\xa7\xa2\x9a\xab\xb2\xdb\xaf\xc3\x1c\xb3\xd3\x5d\xb7\xe3\x9e\xbb\xf3\xdf\xbf";
    static const Case cases[] = {
        {"", "", 0},
        {"aGVsbG8gd29ybGQ=", "hello world", 11},
        {"bGlnaHQgdw==", "light w", 7},
        {"Zm9vYmFy", "foobar", 6},
        /* Every alphabet character in one input. */
        {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", all_alphabet_expect,
         sizeof(all_alphabet_expect) - 1},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const Case &c = cases[i];
        const std::string want(c.expect, c.expect_len);
        bool ok = false;
        ASSERT_TRUE(strict(c.input, Padding::required, &ok) == want);
        ASSERT_TRUE(ok);
        ASSERT_TRUE(strict(c.input, Padding::optional, &ok) == want);
        ASSERT_TRUE(ok);
    }
}

TEST(base64, base64_strict_decode_invalid) {
    /* The invalid inputs from kitty's own strict decoding tests plus
     * some extra padding-placement cases. All of these are invalid for
     * both padding requirements. */
    static const char *const cases[] = {
        "bGlnaHQgdw=",    /* missing one padding byte */
        "bGln!!Qgdw==",   /* invalid characters */
        "bGlnaHQgdw==\n", /* trailing whitespace */
        "\nbGlnaHQgdw==", /* leading whitespace */
        "bGlnaHQg dw==",  /* interior whitespace */
        "!!!!",
        "=",
        "==",
        "A===",
        "AB=C", /* padding must be the suffix */
        "Zm9v YmFy",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        bool ok = true;
        (void)strict(cases[i], Padding::required, &ok);
        ASSERT_TRUE(!ok);
        ok = true;
        (void)strict(cases[i], Padding::optional, &ok);
        ASSERT_TRUE(!ok);
    }

    /* Unpadded input is only tolerated when padding is optional. A
     * single leftover character can never be decoded. */
    bool ok = true;
    (void)strict("bGlnaHQgdw", Padding::required, &ok);
    ASSERT_TRUE(!ok);
    ASSERT_TRUE(strict("bGlnaHQgdw", Padding::optional, &ok) == "light w");
    ASSERT_TRUE(ok);
    ok = true;
    (void)strict("bGl", Padding::required, &ok);
    ASSERT_TRUE(!ok);
    ASSERT_TRUE(strict("bGl", Padding::optional, &ok) == "li");
    ASSERT_TRUE(ok);
    ok = true;
    (void)strict("bGlna", Padding::optional, &ok);
    ASSERT_TRUE(!ok);
}

/* Wisp: try s.feed(input, &output) as a std::string, with `ok` reporting
 * whether the feed succeeded. */
static std::string feed(Streaming *s, const char *input, size_t input_len, uint8_t *output, size_t output_len,
                        bool *ok) {
    size_t out_len = 0;
    *ok = s->feed(u(input), input_len, output, output_len, &out_len);
    if (!*ok) return std::string();
    return std::string((const char *)output, out_len);
}

TEST(base64, base64_streaming_decode_chunk_boundaries) {
    /* Decoding a stream split at every possible boundary, including
     * one byte at a time, matches the single-shot decode. */
    const char *input = "c29tZSBsb25nZXIgZGF0YSB3aXRoIHBhZGRpbmc+Pz8=";
    const size_t input_len = strlen(input);
    const char *expect = "some longer data with padding>??";
    for (size_t split = 0; split <= input_len; split++) {
        Streaming s;
        std::string result;
        uint8_t output[64];
        bool ok = false;
        result += feed(&s, input, split, output, sizeof output, &ok);
        ASSERT_TRUE(ok);
        result += feed(&s, input + split, input_len - split, output, sizeof output, &ok);
        ASSERT_TRUE(ok);
        ASSERT_TRUE(s.finish());
        ASSERT_STR_EQ(expect, result.c_str());
    }
    {
        Streaming s;
        std::string result;
        uint8_t output[4];
        for (size_t i = 0; i < input_len; i++) {
            bool ok = false;
            result += feed(&s, input + i, 1, output, sizeof output, &ok);
            ASSERT_TRUE(ok);
        }
        ASSERT_TRUE(s.finish());
        ASSERT_STR_EQ(expect, result.c_str());
    }
}

TEST(base64, base64_streaming_decode_invalid) {
    uint8_t output[64];
    bool ok = true;

    /* Invalid characters are rejected wherever they appear. */
    {
        Streaming s;
        (void)feed(&s, "!!!!", 4, output, sizeof output, &ok);
        ASSERT_TRUE(!ok);
    }
    {
        Streaming s;
        ok = true;
        (void)feed(&s, "SGVs!!!bG8=", 11, output, sizeof output, &ok);
        ASSERT_TRUE(!ok);
    }
    {
        Streaming s;
        ok = true;
        (void)feed(&s, "\nc29tZSBkYXRh", 13, output, sizeof output, &ok);
        ASSERT_TRUE(!ok);
    }

    /* Data after terminal padding within one feed is rejected, even
     * when the padded group only completes in that feed. */
    {
        Streaming s;
        ok = true;
        (void)feed(&s, "Z29vZA==SGVsbG8=", 16, output, sizeof output, &ok);
        ASSERT_TRUE(!ok);
    }
    {
        Streaming s;
        (void)feed(&s, "Z29vZA=", 7, output, sizeof output, &ok);
        ASSERT_TRUE(ok);
        (void)feed(&s, "=SGVs", 5, output, sizeof output, &ok);
        ASSERT_TRUE(!ok);
    }

    /* A feed that ends exactly at terminal padding resets the stream:
     * the next feed starts fresh, so clients that pad every chunk
     * independently keep working (matching the kitty implementation,
     * which resets its streaming decoder on EOF). */
    {
        Streaming s;
        ASSERT_TRUE(feed(&s, "Z29vZA==", 8, output, sizeof output, &ok) == "good");
        ASSERT_TRUE(ok);
        ASSERT_TRUE(feed(&s, "SGVsbG8=", 8, output, sizeof output, &ok) == "Hello");
        ASSERT_TRUE(ok);
        ASSERT_TRUE(s.finish());
    }
    {
        /* Padding split across feeds resets too. */
        Streaming s;
        ASSERT_TRUE(feed(&s, "Z29vZA=", 7, output, sizeof output, &ok) == "goo");
        ASSERT_TRUE(ok);
        ASSERT_TRUE(feed(&s, "=", 1, output, sizeof output, &ok) == "d");
        ASSERT_TRUE(ok);
        ASSERT_TRUE(feed(&s, "bW9yZQ==", 8, output, sizeof output, &ok) == "more");
        ASSERT_TRUE(ok);
        ASSERT_TRUE(s.finish());
    }

    /* Misplaced padding within a group. */
    {
        Streaming s;
        ok = true;
        (void)feed(&s, "YQ=X", 4, output, sizeof output, &ok);
        ASSERT_TRUE(!ok);
    }
    {
        Streaming s;
        ok = true;
        (void)feed(&s, "=AAA", 4, output, sizeof output, &ok);
        ASSERT_TRUE(!ok);
    }

    /* A stream that ends in a partial group is missing its padding.
     * The failed finish resets the decoder for the next stream. */
    {
        Streaming s;
        ASSERT_TRUE(feed(&s, "SGVsbG8", 7, output, sizeof output, &ok) == "Hel");
        ASSERT_TRUE(ok);
        ASSERT_TRUE(!s.finish());
        ASSERT_TRUE(feed(&s, "SGVsbG8", 7, output, sizeof output, &ok) == "Hel");
        ASSERT_TRUE(ok);
        ASSERT_TRUE(feed(&s, "=", 1, output, sizeof output, &ok) == "lo");
        ASSERT_TRUE(ok);
        ASSERT_TRUE(s.finish());
    }
}
