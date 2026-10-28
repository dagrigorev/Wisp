/* Transliterated from Ghostty src/input/keyboard.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 */
#pragma once
#ifndef WISP_INPUT_KEYBOARD_HPP
#define WISP_INPUT_KEYBOARD_HPP

#include <string.h>

#include "config.hpp"

namespace wisp {
namespace input {
namespace keyboard {

/* Keyboard layouts.
 *
 * These aren't heavily used in Ghostty and having a fully comprehensive
 * list is not important. We only need to distinguish between a few
 * different layouts for some nice-to-have features, such as setting a default
 * value for "macos-option-as-alt". */
enum class Layout {
    /* Unknown, unmapped layout. Ghostty should not make any assumptions
     * about the layout of the keyboard. */
    unknown,

    /* The remaining should be fairly self-explanatory: */
    us_standard,
    us_international,
};

/* Map an Apple keyboard layout ID to a value in this enum. The layout
 * ID can be retrieved using Carbon's TIKeyboardLayoutGetInputSourceProperty
 * function.
 *
 * Even though our layout supports "unknown", we return null if we don't
 * recognize the layout ID so callers can detect this scenario.
 *
 * Wisp: `?Layout` is the bool return plus `*out`. */
inline bool mapAppleId(const char *id, Layout *out) {
    if (strcmp(id, "com.apple.keylayout.US") == 0) {
        *out = Layout::us_standard;
        return true;
    } else if (strcmp(id, "com.apple.keylayout.USInternational") == 0) {
        *out = Layout::us_international;
        return true;
    }

    return false;
}

/* Returns the default macos-option-as-alt value for this layout.
 *
 * We apply some heuristics to change the default based on the keyboard
 * layout if "macos-option-as-alt" is unset. We do this because on some
 * keyboard layouts such as US standard layouts, users generally expect
 * an input such as option-b to map to alt-b but macOS by default will
 * convert it to the codepoint "∫".
 *
 * This behavior however is desired on international layout where the
 * option key is used for important, regularly used inputs. */
inline OptionAsAlt detectOptionAsAlt(Layout self) {
    switch (self) {
        /* On US standard, the option key is typically used as alt
         * and not as a modifier for other codepoints. For example,
         * option-B = ∫ but usually the user wants alt-B. */
        case Layout::us_standard:
        case Layout::us_international: return OptionAsAlt::true_;

        case Layout::unknown: return OptionAsAlt::false_;
    }
    return OptionAsAlt::false_;
}

} /* namespace keyboard */
} /* namespace input */
} /* namespace wisp */

#endif /* WISP_INPUT_KEYBOARD_HPP */
