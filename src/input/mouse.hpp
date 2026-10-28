/* Transliterated from Ghostty src/input/mouse.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp: the input/ mouse types live in namespace `input::mouse`, since
 * `Action` here differs from input::Action (key.hpp). ScrollMods is a plain
 * struct with `int_()`/`fromInt()` giving upstream's packed u8 layout.
 */
#pragma once
#ifndef WISP_INPUT_MOUSE_HPP
#define WISP_INPUT_MOUSE_HPP

#include <stdint.h>

namespace wisp {
namespace input {
namespace mouse {

/* The type of action associated with a mouse event. This is different
 * from ButtonState because button state is simply the current state
 * of a mouse button but an action is something that triggers via
 * an GUI event and supports more. */
enum class Action : int { press, release, motion };

/* The state of a mouse button.
 *
 * This is backed by a c_int so we can use this as-is for our embedding API.
 *
 * IMPORTANT: Any changes here update include/ghostty.h */
enum class ButtonState : int {
    release,
    press,
};

/* Possible mouse buttons. We only track up to 11 because that's the maximum
 * button input that terminal mouse tracking handles without becoming
 * ambiguous.
 *
 * Its a bit silly to name numbers like this but given its a restricted
 * set, it feels better than passing around raw numeric literals.
 *
 * This is backed by a c_int so we can use this as-is for our embedding API.
 *
 * IMPORTANT: Any changes here update include/ghostty.h */
enum class Button : int {
    unknown = 0,
    left = 1,
    right = 2,
    middle = 3,
    four = 4,
    five = 5,
    six = 6,
    seven = 7,
    eight = 8,
    nine = 9,
    ten = 10,
    eleven = 11,
};

/* The maximum value in this enum. This can be used to create a densely
 * packed array, for example.
 *
 * Wisp: upstream computes Button.max at comptime from the fields. */
static const int button_max = 11;

/* The "momentum" of a mouse scroll event. This matches the macOS events
 * because it is the only reliable source right now of momentum events.
 * This is used to handle "inertial scrolling" (i.e. flicking).
 *
 * https://developer.apple.com/documentation/appkit/nseventphase */
enum class Momentum : uint8_t {
    none = 0,
    began = 1,
    stationary = 2,
    changed = 3,
    ended = 4,
    cancelled = 5,
    may_begin = 6,
};

/* The pressure stage of a pressure-sensitive input device.
 *
 * This currently only supports the stages that macOS supports. */
enum class PressureStage : uint8_t {
    /* The input device is unpressed. */
    none = 0,

    /* The input device is pressed a normal amount. On macOS trackpads,
     * this is after a "click". */
    normal = 1,

    /* The input device is pressed a deep amount. On macOS trackpads,
     * this is after a "force click". */
    deep = 2,
};

/* The bitmask for mods for scroll events. */
struct ScrollMods {
    /* True if this is a high-precision scroll event. For example, Apple
     * devices such as Magic Mouse, trackpads, etc. are high-precision
     * and send very detailed scroll events. */
    bool precision = false;

    /* The momentum phase (if available, supported) of the scroll event.
     * This is used to handle "inertial scrolling" (i.e. flicking). */
    Momentum momentum = Momentum::none;

    uint8_t padding_ = 0; /* u4 */

    uint8_t int_() const {
        return (uint8_t)((precision ? 1 : 0) | (((uint8_t)momentum & 0x7) << 1) | ((padding_ & 0xF) << 4));
    }

    static ScrollMods fromInt(uint8_t v) {
        ScrollMods m;
        m.precision = (v & 1) != 0;
        m.momentum = (Momentum)((v >> 1) & 0x7);
        m.padding_ = (uint8_t)(v >> 4);
        return m;
    }
};

} /* namespace mouse */
} /* namespace input */
} /* namespace wisp */

#endif /* WISP_INPUT_MOUSE_HPP */
