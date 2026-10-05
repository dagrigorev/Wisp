/* Transliterated from the test blocks in Ghostty src/simd/vt.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names. "decode simd
 * matches scalar" skips upstream when simd is off, which is this build.
 */

#include "test_helpers.h"
#include "../simd/vt.hpp"

#include <string>

using namespace wisp::simd::vt;

static DecodeResult dec(const std::string &s, uint32_t *out) {
    return utf8DecodeUntilControlSeq((const uint8_t *)s.data(), s.size(), out);
}

static std::string rep(const char *s, int n) {
    std::string r;
    for (int i = 0; i < n; i++) r += s;
    return r;
}

TEST(simd_vt, decode_no_escape) {
    static uint32_t output[1024];
    const std::string str = rep("hello", 128);
    const DecodeResult r = dec(str, output);
    ASSERT_TRUE(r.consumed == str.size() && r.decoded == str.size());
}

TEST(simd_vt, decode_ASCII_to_escape) {
    static uint32_t output[1024];
    const std::string prefix = rep("hello", 64);
    const std::string str = prefix + "\x1b" + rep("world", 64);
    const DecodeResult r = dec(str, output);
    ASSERT_TRUE(r.consumed == prefix.size() && r.decoded == prefix.size());
}

TEST(simd_vt, decode_immediate_esc_sequence) {
    uint32_t output[64];
    const DecodeResult r = dec("\x1b[?5s", output);
    ASSERT_TRUE(r.consumed == 0 && r.decoded == 0);
}

TEST(simd_vt, decode_incomplete_UTF_8) {
    uint32_t output[64];

    /* 2-byte truncated at end of buffer */
    {
        const DecodeResult r = dec("hello\xc2", output);
        ASSERT_TRUE(r.consumed == 5 && r.decoded == 5);
    }

    /* 3-byte: \xe0 expects A0-BF next, but \x00 is not in range.
     * \xe0 is a maximal subpart of length 1 → FFFD, then \x00 is ASCII NUL. */
    {
        const DecodeResult r = dec(std::string("hello\xe0\x00", 7), output);
        ASSERT_TRUE(r.consumed == 7);
        ASSERT_TRUE(r.decoded == 7);
        ASSERT_TRUE(output[5] == 0xFFFD);
        ASSERT_TRUE(output[6] == 0x00);
    }

    /* 4-byte truncated at end of buffer (F0 90 is valid so far) */
    {
        const DecodeResult r = dec("hello\xf0\x90", output);
        ASSERT_TRUE(r.consumed == 5 && r.decoded == 5);
    }
}

TEST(simd_vt, decode_invalid_UTF_8) {
    uint32_t output[64];

    /* Invalid leading 2-byte sequence */
    {
        const DecodeResult r = dec("hello\xc2\x01", output);
        ASSERT_TRUE(r.consumed == 7 && r.decoded == 7);
    }

    /* Replacement will only replace the invalid leading byte. */
    ASSERT_TRUE(output[5] == 0xFFFD);
    ASSERT_TRUE(output[6] == 0x01);
}

/* Per the maximal subpart spec, bytes F5-FF are each replaced with FFFD. */
TEST(simd_vt, decode_invalid_leading_byte_is_replaced) {
    uint32_t output[64];
    const DecodeResult r = dec("hello\xFF", output);
    ASSERT_TRUE(r.consumed == 6);
    ASSERT_TRUE(r.decoded == 6);
    ASSERT_TRUE(output[5] == 0xFFFD);
}

TEST(simd_vt, decode_invalid_continuation_in_3_byte_sequence) {
    uint32_t output[64];
    /* \xe2 expects two continuation bytes, \x28 is not one */
    const DecodeResult r = dec("hello\xe2\x28world", output);
    /* "hello" + replacement + "(" + "world" = 12 codepoints */
    ASSERT_TRUE(r.decoded == 12);
    ASSERT_TRUE(output[5] == 0xFFFD);
    ASSERT_TRUE(output[6] == '(');
    ASSERT_TRUE(output[7] == 'w');
}

TEST(simd_vt, decode_invalid_continuation_in_4_byte_sequence) {
    uint32_t output[64];
    /* \xf0\x90 is a valid prefix of a 4-byte sequence, but \x28 breaks it.
     * Maximal subpart is F0 90 (length 2) → single FFFD, then '(' proceeds. */
    const DecodeResult r = dec("hello\xf0\x90\x28world", output);
    ASSERT_TRUE(r.decoded == 12);
    ASSERT_TRUE(output[5] == 0xFFFD);
    ASSERT_TRUE(output[6] == '(');
    ASSERT_TRUE(output[7] == 'w');
}

TEST(simd_vt, decode_multiple_consecutive_invalid_bytes) {
    uint32_t output[64];

    /* Each lone continuation byte is its own maximal subpart → one FFFD each. */
    {
        const DecodeResult r = dec("a\x80\x80" "b", output);
        ASSERT_TRUE(r.decoded == 4);
        ASSERT_TRUE(output[0] == 'a');
        ASSERT_TRUE(output[1] == 0xFFFD);
        ASSERT_TRUE(output[2] == 0xFFFD);
        ASSERT_TRUE(output[3] == 'b');
    }

    /* C0 is an invalid lead byte (< C2), each byte gets its own FFFD. */
    {
        const DecodeResult r = dec("a\xc0\xc0" "b", output);
        ASSERT_TRUE(r.decoded == 4);
        ASSERT_TRUE(output[0] == 'a');
        ASSERT_TRUE(output[1] == 0xFFFD);
        ASSERT_TRUE(output[2] == 0xFFFD);
        ASSERT_TRUE(output[3] == 'b');
    }
}

TEST(simd_vt, decode_unexpected_continuation_byte_as_lead) {
    uint32_t output[64];
    /* 0x80 is a continuation byte appearing as a lead byte */
    const DecodeResult r = dec("a\x80" "b", output);
    ASSERT_TRUE(r.decoded == 3);
    ASSERT_TRUE(output[0] == 'a');
    ASSERT_TRUE(output[1] == 0xFFFD);
    ASSERT_TRUE(output[2] == 'b');
}

TEST(simd_vt, decode_overlong_2_byte_encoding) {
    uint32_t output[64];
    /* \xc0\xaf: C0 is invalid lead (< C2) → FFFD, AF is lone continuation → FFFD
     * Per Table 3-8: C0 AF → FFFD FFFD */
    const DecodeResult r = dec("a\xc0\xaf" "b", output);
    ASSERT_TRUE(r.decoded == 4);
    ASSERT_TRUE(output[0] == 'a');
    ASSERT_TRUE(output[1] == 0xFFFD);
    ASSERT_TRUE(output[2] == 0xFFFD);
    ASSERT_TRUE(output[3] == 'b');
}

TEST(simd_vt, decode_surrogate_half) {
    uint32_t output[64];
    /* \xed\xa0\x80 encodes U+D800 (a surrogate). Per Table 3-7, after ED
     * the next byte must be 80-9F. A0 is out of range, so ED is a maximal
     * subpart of length 1 → FFFD. Then A0 and 80 are lone continuations
     * → FFFD each. Per Table 3-9: ED A0 80 → FFFD FFFD FFFD */
    const DecodeResult r = dec("a\xed\xa0\x80" "b", output);
    ASSERT_TRUE(r.decoded == 5);
    ASSERT_TRUE(output[0] == 'a');
    ASSERT_TRUE(output[1] == 0xFFFD);
    ASSERT_TRUE(output[2] == 0xFFFD);
    ASSERT_TRUE(output[3] == 0xFFFD);
    ASSERT_TRUE(output[4] == 'b');
}

TEST(simd_vt, decode_valid_multibyte_surrounded_by_invalid) {
    uint32_t output[64];
    /* \xc3\xa9 = é (U+00E9), surrounded by invalid continuation bytes */
    const DecodeResult r = dec("\x80\xc3\xa9\x80", output);
    ASSERT_TRUE(r.decoded == 3);
    ASSERT_TRUE(output[0] == 0xFFFD);
    ASSERT_TRUE(output[1] == 0x00E9);
    ASSERT_TRUE(output[2] == 0xFFFD);
}

TEST(simd_vt, decode_partial_UTF_8_before_escape) {
    /* A valid-so-far but incomplete sequence cut off by an ESC can
     * never be completed, so it is consumed and replaced by a single
     * U+FFFD (maximal subpart) rather than left pending. Only
     * sequences cut off by the true end of input are left pending. */
    uint32_t output[64];

    /* 2-byte lead cut off by ESC. */
    {
        const DecodeResult r = dec("hi\xc2\x1b[0m", output);
        ASSERT_TRUE(r.consumed == 3);
        ASSERT_TRUE(r.decoded == 3);
        ASSERT_TRUE(output[2] == 0xFFFD);
    }

    /* 3-byte lead plus one valid continuation cut off by ESC:
     * the whole prefix is one maximal subpart, one U+FFFD. */
    {
        const DecodeResult r = dec("\xe0\xa0\x1bX", output);
        ASSERT_TRUE(r.consumed == 2);
        ASSERT_TRUE(r.decoded == 1);
        ASSERT_TRUE(output[0] == 0xFFFD);
    }
}

TEST(simd_vt, decode_invalid_byte_before_escape) {
    uint32_t output[64];
    /* Invalid byte followed by ESC - should replace then stop */
    const DecodeResult r = dec("hi\x80\x1b[0m", output);
    ASSERT_TRUE(r.consumed == 3);
    ASSERT_TRUE(r.decoded == 3);
    ASSERT_TRUE(output[0] == 'h');
    ASSERT_TRUE(output[1] == 'i');
    ASSERT_TRUE(output[2] == 0xFFFD);
}

/* Unicode Table 3-8: U+FFFD for Non-Shortest Form Sequences */
TEST(simd_vt, Table_3_8_non_shortest_form_sequences) {
    uint32_t output[64];
    const DecodeResult r = dec("\xC0\xAF\xE0\x80\xBF\xF0\x81\x82\x41", output);
    ASSERT_TRUE(r.consumed == 9);
    ASSERT_TRUE(r.decoded == 9);
    for (int i = 0; i < 8; i++) ASSERT_TRUE(output[i] == 0xFFFD);
    ASSERT_TRUE(output[8] == 0x41);
}

/* Unicode Table 3-9: U+FFFD for Ill-Formed Sequences for Surrogates */
TEST(simd_vt, Table_3_9_surrogate_sequences) {
    uint32_t output[64];
    const DecodeResult r = dec("\xED\xA0\x80\xED\xBF\xBF\xED\xAF\x41", output);
    ASSERT_TRUE(r.consumed == 9);
    ASSERT_TRUE(r.decoded == 9);
    for (int i = 0; i < 8; i++) ASSERT_TRUE(output[i] == 0xFFFD);
    ASSERT_TRUE(output[8] == 0x41);
}

/* Unicode Table 3-10: U+FFFD for Other Ill-Formed Sequences */
TEST(simd_vt, Table_3_10_other_ill_formed_sequences) {
    uint32_t output[64];
    const DecodeResult r = dec("\xF4\x91\x92\x93\xFF\x41\x80\xBF\x42", output);
    ASSERT_TRUE(r.consumed == 9);
    ASSERT_TRUE(r.decoded == 9);
    const uint32_t want[] = { 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0x41, 0xFFFD, 0xFFFD, 0x42 };
    for (int i = 0; i < 9; i++) ASSERT_TRUE(output[i] == want[i]);
}

/* Unicode Table 3-11: U+FFFD for Truncated Sequences */
TEST(simd_vt, Table_3_11_truncated_sequences) {
    uint32_t output[64];
    const DecodeResult r = dec("\xE1\x80\xE2\xF0\x91\x92\xF1\xBF\x41", output);
    ASSERT_TRUE(r.consumed == 9);
    ASSERT_TRUE(r.decoded == 5);
    const uint32_t want[] = { 0xFFFD, 0xFFFD, 0xFFFD, 0xFFFD, 0x41 };
    for (int i = 0; i < 5; i++) ASSERT_TRUE(output[i] == want[i]);
}
