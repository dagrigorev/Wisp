/* Transliterated from Ghostty src/input/kitty.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: upstream builds `entries` at comptime from `raw_entries` tuples. The
 * table here is the structured `entries` directly, in the same order; there
 * is nothing to convert at runtime.
 */
#pragma once
#ifndef WISP_INPUT_KITTY_HPP
#define WISP_INPUT_KITTY_HPP

#include <stddef.h>
#include <stdint.h>

#include "key.hpp"

namespace wisp {
namespace input {
namespace kitty {

/* A single entry in the kitty keymap data. There are only ~100 entries
 * so the recommendation is to just use a linear search to find the entry
 * for a given key. */
struct Entry {
    Key key;
    uint32_t code;
    uint8_t final;
    bool modifier;
};

/* The raw data for how to map keys to Kitty data. Based on the information:
 * https://sw.kovidgoyal.net/kitty/keyboard-protocol/#functional-key-definitions
 * And the exact table is ported from Foot:
 * https://codeberg.org/dnkl/foot/src/branch/master/kitty-keymap.h
 *
 * Note that we currently don't support all the same keysyms as Kitty,
 * but we can add them as we add support.
 *
 * These are stored in order of the above Kitty link, so it is easy to
 * find the entry for a given key.
 *
 * Wisp: this is upstream's `entries` (the full list of entries for the
 * current platform); `entries_len` is its length. */
static const Entry entries[] = {
    {Key::escape, 27, 'u', false},
    {Key::enter, 13, 'u', false},
    {Key::tab, 9, 'u', false},
    {Key::backspace, 127, 'u', false},
    {Key::insert, 2, '~', false},
    {Key::delete_, 3, '~', false},
    {Key::arrow_left, 1, 'D', false},
    {Key::arrow_right, 1, 'C', false},
    {Key::arrow_up, 1, 'A', false},
    {Key::arrow_down, 1, 'B', false},
    {Key::page_up, 5, '~', false},
    {Key::page_down, 6, '~', false},
    {Key::home, 1, 'H', false},
    {Key::end, 1, 'F', false},
    {Key::caps_lock, 57358, 'u', true},
    {Key::scroll_lock, 57359, 'u', false},
    {Key::num_lock, 57360, 'u', true},
    {Key::print_screen, 57361, 'u', false},
    {Key::pause, 57362, 'u', false},

    {Key::f1, 1, 'P', false},
    {Key::f2, 1, 'Q', false},
    {Key::f3, 13, '~', false},
    {Key::f4, 1, 'S', false},
    {Key::f5, 15, '~', false},
    {Key::f6, 17, '~', false},
    {Key::f7, 18, '~', false},
    {Key::f8, 19, '~', false},
    {Key::f9, 20, '~', false},
    {Key::f10, 21, '~', false},
    {Key::f11, 23, '~', false},
    {Key::f12, 24, '~', false},
    {Key::f13, 57376, 'u', false},
    {Key::f14, 57377, 'u', false},
    {Key::f15, 57378, 'u', false},
    {Key::f16, 57379, 'u', false},
    {Key::f17, 57380, 'u', false},
    {Key::f18, 57381, 'u', false},
    {Key::f19, 57382, 'u', false},
    {Key::f20, 57383, 'u', false},
    {Key::f21, 57384, 'u', false},
    {Key::f22, 57385, 'u', false},
    {Key::f23, 57386, 'u', false},
    {Key::f24, 57387, 'u', false},
    {Key::f25, 57388, 'u', false},

    {Key::numpad_0, 57399, 'u', false},
    {Key::numpad_1, 57400, 'u', false},
    {Key::numpad_2, 57401, 'u', false},
    {Key::numpad_3, 57402, 'u', false},
    {Key::numpad_4, 57403, 'u', false},
    {Key::numpad_5, 57404, 'u', false},
    {Key::numpad_6, 57405, 'u', false},
    {Key::numpad_7, 57406, 'u', false},
    {Key::numpad_8, 57407, 'u', false},
    {Key::numpad_9, 57408, 'u', false},
    {Key::numpad_decimal, 57409, 'u', false},
    {Key::numpad_divide, 57410, 'u', false},
    {Key::numpad_multiply, 57411, 'u', false},
    {Key::numpad_subtract, 57412, 'u', false},
    {Key::numpad_add, 57413, 'u', false},
    {Key::numpad_enter, 57414, 'u', false},
    {Key::numpad_equal, 57415, 'u', false},
    {Key::numpad_separator, 57416, 'u', false},
    {Key::numpad_left, 57417, 'u', false},
    {Key::numpad_right, 57418, 'u', false},
    {Key::numpad_up, 57419, 'u', false},
    {Key::numpad_down, 57420, 'u', false},
    {Key::numpad_page_up, 57421, 'u', false},
    {Key::numpad_page_down, 57422, 'u', false},
    {Key::numpad_home, 57423, 'u', false},
    {Key::numpad_end, 57424, 'u', false},
    {Key::numpad_insert, 57425, 'u', false},
    {Key::numpad_delete, 57426, 'u', false},
    {Key::numpad_begin, 57427, 'u', false},

    {Key::shift_left, 57441, 'u', true},
    {Key::shift_right, 57447, 'u', true},
    {Key::control_left, 57442, 'u', true},
    {Key::control_right, 57448, 'u', true},
    {Key::meta_left, 57444, 'u', true},
    {Key::meta_right, 57450, 'u', true},
    {Key::alt_left, 57443, 'u', true},
    {Key::alt_right, 57449, 'u', true},
};
static const size_t entries_len = sizeof(entries) / sizeof(entries[0]);

} /* namespace kitty */
} /* namespace input */
} /* namespace wisp */

#endif /* WISP_INPUT_KITTY_HPP */
