/* Transliterated from Ghostty src/terminal/focus.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 * Wisp: *std.Io.Writer is a std::string appended to, which cannot fail.
 */

#pragma once
#ifndef WISP_TERMINAL_FOCUS_HPP
#define WISP_TERMINAL_FOCUS_HPP

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace wisp {
namespace terminal {
namespace focus {

/* Maximum number of bytes that `encode` will write. Any users of this
 * should be resilient to this changing, so this is always a specific
 * value (e.g. we don't add unnecessary padding). */
static const size_t max_encode_size = 3;

/* A focus event that can be reported to the application running in the
 * terminal when focus reporting mode (mode 1004) is enabled. */
enum class Event : uint8_t {
    gained,
    lost,
};

/* Encode a focus in/out report (CSI I / CSI O). */
inline void encode(std::string *writer, Event event) {
    writer->append(event == Event::gained ? "\x1B[I" : "\x1B[O");
}

}  // namespace focus
}  // namespace terminal
}  // namespace wisp

#endif  // WISP_TERMINAL_FOCUS_HPP
