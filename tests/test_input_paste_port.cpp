/* Transliterated from the test blocks in Ghostty src/input/paste.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Wisp: "encodeWriter chunks through a small writer buffer" and
 * "encodeWriter too small" exercise std.Io.Writer's buffering and failure
 * modes. The writer here is a std::string, which has neither, so those two
 * have no counterpart and are not ported.
 */

#include <string.h>

#include <string>

#include "test_helpers.h"
#include "../input/paste.hpp"

using namespace wisp;

namespace paste = wisp::input::paste;

typedef paste::Options Options;
typedef paste::Encoded Encoded;
typedef paste::Error Error;

static const uint8_t *u(const char *s) { return (const uint8_t *)s; }

/* Wisp: `.{ .bracketed = b }` */
static Options opts(bool bracketed) { return Options(bracketed); }

static std::string part(const Encoded &e, size_t i) {
    return std::string(e.parts[i].ptr, e.parts[i].len);
}

TEST(input_paste, isSafe) {
    ASSERT_TRUE(paste::isSafe(u("hello"), 5));
    ASSERT_TRUE(!paste::isSafe(u("hello\n"), 6));
    ASSERT_TRUE(!paste::isSafe(u("hello\nworld"), 11));
    ASSERT_TRUE(!paste::isSafe(u("he\x1b[201~llo"), 11));
}

TEST(input_paste, isSafeWith) {
    /* Bracketed: newlines are fine, the frame terminator is not. */
    ASSERT_TRUE(paste::isSafeWith(u("hello"), 5, opts(true)));
    ASSERT_TRUE(paste::isSafeWith(u("hello\nworld"), 11, opts(true)));
    ASSERT_TRUE(!paste::isSafeWith(u("he\x1b[201~llo"), 11, opts(true)));
    ASSERT_TRUE(!paste::isSafeWith(u("hello\n\x1b[201~"), 12, opts(true)));

    /* Unbracketed: the conservative rule. */
    ASSERT_TRUE(paste::isSafeWith(u("hello"), 5, opts(false)));
    ASSERT_TRUE(!paste::isSafeWith(u("hello\nworld"), 11, opts(false)));
    ASSERT_TRUE(!paste::isSafeWith(u("he\x1b[201~llo"), 11, opts(false)));
}

TEST(input_paste, encodeWriter_bracketed) {
    std::string writer;
    paste::encodeWriter(&writer, u("hel\x1blo\nworld"), 12, opts(true));
    ASSERT_STR_EQ("\x1b[200~hel lo\nworld\x1b[201~", writer.c_str());
}

TEST(input_paste, encodeWriter_unbracketed) {
    std::string writer;
    paste::encodeWriter(&writer, u("hel\x00lo\r\nworld"), 13, opts(false));
    ASSERT_STR_EQ("hel lo\r\rworld", writer.c_str());
}

TEST(input_paste, encodeWriter_empty) {
    std::string writer;
    paste::encodeWriter(&writer, u(""), 0, opts(true));
    ASSERT_STR_EQ("\x1b[200~\x1b[201~", writer.c_str());
    writer.clear();
    paste::encodeWriter(&writer, u(""), 0, opts(false));
    ASSERT_STR_EQ("", writer.c_str());
}

TEST(input_paste, max_frame_size) {
    Encoded result;
    ASSERT_TRUE(paste::encodeConst(u(""), 0, opts(true), &result) == Error::none);
    ASSERT_TRUE(paste::max_frame_size == result.parts[0].len + result.parts[2].len);
}

TEST(input_paste, encode_bracketed) {
    Encoded result;
    ASSERT_TRUE(paste::encodeConst(u("hello"), 5, opts(true), &result) == Error::none);
    ASSERT_TRUE(part(result, 0) == "\x1b[200~");
    ASSERT_TRUE(part(result, 1) == "hello");
    ASSERT_TRUE(part(result, 2) == "\x1b[201~");
}

TEST(input_paste, encode_unbracketed_no_newlines) {
    Encoded result;
    ASSERT_TRUE(paste::encodeConst(u("hello"), 5, opts(false), &result) == Error::none);
    ASSERT_TRUE(part(result, 0) == "");
    ASSERT_TRUE(part(result, 1) == "hello");
    ASSERT_TRUE(part(result, 2) == "");
}

TEST(input_paste, encode_unbracketed_newlines_const) {
    Encoded result;
    ASSERT_TRUE(paste::encodeConst(u("hello\nworld"), 11, opts(false), &result) ==
                Error::MutableRequired);
}

TEST(input_paste, encode_unbracketed_newlines) {
    std::string data = "hello\nworld";
    const Encoded result = paste::encode((uint8_t *)data.data(), data.size(), opts(false));
    ASSERT_TRUE(part(result, 0) == "");
    ASSERT_TRUE(part(result, 1) == "hello\rworld");
    ASSERT_TRUE(part(result, 2) == "");
}

TEST(input_paste, encode_unbracketed_windows_stye_newline) {
    std::string data = "hello\r\nworld";
    const Encoded result = paste::encode((uint8_t *)data.data(), data.size(), opts(false));
    ASSERT_TRUE(part(result, 0) == "");
    ASSERT_TRUE(part(result, 1) == "hello\r\rworld");
    ASSERT_TRUE(part(result, 2) == "");
}

TEST(input_paste, encode_strip_unsafe_bytes_const) {
    Encoded result;
    ASSERT_TRUE(paste::encodeConst(u("hello\x00world"), 11, opts(true), &result) ==
                Error::MutableRequired);
}

TEST(input_paste, encode_strip_unsafe_bytes_mutable_bracketed) {
    std::string data("hel\x1blo\x00world", 12);
    const Encoded result = paste::encode((uint8_t *)data.data(), data.size(), opts(true));
    ASSERT_TRUE(part(result, 0) == "\x1b[200~");
    ASSERT_TRUE(part(result, 1) == "hel lo world");
    ASSERT_TRUE(part(result, 2) == "\x1b[201~");
}

TEST(input_paste, encode_strip_unsafe_bytes_mutable_unbracketed) {
    std::string data = "hel\x03lo";
    const Encoded result = paste::encode((uint8_t *)data.data(), data.size(), opts(false));
    ASSERT_TRUE(part(result, 0) == "");
    ASSERT_TRUE(part(result, 1) == "hel lo");
    ASSERT_TRUE(part(result, 2) == "");
}

TEST(input_paste, encode_strip_multiple_unsafe_bytes) {
    std::string data("\x00\x08\x7f", 3);
    const Encoded result = paste::encode((uint8_t *)data.data(), data.size(), opts(true));
    ASSERT_TRUE(part(result, 1) == "   ");
}
