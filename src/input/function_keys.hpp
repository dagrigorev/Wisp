/* Transliterated from Ghostty src/input/function_keys.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * This is the list of "PC style function keys" that xterm supports for
 * the legacy keyboard protocols. These always take priority since at the
 * time of writing this, even the most modern keyboard protocols still
 * are backwards compatible with regards to these sequences.
 *
 * This is based on a variety of sources cross-referenced but mostly
 * based on foot's keymap.h: https://codeberg.org/dnkl/foot/src/branch/master/keymap.h
 *
 * Wisp, differences in shape rather than behavior:
 *   - Upstream builds `keys` at comptime. Here `keys()` builds the same
 *     table once, on first use, with the same helper functions in the same
 *     order; `pcStyle`'s comptimePrint becomes a "{}" substitution.
 *   - `std.EnumArray(Key, []const Entry)` is an array of vectors indexed by
 *     the Key's integer value.
 *   - `?[]const u8` sequence_decbkm is `has_sequence_decbkm` plus a string.
 */
#pragma once
#ifndef WISP_INPUT_FUNCTION_KEYS_HPP
#define WISP_INPUT_FUNCTION_KEYS_HPP

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "key.hpp"

namespace wisp {
namespace input {
namespace function_keys {

enum class CursorMode : uint8_t { any, normal, application };
enum class KeypadMode : uint8_t { any, normal, application };

/* A bit confusing so I'll document this one: this is the "modify other keys"
 * setting. We only change behavior for "set_other" which is ESC [ > 4; 2 m.
 * So this can be "any" which means we don't care what's going on. Or it
 * can be "set" which means modify keys must be set EXCEPT FOR "other keys"
 * mode, and "set_other" which means modify keys must be set to "other keys"
 * mode.
 *
 * See: https://invisible-island.net/xterm/modified-keys.html */
enum class ModifyKeys : uint8_t {
    any,
    set,
    set_other,
};

/* A single entry in the table of keys. */
struct Entry {
    /* The exact set of modifiers that must be active for this entry to match.
     * If mods_empty_is_any is true then empty mods means any set of mods can
     * match. Otherwise, empty mods means no mods must be active. */
    Mods mods;
    bool mods_empty_is_any = true;

    /* The state required for cursor/keypad mode. */
    CursorMode cursor = CursorMode::any;
    KeypadMode keypad = KeypadMode::any;

    /* Whether or not this entry should be used */
    ModifyKeys modify_other_keys = ModifyKeys::any;

    /* The sequence to send to the pty if this entry matches. */
    std::string sequence;

    /* Sequence to send to the PTY if DECBKM is set. */
    bool has_sequence_decbkm = false;
    std::string sequence_decbkm;
};

namespace detail {

inline Mods mods(bool shift, bool alt, bool ctrl, bool super) {
    Mods m;
    m.shift = shift;
    m.alt = alt;
    m.ctrl = ctrl;
    m.super = super;
    return m;
}

/* Wisp: `.{ .sequence = s }` */
inline Entry seq(const std::string &s) {
    Entry e;
    e.sequence = s;
    return e;
}

/* Wisp: `.{ .mods = m, .modify_other_keys = k, .sequence = s }` */
inline Entry modSeq(Mods m, ModifyKeys k, const std::string &s) {
    Entry e;
    e.mods = m;
    e.modify_other_keys = k;
    e.sequence = s;
    return e;
}

inline std::vector<Entry> concat(std::vector<Entry> a, const std::vector<Entry> &b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

} /* namespace detail */

/* The list of modifier combinations for modify other key sequences.
 * The mode value is index + 2. */
inline const std::vector<Mods> &modifiers() {
    using detail::mods;
    static const std::vector<Mods> result = {
        mods(true, false, false, false),
        mods(false, true, false, false),
        mods(true, true, false, false),
        mods(false, false, true, false),
        mods(true, false, true, false),
        mods(false, true, true, false),
        mods(true, true, true, false),
        mods(false, false, false, true),
        mods(true, false, false, true),
        mods(false, true, false, true),
        mods(true, true, false, true),
        mods(false, false, true, true),
        mods(true, false, true, true),
        mods(false, true, true, true),
        mods(true, true, true, true),
    };
    return result;
}

/* Constructs a set of pcStyle function keys using the given format. The
 * format should have exactly one "hole" for the mods code.
 * Example: "\x1b[11;{}~" for F1. */
inline std::vector<Entry> pcStyle(const std::string &fmt) {
    std::vector<Entry> entries;
    const std::vector<Mods> &mods = modifiers();
    const size_t hole = fmt.find("{}");
    for (size_t i = 0; i < mods.size(); i++) {
        const size_t code = i + 2;
        Entry e;
        e.mods = mods[i];
        e.sequence = fmt.substr(0, hole) + std::to_string(code) + fmt.substr(hole + 2);
        entries.push_back(e);
    }

    return entries;
}

/* Returns the default keypad application mode entry. */
inline std::vector<Entry> kpDefault(const std::string &suffix) {
    Entry e;
    e.mods_empty_is_any = false;
    e.keypad = KeypadMode::application;
    e.sequence = "\x1bO" + suffix;
    return {e};
}

/* Returns the entries for a keypad key. The suffix is the final character
 * of the sent sequence, such as "r" for kp_2. */
inline std::vector<Entry> kpKeys(const std::string &suffix, const std::string &normal) {
    std::vector<Entry> pc = pcStyle("\x1bO{}" + suffix);
    for (Entry &entry : pc) entry.keypad = KeypadMode::application;
    Entry n;
    n.keypad = KeypadMode::normal;
    n.sequence = normal;
    return detail::concat(detail::concat(kpDefault(suffix), pc), {n});
}

/* Returns entries that are dependent on cursor key settings. */
inline std::vector<Entry> cursorKey(const std::string &normal, const std::string &application) {
    Entry a;
    a.cursor = CursorMode::normal;
    a.sequence = normal;
    Entry b;
    b.cursor = CursorMode::application;
    b.sequence = application;
    return {a, b};
}

/* This is the array of entries for the PC style function keys as mapped to
 * our set of possible key codes. Not every key has any entries. */
struct KeyEntryArray {
    std::vector<Entry> values[key_count];

    void set(Key key, const std::vector<Entry> &entries) { values[(size_t)key] = entries; }
    const std::vector<Entry> &get(Key key) const { return values[(size_t)key]; }
};

/* The list of keys that we support and their entry values. It is expected
 * that you'll just use a for loop through the entry values, there are never
 * more than a couple dozen or so. */
inline const KeyEntryArray &keys() {
    static const KeyEntryArray table = [] {
        using detail::concat;
        using detail::modSeq;
        using detail::mods;
        using detail::seq;
        const ModifyKeys set = ModifyKeys::set;
        const ModifyKeys set_other = ModifyKeys::set_other;
        const ModifyKeys any = ModifyKeys::any;

        KeyEntryArray result;

        result.set(Key::arrow_up, concat(pcStyle("\x1b[1;{}A"), cursorKey("\x1b[A", "\x1bOA")));
        result.set(Key::arrow_down, concat(pcStyle("\x1b[1;{}B"), cursorKey("\x1b[B", "\x1bOB")));
        result.set(Key::arrow_right, concat(pcStyle("\x1b[1;{}C"), cursorKey("\x1b[C", "\x1bOC")));
        result.set(Key::arrow_left, concat(pcStyle("\x1b[1;{}D"), cursorKey("\x1b[D", "\x1bOD")));
        result.set(Key::home, concat(pcStyle("\x1b[1;{}H"), cursorKey("\x1b[H", "\x1bOH")));
        result.set(Key::end, concat(pcStyle("\x1b[1;{}F"), cursorKey("\x1b[F", "\x1bOF")));
        result.set(Key::insert, concat(pcStyle("\x1b[2;{}~"), {seq("\x1B[2~")}));
        result.set(Key::delete_, concat(pcStyle("\x1b[3;{}~"), {seq("\x1B[3~")}));
        result.set(Key::page_up, concat(pcStyle("\x1b[5;{}~"), {seq("\x1B[5~")}));
        result.set(Key::page_down, concat(pcStyle("\x1b[6;{}~"), {seq("\x1B[6~")}));
        result.set(Key::help, concat(pcStyle("\x1b[28;{}~"), {seq("\x1B[28~")}));
        result.set(Key::context_menu, concat(pcStyle("\x1b[29;{}~"), {seq("\x1B[29~")}));

        /* Function Keys. todo: f26-f35 but we need to add to input.Key */
        result.set(Key::f1, concat(pcStyle("\x1b[1;{}P"), {seq("\x1BOP")}));
        result.set(Key::f2, concat(pcStyle("\x1b[1;{}Q"), {seq("\x1BOQ")}));
        result.set(Key::f3, concat(pcStyle("\x1b[13;{}~"), {seq("\x1BOR")}));
        result.set(Key::f4, concat(pcStyle("\x1b[1;{}S"), {seq("\x1BOS")}));
        result.set(Key::f5, concat(pcStyle("\x1b[15;{}~"), {seq("\x1B[15~")}));
        result.set(Key::f6, concat(pcStyle("\x1b[17;{}~"), {seq("\x1B[17~")}));
        result.set(Key::f7, concat(pcStyle("\x1b[18;{}~"), {seq("\x1B[18~")}));
        result.set(Key::f8, concat(pcStyle("\x1b[19;{}~"), {seq("\x1B[19~")}));
        result.set(Key::f9, concat(pcStyle("\x1b[20;{}~"), {seq("\x1B[20~")}));
        result.set(Key::f10, concat(pcStyle("\x1b[21;{}~"), {seq("\x1B[21~")}));
        result.set(Key::f11, concat(pcStyle("\x1b[23;{}~"), {seq("\x1B[23~")}));
        result.set(Key::f12, concat(pcStyle("\x1b[24;{}~"), {seq("\x1B[24~")}));
        result.set(Key::f13, concat(pcStyle("\x1b[25;{}~"), {seq("\x1B[25~")}));
        result.set(Key::f14, concat(pcStyle("\x1b[26;{}~"), {seq("\x1B[26~")}));
        result.set(Key::f15, concat(pcStyle("\x1b[28;{}~"), {seq("\x1B[28~")}));
        result.set(Key::f16, concat(pcStyle("\x1b[29;{}~"), {seq("\x1B[29~")}));
        result.set(Key::f17, concat(pcStyle("\x1b[31;{}~"), {seq("\x1B[31~")}));
        result.set(Key::f18, concat(pcStyle("\x1b[32;{}~"), {seq("\x1B[32~")}));
        result.set(Key::f19, concat(pcStyle("\x1b[33;{}~"), {seq("\x1B[33~")}));
        result.set(Key::f20, concat(pcStyle("\x1b[34;{}~"), {seq("\x1B[34~")}));
        result.set(Key::f21, concat(pcStyle("\x1b[42;{}~"), {seq("\x1B[42~")}));
        result.set(Key::f22, concat(pcStyle("\x1b[43;{}~"), {seq("\x1B[43~")}));
        result.set(Key::f23, concat(pcStyle("\x1b[44;{}~"), {seq("\x1B[44~")}));
        result.set(Key::f24, concat(pcStyle("\x1b[45;{}~"), {seq("\x1B[45~")}));
        result.set(Key::f25, concat(pcStyle("\x1b[46;{}~"), {seq("\x1B[46~")}));

        /* Keypad keys */
        result.set(Key::numpad_0, kpKeys("p", "0"));
        result.set(Key::numpad_1, kpKeys("q", "1"));
        result.set(Key::numpad_2, kpKeys("r", "2"));
        result.set(Key::numpad_3, kpKeys("s", "3"));
        result.set(Key::numpad_4, kpKeys("t", "4"));
        result.set(Key::numpad_5, kpKeys("u", "5"));
        result.set(Key::numpad_6, kpKeys("v", "6"));
        result.set(Key::numpad_7, kpKeys("w", "7"));
        result.set(Key::numpad_8, kpKeys("x", "8"));
        result.set(Key::numpad_9, kpKeys("y", "9"));
        result.set(Key::numpad_decimal, kpKeys("n", "."));
        result.set(Key::numpad_divide, kpKeys("o", "/"));
        result.set(Key::numpad_multiply, kpKeys("j", "*"));
        result.set(Key::numpad_subtract, kpKeys("m", "-"));
        result.set(Key::numpad_add, kpKeys("k", "+"));
        result.set(Key::numpad_enter, kpKeys("M", "\r"));
        result.set(Key::numpad_up, concat(pcStyle("\x1b[1;{}A"), cursorKey("\x1b[A", "\x1bOA")));
        result.set(Key::numpad_down, concat(pcStyle("\x1b[1;{}B"), cursorKey("\x1b[B", "\x1bOB")));
        result.set(Key::numpad_right, concat(pcStyle("\x1b[1;{}C"), cursorKey("\x1b[C", "\x1bOC")));
        result.set(Key::numpad_left, concat(pcStyle("\x1b[1;{}D"), cursorKey("\x1b[D", "\x1bOD")));
        result.set(Key::numpad_begin, concat(pcStyle("\x1b[1;{}E"), cursorKey("\x1b[E", "\x1bOE")));
        result.set(Key::numpad_home, concat(pcStyle("\x1b[1;{}H"), cursorKey("\x1b[H", "\x1bOH")));
        result.set(Key::numpad_end, concat(pcStyle("\x1b[1;{}F"), cursorKey("\x1b[F", "\x1bOF")));
        result.set(Key::numpad_insert, concat(pcStyle("\x1b[2;{}~"), {seq("\x1B[2~")}));
        result.set(Key::numpad_delete, concat(pcStyle("\x1b[3;{}~"), {seq("\x1B[3~")}));
        result.set(Key::numpad_page_up, concat(pcStyle("\x1b[5;{}~"), {seq("\x1B[5~")}));
        result.set(Key::numpad_page_down, concat(pcStyle("\x1b[6;{}~"), {seq("\x1B[6~")}));

        Entry ctrl_backspace = seq("\x08");
        ctrl_backspace.mods = mods(false, false, true, false);
        ctrl_backspace.has_sequence_decbkm = true;
        ctrl_backspace.sequence_decbkm = "\x7f";
        Entry plain_backspace = seq("\x7f");
        plain_backspace.has_sequence_decbkm = true;
        plain_backspace.sequence_decbkm = "\x08";
        result.set(Key::backspace, {
            /* Modify Keys Normal */
            modSeq(mods(true, false, false, false), set, "\x7f"),
            modSeq(mods(false, true, false, false), set, "\x1b\x7f"),
            modSeq(mods(true, true, false, false), set, "\x1b\x7f"),
            modSeq(mods(true, false, true, false), set, "\x08"),
            modSeq(mods(false, true, true, false), set, "\x1b\x08"),
            modSeq(mods(false, false, false, true), set, "\x7f"),
            modSeq(mods(true, false, false, true), set, "\x7f"),
            modSeq(mods(false, true, false, true), set, "\x1b\x7f"),
            modSeq(mods(true, true, false, true), set, "\x1b\x7f"),
            modSeq(mods(false, false, true, true), set, "\x08"),
            modSeq(mods(true, false, true, true), set, "\x08"),
            modSeq(mods(false, true, true, true), set, "\x1b\x08"),
            modSeq(mods(true, true, true, true), set, "\x1b\x08"),

            /* Modify Keys Other */
            modSeq(mods(true, false, false, false), set_other, "\x1b[27;2;127~"),
            modSeq(mods(false, true, false, false), set_other, "\x1b[27;3;127~"),
            modSeq(mods(true, true, false, false), set_other, "\x1b[27;4;127~"),
            modSeq(mods(true, false, true, false), set_other, "\x1b[27;6;127~"),
            modSeq(mods(false, true, true, false), set_other, "\x1b[27;7;127~"),
            modSeq(mods(true, true, true, false), set_other, "\x1b[27;8;127~"),
            modSeq(mods(false, false, false, true), set_other, "\x1b[27;9;127~"),
            modSeq(mods(true, false, false, true), set_other, "\x1b[27;10;127~"),
            modSeq(mods(false, true, false, true), set_other, "\x1b[27;11;127~"),
            modSeq(mods(true, true, false, true), set_other, "\x1b[27;12;127~"),
            modSeq(mods(false, false, true, true), set_other, "\x1b[27;13;127~"),
            modSeq(mods(true, false, true, true), set_other, "\x1b[27;14;127~"),
            modSeq(mods(false, true, true, true), set_other, "\x1b[27;15;127~"),
            modSeq(mods(true, true, true, true), set_other, "\x1b[27;16;127~"),

            ctrl_backspace,
            plain_backspace,
        });

        result.set(Key::tab, {
            /* Modify Keys Normal */
            modSeq(mods(true, false, false, false), set, "\x1b[Z"),
            modSeq(mods(false, true, false, false), set, "\x1b\t"),

            /* Modify Keys Other */
            modSeq(mods(true, false, false, false), set_other, "\x1b[27;2;9~"),
            modSeq(mods(false, true, false, false), set_other, "\x1b[27;3;9~"),

            /* Everything else */
            modSeq(mods(true, true, false, false), any, "\x1b[27;4;9~"),
            modSeq(mods(false, false, true, false), any, "\x1b[27;5;9~"),
            modSeq(mods(true, false, true, false), any, "\x1b[27;6;9~"),
            modSeq(mods(false, true, true, false), any, "\x1b[27;7;9~"),
            modSeq(mods(true, true, true, false), any, "\x1b[27;8;9~"),
            modSeq(mods(false, false, false, true), any, "\x1b[27;9;9~"),
            modSeq(mods(true, false, false, true), any, "\x1b[27;10;9~"),
            modSeq(mods(false, true, false, true), any, "\x1b[27;11;9~"),
            modSeq(mods(true, true, false, true), any, "\x1b[27;12;9~"),
            modSeq(mods(false, false, true, true), any, "\x1b[27;13;9~"),
            modSeq(mods(true, false, true, true), any, "\x1b[27;14;9~"),
            modSeq(mods(false, true, true, true), any, "\x1b[27;15;9~"),
            modSeq(mods(true, true, true, true), any, "\x1b[27;16;9~"),

            seq("\t"),
        });

        result.set(Key::enter, {
            modSeq(mods(true, false, false, false), any, "\x1b[27;2;13~"),

            /* Modify Keys Normal */
            modSeq(mods(false, true, false, false), set, "\x1b\r"),

            /* Modify Keys Other */
            modSeq(mods(false, true, false, false), set_other, "\x1b[27;3;13~"),

            /* Everything else */
            modSeq(mods(true, true, false, false), any, "\x1b[27;4;13~"),
            modSeq(mods(false, false, true, false), any, "\x1b[27;5;13~"),
            modSeq(mods(true, false, true, false), any, "\x1b[27;6;13~"),
            modSeq(mods(false, true, true, false), any, "\x1b[27;7;13~"),
            modSeq(mods(true, true, true, false), any, "\x1b[27;8;13~"),
            modSeq(mods(false, false, false, true), any, "\x1b[27;9;13~"),
            modSeq(mods(true, false, false, true), any, "\x1b[27;10;13~"),
            modSeq(mods(false, true, false, true), any, "\x1b[27;11;13~"),
            modSeq(mods(true, true, false, true), any, "\x1b[27;12;13~"),
            modSeq(mods(false, false, true, true), any, "\x1b[27;13;13~"),
            modSeq(mods(true, false, true, true), any, "\x1b[27;14;13~"),
            modSeq(mods(false, true, true, true), any, "\x1b[27;15;13~"),
            modSeq(mods(true, true, true, true), any, "\x1b[27;16;13~"),

            seq("\r"),
        });

        result.set(Key::escape, {
            modSeq(mods(true, false, false, false), any, "\x1b[27;2;27~"),
            modSeq(mods(false, true, false, false), set, "\x1b\x1b"),
            modSeq(mods(false, true, false, false), set_other, "\x1b[27;3;27~"),
            modSeq(mods(true, true, false, false), any, "\x1b[27;4;27~"),
            modSeq(mods(false, false, true, false), any, "\x1b[27;5;27~"),
            modSeq(mods(true, false, true, false), any, "\x1b[27;6;27~"),
            modSeq(mods(false, true, true, false), any, "\x1b[27;7;27~"),
            modSeq(mods(true, true, true, false), any, "\x1b[27;8;27~"),
            modSeq(mods(false, false, false, true), any, "\x1b[27;9;27~"),
            modSeq(mods(true, false, false, true), any, "\x1b[27;10;27~"),
            modSeq(mods(false, true, false, true), any, "\x1b[27;11;27~"),
            modSeq(mods(true, true, false, true), any, "\x1b[27;12;27~"),
            modSeq(mods(false, false, true, true), any, "\x1b[27;13;27~"),
            modSeq(mods(true, false, true, true), any, "\x1b[27;14;27~"),
            modSeq(mods(false, true, true, true), any, "\x1b[27;15;27~"),
            modSeq(mods(true, true, true, true), any, "\x1b[27;16;27~"),
            seq("\x1b"),
        });

        return result;
    }();
    return table;
}

} /* namespace function_keys */
} /* namespace input */
} /* namespace wisp */

#endif /* WISP_INPUT_FUNCTION_KEYS_HPP */
