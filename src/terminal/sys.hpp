/* Transliterated from Ghostty src/terminal/sys.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * System interface for the terminal package.
 *
 * This provides runtime-swappable function pointers for operations that
 * depend on external implementations (e.g. image decoding). Each function
 * pointer is initialized with a default implementation if available.
 *
 * This exists so that the terminal package doesn't have hard dependencies
 * on specific libraries and enables embedders of the terminal package to
 * swap out implementations as needed at startup to provide their own
 * implementations.
 *
 * Wisp, differences in shape rather than behavior:
 *   - decode_png and the Kitty graphics image types are not ported; this
 *     build sets kitty_graphics = false.
 *   - There is no std.Io, so randomSecure takes only the buffer and the
 *     default source is the platform CSPRNG rather than an Io.
 */

#pragma once
#ifndef WISP_TERMINAL_SYS_HPP
#define WISP_TERMINAL_SYS_HPP

#include <stddef.h>
#include <stdint.h>

namespace wisp {
namespace terminal {
namespace sys {

enum class RandomSecureError : uint8_t {
    none,
    EntropyUnavailable,
};

typedef RandomSecureError (*RandomSecureFn)(uint8_t *buffer, size_t len);

/* Fill a buffer with cryptographically secure random bytes. If null,
 * the platform CSPRNG is used. This is an override for embedders whose
 * platform has no entropy source or that want to control the source;
 * when set it is used on every target.
 *
 * This is used for secrets, so it must be a real CSPRNG. An error
 * makes the operation that needed the entropy fail; nothing falls back
 * to weaker randomness. */
inline RandomSecureFn &random_secure() {
    static RandomSecureFn f = nullptr;
    return f;
}

/* Wisp: the platform CSPRNG standing in for `io.randomSecure`. Defined
 * out of line in sys.cpp so this header pulls in no platform headers. */
RandomSecureError platformRandomSecure(uint8_t *buffer, size_t len);

/* Fill `buffer` with secure random bytes from `random_secure` if set,
 * otherwise from the platform CSPRNG. Every use of secure entropy in the
 * terminal package goes through this so the override applies uniformly. */
inline RandomSecureError randomSecure(uint8_t *buffer, size_t len) {
    if (random_secure() != nullptr) return random_secure()(buffer, len);
    return platformRandomSecure(buffer, len);
}

} /* namespace sys */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_SYS_HPP */
