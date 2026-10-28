/* Transliterated from Ghostty src/terminal/snapshot/checkpoint.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Wisp: tests follow the checkpoint.zig test structure.
 */

#include <string.h>

#include <string>

#include "test_helpers.h"
#include "snapshot_fixture.hpp"
#include "../src/terminal/snapshot/checkpoint.hpp"
#include "../src/terminal/snapshot/record.hpp"
#include "../src/zigstd/reader.hpp"

using namespace wisp;

namespace checkpoint = wisp::terminal::snapshot::checkpoint;
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

/* Helper to skip n bytes in a Reader by reading into a temporary buffer. */
static bool skipBytes(Reader *reader, size_t n) {
    uint8_t buf[256];
    while (n > 0) {
        size_t to_read = n < sizeof(buf) ? n : sizeof(buf);
        ReadError err = ReadError::none;
        if (!reader->readSliceAll(buf, to_read, &err)) {
            return false;
        }
        n -= to_read;
    }
    return true;
}

static std::string test_ready_fixture;

static void loadTestFixtures() {
    static bool loaded = false;
    if (!loaded) {
        std::string path = fixturePath("checkpoint-ready-v1.hex");
        std::string hex_content;
        ASSERT_TRUE(fixture::readFile(path.c_str(), &hex_content));
        ASSERT_TRUE(fixture::parse(hex_content.c_str(), hex_content.size(),
                                    &test_ready_fixture));
        loaded = true;
    }
}

/* ─── checkpoint.zig ─────────────────────────────────────────────────── */

TEST(snapshot_checkpoint, READY_golden_encoding) {
    loadTestFixtures();
    const std::string prefix = "abc";

    std::string snapshot;
    record::Writer stream;
    stream.init(&snapshot);

    snapshot.append(prefix);
    checkpoint::EncodeError err = checkpoint::encode(checkpoint::Kind::ready, &stream);
    ASSERT_EQ(err, record::Writer::FinishError::none);

    /* The encoded snapshot should match the golden fixture. */
    ASSERT_EQ(snapshot.size(), test_ready_fixture.size());
    for (size_t i = 0; i < snapshot.size(); i++) {
        if ((uint8_t)snapshot[i] != (uint8_t)test_ready_fixture[i]) {
            fprintf(stderr, "  MISMATCH at byte %zu: got %02x expected %02x\n",
                    i, (uint8_t)snapshot[i], (uint8_t)test_ready_fixture[i]);
            ASSERT(false);
        }
    }

    /* Decode the golden fixture to verify it's valid. */
    Reader source = Reader::fixed(test_ready_fixture.data(),
                                   test_ready_fixture.size());
    /* Skip the prefix bytes. */
    ASSERT_TRUE(skipBytes(&source, prefix.size()));

    checkpoint::DecodeError decode_err = checkpoint::decode(checkpoint::Kind::ready, &source);
    ASSERT_EQ(decode_err, checkpoint::DecodeError::none);
}

TEST(snapshot_checkpoint, READY_and_FINISH_are_empty_marker_records) {
    std::string snapshot;
    record::Writer stream;
    stream.init(&snapshot);

    snapshot.append("prefix");

    const size_t ready_offset = snapshot.size();
    checkpoint::EncodeError ready_err =
        checkpoint::encode(checkpoint::Kind::ready, &stream);
    ASSERT_EQ(ready_err, record::Writer::FinishError::none);

    snapshot.append("history");

    const size_t finish_offset = snapshot.size();
    checkpoint::EncodeError finish_err =
        checkpoint::encode(checkpoint::Kind::finish, &stream);
    ASSERT_EQ(finish_err, record::Writer::FinishError::none);

    /* Both READY and FINISH should be exactly one header with no payload. */
    const std::string history = "history";
    ASSERT_EQ(finish_offset - ready_offset - history.size(),
              record::Header::len);
    ASSERT_EQ(snapshot.size() - finish_offset, record::Header::len);

    /* Decode READY marker. */
    Reader source1 = Reader::fixed(snapshot.data(), snapshot.size());
    ASSERT_TRUE(skipBytes(&source1, ready_offset));

    checkpoint::DecodeError ready_decode_err =
        checkpoint::decode(checkpoint::Kind::ready, &source1);
    ASSERT_EQ(ready_decode_err, checkpoint::DecodeError::none);

    /* Decode FINISH marker. */
    Reader source2 = Reader::fixed(snapshot.data(), snapshot.size());
    ASSERT_TRUE(skipBytes(&source2, finish_offset));

    checkpoint::DecodeError finish_decode_err =
        checkpoint::decode(checkpoint::Kind::finish, &source2);
    ASSERT_EQ(finish_decode_err, checkpoint::DecodeError::none);
}

TEST(snapshot_checkpoint, checkpoint_rejects_wrong_tags_and_nonempty_payloads) {
    std::string snapshot;
    record::Writer stream;
    stream.init(&snapshot);

    checkpoint::EncodeError encode_err =
        checkpoint::encode(checkpoint::Kind::ready, &stream);
    ASSERT_EQ(encode_err, record::Writer::FinishError::none);

    /* Test: wrong tag should be rejected. */
    Reader source1 = Reader::fixed(snapshot.data(), snapshot.size());
    checkpoint::DecodeError wrong_tag_err =
        checkpoint::decode(checkpoint::Kind::finish, &source1);
    ASSERT_EQ(wrong_tag_err, checkpoint::DecodeError::UnexpectedRecordTag);

    /* Test: payload with content should be rejected. */
    std::string invalid;
    record::Writer invalid_stream;
    invalid_stream.init(&invalid);

    std::string *payload = invalid_stream.begin(record::Tag::ready);
    payload->push_back(0);
    record::Writer::FinishError finish_err = invalid_stream.finish();
    ASSERT_EQ(finish_err, record::Writer::FinishError::none);

    Reader source2 = Reader::fixed(invalid.data(), invalid.size());
    checkpoint::DecodeError payload_err =
        checkpoint::decode(checkpoint::Kind::ready, &source2);
    ASSERT_EQ(payload_err, checkpoint::DecodeError::PayloadNotExhausted);
}

TEST(snapshot_checkpoint, FINISH_leaves_continuation_bytes_unread) {
    std::string finished;
    record::Writer finished_stream;
    finished_stream.init(&finished);

    checkpoint::EncodeError encode_err =
        checkpoint::encode(checkpoint::Kind::finish, &finished_stream);
    ASSERT_EQ(encode_err, record::Writer::FinishError::none);

    /* Append a continuation byte after FINISH. */
    finished.push_back(0);

    Reader source = Reader::fixed(finished.data(), finished.size());

    checkpoint::DecodeError decode_err =
        checkpoint::decode(checkpoint::Kind::finish, &source);
    ASSERT_EQ(decode_err, checkpoint::DecodeError::none);

    /* Verify the continuation byte is still there (not consumed by decode). */
    uint8_t continuation = 0;
    ReadError read_err = ReadError::none;
    ASSERT_TRUE(source.readSliceAll(&continuation, 1, &read_err));
    ASSERT_EQ(continuation, 0);
}
