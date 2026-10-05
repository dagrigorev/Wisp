/* Transliterated from Ghostty src/terminal/device_status.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 * Wisp: *std.Io.Writer is a std::string appended to, which cannot fail.
 */

#pragma once
#ifndef WISP_TERMINAL_DEVICE_STATUS_HPP
#define WISP_TERMINAL_DEVICE_STATUS_HPP

#include <stddef.h>
#include <stdint.h>

#include <string>

namespace wisp {
namespace terminal {
namespace device_status {

/* The color scheme reported in response to a CSI ? 996 n query. */
enum class ColorScheme : uint8_t {
    light,
    dark,
};

/* The visibility state reported in response to a CSI ? 998 n query or when
 * DEC mode 2033 is enabled. */
enum class Visibility : uint8_t {
    potentially_visible,
    not_visible,
};

/* Encode a color scheme report response for CSI ? 996 n queries. */
inline void encodeColorSchemeReport(std::string *writer, ColorScheme scheme) {
    writer->append(scheme == ColorScheme::dark ? "\x1B[?997;1n" : "\x1B[?997;2n");
}

/* Maximum number of bytes that `encodeColorSchemeReport` will write.
 * Wisp: upstream computes this at comptime by encoding every scheme into a
 * discarding writer; both reports are 9 bytes. */
static const size_t max_color_scheme_report_encode_size = sizeof("\x1B[?997;1n") - 1;

/* Maximum number of bytes that `encodeVisibilityReport` will write. */
static const size_t max_visibility_report_encode_size = sizeof("\x1B[?999;2n") - 1;

/* Encode a visibility report response for CSI ? 998 n queries and DEC mode
 * 2033 notifications. */
inline void encodeVisibilityReport(std::string *writer, Visibility visibility) {
    writer->append(visibility == Visibility::potentially_visible
                       ? "\x1B[?999;1n"
                       : "\x1B[?999;2n");
}

/* The tag type for our enum is a u16 but we use a packed struct
 * in order to pack the question bit into the tag. The "u16" size is
 * chosen somewhat arbitrarily to match the largest expected size
 * we see as a multiple of 8 bits.
 *
 * Wisp: packed struct(u16) { value: u15, question: bool } — value in the
 * low 15 bits, question in bit 15. */
struct Tag {
    typedef uint16_t Backing;
    static Backing make(uint16_t value, bool question) {
        return (Backing)((value & 0x7FFF) | (question ? 0x8000 : 0));
    }
};

/* An enum(u16) of the available device status requests.
 * Wisp: upstream builds this from `entries` at comptime. */
enum class Request : uint16_t {
    operating_status = 5,
    cursor_position = 6,
    color_scheme = 996 | 0x8000,
    visibility = 998 | 0x8000,
};

/* A single entry of a possible device status request we support. The
 * "question" field determines if it is valid with or without the "?"
 * prefix. */
struct Entry {
    const char *name;
    uint16_t value;
    bool question; /* "?" request */
};

/* The full list of device status request entries. */
static const Entry entries[] = {
    { "operating_status", 5, false },
    { "cursor_position", 6, false },
    { "color_scheme", 996, true },
    { "visibility", 998, true },
};

/* Wisp: ?Request is the bool return plus *out. */
inline bool reqFromInt(uint16_t v, bool question, Request *out) {
    for (size_t i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        const Entry &entry = entries[i];
        if (entry.value == v && entry.question == question) {
            *out = (Request)Tag::make(entry.value, question);
            return true;
        }
    }

    return false;
}

} /* namespace device_status */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_DEVICE_STATUS_HPP */
