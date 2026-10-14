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
 *   - decode_png is declared and left null, which is upstream's own
 *     configuration for its `.lib` artifact (`build_options.artifact ==
 *     .lib`). `decodePngWuffs` is not ported — there is no Wuffs here —
 *     so PNG transmission yields error.UnsupportedFormat, exactly as
 *     upstream's null hook does. `DecodeError!Image` is a DecodeError
 *     return with the Image through an out parameter, and `[]const u8` /
 *     `[]u8` are a pointer plus a length.
 *   - There is no std.Io, so randomSecure takes only the buffer and the
 *     default source is the platform CSPRNG rather than an Io.
 */

#pragma once
#ifndef WISP_TERMINAL_SYS_HPP
#define WISP_TERMINAL_SYS_HPP

#include <stddef.h>
#include <stdint.h>

#include "../zigstd/allocator.hpp"

namespace wisp {
namespace terminal {
namespace sys {

/* Wisp: `Allocator.Error || error{InvalidData}`. */
enum class DecodeError : uint8_t {
    none,
    OutOfMemory,
    InvalidData,
};

/* The result of decoding an image. The caller owns the returned data
 * and must free it with the same allocator that was passed to the
 * decode function. */
struct Image {
    uint32_t width;
    uint32_t height;
    uint8_t *data;
    size_t data_len;

    Image() : width(0), height(0), data(nullptr), data_len(0) {}
};

typedef DecodeError (*DecodePngFn)(::wisp::zigstd::Allocator alloc, const uint8_t *data,
                                   size_t data_len, Image *out);

/* Decode PNG data into RGBA pixels. If null, PNG decoding is unsupported
 * and the exact semantics are up to callers. For example, the Kitty Graphics
 * Protocol will work but cannot accept PNG images.
 *
 * Wisp: a `pub var` is a function returning a reference to the storage,
 * as random_secure below is. It is null here; see the header note. */
inline DecodePngFn &decode_png() {
    static DecodePngFn f = nullptr;
    return f;
}

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
