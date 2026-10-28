/* Transliterated from Ghostty src/input/config.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 */
#pragma once
#ifndef WISP_INPUT_CONFIG_HPP
#define WISP_INPUT_CONFIG_HPP

namespace wisp {
namespace input {

/* Determines the macOS option key behavior. See the config
 * `macos-option-as-alt` for a lot more details.
 *
 * Wisp: `false`/`true` are keywords, so those tags are `false_`/`true_`. */
enum class OptionAsAlt : int {
    false_,
    true_,
    left,
    right,
};

} /* namespace input */
} /* namespace wisp */

#endif /* WISP_INPUT_CONFIG_HPP */
