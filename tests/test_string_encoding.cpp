/* Transliterated from the test blocks in Ghostty src/os/string_encoding.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include "test_helpers.h"
#include "../os/string_encoding.hpp"

#include <stdio.h>

using namespace wisp::os::string_encoding;

static bool q(std::string *w, const char *s) { return printfQDecode(w, s, strlen(s)); }
static bool pct(std::string *w, const char *s) { return urlPercentDecode(w, s, strlen(s)); }

TEST(string_encoding, printf_q_1) {
    std::string w;
    ASSERT_TRUE(q(&w, "bobr\\ kurwa"));
    ASSERT_TRUE(w == "bobr kurwa");
}

TEST(string_encoding, printf_q_2) {
    std::string w;
    ASSERT_TRUE(q(&w, "bobr\\nkurwa"));
    ASSERT_TRUE(w == "bobr\nkurwa");
}

TEST(string_encoding, printf_q_3) {
    std::string w;
    ASSERT_FALSE(q(&w, "bobr\\dkurwa"));
}

TEST(string_encoding, printf_q_4) {
    std::string w;
    ASSERT_FALSE(q(&w, "bobr kurwa\\"));
}

TEST(string_encoding, printf_q_5) {
    std::string w;
    ASSERT_TRUE(q(&w, "$'bobr kurwa'"));
    ASSERT_TRUE(w == "bobr kurwa");
}

TEST(string_encoding, printf_q_6) {
    std::string w;
    ASSERT_TRUE(q(&w, "'bobr kurwa'"));
    ASSERT_TRUE(w == "bobr kurwa");
}

TEST(string_encoding, printf_q_7) {
    std::string w;
    ASSERT_FALSE(q(&w, "$'bobr kurwa"));
}

TEST(string_encoding, printf_q_8) {
    std::string w;
    ASSERT_FALSE(q(&w, "$'"));
}

TEST(string_encoding, printf_q_9) {
    std::string w;
    ASSERT_FALSE(q(&w, "'bobr kurwa"));
}

TEST(string_encoding, printf_q_10) {
    std::string w;
    ASSERT_FALSE(q(&w, "'"));
}

TEST(string_encoding, singles_percent) {
    for (unsigned c = 0; c < 255; c++) {
        std::string w;
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02x", c);
        ASSERT_TRUE(pct(&w, buf));
        ASSERT_TRUE(w.size() == 1);
        ASSERT_TRUE((uint8_t)w[0] == c);
    }
    for (unsigned c = 0; c < 255; c++) {
        std::string w;
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02X", c);
        ASSERT_TRUE(pct(&w, buf));
        ASSERT_TRUE(w.size() == 1);
        ASSERT_TRUE((uint8_t)w[0] == c);
    }
}

TEST(string_encoding, percent_1) {
    std::string w;
    ASSERT_TRUE(pct(&w, "bobr%20kurwa"));
    ASSERT_TRUE(w == "bobr kurwa");
}

TEST(string_encoding, percent_2) {
    std::string w;
    ASSERT_FALSE(pct(&w, "bobr%2kurwa"));
}

TEST(string_encoding, percent_3) {
    std::string w;
    ASSERT_FALSE(pct(&w, "bobr%kurwa"));
}

TEST(string_encoding, percent_4) {
    std::string w;
    ASSERT_FALSE(pct(&w, "bobr%%kurwa"));
}

TEST(string_encoding, percent_5) {
    std::string w;
    ASSERT_TRUE(pct(&w, "bobr%20kurwa%20"));
    ASSERT_TRUE(w == "bobr kurwa ");
}

TEST(string_encoding, percent_6) {
    std::string w;
    ASSERT_FALSE(pct(&w, "bobr%20kurwa%2"));
}

TEST(string_encoding, percent_7) {
    std::string w;
    ASSERT_FALSE(pct(&w, "bobr%20kurwa%"));
}
