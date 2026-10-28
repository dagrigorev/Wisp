/* Transliterated from the test blocks in Ghostty
 * src/terminal/snapshot/record.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include <string.h>

#include <string>

#include "test_helpers.h"
#include "snapshot_fixture.hpp"
#include "../src/terminal/snapshot/record.hpp"
#include "../src/zigstd/reader.hpp"

using namespace wisp;
using namespace wisp::terminal::snapshot;

namespace fixture = wisp::test_fixture;
typedef zigstd::Reader SourceReader;
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

static uint32_t checksumOf(record::Tag tag, const char *payload, size_t len) {
    record::Checksum checksum;
    checksum.init(tag, (uint32_t)len);
    checksum.update((const uint8_t *)payload, len);
    return checksum.final();
}

static std::string encodeRecord(const record::Header &header, const char *bytes, size_t len) {
    std::string out;
    header.encode(&out);
    out.append(bytes, len);
    return out;
}

TEST(snapshot_record, golden_PAGE_record_header_and_checksum) {
    static const char page_header[24] = {
        0x50, 0x00, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    ASSERT_TRUE(0x7178441bu == checksumOf(record::Tag::page, page_header, sizeof page_header));

    record::Header header;
    header.tag = record::Tag::page;
    header.payload_len = sizeof page_header;
    header.crc32c = 0x7178441b;
    std::string buf;
    header.encode(&buf);

    const std::string path = fixturePath("record-page-header-v1.hex");
    std::string text, golden;
    ASSERT_TRUE(fixture::readFile(path.c_str(), &text));
    ASSERT_TRUE(fixture::parse(text.data(), text.size(), &golden));
    ASSERT_TRUE(fixture::expectEqual(fixture::Kind::bytes, path.c_str(),
                                     "snapshot_fixture-record-page-header-v1.hex", golden, buf));

    SourceReader reader = SourceReader::fixed(golden.data(), golden.size());
    record::Header decoded;
    record::DecodeError err = record::DecodeError::none;
    ASSERT_TRUE(record::Header::decode(&reader, &decoded, &err));
    ASSERT_TRUE(header == decoded);
}

TEST(snapshot_record, reject_invalid_tags) {
    const uint16_t tags[] = { 0, 8, 0xFFFF };
    for (size_t i = 0; i < 3; i++) {
        uint8_t fx[record::Header::len] = { 0 };
        fx[0] = (uint8_t)(tags[i] & 0xFF);
        fx[1] = (uint8_t)(tags[i] >> 8);
        SourceReader reader = SourceReader::fixed(fx, sizeof fx);
        record::Header out;
        record::DecodeError err = record::DecodeError::none;
        ASSERT_FALSE(record::Header::decode(&reader, &out, &err));
        ASSERT_TRUE(err == record::DecodeError::InvalidTag);
    }
}

TEST(snapshot_record, reject_every_header_truncation) {
    std::string text, golden;
    const std::string path = fixturePath("record-page-header-v1.hex");
    ASSERT_TRUE(fixture::readFile(path.c_str(), &text));
    ASSERT_TRUE(fixture::parse(text.data(), text.size(), &golden));
    for (size_t len = 0; len < record::Header::len; len++) {
        SourceReader reader = SourceReader::fixed(golden.data(), len);
        record::Header out;
        record::DecodeError err = record::DecodeError::none;
        ASSERT_FALSE(record::Header::decode(&reader, &out, &err));
        ASSERT_TRUE(err == record::DecodeError::EndOfStream);
    }
}

TEST(snapshot_record, record_reader_verifies_exhaustion_and_checksum) {
    const char payload[] = "snapshot payload";
    const size_t plen = sizeof payload - 1;
    record::Header header;
    header.tag = record::Tag::page;
    header.payload_len = (uint32_t)plen;
    header.crc32c = checksumOf(record::Tag::page, payload, plen);

    std::string encoded = encodeRecord(header, payload, plen);
    encoded += "next";

    SourceReader source = SourceReader::fixed(encoded.data(), encoded.size());
    record::Reader record_reader;
    record::Reader::InitError init_err = record::Reader::InitError::none;
    ASSERT_TRUE(record_reader.init(&source, &init_err));
    char decoded[sizeof payload - 1];
    ReadError rerr = ReadError::none;
    ASSERT_TRUE(record_reader.payloadReader()->readSliceAll(decoded, plen, &rerr));
    ASSERT_TRUE(record_reader.finish() == record::Reader::FinishError::none);
    ASSERT_TRUE(memcmp(payload, decoded, plen) == 0);
    const uint8_t *next = source.take(4, &rerr);
    ASSERT_TRUE(next != nullptr && memcmp(next, "next", 4) == 0);
}

TEST(snapshot_record, record_reader_rejects_remaining_bytes_and_invalid_checksum) {
    const char payload[] = "payload";
    const size_t plen = sizeof payload - 1;
    record::Header header;
    header.tag = record::Tag::page;
    header.payload_len = (uint32_t)plen;
    header.crc32c = 0;
    const std::string encoded = encodeRecord(header, payload, plen);

    {
        SourceReader source = SourceReader::fixed(encoded.data(), encoded.size());
        record::Reader record_reader;
        record::Reader::InitError init_err = record::Reader::InitError::none;
        ASSERT_TRUE(record_reader.init(&source, &init_err));
        uint8_t b = 0;
        ReadError rerr = ReadError::none;
        ASSERT_TRUE(record_reader.payloadReader()->takeByte(&b, &rerr));
        ASSERT_TRUE(record_reader.finish() == record::Reader::FinishError::PayloadNotExhausted);
    }

    {
        SourceReader source = SourceReader::fixed(encoded.data(), encoded.size());
        record::Reader record_reader;
        record::Reader::InitError init_err = record::Reader::InitError::none;
        ASSERT_TRUE(record_reader.init(&source, &init_err));
        ReadError rerr = ReadError::none;
        ASSERT_TRUE(record_reader.payloadReader()->discardAll(plen, &rerr));
        ASSERT_TRUE(record_reader.finish() == record::Reader::FinishError::InvalidChecksum);
    }
}

TEST(snapshot_record, payload_limit_does_not_consume_the_next_record) {
    const char payload[] = "ab";
    const size_t plen = 2;
    record::Header header;
    header.tag = record::Tag::page;
    header.payload_len = (uint32_t)plen;
    header.crc32c = checksumOf(record::Tag::page, payload, plen);

    std::string encoded = encodeRecord(header, payload, plen);
    encoded += "next";

    SourceReader source = SourceReader::fixed(encoded.data(), encoded.size());
    record::Reader record_reader;
    record::Reader::InitError init_err = record::Reader::InitError::none;
    ASSERT_TRUE(record_reader.init(&source, &init_err));
    uint8_t too_long[3];
    ReadError rerr = ReadError::none;
    ASSERT_FALSE(record_reader.payloadReader()->readSliceAll(too_long, 3, &rerr));
    ASSERT_TRUE(rerr == ReadError::EndOfStream);
    ASSERT_TRUE(record_reader.finish() == record::Reader::FinishError::none);
    const uint8_t *next = source.take(4, &rerr);
    ASSERT_TRUE(next != nullptr && memcmp(next, "next", 4) == 0);
}

TEST(snapshot_record, record_writer_buffers_a_payload_and_appends_framing) {
    std::string destination = "prefix";
    record::Writer stream;
    stream.init(&destination);

    std::string *payload = stream.begin(record::Tag::page);
    payload->append("payload");
    ASSERT_TRUE(stream.finish() == record::Writer::FinishError::none);

    ASSERT_TRUE(destination.compare(0, 6, "prefix") == 0);

    SourceReader source = SourceReader::fixed(destination.data() + 6, destination.size() - 6);
    record::Reader record_reader;
    record::Reader::InitError init_err = record::Reader::InitError::none;
    ASSERT_TRUE(record_reader.init(&source, &init_err));
    ASSERT_TRUE(record::Tag::page == record_reader.header.tag);
    ASSERT_EQ(7, (long)record_reader.header.payload_len);
    ReadError rerr = ReadError::none;
    const uint8_t *got = record_reader.payloadReader()->take(7, &rerr);
    ASSERT_TRUE(got != nullptr && memcmp(got, "payload", 7) == 0);
    ASSERT_TRUE(record_reader.finish() == record::Reader::FinishError::none);
}

TEST(snapshot_record, record_writer_cancel_preserves_preceding_bytes) {
    std::string destination = "prefix";
    record::Writer stream;
    stream.init(&destination);

    std::string *partial = stream.begin(record::Tag::page);
    partial->append("partial");
    const size_t scratch_capacity = stream.scratch.capacity();
    stream.cancel();

    ASSERT_TRUE(destination == "prefix");
    ASSERT_EQ((long)scratch_capacity, (long)stream.scratch.capacity());

    /* Cancel retains the allocation but leaves it ready for the next record. */
    std::string *next = stream.begin(record::Tag::page);
    next->append("next");
    ASSERT_TRUE(stream.finish() == record::Writer::FinishError::none);

    SourceReader source = SourceReader::fixed(destination.data() + 6, destination.size() - 6);
    record::Reader record_reader;
    record::Reader::InitError init_err = record::Reader::InitError::none;
    ASSERT_TRUE(record_reader.init(&source, &init_err));
    ReadError rerr = ReadError::none;
    const uint8_t *got = record_reader.payloadReader()->take(4, &rerr);
    ASSERT_TRUE(got != nullptr && memcmp(got, "next", 4) == 0);
    ASSERT_TRUE(record_reader.finish() == record::Reader::FinishError::none);
}

/* Wisp: upstream's destination here is a fixed 10-byte writer that fails on
 * the payload. A `std::string *` destination cannot fail, so the failure
 * itself is not expressible; what the test pins, that `finish` leaves the
 * writer idle with empty scratch whatever happened, is asserted on the
 * success path that the same `defer self.cancel()` covers. */
TEST(snapshot_record, record_writer_clears_active_state_after_destination_failure) {
    std::string destination;
    record::Writer stream;
    stream.init(&destination);

    std::string *payload = stream.begin(record::Tag::page);
    payload->push_back(0);
    (void)stream.finish();
    ASSERT_FALSE(stream.has_active);
    ASSERT_EQ(0, (long)stream.scratch.size());
}
