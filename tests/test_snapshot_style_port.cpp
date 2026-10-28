/* Transliterated from the test blocks in Ghostty
 * src/terminal/snapshot/style.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include <string.h>

#include <string>

#include "test_helpers.h"
#include "snapshot_fixture.hpp"
#include "../src/terminal/snapshot/style.hpp"
#include "../src/zigstd/reader.hpp"

using namespace wisp;

namespace ss = wisp::terminal::snapshot::style;
namespace fixture = wisp::test_fixture;

typedef zigstd::Reader Reader;
typedef zigstd::LimitedReader LimitedReader;
typedef ss::Style Style;
typedef ss::DecodeError DecodeError;
typedef ss::Underline Underline;

#ifndef WISP_SNAPSHOT_TESTDATA
#define WISP_SNAPSHOT_TESTDATA "snapshot_testdata"
#endif

static std::string fixturePath(const char *name) {
    std::string p = WISP_SNAPSHOT_TESTDATA;
    p += "/";
    p += name;
    return p;
}

static void writeU16(uint8_t *dst, uint16_t v) {
    dst[0] = (uint8_t)(v & 0xFF);
    dst[1] = (uint8_t)(v >> 8);
}

TEST(snapshot_style, golden_encoding_and_decoding) {
    Style value;
    value.fg_color = Style::Color::none();
    value.bg_color = Style::Color::makePalette(0x7f);
    terminal::RGB rgb;
    rgb.r = 0x12;
    rgb.g = 0x34;
    rgb.b = 0x56;
    value.underline_color = Style::Color::makeRgb(rgb);
    value.flags.bold = true;
    value.flags.italic = true;
    value.flags.faint = true;
    value.flags.blink = true;
    value.flags.inverse = true;
    value.flags.invisible = true;
    value.flags.strikethrough = true;
    value.flags.overline = true;
    value.flags.underline = Underline::curly;

    std::string buf;
    ss::encode(value, &buf);

    const std::string reference_path = fixturePath("style-v1.hex");
    std::string source_text;
    ASSERT_TRUE(fixture::readFile(reference_path.c_str(), &source_text));
    std::string golden;
    ASSERT_TRUE(fixture::parse(source_text.data(), source_text.size(), &golden));
    ASSERT_TRUE(fixture::expectEqual(fixture::Kind::bytes, reference_path.c_str(),
                                     "snapshot_fixture-style-v1.hex", golden, buf));

    Reader source = Reader::fixed(golden.data(), golden.size());
    uint8_t read_buf[1];
    LimitedReader limited = LimitedReader::limited(&source, read_buf, sizeof read_buf);
    Style decoded;
    DecodeError err = DecodeError::none;
    ASSERT_TRUE(ss::decode(&limited, &decoded, &err));
    ASSERT_TRUE(value.eql(decoded));
}

TEST(snapshot_style, flag_bit_layout) {
    struct Case {
        ss::Flags value;
        uint16_t expected;
    };
    Case cases[14];
    for (size_t i = 0; i < 14; i++) cases[i].value = ss::Flags();
    cases[0].value.bold = true;            cases[0].expected = 1 << 0;
    cases[1].value.italic = true;          cases[1].expected = 1 << 1;
    cases[2].value.faint = true;           cases[2].expected = 1 << 2;
    cases[3].value.blink = true;           cases[3].expected = 1 << 3;
    cases[4].value.inverse = true;         cases[4].expected = 1 << 4;
    cases[5].value.invisible = true;       cases[5].expected = 1 << 5;
    cases[6].value.strikethrough = true;   cases[6].expected = 1 << 6;
    cases[7].value.overline = true;        cases[7].expected = 1 << 7;
    cases[8].value.underline = (uint8_t)Underline::single;   cases[8].expected = 1 << 8;
    cases[9].value.underline = (uint8_t)Underline::double_;  cases[9].expected = 2 << 8;
    cases[10].value.underline = (uint8_t)Underline::curly;   cases[10].expected = 3 << 8;
    cases[11].value.underline = (uint8_t)Underline::dotted;  cases[11].expected = 4 << 8;
    cases[12].value.underline = (uint8_t)Underline::dashed;  cases[12].expected = 5 << 8;
    cases[13].value.reserved = 1;          cases[13].expected = 1 << 11;

    for (size_t i = 0; i < 14; i++) {
        ASSERT_EQ(cases[i].expected, cases[i].value.bits());
    }
}

TEST(snapshot_style, reject_invalid_colors) {
    const size_t offsets[] = { 0, 4, 8 };
    for (size_t i = 0; i < 3; i++) {
        uint8_t invalid_kind[ss::len] = { 0 };
        invalid_kind[offsets[i]] = 3;
        Reader reader = Reader::fixed(invalid_kind, sizeof invalid_kind);
        Style out;
        DecodeError err = DecodeError::none;
        ASSERT_FALSE(ss::decode(&reader, &out, &err));
        ASSERT_TRUE(err == DecodeError::InvalidColorKind);
    }

    uint8_t invalid_none[ss::len] = { 0 };
    invalid_none[1] = 1;
    {
        Reader reader = Reader::fixed(invalid_none, sizeof invalid_none);
        Style out;
        DecodeError err = DecodeError::none;
        ASSERT_FALSE(ss::decode(&reader, &out, &err));
        ASSERT_TRUE(err == DecodeError::InvalidColor);
    }

    uint8_t invalid_palette[ss::len] = { 0 };
    invalid_palette[0] = (uint8_t)ss::ColorKind::palette;
    invalid_palette[2] = 1;
    {
        Reader reader = Reader::fixed(invalid_palette, sizeof invalid_palette);
        Style out;
        DecodeError err = DecodeError::none;
        ASSERT_FALSE(ss::decode(&reader, &out, &err));
        ASSERT_TRUE(err == DecodeError::InvalidColor);
    }
}

TEST(snapshot_style, reject_invalid_flags_and_reserved_field) {
    const uint16_t underlines[] = { 6, 7 };
    for (size_t i = 0; i < 2; i++) {
        uint8_t invalid_underline[ss::len] = { 0 };
        writeU16(&invalid_underline[12], (uint16_t)(underlines[i] << 8));
        Reader reader = Reader::fixed(invalid_underline, sizeof invalid_underline);
        Style out;
        DecodeError err = DecodeError::none;
        ASSERT_FALSE(ss::decode(&reader, &out, &err));
        ASSERT_TRUE(err == DecodeError::InvalidUnderline);
    }

    uint8_t invalid_flags[ss::len] = { 0 };
    writeU16(&invalid_flags[12], 1 << 11);
    {
        Reader reader = Reader::fixed(invalid_flags, sizeof invalid_flags);
        Style out;
        DecodeError err = DecodeError::none;
        ASSERT_FALSE(ss::decode(&reader, &out, &err));
        ASSERT_TRUE(err == DecodeError::InvalidFlags);
    }

    uint8_t invalid_reserved[ss::len] = { 0 };
    writeU16(&invalid_reserved[14], 1);
    {
        Reader reader = Reader::fixed(invalid_reserved, sizeof invalid_reserved);
        Style out;
        DecodeError err = DecodeError::none;
        ASSERT_FALSE(ss::decode(&reader, &out, &err));
        ASSERT_TRUE(err == DecodeError::InvalidReserved);
    }
}

TEST(snapshot_style, decodeOrDiscard_preserves_the_next_entry_boundary) {
    uint8_t fixture_bytes[ss::len + 1] = { 0 };
    fixture_bytes[0] = 3;
    fixture_bytes[ss::len] = 0xFF;

    Reader reader = Reader::fixed(fixture_bytes, sizeof fixture_bytes);
    Style out;
    DecodeError err = DecodeError::none;
    ASSERT_FALSE(ss::decodeOrDiscard(&reader, &out, &err));
    ASSERT_TRUE(err == DecodeError::InvalidColorKind);

    uint8_t next = 0;
    zigstd::ReadError rerr = zigstd::ReadError::none;
    ASSERT_TRUE(reader.readSliceAll(&next, 1, &rerr));
    ASSERT_EQ(0xFF, next);
}

TEST(snapshot_style, decodeOrNull_distinguishes_semantic_errors_from_truncation) {
    uint8_t invalid[ss::len] = { 0 };
    invalid[0] = 3;
    {
        Reader reader = Reader::fixed(invalid, sizeof invalid);
        Style out;
        bool has = true;
        zigstd::ReadError rerr = zigstd::ReadError::none;
        ASSERT_TRUE(ss::decodeOrNull(&reader, &out, &has, &rerr));
        ASSERT_FALSE(has);
    }
    {
        Reader truncated = Reader::fixed(invalid, ss::len - 1);
        Style out;
        bool has = true;
        zigstd::ReadError rerr = zigstd::ReadError::none;
        ASSERT_FALSE(ss::decodeOrNull(&truncated, &out, &has, &rerr));
        ASSERT_TRUE(rerr == zigstd::ReadError::EndOfStream);
    }
}

TEST(snapshot_style, reject_every_truncation) {
    const uint8_t fixture_bytes[ss::len] = { 0 };
    for (size_t fixture_len = 0; fixture_len < ss::len; fixture_len++) {
        Reader reader = Reader::fixed(fixture_bytes, fixture_len);
        Style out;
        DecodeError err = DecodeError::none;
        ASSERT_FALSE(ss::decode(&reader, &out, &err));
        ASSERT_TRUE(err == DecodeError::EndOfStream);
    }
}
