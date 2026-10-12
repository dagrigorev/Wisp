/* Transliterated from the test blocks in Ghostty src/terminal/focus.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 */

#include <string>

#include "test_helpers.h"
#include "focus.hpp"

using namespace wisp::terminal::focus;

/* ASSERT_STR_EQ expands to several statements, so a temporary std::string
 * argument would dangle; bind it first. */
#define EXPECT_STR(expected, actual)                                                               \
    do {                                                                                           \
        const std::string _actual = (actual);                                                      \
        ASSERT_STR_EQ(expected, _actual.c_str());                                                  \
    } while (0)

TEST(focus, encode_focus_gained) {
    /* Wisp: upstream writes into a [max_encode_size]u8 fixed writer; the
     * std::string writer cannot overflow, so the bound is asserted instead. */
    std::string writer;
    encode(&writer, Event::gained);
    ASSERT_TRUE(writer.size() <= max_encode_size);
    EXPECT_STR("\x1B[I", writer);
}

TEST(focus, encode_focus_lost) {
    std::string writer;
    encode(&writer, Event::lost);
    ASSERT_TRUE(writer.size() <= max_encode_size);
    EXPECT_STR("\x1B[O", writer);
}
