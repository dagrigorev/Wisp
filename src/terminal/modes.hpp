/* Transliterated from Ghostty src/terminal/modes.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * This file contains all the terminal modes that we support
 * and various support types for them: an enum of supported modes,
 * a packed struct to store mode values, a more generalized state
 * struct to store values plus handle save/restore, and much more.
 *
 * There is pretty heavy comptime usage and type generation here.
 * I don't love to have this sort of complexity but its a good way
 * to ensure all our various types and logic remain in sync.
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: upstream generates Mode, ModePacked and the lookups from `entries`
 * at comptime. Here the Mode enum and the entry table were generated once
 * from upstream's entry list and are checked in; ModePacked is a uint64_t whose bit i is entries[i], which is what a
 * packed struct of bools lays out. Mode names that start with a digit get
 * a leading underscore (132_column is _132_column).
 */

#pragma once
#ifndef WISP_TERMINAL_MODES_HPP
#define WISP_TERMINAL_MODES_HPP

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <string>

namespace wisp {
namespace terminal {
namespace modes {

/* An enum(u16) of the available modes. See entries for available values.
 * Wisp: each value is @bitCast(ModeTag{ .value, .ansi }). */
enum class Mode : uint16_t {
    disable_keyboard = 32770,  /* KAM */
    insert = 32772,
    send_receive_mode = 32780,  /* SRM */
    linefeed = 32788,  /* DEC */
    cursor_keys = 1,  /* DECCKM */
    _132_column = 3,
    slow_scroll = 4,
    reverse_colors = 5,
    origin = 6,
    wraparound = 7,
    autorepeat = 8,
    mouse_event_x10 = 9,
    cursor_blinking = 12,
    cursor_visible = 25,
    enable_mode_3 = 40,
    reverse_wrap = 45,
    alt_screen_legacy = 47,
    keypad_keys = 66,  /* DEC Backarrow Key Mode (DECBKM) */
    backarrow_key_mode = 67,
    enable_left_and_right_margin = 69,
    mouse_event_normal = 1000,
    mouse_event_button = 1002,
    mouse_event_any = 1003,
    focus_event = 1004,
    mouse_format_utf8 = 1005,
    mouse_format_sgr = 1006,
    mouse_alternate_scroll = 1007,
    mouse_format_urxvt = 1015,
    mouse_format_sgr_pixels = 1016,
    ignore_keypad_with_numlock = 1035,
    alt_esc_prefix = 1036,
    alt_sends_escape = 1039,
    reverse_wrap_extended = 1045,
    alt_screen = 1047,
    save_cursor = 1048,
    alt_screen_save_cursor_clear_enter = 1049,
    bracketed_paste = 2004,
    synchronized_output = 2026,
    grapheme_cluster = 2027,
    report_color_scheme = 2031,
    report_visibility = 2033,
    in_band_size_reports = 2048,  /* Kitty clipboard protocol paste events. When set, a user-initiated */
    kitty_paste_events = 5522,
};

/* The tag type for our enum is a u16 but we use a packed struct
 * in order to pack the ansi bit into the tag.
 * Wisp: packed struct(u16) { value: u15, ansi: bool }. */
struct ModeTag {
    typedef uint16_t Backing;
    uint16_t value; /* u15 */
    bool ansi;      /* = false */

    Backing bits() const { return (Backing)((value & 0x7FFF) | (ansi ? 0x8000 : 0)); }

    static ModeTag fromMode(Mode mode) {
        ModeTag t;
        t.value = (uint16_t)((uint16_t)mode & 0x7FFF);
        t.ansi = ((uint16_t)mode & 0x8000) != 0;
        return t;
    }
};

/* A single entry of a possible mode we support. This is used to
 * dynamically define the enum and other tables. */
struct ModeEntry {
    const char *name;
    Mode mode;   /* Wisp: the enum member this entry defines */
    uint16_t value;
    bool default_;

    /* True if this is an ANSI mode, false if its a DEC mode (?-prefixed). */
    bool ansi;

    /* If true, this mode is disabled and Ghostty will not allow it to be
     * set or queried. The mode enum still has it, allowing Ghostty developers
     * to develop a mode without exposing it to real users. */
    bool disabled;

    /* Whether an embedder may safely configure this bit as reset policy.
     * Modes that perform a transition or mirror additional terminal state
     * must use their semantic configuration API instead. */
    bool default_configurable;
};

/* The full list of available entries. For documentation see how
 * they're used within Ghostty or google their values. It is not
 * valuable to redocument them all here.
 *
 * Wisp: kitty_paste_events is disabled upstream when
 * `build_options.artifact != .lib and builtin.os.tag != .macos`. Wisp is an
 * application on Windows, so it is disabled. */
static const size_t entries_len = 43;

inline const ModeEntry *entries() {
    static const ModeEntry tbl[entries_len] = {
        { "disable_keyboard", Mode::disable_keyboard, 2, false, true, false, true },
        { "insert", Mode::insert, 4, false, true, false, true },
        { "send_receive_mode", Mode::send_receive_mode, 12, true, true, false, true },
        { "linefeed", Mode::linefeed, 20, false, true, false, true },
        { "cursor_keys", Mode::cursor_keys, 1, false, false, false, true },
        { "132_column", Mode::_132_column, 3, false, false, false, false },
        { "slow_scroll", Mode::slow_scroll, 4, false, false, false, true },
        { "reverse_colors", Mode::reverse_colors, 5, false, false, false, true },
        { "origin", Mode::origin, 6, false, false, false, false },
        { "wraparound", Mode::wraparound, 7, true, false, false, true },
        { "autorepeat", Mode::autorepeat, 8, false, false, false, true },
        { "mouse_event_x10", Mode::mouse_event_x10, 9, false, false, false, false },
        { "cursor_blinking", Mode::cursor_blinking, 12, false, false, false, false },
        { "cursor_visible", Mode::cursor_visible, 25, true, false, false, true },
        { "enable_mode_3", Mode::enable_mode_3, 40, false, false, false, true },
        { "reverse_wrap", Mode::reverse_wrap, 45, false, false, false, true },
        { "alt_screen_legacy", Mode::alt_screen_legacy, 47, false, false, false, false },
        { "keypad_keys", Mode::keypad_keys, 66, false, false, false, true },
        { "backarrow_key_mode", Mode::backarrow_key_mode, 67, false, false, false, true },
        { "enable_left_and_right_margin", Mode::enable_left_and_right_margin, 69, false, false, false, false },
        { "mouse_event_normal", Mode::mouse_event_normal, 1000, false, false, false, false },
        { "mouse_event_button", Mode::mouse_event_button, 1002, false, false, false, false },
        { "mouse_event_any", Mode::mouse_event_any, 1003, false, false, false, false },
        { "focus_event", Mode::focus_event, 1004, false, false, false, true },
        { "mouse_format_utf8", Mode::mouse_format_utf8, 1005, false, false, false, false },
        { "mouse_format_sgr", Mode::mouse_format_sgr, 1006, false, false, false, false },
        { "mouse_alternate_scroll", Mode::mouse_alternate_scroll, 1007, true, false, false, true },
        { "mouse_format_urxvt", Mode::mouse_format_urxvt, 1015, false, false, false, false },
        { "mouse_format_sgr_pixels", Mode::mouse_format_sgr_pixels, 1016, false, false, false, false },
        { "ignore_keypad_with_numlock", Mode::ignore_keypad_with_numlock, 1035, true, false, false, true },
        { "alt_esc_prefix", Mode::alt_esc_prefix, 1036, true, false, false, true },
        { "alt_sends_escape", Mode::alt_sends_escape, 1039, false, false, false, true },
        { "reverse_wrap_extended", Mode::reverse_wrap_extended, 1045, false, false, false, true },
        { "alt_screen", Mode::alt_screen, 1047, false, false, false, false },
        { "save_cursor", Mode::save_cursor, 1048, false, false, false, false },
        { "alt_screen_save_cursor_clear_enter", Mode::alt_screen_save_cursor_clear_enter, 1049, false, false, false, false },
        { "bracketed_paste", Mode::bracketed_paste, 2004, false, false, false, true },
        { "synchronized_output", Mode::synchronized_output, 2026, false, false, false, false },
        { "grapheme_cluster", Mode::grapheme_cluster, 2027, false, false, false, true },
        { "report_color_scheme", Mode::report_color_scheme, 2031, false, false, false, true },
        { "report_visibility", Mode::report_visibility, 2033, false, false, false, false },
        { "in_band_size_reports", Mode::in_band_size_reports, 2048, false, false, false, true },
        { "kitty_paste_events", Mode::kitty_paste_events, 5522, false, false, true /* Wisp: see below */, true },
    };
    return tbl;
}

/* Wisp: entryForMode, at run time — the index of mode in entries, which is
 * also its bit in ModePacked. */
inline size_t entryIndex(Mode mode) {
    const ModeEntry *e = entries();
    for (size_t i = 0; i < entries_len; i++) {
        if (e[i].mode == mode) return i;
    }
    return 0; /* unreachable */
}

/* A packed struct of all the settable modes. This shouldn't
 * be used directly but rather through the ModeState struct.
 * Wisp: bit i is entries[i]. */
struct ModePacked {
    uint64_t bits;

    /* Wisp: `.{}` — every field at its entry's default. */
    ModePacked() : bits(0) {
        const ModeEntry *e = entries();
        for (size_t i = 0; i < entries_len; i++) {
            if (e[i].default_) bits |= (uint64_t)1 << i;
        }
    }

    /* Wisp: @field(values, entry.name) */
    bool field(Mode mode) const { return (bits >> entryIndex(mode)) & 1; }
    void setField(Mode mode, bool value) {
        const uint64_t bit = (uint64_t)1 << entryIndex(mode);
        if (value) bits |= bit; else bits &= ~bit;
    }
};

inline void setPacked(ModePacked *values, Mode mode, bool value) {
    values->setField(mode, value);
}

inline bool getPacked(const ModePacked *values, Mode mode) {
    return values->field(mode);
}

/* Wisp: ?Mode is the bool return plus *out. */
inline bool modeFromInt(uint16_t v, bool ansi, Mode *out) {
    const ModeEntry *e = entries();
    for (size_t i = 0; i < entries_len; i++) {
        const ModeEntry &entry = e[i];
        if (!entry.disabled) {
            if (entry.value == v && entry.ansi == ansi) {
                *out = entry.mode;
                return true;
            }
        }
    }

    return false;
}

/* A DECRPM mode report response. */
struct Report {
    /* A query identifier can use the full parser parameter range. Keep it
     * separate from ModeTag, which packs supported modes and the ANSI flag
     * into a u16 and is also used by the public C API. */
    struct Tag {
        uint16_t value;
        bool ansi; /* = false */

        static Tag make(uint16_t value, bool ansi = false) {
            Tag t;
            t.value = value;
            t.ansi = ansi;
            return t;
        }

        static Tag fromMode(Mode mode) {
            const ModeTag tag = ModeTag::fromMode(mode);
            return make(tag.value, tag.ansi);
        }

        bool eql(const Tag &o) const { return value == o.value && ansi == o.ansi; }
    };

    /* The state of a mode as reported in a DECRPM response. */
    enum class State : uint8_t {
        not_recognized = 0,
        set = 1,
        reset = 2,
        permanently_set = 3,
        permanently_reset = 4,
    };

    Tag tag;
    State state;

    /* Wisp: upstream measures the largest report (value 65535, DEC,
     * permanently_reset) at comptime: "\x1B[?65535;4$y". */
    static const size_t max_size = 12;

    /* Encode the DECRPM report sequence. */
    void encode(std::string *writer) const {
        char buf[32];
        snprintf(buf, sizeof(buf), "\x1B[%s%u;%u$y",
                 tag.ansi ? "" : "?", (unsigned)tag.value, (unsigned)state);
        writer->append(buf);
    }
};

/* A struct that maintains the state of all the settable modes. */
struct ModeState {
    /* The values of the current modes. */
    ModePacked values;

    /* The saved values. We only allow saving each mode once.
     * This is in line with other terminals that implement XTSAVE
     * and XTRESTORE. We can improve this in the future if it becomes
     * a real-world issue but we need to be aware of a DoS vector. */
    ModePacked saved;

    /* The default values for the modes. This is used to reset
     * the modes to their default values during reset. */
    ModePacked default_;

    /* Reset the modes to their default values. This also clears the
     * saved state. */
    void reset() {
        values = default_;
        saved = ModePacked();
    }

    /* Set a mode to a value. */
    void set(Mode mode, bool value) { setPacked(&values, mode, value); }

    /* Set the reset default and current value for a mode. */
    void setDefault(Mode mode, bool value) {
        setPacked(&values, mode, value);
        setPacked(&default_, mode, value);
    }

    /* Get the value of a mode. */
    bool get(Mode mode) const { return getPacked(&values, mode); }

    /* Save the state of the given mode. This can then be restored
     * with restore. This will only be accurate if the previous
     * mode was saved exactly once and not restored. Otherwise this
     * will just keep restoring the last stored value in memory. */
    void save(Mode mode) { saved.setField(mode, values.field(mode)); }

    /* See save. This will return the restored value. */
    bool restore(Mode mode) {
        values.setField(mode, saved.field(mode));
        return values.field(mode);
    }

    /* Return a DECRPM report for the given mode tag. If the tag does
     * not correspond to a known mode, the report state is .not_recognized. */
    Report getReport(Report::Tag tag) const {
        /* DECECM (Erase Color Mode, DEC private mode 117) controls whether erasing
         * and scrolling use the default background or the active background color.
         * Ghostty's behavior is fixed equivalent to DECECM reset, and DECRQM has a
         * "permanently reset" response for recognized modes that cannot be changed.
         * Report that instead of "not recognized" so applications can query and adapt
         * to Ghostty's erase-color behavior.
         *
         * See VT520/VT525 Programmer Information, "Erase Color" and DECRQM/DECRPM:
         * https://web.mit.edu/dosathena/doc/www/ek-vt520-rm.pdf */

        Report r;
        r.tag = tag;
        if (!tag.ansi && tag.value == 117) {
            r.state = Report::State::permanently_reset;
            return r;
        }
        Mode mode;
        if (!modeFromInt(tag.value, tag.ansi, &mode)) {
            r.state = Report::State::not_recognized;
            return r;
        }
        r.state = get(mode) ? Report::State::set : Report::State::reset;
        return r;
    }
};

/* Return whether a mode can safely be configured as a reset default by an
 * embedder. This excludes modes whose set/reset operations have side effects
 * beyond changing the mode bit. */
inline bool defaultConfigurable(Mode mode) {
    return entries()[entryIndex(mode)].default_configurable;
}

} /* namespace modes */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_MODES_HPP */
