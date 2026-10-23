/* Transliterated from the test blocks in Ghostty
 * src/terminal/snapshot/io.zig and src/terminal/snapshot/envelope.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Wisp: two upstream files' tests share one target, because io.zig has a
 * single test and envelope.zig is the only thing that uses it. The test
 * names keep their upstream suite, so io's test is `snapshot_io` and
 * envelope's are `snapshot_envelope`.
 *
 * Wisp: the golden fixture is read from disk rather than @embedFile'd at
 * compile time, so it is located relative to the source tree. The test
 * is run by ctest from the build directory, so the path comes from
 * WISP_SNAPSHOT_TESTDATA, which the test's CMake target defines.
 */

#include <string.h>

#include <string>

#include "test_helpers.h"
#include "snapshot_fixture.hpp"
#include "../src/terminal/snapshot/envelope.hpp"
#include "../src/terminal/snapshot/io.hpp"
#include "../src/zigstd/reader.hpp"

using namespace wisp;

namespace envelope = wisp::terminal::snapshot::envelope;
namespace snap_io = wisp::terminal::snapshot::io;
namespace fixture = wisp::test_fixture;

typedef zigstd::Reader Reader;
typedef zigstd::LimitedReader LimitedReader;
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

/* ─── io.zig ─────────────────────────────────────────────────────────── */

TEST(snapshot_io, integer_round_trip_with_a_one_byte_reader_buffer) {
    std::string encoded;
    snap_io::writeInt<uint16_t>(&encoded, 0x1234);
    snap_io::writeInt<uint32_t>(&encoded, 0x56789abc);
    ASSERT_EQ(encoded.size(), 6u);

    /* Keep the reader buffer smaller than either integer to verify readInt
     * does not inherit std.Io.Reader.takeInt's buffer-size requirement. */
    Reader source = Reader::fixed(encoded.data(), encoded.size());
    uint8_t buf[1];
    LimitedReader limited = LimitedReader::limited(&source, buf, sizeof buf);

    ReadError err = ReadError::none;
    uint16_t a = 0;
    ASSERT_TRUE(snap_io::readInt<uint16_t>(&limited, &a, &err));
    ASSERT_EQ(a, 0x1234);

    uint32_t b = 0;
    ASSERT_TRUE(snap_io::readInt<uint32_t>(&limited, &b, &err));
    ASSERT_TRUE(b == 0x56789abcu);
}

/* ─── envelope.zig ───────────────────────────────────────────────────── */

TEST(snapshot_envelope, golden_encoding) {
    std::string buf;
    envelope::encode(&buf);
    ASSERT_EQ(buf.size(), envelope::encoded_len);

    const std::string reference_path = fixturePath("envelope-v1.hex");
    std::string source;
    ASSERT_TRUE(fixture::readFile(reference_path.c_str(), &source));

    std::string golden;
    ASSERT_TRUE(fixture::parse(source.data(), source.size(), &golden));

    ASSERT_TRUE(fixture::expectEqual(fixture::Kind::bytes,
                                     reference_path.c_str(),
                                     "snapshot_fixture-envelope-v1.hex",
                                     golden, buf));

    Reader reader = Reader::fixed(golden.data(), golden.size());
    envelope::Error err = envelope::Error::none;
    ASSERT_TRUE(envelope::decode(&reader, &err));
}

TEST(snapshot_envelope, reject_invalid_magic_and_version) {
    {
        const char bytes[] = "GHOSTSNX\x01\x00";
        Reader invalid_magic = Reader::fixed(bytes, envelope::encoded_len);
        envelope::Error err = envelope::Error::none;
        ASSERT_FALSE(envelope::decode(&invalid_magic, &err));
        ASSERT_TRUE(err == envelope::Error::InvalidMagic);
    }
    {
        const char bytes[] = "GHOSTSNP\x00\x00";
        Reader invalid_version = Reader::fixed(bytes, envelope::encoded_len);
        envelope::Error err = envelope::Error::none;
        ASSERT_FALSE(envelope::decode(&invalid_version, &err));
        ASSERT_TRUE(err == envelope::Error::UnsupportedVersion);
    }
}

TEST(snapshot_envelope, reject_every_truncation) {
    const std::string reference_path = fixturePath("envelope-v1.hex");
    std::string source;
    ASSERT_TRUE(fixture::readFile(reference_path.c_str(), &source));
    std::string golden;
    ASSERT_TRUE(fixture::parse(source.data(), source.size(), &golden));
    ASSERT_EQ(golden.size(), envelope::encoded_len);

    for (size_t len = 0; len < envelope::encoded_len; len++) {
        Reader reader = Reader::fixed(golden.data(), len);
        envelope::Error err = envelope::Error::none;
        ASSERT_FALSE(envelope::decode(&reader, &err));
        ASSERT_TRUE(err == envelope::Error::EndOfStream);
    }
}
