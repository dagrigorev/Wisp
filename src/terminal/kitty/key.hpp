/* Transliterated from Ghostty src/terminal/kitty/key.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Kitty keyboard protocol support.
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 */

#pragma once
#ifndef WISP_TERMINAL_KITTY_KEY_HPP
#define WISP_TERMINAL_KITTY_KEY_HPP

#include <stddef.h>
#include <stdint.h>

namespace wisp {
namespace terminal {
namespace kitty {

/* The possible flags for the Kitty keyboard protocol.
 * Wisp: packed struct(u5) — field i is bit i. */
struct KeyFlags {
    bool disambiguate;
    bool report_events;
    bool report_alternates;
    bool report_all;
    bool report_associated;

    KeyFlags()
        : disambiguate(false), report_events(false), report_alternates(false),
          report_all(false), report_associated(false) {}

    /* Kitty keyboard protocol disabled (all flags off). */
    static KeyFlags disabled() { return KeyFlags(); }

    /* Sets all modes on. Wisp: @"true" */
    static KeyFlags true_() { return fromInt(0x1F); }

    uint8_t int_() const {
        return (uint8_t)((disambiguate ? 1 : 0) | (report_events ? 2 : 0) |
                         (report_alternates ? 4 : 0) | (report_all ? 8 : 0) |
                         (report_associated ? 16 : 0));
    }

    /* Wisp: @bitCast from u5. */
    static KeyFlags fromInt(uint8_t v) {
        KeyFlags f;
        f.disambiguate = (v & 1) != 0;
        f.report_events = (v & 2) != 0;
        f.report_alternates = (v & 4) != 0;
        f.report_all = (v & 8) != 0;
        f.report_associated = (v & 16) != 0;
        return f;
    }

    bool eql(const KeyFlags &o) const { return int_() == o.int_(); }
};

/* The possible modes for setting the key flags. */
enum class KeySetMode : uint8_t { set, or_, not_ };

/* Stack for the key flags. This implements the push/pop behavior
 * of the CSI > u and CSI < u sequences. We implement the stack as
 * fixed size to avoid heap allocation. */
struct KeyFlagStack {
    static const size_t len = 8;

    KeyFlags flags[len]; /* = @splat(.disabled) */
    uint8_t idx;         /* u3 = 0 */

    KeyFlagStack() : idx(0) {}

    /* Return the current stack value */
    KeyFlags current() const { return flags[idx]; }

    /* Perform the "set" operation as described in the spec for
     * the CSI = u sequence. */
    void set(KeySetMode mode, KeyFlags v) {
        switch (mode) {
            case KeySetMode::set: flags[idx] = v; break;
            case KeySetMode::or_:
                flags[idx] = KeyFlags::fromInt((uint8_t)(flags[idx].int_() | v.int_()));
                break;
            case KeySetMode::not_:
                flags[idx] = KeyFlags::fromInt((uint8_t)(flags[idx].int_() & ~v.int_() & 0x1F));
                break;
        }
    }

    /* Push a new set of flags onto the stack. If the stack is full
     * then the oldest entry is evicted. */
    void push(KeyFlags f) {
        /* Overflow and wrap around if we're full, which evicts
         * the oldest entry. */
        idx = (uint8_t)((idx + 1) & 7);
        flags[idx] = f;
    }

    /* Pop `n` entries from the stack. This will just wrap around
     * if `n` is greater than the amount in the stack. */
    void pop(size_t n) {
        /* If n is more than our length then we just reset the stack.
         * This also avoids a DoS vector where a malicious client
         * could send a huge number of pop commands to waste cpu. */
        if (n >= len) {
            idx = 0;
            for (size_t i = 0; i < len; i++) flags[i] = KeyFlags::disabled();
            return;
        }

        for (size_t i = 0; i < n; i++) {
            flags[idx] = KeyFlags::disabled();
            idx = (uint8_t)((idx - 1) & 7);
        }
    }
};

} /* namespace kitty */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_KITTY_KEY_HPP */
