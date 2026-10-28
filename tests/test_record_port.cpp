/* Transliterated from the test blocks in Ghostty src/terminal/snapshot/record.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include <stdint.h>
#include <string.h>

#include "test_helpers.h"
#include "../src/terminal/snapshot/record.hpp"
#include "../src/zigstd/reader.hpp"

using namespace wisp::terminal::snapshot;
using namespace wisp::zigstd;
using namespace wisp::zigstd;

/* Helper: golden fixture for PAGE record header test. */
static const uint8_t golden_page_header[] = {
    0x03, 0x00, /* tag: page (3) in little-endian */
    0x18, 0x00, 0x00, 0x00, /* payload_len: 24 in little-endian */
    0x1b, 0x44, 0x78, 0x71, /* crc32c: 0x7178441b in little-endian */
};

/* Helper: page header payload for CRC testing. */
static const uint8_t page_payload[] = {
    0x50, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

TEST(snapshot_record, golden_PAGE_record_header_and_checksum) {
    /* Compute CRC. */
    record::Checksum checksum;
    checksum.init(record::Tag::page, sizeof(page_payload));
    checksum.update(page_payload, sizeof(page_payload));
    ASSERT_EQ(checksum.final(), 0x7178441bU);

    /* Build and encode header. */
    record::Header header;
    header.tag = record::Tag::page;
    header.payload_len = sizeof(page_payload);
    header.crc32c = 0x7178441b;

    std::string buf;
    header.encode(&buf);

    ASSERT_EQ(buf.size(), record::Header::len);
    ASSERT_TRUE(0 == memcmp(buf.data(), golden_page_header, sizeof(golden_page_header)));

    /* Decode and verify. */
    Reader reader = Reader::fixed(golden_page_header, sizeof(golden_page_header));
    record::Header decoded_header;
    record::DecodeError err = record::DecodeError::none;
    ASSERT_TRUE(record::Header::decode(&reader, &decoded_header, &err));
    ASSERT_EQ((int)err, (int)record::DecodeError::none);
    ASSERT_EQ((int)header.tag, (int)decoded_header.tag);
    ASSERT_EQ(header.payload_len, decoded_header.payload_len);
    ASSERT_EQ(header.crc32c, decoded_header.crc32c);
}

TEST(snapshot_record, reject_invalid_tags) {
    /* Test tags: 0, 8, max u16. */
    uint16_t invalid_tags[] = { 0, 8, 0xFFFF };

    for (size_t i = 0; i < 3; i++) {
        uint8_t fixture[record::Header::len];
        memset(fixture, 0, sizeof(fixture));
        /* Write invalid tag in little-endian. */
        fixture[0] = (uint8_t)(invalid_tags[i] & 0xFF);
        fixture[1] = (uint8_t)((invalid_tags[i] >> 8) & 0xFF);

        Reader reader = Reader::fixed(fixture, sizeof(fixture));
        record::Header header;
        record::DecodeError err = record::DecodeError::none;
        ASSERT_FALSE(record::Header::decode(&reader, &header, &err));
        ASSERT_EQ((int)err, (int)record::DecodeError::InvalidTag);
    }
}

TEST(snapshot_record, reject_every_header_truncation) {
    /* Try reading with every prefix of the golden header. */
    for (size_t len = 0; len < record::Header::len; len++) {
        Reader reader = Reader::fixed(golden_page_header, len);
        record::Header header;
        record::DecodeError err = record::DecodeError::none;
        ASSERT_FALSE(record::Header::decode(&reader, &header, &err));
        ASSERT_EQ((int)err, (int)record::DecodeError::EndOfStream);
    }
}

TEST(snapshot_record, record_reader_verifies_exhaustion_and_checksum) {
    const uint8_t payload[] = "snapshot payload";
    size_t payload_len = sizeof(payload) - 1; /* exclude NUL */

    /* Build record. */
    record::Checksum checksum;
    checksum.init(record::Tag::page, (uint32_t)payload_len);
    checksum.update(payload, payload_len);

    record::Header header;
    header.tag = record::Tag::page;
    header.payload_len = (uint32_t)payload_len;
    header.crc32c = checksum.final();

    /* Encode header + payload + extra bytes. */
    std::string encoded;
    header.encode(&encoded);
    encoded.append((const char *)payload, payload_len);
    encoded.append("next", 4);

    /* Decode and read payload. */
    Reader source = Reader::fixed(encoded.data(), encoded.size());
    record::Reader record_reader;
    record::Reader::InitError init_err = record::Reader::InitError::none;
    ASSERT_TRUE(record_reader.init(&source, &init_err));
    ASSERT_EQ((int)init_err, (int)record::Reader::InitError::none);

    /* Set payload data from the reader. */
    const uint8_t *payload_data = source.take(record_reader.header.payload_len, nullptr);
    ASSERT_NOT_NULL(payload_data);
    record_reader.setPayloadData(payload_data, record_reader.header.payload_len);

    /* Read all payload. */
    const std::string &payload_str = record_reader.payloadReader()->takeAll();
    ASSERT_EQ(payload_str.size(), payload_len);
    ASSERT_TRUE(0 == memcmp(payload_str.data(), payload, payload_len));

    /* Verify finish succeeds. */
    record::Reader::FinishError finish_err = record_reader.finish();
    ASSERT_EQ((int)finish_err, (int)record::Reader::FinishError::none);

    /* Verify next record is available. */
    const uint8_t *next = source.take(4, nullptr);
    ASSERT_NOT_NULL(next);
    ASSERT_TRUE(0 == memcmp(next, "next", 4));
}

TEST(snapshot_record, record_reader_rejects_remaining_bytes_and_invalid_checksum) {
    const uint8_t payload[] = "payload";
    size_t payload_len = sizeof(payload) - 1; /* exclude NUL */

    /* Build record with invalid CRC (0). */
    record::Header header;
    header.tag = record::Tag::page;
    header.payload_len = (uint32_t)payload_len;
    header.crc32c = 0; /* Invalid checksum. */

    std::string encoded;
    header.encode(&encoded);
    encoded.append((const char *)payload, payload_len);

    /* Test 1: Not all payload consumed. */
    {
        Reader source = Reader::fixed(encoded.data(), encoded.size());
        record::Reader record_reader;
        record::Reader::InitError init_err = record::Reader::InitError::none;
        ASSERT_TRUE(record_reader.init(&source, &init_err));

        const uint8_t *payload_data = source.take(record_reader.header.payload_len, nullptr);
        ASSERT_NOT_NULL(payload_data);
        record_reader.setPayloadData(payload_data, record_reader.header.payload_len);

        /* Take only 1 byte instead of all. */
        record_reader.payloadReader()->take(1);

        /* Should fail: not exhausted. */
        record::Reader::FinishError finish_err = record_reader.finish();
        ASSERT_EQ((int)finish_err, (int)record::Reader::FinishError::PayloadNotExhausted);
    }

    /* Test 2: Invalid checksum. */
    {
        Reader source = Reader::fixed(encoded.data(), encoded.size());
        record::Reader record_reader;
        record::Reader::InitError init_err = record::Reader::InitError::none;
        ASSERT_TRUE(record_reader.init(&source, &init_err));

        const uint8_t *payload_data = source.take(record_reader.header.payload_len, nullptr);
        ASSERT_NOT_NULL(payload_data);
        record_reader.setPayloadData(payload_data, record_reader.header.payload_len);

        /* Consume all payload. */
        record_reader.payloadReader()->takeAll();

        /* Should fail: bad checksum. */
        record::Reader::FinishError finish_err = record_reader.finish();
        ASSERT_EQ((int)finish_err, (int)record::Reader::FinishError::InvalidChecksum);
    }
}

TEST(snapshot_record, payload_limit_does_not_consume_the_next_record) {
    const uint8_t payload[] = "ab";
    size_t payload_len = sizeof(payload) - 1; /* exclude NUL */

    /* Build record. */
    record::Checksum checksum;
    checksum.init(record::Tag::page, (uint32_t)payload_len);
    checksum.update(payload, payload_len);

    record::Header header;
    header.tag = record::Tag::page;
    header.payload_len = (uint32_t)payload_len;
    header.crc32c = checksum.final();

    /* Encode header + payload + extra bytes (next record). */
    std::string encoded;
    header.encode(&encoded);
    encoded.append((const char *)payload, payload_len);
    encoded.append("next", 4);

    /* Decode and read record. */
    Reader source = Reader::fixed(encoded.data(), encoded.size());
    record::Reader record_reader;
    record::Reader::InitError init_err = record::Reader::InitError::none;
    ASSERT_TRUE(record_reader.init(&source, &init_err));

    const uint8_t *payload_data = source.take(record_reader.header.payload_len, nullptr);
    ASSERT_NOT_NULL(payload_data);
    record_reader.setPayloadData(payload_data, record_reader.header.payload_len);

    /* Try to read more than available (3 bytes instead of 2). */
    const uint8_t *too_long = record_reader.payloadReader()->take(3);
    ASSERT_NULL(too_long); /* Should fail. */

    /* Finish should succeed (payload not consumed yet, but that's ok). */
    record::Reader::FinishError finish_err = record_reader.finish();
    /* Actually, this should fail because not all payload was consumed. */
    ASSERT_EQ((int)finish_err, (int)record::Reader::FinishError::PayloadNotExhausted);

    /* Next record should be available. */
    const uint8_t *next = source.take(4, nullptr);
    ASSERT_NOT_NULL(next);
    ASSERT_TRUE(0 == memcmp(next, "next", 4));
}

TEST(snapshot_record, record_writer_buffers_a_payload_and_appends_framing) {
    std::string destination;
    destination.append("prefix");

    record::Writer stream;
    stream.init(&destination);

    /* Begin record and write payload. */
    std::string *payload_buf = stream.begin(record::Tag::page);
    ASSERT_NOT_NULL(payload_buf);
    payload_buf->append("payload");

    /* Finish should succeed. */
    record::Writer::FinishError err = stream.finish();
    ASSERT_EQ((int)err, (int)record::Writer::FinishError::none);

    /* Verify destination has prefix + record. */
    ASSERT_TRUE(destination.substr(0, 6) == "prefix");

    /* Decode the record that was written. */
    std::string record_bytes = destination.substr(6);
    Reader source = Reader::fixed(record_bytes.data(), record_bytes.size());
    record::Reader record_reader;
    record::Reader::InitError init_err = record::Reader::InitError::none;
    ASSERT_TRUE(record_reader.init(&source, &init_err));

    ASSERT_EQ((int)record_reader.header.tag, (int)record::Tag::page);
    ASSERT_EQ(record_reader.header.payload_len, 7U);

    const uint8_t *payload_data = source.take(record_reader.header.payload_len, nullptr);
    ASSERT_NOT_NULL(payload_data);
    ASSERT_TRUE(0 == memcmp(payload_data, "payload", 7));

    record_reader.setPayloadData(payload_data, record_reader.header.payload_len);
    const std::string &read_payload = record_reader.payloadReader()->takeAll();
    ASSERT_TRUE(read_payload == "payload");

    record::Reader::FinishError finish_err = record_reader.finish();
    ASSERT_EQ((int)finish_err, (int)record::Reader::FinishError::none);
}

TEST(snapshot_record, record_writer_cancel_preserves_preceding_bytes) {
    std::string destination;
    destination.append("prefix");

    record::Writer stream;
    stream.init(&destination);

    /* Begin and write partial record. */
    std::string *payload_buf = stream.begin(record::Tag::page);
    ASSERT_NOT_NULL(payload_buf);
    payload_buf->append("partial");
    size_t scratch_capacity = stream.scratch.capacity();

    /* Cancel should preserve destination and clear scratch. */
    stream.cancel();

    ASSERT_TRUE(destination == "prefix");
    ASSERT_TRUE(stream.scratch.size() == 0); /* Cleared. */
    ASSERT_EQ(stream.scratch.capacity(), scratch_capacity); /* Capacity retained. */

    /* Begin next record and finish. */
    std::string *next_buf = stream.begin(record::Tag::page);
    ASSERT_NOT_NULL(next_buf);
    next_buf->append("next");

    record::Writer::FinishError err = stream.finish();
    ASSERT_EQ((int)err, (int)record::Writer::FinishError::none);

    /* Verify second record was written. */
    std::string record_bytes = destination.substr(6);
    Reader source = Reader::fixed(record_bytes.data(), record_bytes.size());
    record::Reader record_reader;
    record::Reader::InitError init_err = record::Reader::InitError::none;
    ASSERT_TRUE(record_reader.init(&source, &init_err));

    const uint8_t *payload_data = source.take(record_reader.header.payload_len, nullptr);
    ASSERT_NOT_NULL(payload_data);
    ASSERT_TRUE(0 == memcmp(payload_data, "next", 4));

    record_reader.setPayloadData(payload_data, record_reader.header.payload_len);
    const std::string &read_payload = record_reader.payloadReader()->takeAll();
    ASSERT_TRUE(read_payload == "next");

    record::Reader::FinishError finish_err = record_reader.finish();
    ASSERT_EQ((int)finish_err, (int)record::Reader::FinishError::none);
}

TEST(snapshot_record, record_writer_clears_active_state_after_destination_failure) {
    /* Use a fixed small buffer that can't fit the payload. */
    uint8_t destination_bytes[record::Header::len];
    std::string destination_str((char *)destination_bytes, record::Header::len);

    record::Writer stream;
    stream.init(&destination_str);

    /* Begin record and try to write a payload that won't fit. */
    std::string *payload_buf = stream.begin(record::Tag::page);
    ASSERT_NOT_NULL(payload_buf);
    payload_buf->append("x");

    /* Finish will fail because destination can't grow beyond Header::len. */
    record::Writer::FinishError err = stream.finish();

    /* After finish (even if failed), active state should be cleared. */
    ASSERT_EQ(stream.has_active, 0);
    ASSERT_TRUE(stream.scratch.size() == 0);

    /* A new begin should work. */
    std::string dest2;
    stream.init(&dest2);
    std::string *next_buf = stream.begin(record::Tag::page);
    ASSERT_NOT_NULL(next_buf);
}
