/* Transliterated from the test blocks in Ghostty's stream_terminal.zig
 * dependencies: src/terminal/size_report.zig and src/terminal/clipboard.zig.
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include <string.h>

#include <string>

#include "test_helpers.h"
#include "../terminal/clipboard.hpp"
#include "../terminal/paste.hpp"
#include "../terminal/size_report.hpp"

using namespace wisp;
using namespace wisp::terminal;

/* ─── size_report.zig ──────────────────────────────────────────────────── */

static size_report::Size testSize() { return size_report::Size(24, 80, 9, 18); }

TEST(size_report, encode_mode_2048) {
    std::string writer;
    size_report::encode(&writer, size_report::Style::mode_2048, testSize());

    ASSERT_STR_EQ("\x1B[48;24;80;432;720t", writer.c_str());
}

TEST(size_report, encode_csi_14_t) {
    std::string writer;
    size_report::encode(&writer, size_report::Style::csi_14_t, testSize());

    ASSERT_STR_EQ("\x1b[4;432;720t", writer.c_str());
}

TEST(size_report, encode_csi_16_t) {
    std::string writer;
    size_report::encode(&writer, size_report::Style::csi_16_t, testSize());

    ASSERT_STR_EQ("\x1b[6;18;9t", writer.c_str());
}

TEST(size_report, encode_csi_18_t) {
    std::string writer;
    size_report::encode(&writer, size_report::Style::csi_18_t, testSize());

    ASSERT_STR_EQ("\x1b[8;24;80t", writer.c_str());
}

TEST(size_report, encode_max_values_for_all_fields) {
    const size_report::Size max_size(UINT16_MAX, UINT16_MAX, UINT32_MAX, UINT32_MAX);

    struct Case {
        size_report::Style style;
        const char *expected;
    };

    static const Case cases[] = {
        {size_report::Style::mode_2048, "\x1B[48;65535;65535;281470681677825;281470681677825t"},
        {size_report::Style::csi_14_t, "\x1b[4;281470681677825;281470681677825t"},
        {size_report::Style::csi_16_t, "\x1b[6;4294967295;4294967295t"},
        {size_report::Style::csi_18_t, "\x1b[8;65535;65535t"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        std::string writer;
        size_report::encode(&writer, cases[i].style, max_size);
        ASSERT_STR_EQ(cases[i].expected, writer.c_str());
    }
}

/* ─── clipboard.zig ────────────────────────────────────────────────────── */

static clipboard::ZStr z(const char *s) { return clipboard::ZStr(s, strlen(s)); }

TEST(clipboard, isTextMime) {
    ASSERT_TRUE(clipboard::isTextMime(z("text/plain")));
    ASSERT_TRUE(clipboard::isTextMime(z("UTF8_STRING")));
    ASSERT_TRUE(!clipboard::isTextMime(z("image/png")));
    ASSERT_TRUE(!clipboard::isTextMime(z(".")));
}

/* ─── paste.zig ────────────────────────────────────────────────────────── */
/* Wisp: upstream's paste.zig has no tests of its own ("The behavior is
 * tested end to end through the stream handler"), so this only pins the
 * shapes the stream handler builds. */

TEST(paste, source_and_contents_shapes) {
    namespace paste = wisp::terminal::paste;

    const paste::Source clip_src = paste::Source::makeClipboard(clipboard::Location::primary);
    ASSERT_TRUE(clip_src.tag == paste::Source::Tag::clipboard);
    ASSERT_TRUE(clip_src.clipboard == clipboard::Location::primary);
    ASSERT_TRUE(paste::Source::makeText().tag == paste::Source::Tag::text);

    const clipboard::Content memory[] = {
        clipboard::Content(z("text/plain"), z("hello")),
        clipboard::Content(z("image/png"), z("")),
    };
    paste::Contents contents;
    contents.tag = paste::Contents::Tag::memory;
    contents.memory = memory;
    contents.memory_len = 2;
    ASSERT_TRUE(2 == contents.len());
    ASSERT_TRUE(contents.mime(0).eql("text/plain"));
    ASSERT_TRUE(contents.mime(1).eql("image/png"));

    const clipboard::ZStr mimes[] = {z("text/uri-list"), z("text/plain")};
    paste::Contents reader;
    reader.tag = paste::Contents::Tag::reader;
    reader.reader.mimes = mimes;
    reader.reader.mimes_len = 2;
    ASSERT_TRUE(2 == reader.len());
    ASSERT_TRUE(reader.mime(0).eql("text/uri-list"));
}
