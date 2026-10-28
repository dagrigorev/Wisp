/* Transliterated from the test blocks in Ghostty src/input/kitty.zig,
 * src/input/function_keys.zig and src/input/keycodes.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Wisp:
 *   - kitty.zig's and keycodes.zig's unnamed tests only force comptime
 *     evaluation of `entries`. Here they are named kitty_entries and
 *     keycodes_entries and build the tables at runtime instead.
 *   - function_keys.zig "keys" compares against
 *     termio.Message.WriteReq.Small.Max, which is `MessageData(u8, 38)`
 *     upstream. termio is not ported yet, so the constant is written out.
 */

#include <stddef.h>

#include "test_helpers.h"
#include "../src/input/function_keys.hpp"
#include "../src/input/keycodes.hpp"
#include "../src/input/kitty.hpp"

using namespace wisp;
using namespace wisp::input;

TEST(input_kitty, kitty_entries) {
    /* To force comptime to test it */
    ASSERT_TRUE(kitty::entries_len > 0);
}

TEST(input_function_keys, keys) {
    /* Force resolution for comptime evaluation. */
    const function_keys::KeyEntryArray &keys = function_keys::keys();

    /* All key sequences must fit into a termio array. */
    const size_t max = 38;
    for (size_t i = 0; i < key_count; i++) {
        for (const function_keys::Entry &entry : keys.values[i]) {
            ASSERT_TRUE(entry.sequence.size() <= max);
        }
    }
}

TEST(input_keycodes, keycodes_entries) {
    ASSERT_EQ((long)keycodes::raw_entries_len, (long)keycodes::entries().size());
}
