/* Transliterated from Ghostty src/terminal/snapshot/continuation.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Wisp: tests follow the continuation.zig test structure.
 */

#include <stdint.h>
#include <string.h>

#include <string>

#include "test_helpers.h"
#include "snapshot_fixture.hpp"
#include "../src/terminal/snapshot/continuation.hpp"
#include "../src/terminal/snapshot/record.hpp"
#include "../src/zigstd/reader.hpp"

using namespace wisp;

namespace continuation = wisp::terminal::snapshot::continuation;
namespace record = wisp::terminal::snapshot::record;
namespace fixture = wisp::test_fixture;

typedef zigstd::Reader Reader;
typedef zigstd::ReadError ReadError;

#ifndef WISP_SNAPSHOT_TESTDATA
#define WISP_SNAPSHOT_TESTDATA "snapshot_testdata"
#endif

static std::string fixturePath(const char *name) {
    std::string p = WISP_SNAPSHOT_TESTDATA;
    p += "/";
    p += name;
    return p;
}

/* Test fixtures loaded from hex files. */
static std::string test_ground_fixture;
static std::string test_utf8_fixture;
static std::string test_esc_fixture;
static std::string test_csi_fixture;
static std::string test_osc_fixture;
static std::string test_dcs_fixture;
static std::string test_apc_fixture;

static void loadTestFixtures() {
    static bool loaded = false;
    if (!loaded) {
        const char *fixtures[] = {
            "continuation-ground-v1.hex",
            "continuation-utf8-v1.hex",
            "continuation-esc-v1.hex",
            "continuation-csi-v1.hex",
            "continuation-osc-v1.hex",
            "continuation-dcs-v1.hex",
            "continuation-apc-v1.hex",
        };
        std::string *fixture_ptrs[] = {
            &test_ground_fixture,
            &test_utf8_fixture,
            &test_esc_fixture,
            &test_csi_fixture,
            &test_osc_fixture,
            &test_dcs_fixture,
            &test_apc_fixture,
        };

        for (size_t i = 0; i < 7; i++) {
            std::string path = fixturePath(fixtures[i]);
            std::string hex_content;
            ASSERT_TRUE(fixture::readFile(path.c_str(), &hex_content));
            ASSERT_TRUE(fixture::parse(hex_content.c_str(), hex_content.size(),
                                      fixture_ptrs[i]));
        }
        loaded = true;
    }
}

/* ─── continuation.zig ───────────────────────────────────────────────── */

TEST(snapshot_continuation, golden_records) {
    loadTestFixtures();

    struct TestCase {
        const char *fixture_name;
        continuation::Value value;
        const std::string *expected;
    };

    const uint8_t utf8_bytes[] = {0xF0, 0x9F, 0x98};
    const uint8_t esc_bytes[] = {0x1b};
    const uint8_t csi_bytes[] = {0x1b, '[', '3', '1'};
    const uint8_t osc_bytes[] = {0x1b, ']', '2', ';', 't', 'i', 't', 'l', 'e'};
    const uint8_t dcs_bytes[] = {0x1b, 'P', 'q', 'd', 'a', 't', 'a'};
    const uint8_t apc_bytes[] = {0x1b, '_', 'G', 'd', 'a', 't', 'a'};

    TestCase tests[] = {
        {"continuation-ground-v1.hex", continuation::Value::makeGround(), &test_ground_fixture},
        {"continuation-utf8-v1.hex", continuation::Value::makeBytes(utf8_bytes, 3),
         &test_utf8_fixture},
        {"continuation-esc-v1.hex", continuation::Value::makeBytes(esc_bytes, 1),
         &test_esc_fixture},
        {"continuation-csi-v1.hex", continuation::Value::makeBytes(csi_bytes, 4),
         &test_csi_fixture},
        {"continuation-osc-v1.hex", continuation::Value::makeBytes(osc_bytes, 9),
         &test_osc_fixture},
        {"continuation-dcs-v1.hex", continuation::Value::makeBytes(dcs_bytes, 7),
         &test_dcs_fixture},
        {"continuation-apc-v1.hex", continuation::Value::makeBytes(apc_bytes, 7),
         &test_apc_fixture},
    };

    for (size_t i = 0; i < 7; i++) {
        std::string encoded;
        record::Writer stream;
        stream.init(&encoded);

        continuation::EncodeError err = continuation::encode(tests[i].value, &stream);
        ASSERT_EQ(err, continuation::EncodeError::none);

        ASSERT_EQ(encoded.size(), tests[i].expected->size());

        for (size_t j = 0; j < encoded.size(); j++) {
            if ((uint8_t)encoded[j] != (uint8_t)(*tests[i].expected)[j]) {
                fprintf(stderr,
                        "Test %zu (%s): byte %zu mismatch: got %02x expected %02x\n",
                        i, tests[i].fixture_name, j, (uint8_t)encoded[j],
                        (uint8_t)(*tests[i].expected)[j]);
                ASSERT(false);
            }
        }
    }
}

TEST(snapshot_continuation, validates_supported_unfinished_states) {
    const uint8_t test_cases[][32] = {
        {0x1b},                                    /* ESC */
        {0x1b, '[', '3', '1'},                    /* CSI */
        {0x1b, ']', '2', ';', 't', 'i', 't', 'l', 'e'},  /* OSC */
        {0x1b, 'P', 'q', 'd', 'a', 't', 'a'},    /* DCS */
        {0x1b, '_', 'G', 'd', 'a', 't', 'a'},    /* APC */
        {0xF0, 0x9F, 0x98},                       /* UTF-8 */
    };
    size_t lengths[] = {1, 4, 9, 7, 7, 3};

    for (size_t i = 0; i < 6; i++) {
        continuation::Value v = continuation::Value::makeBytes(test_cases[i], lengths[i]);
        continuation::ValidateError err = continuation::validate(v);
        ASSERT_EQ(err, continuation::ValidateError::none);
    }
}

TEST(snapshot_continuation, empty_bytes_encode_and_decode_as_ground) {
    loadTestFixtures();

    continuation::ValidateError verr = continuation::validate(continuation::Value::makeBytes((const uint8_t *)"", 0));
    ASSERT_EQ(verr, continuation::ValidateError::none);

    std::string encoded;
    record::Writer stream;
    stream.init(&encoded);

    continuation::EncodeError err = continuation::encode(continuation::Value::makeBytes((const uint8_t *)"", 0), &stream);
    ASSERT_EQ(err, continuation::EncodeError::none);

    ASSERT_EQ(encoded.size(), test_ground_fixture.size());

    /* Decode and verify it's ground. */
    Reader source = Reader::fixed(encoded.data(), encoded.size());
    continuation::Value decoded;
    continuation::DecodeError decode_err = continuation::decode(&source, &decoded, 0);
    ASSERT_EQ(decode_err, continuation::DecodeError::none);
    ASSERT_EQ(decoded.tag, continuation::Value::Tag::ground);
}

TEST(snapshot_continuation, rejects_invalid_semantic_shapes) {
    /* Complete SGR (ground). */
    {
        continuation::Value v = continuation::Value::makeBytes((const uint8_t *)"\x1b[31m", 5);
        continuation::ValidateError err = continuation::validate(v);
        ASSERT_EQ(err, continuation::ValidateError::NoPendingState);
    }

    /* CSI with BEL terminator. */
    {
        continuation::Value v = continuation::Value::makeBytes((const uint8_t *)"\x1b[31\x07", 5);
        continuation::ValidateError err = continuation::validate(v);
        ASSERT_EQ(err, continuation::ValidateError::ReplayWouldCommit);
    }

    /* Prefix before unfinished sequence. */
    {
        continuation::Value v = continuation::Value::makeBytes((const uint8_t *)"prefix\x1b[31", 11);
        continuation::ValidateError err = continuation::validate(v);
        ASSERT_EQ(err, continuation::ValidateError::NonCanonicalContinuation);
    }

    /* Multiple unfinished sequences. */
    {
        continuation::Value v = continuation::Value::makeBytes((const uint8_t *)"\x1b[31\x1b[4", 8);
        continuation::ValidateError err = continuation::validate(v);
        ASSERT_EQ(err, continuation::ValidateError::NonCanonicalContinuation);
    }
}

TEST(snapshot_continuation, record_round_trip_and_cap) {
    loadTestFixtures();

    continuation::Value values[] = {
        continuation::Value::makeGround(),
        continuation::Value::makeBytes((const uint8_t *)"\x1b[31", 4),
        continuation::Value::makeBytes((const uint8_t *)"\xF0\x9F", 2),
    };

    for (size_t i = 0; i < 3; i++) {
        std::string encoded;
        record::Writer stream;
        stream.init(&encoded);

        continuation::EncodeError encode_err = continuation::encode(values[i], &stream);
        ASSERT_EQ(encode_err, continuation::EncodeError::none);

        Reader source = Reader::fixed(encoded.data(), encoded.size());
        continuation::Value decoded;
        continuation::DecodeError decode_err =
            continuation::decode(&source, &decoded, 1024);
        ASSERT_EQ(decode_err, continuation::DecodeError::none);

        ASSERT_EQ(decoded.tag, values[i].tag);
        if (decoded.tag == continuation::Value::Tag::bytes) {
            ASSERT_TRUE(decoded.bytes_payload == values[i].bytes_payload);
        }
    }

    /* Test cap: encode a value and try to decode with a lower limit. */
    std::string capped_encoded;
    record::Writer capped_stream;
    capped_stream.init(&capped_encoded);

    continuation::EncodeError capped_encode_err =
        continuation::encode(continuation::Value::makeBytes((const uint8_t *)"\x1b[31", 4),
                                                            &capped_stream);
    ASSERT_EQ(capped_encode_err, continuation::EncodeError::none);

    Reader capped_source = Reader::fixed(capped_encoded.data(), capped_encoded.size());
    continuation::Value capped_decoded;
    continuation::DecodeError capped_decode_err =
        continuation::decode(&capped_source, &capped_decoded, 3);
    ASSERT_EQ(capped_decode_err, continuation::DecodeError::ContinuationLimitExceeded);
}

TEST(snapshot_continuation, rejects_truncation_checksum_and_tag) {
    loadTestFixtures();

    /* Test truncation: try to decode partial records. */
    for (size_t len = 0; len < test_csi_fixture.size(); len++) {
        Reader source = Reader::fixed(test_csi_fixture.data(), len);
        continuation::Value dummy;
        continuation::DecodeError err = continuation::decode(&source, &dummy, 1024);
        ASSERT_EQ(err, continuation::DecodeError::EndOfStream);
    }

    /* Test checksum: flip a bit and verify rejection. */
    std::string invalid_checksum = test_csi_fixture;
    if (invalid_checksum.size() > 6) {
        invalid_checksum[6] ^= 1;  /* Flip one bit in the CRC field */
        Reader checksum_source = Reader::fixed(invalid_checksum.data(),
                                               invalid_checksum.size());
        continuation::Value dummy;
        continuation::DecodeError err = continuation::decode(&checksum_source, &dummy, 1024);
        ASSERT_EQ(err, continuation::DecodeError::InvalidChecksum);
    }

    std::string wrong_tag = test_ground_fixture;
    wrong_tag[0] = (char)(uint8_t)record::Tag::ready;
    wrong_tag[1] = 0;
    Reader tag_source = Reader::fixed(wrong_tag.data(), wrong_tag.size());
    continuation::Value dummy;
    ASSERT_TRUE(continuation::decode(&tag_source, &dummy, 1024) ==
                continuation::DecodeError::UnexpectedRecordTag);
}
