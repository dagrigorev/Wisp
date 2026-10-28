/* Transliterated from the test blocks in Ghostty
 * src/terminal/snapshot/hyperlink.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include <string.h>

#include <string>

#include "test_helpers.h"
#include "snapshot_fixture.hpp"
#include "../src/terminal/snapshot/hyperlink.hpp"
#include "../src/zigstd/allocator.hpp"
#include "../src/zigstd/reader.hpp"

using namespace wisp;

namespace sh = wisp::terminal::snapshot::hyperlink;
namespace fixture = wisp::test_fixture;
namespace th = wisp::vt::hyperlink;
namespace tp = wisp::vt::page;

typedef zigstd::Reader Reader;
typedef zigstd::ReadError ReadError;
typedef th::Hyperlink Hyperlink;
typedef sh::DecodeError DecodeError;

#ifndef WISP_SNAPSHOT_TESTDATA
#define WISP_SNAPSHOT_TESTDATA "snapshot_testdata"
#endif

static std::string fixturePath(const char *name) {
    std::string p = WISP_SNAPSHOT_TESTDATA;
    p += "/";
    p += name;
    return p;
}

static bool loadFixture(const char *name, std::string *out) {
    std::string source;
    if (!fixture::readFile(fixturePath(name).c_str(), &source)) return false;
    return fixture::parse(source.data(), source.size(), out);
}

static const uint8_t *u8(const char *s) { return (const uint8_t *)s; }

static Hyperlink implicitLink(uint32_t id, const char *uri) {
    Hyperlink h;
    h.id = Hyperlink::Id::makeImplicit(id);
    h.uri = u8(uri);
    h.uri_len = strlen(uri);
    return h;
}

static Hyperlink explicitLink(const char *id, const char *uri) {
    Hyperlink h;
    h.id = Hyperlink::Id::makeExplicit(u8(id), strlen(id));
    h.uri = u8(uri);
    h.uri_len = strlen(uri);
    return h;
}

/* Wisp: std.testing.expectEqualDeep over Hyperlink. */
static bool eqlDeep(const Hyperlink &a, const Hyperlink &b) {
    if (a.id.tag != b.id.tag) return false;
    switch (a.id.tag) {
        case Hyperlink::Id::Tag::implicit:
            if (a.id.implicit != b.id.implicit) return false;
            break;
        case Hyperlink::Id::Tag::explicit_:
            if (a.id.explicit_len != b.id.explicit_len) return false;
            if (memcmp(a.id.explicit_ptr, b.id.explicit_ptr, a.id.explicit_len) != 0) return false;
            break;
    }
    return a.uri_len == b.uri_len && memcmp(a.uri, b.uri, a.uri_len) == 0;
}

static void goldenCase(const Hyperlink &value, const char *name, const char *candidate) {
    std::string buf;
    ASSERT_TRUE(sh::encode(value, &buf) == sh::EncodeError::none);

    std::string golden;
    ASSERT_TRUE(loadFixture(name, &golden));
    const std::string path = fixturePath(name);
    ASSERT_TRUE(fixture::expectEqual(fixture::Kind::bytes, path.c_str(), candidate, golden, buf));

    Reader reader = Reader::fixed(golden.data(), golden.size());
    zigstd::Allocator alloc = zigstd::testing_allocator();
    Hyperlink decoded;
    DecodeError err = DecodeError::none;
    ASSERT_TRUE(sh::decode(&reader, alloc, &decoded, &err));
    const bool same = eqlDeep(value, decoded);
    decoded.deinit(alloc);
    ASSERT_TRUE(same);
}

TEST(snapshot_hyperlink, golden_implicit_encoding) {
    goldenCase(implicitLink(0x01020304, "uri"), "hyperlink-implicit-v1.hex",
               "snapshot_fixture-hyperlink-implicit-v1.hex");
}

TEST(snapshot_hyperlink, golden_explicit_encoding) {
    goldenCase(explicitLink("id", "uri"), "hyperlink-explicit-v1.hex",
               "snapshot_fixture-hyperlink-explicit-v1.hex");
}

TEST(snapshot_hyperlink, encode_rejects_invalid_strings_before_writing) {
    struct Case {
        Hyperlink value;
        sh::EncodeError expected;
    };
    const Case cases[] = {
        { implicitLink(1, ""), sh::EncodeError::InvalidUri },
        { explicitLink("", "uri"), sh::EncodeError::InvalidExplicitId },
        { explicitLink("id", ""), sh::EncodeError::InvalidUri },
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        std::string encoded;
        ASSERT_TRUE(cases[i].expected == sh::encode(cases[i].value, &encoded));
        ASSERT_EQ(0, (long)encoded.size());
    }
}

TEST(snapshot_hyperlink, decode_rejects_empty_strings) {
    struct Case {
        const char *fixture;
        size_t len;
        DecodeError expected;
    };
    const Case cases[] = {
        { "\x01\x04\x03\x02\x01\x00\x00\x00\x00", 9, DecodeError::InvalidUri },
        { "\x02\x00\x00\x00\x00", 5, DecodeError::InvalidExplicitId },
        { "\x02\x02\x00\x00\x00" "id" "\x00\x00\x00\x00", 11, DecodeError::InvalidUri },
    };

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Reader reader = Reader::fixed(cases[i].fixture, cases[i].len);
        Hyperlink out;
        DecodeError err = DecodeError::none;
        ASSERT_FALSE(sh::decode(&reader, zigstd::testing_allocator(), &out, &err));
        ASSERT_TRUE(err == cases[i].expected);
    }
}

TEST(snapshot_hyperlink, decodePage_ignores_empty_strings) {
    struct Fixture {
        const char *bytes;
        size_t len;
    };
    const Fixture fixtures[] = {
        { "\x01\x04\x03\x02\x01\x00\x00\x00\x00", 9 },
        { "\x02\x00\x00\x00\x00\x03\x00\x00\x00" "uri", 12 },
        { "\x02\x02\x00\x00\x00" "id" "\x00\x00\x00\x00", 11 },
    };

    for (size_t i = 0; i < sizeof fixtures / sizeof fixtures[0]; i++) {
        tp::Capacity cap;
        cap.cols = 1;
        cap.rows = 1;
        cap.hyperlink_bytes = 512;
        cap.string_bytes = 16;
        tp::Page page;
        ASSERT_TRUE(tp::Page::init(cap, &page));

        Reader reader = Reader::fixed(fixtures[i].bytes, fixtures[i].len);
        th::Id id = 0xFFFF;
        sh::DecodePageError err = sh::DecodePageError::none;
        const bool ok = sh::decodePage(&page, &reader, &id, &err);
        const size_t count = page.hyperlink_set.count();
        page.deinit();
        ASSERT_TRUE(ok);
        ASSERT_EQ(0, (long)id);
        ASSERT_EQ(0, (long)count);

        uint8_t b = 0;
        ReadError rerr = ReadError::none;
        ASSERT_FALSE(reader.takeByte(&b, &rerr));
        ASSERT_TRUE(rerr == ReadError::EndOfStream);
    }
}

TEST(snapshot_hyperlink, decode_rejects_invalid_kinds) {
    const uint8_t kinds[] = { 0, 3, 0xFF };
    for (size_t i = 0; i < 3; i++) {
        uint8_t fx[1] = { kinds[i] };
        Reader reader = Reader::fixed(fx, 1);
        Hyperlink out;
        DecodeError err = DecodeError::none;
        ASSERT_FALSE(sh::decode(&reader, zigstd::testing_allocator(), &out, &err));
        ASSERT_TRUE(err == DecodeError::InvalidKind);
    }
}

TEST(snapshot_hyperlink, decode_allocation_failure) {
    std::string implicit_fx, explicit_fx;
    ASSERT_TRUE(loadFixture("hyperlink-implicit-v1.hex", &implicit_fx));
    ASSERT_TRUE(loadFixture("hyperlink-explicit-v1.hex", &explicit_fx));

    {
        Reader reader = Reader::fixed(implicit_fx.data(), implicit_fx.size());
        zigstd::FailingAllocator failing(zigstd::testing_allocator(), 0);
        Hyperlink out;
        DecodeError err = DecodeError::none;
        ASSERT_FALSE(sh::decode(&reader, failing.allocator(), &out, &err));
        ASSERT_TRUE(err == DecodeError::OutOfMemory);
    }

    for (size_t fail_index = 0; fail_index < 2; fail_index++) {
        zigstd::FailingAllocator failing(zigstd::testing_allocator(), fail_index);
        Reader reader = Reader::fixed(explicit_fx.data(), explicit_fx.size());
        Hyperlink out;
        DecodeError err = DecodeError::none;
        ASSERT_FALSE(sh::decode(&reader, failing.allocator(), &out, &err));
        ASSERT_TRUE(err == DecodeError::OutOfMemory);
    }
}

TEST(snapshot_hyperlink, reject_every_truncation) {
    std::string fixtures[2];
    ASSERT_TRUE(loadFixture("hyperlink-implicit-v1.hex", &fixtures[0]));
    ASSERT_TRUE(loadFixture("hyperlink-explicit-v1.hex", &fixtures[1]));

    for (size_t f = 0; f < 2; f++) {
        const size_t n = fixtures[f].size();
        for (size_t fixture_len = 0; fixture_len < n; fixture_len++) {
            Reader reader = Reader::fixed(fixtures[f].data(), fixture_len);
            Hyperlink out;
            DecodeError err = DecodeError::none;
            ASSERT_FALSE(sh::decode(&reader, zigstd::testing_allocator(), &out, &err));
            ASSERT_TRUE(err == DecodeError::EndOfStream);
        }
    }
}

TEST(snapshot_hyperlink, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
