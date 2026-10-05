/* Transliterated from Ghostty src/input/paste.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see parser.hpp and osc.hpp for the Zig-to-C++ mapping.
 * Comments are upstream's unless marked "Wisp:".
 *
 * Wisp, differences in shape rather than behavior:
 *   - `encode(data: anytype)` dispatches at comptime on whether the data is
 *     mutable. That is two functions here: `encode` takes mutable data and
 *     cannot fail, `encodeConst` takes const data and returns
 *     Error::MutableRequired where upstream's const instantiation does.
 *   - The `[3][]const u8` result is an Encoded struct of three ZStrs.
 *   - `*std.Io.Writer` is `std::string *`, so `encodeWriter` cannot fail
 *     and needs no chunking; it appends the encoded data directly.
 */

#pragma once
#ifndef WISP_INPUT_PASTE_HPP
#define WISP_INPUT_PASTE_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <string>

#include "../terminal/osc.hpp"
#include "../vt/terminal.hpp"

namespace wisp {
namespace input {
namespace paste {

typedef ::wisp::terminal::osc::ZStr ZStr;

/* The bracketed paste (mode 2004) frame written around the data. */
static const char bracketed_prefix[] = "\x1b[200~";
static const char bracketed_suffix[] = "\x1b[201~";
static const size_t bracketed_prefix_len = sizeof(bracketed_prefix) - 1;
static const size_t bracketed_suffix_len = sizeof(bracketed_suffix) - 1;

/* The maximum number of bytes `encode` adds around the data, so callers
 * can size a buffer for the full encoded result. */
static const size_t max_frame_size = bracketed_prefix_len + bracketed_suffix_len;

struct Options {
    /* True if bracketed paste mode is on. */
    bool bracketed;

    Options() : bracketed(false) {}
    explicit Options(bool b) : bracketed(b) {}

    /* Return the encoding options based on the current terminal state. */
    static Options fromTerminal(const vt::Terminal *t) {
        Options o;
        o.bracketed = t->modes.get(terminal::modes::Mode::bracketed_paste);
        return o;
    }
};

/* Wisp: the `[3][]const u8` result of encode. */
struct Encoded {
    ZStr parts[3];
};

enum class Error : uint8_t {
    none,

    /* Returned if encoding requires a mutable copy of the data. This
     * can only be returned if the input data type is const. */
    MutableRequired,
};

/* These are the set of byte values that are always replaced by
 * a space (per xterm's behavior) for any text insertion method e.g.
 * a paste, drag and drop, etc. These are copied directly from xterm's
 * source. */
inline bool isStripByte(uint8_t c) {
    switch (c) {
    case 0x00: /* NUL */
    case 0x08: /* BS */
    case 0x05: /* ENQ */
    case 0x04: /* EOT */
    case 0x1B: /* ESC */
    case 0x7F: /* DEL */

    /* These can be overridden by the running terminal program
     * via tcsetattr, so they aren't totally safe to hardcode like
     * this. In practice, I haven't seen modern programs change these
     * and its a much bigger architectural change to pass these through
     * so for now they're hardcoded. */
    case 0x03: /* VINTR (Ctrl+C) */
    case 0x1C: /* VQUIT (Ctrl+\) */
    case 0x15: /* VKILL (Ctrl+U) */
    case 0x1A: /* VSUSP (Ctrl+Z) */
    case 0x11: /* VSTART (Ctrl+Q) */
    case 0x13: /* VSTOP (Ctrl+S) */
    case 0x17: /* VWERASE (Ctrl+W) */
    case 0x16: /* VLNEXT (Ctrl+V) */
    case 0x12: /* VREPRINT (Ctrl+R) */
    case 0x0F: /* VDISCARD (Ctrl+O) */
        return true;
    default: return false;
    }
}

/* Encode the given data for pasting. The resulting value can be written
 * to the pty to perform a paste of the input data.
 *
 * The data is returned as a set of slices to limit allocations. The caller
 * can combine the slices into a single buffer if desired.
 *
 * WARNING: The input data is not checked for safety. See the `isSafe`
 * function to check if the data is safe to paste.
 *
 * Wisp: the mutable instantiation, which cannot fail. */
inline Encoded encode(uint8_t *data, size_t data_len, Options opts) {
    Encoded result;
    result.parts[0] = ZStr("", 0);
    result.parts[1] = ZStr((const char *)data, data_len);
    result.parts[2] = ZStr("", 0);

    /* If we have any of the strip values, then we need to replace them
     * with spaces. This is what xterm does and it does it regardless
     * of bracketed paste mode. This is a security measure to prevent pastes
     * from containing bytes that could be used to inject commands. */
    for (size_t i = 0; i < data_len; i++) {
        if (isStripByte(data[i])) data[i] = ' ';
    }

    /* Bracketed paste mode (mode 2004) wraps pasted data in
     * fenceposts so that the terminal can ignore things like newlines. */
    if (opts.bracketed) {
        result.parts[0] = ZStr(bracketed_prefix, bracketed_prefix_len);
        result.parts[2] = ZStr(bracketed_suffix, bracketed_suffix_len);
        return result;
    }

    /* Non-bracketed. We have to replace newline with `\r`. This matches
     * the behavior of xterm and other terminals. For `\r\n` this will
     * result in `\r\r` which does match xterm. */
    for (size_t i = 0; i < data_len; i++) {
        if (data[i] == '\n') data[i] = '\r';
    }

    return result;
}

/* Wisp: the const instantiation, which reports when a mutable copy is
 * required rather than modifying the caller's data. */
inline Error encodeConst(const uint8_t *data, size_t data_len, Options opts, Encoded *out) {
    Encoded result;
    result.parts[0] = ZStr("", 0);
    result.parts[1] = ZStr((const char *)data, data_len);
    result.parts[2] = ZStr("", 0);

    for (size_t i = 0; i < data_len; i++) {
        if (isStripByte(data[i])) return Error::MutableRequired;
    }

    if (opts.bracketed) {
        result.parts[0] = ZStr(bracketed_prefix, bracketed_prefix_len);
        result.parts[2] = ZStr(bracketed_suffix, bracketed_suffix_len);
        *out = result;
        return Error::none;
    }

    for (size_t i = 0; i < data_len; i++) {
        if (data[i] == '\n') return Error::MutableRequired;
    }

    *out = result;
    return Error::none;
}

/* Encode the given data for pasting directly into a writer. This is
 * the same transformation as `encode` (unsafe bytes replaced, bracketed
 * frame or newline conversion per `opts`) but the data is copied
 * exactly once: into the writer's buffer, where it is modified in place.
 * This is the form to use when the data is const and the result is
 * being assembled into a single buffer anyway.
 *
 * WARNING: The input data is not checked for safety. See `isSafe`
 * and `isSafeWith` to check if the data is safe to paste. */
inline void encodeWriter(std::string *writer, const uint8_t *data, size_t data_len, Options opts) {
    if (opts.bracketed) writer->append(bracketed_prefix, bracketed_prefix_len);

    /* The byte transformations are position-independent, so the data
     * can be copied and encoded chunk by chunk. The frame returned by
     * encode is ignored since it's written around the whole data here. */
    const size_t start = writer->size();
    writer->append((const char *)data, data_len);
    (void)encode((uint8_t *)writer->data() + start, data_len, opts);

    if (opts.bracketed) writer->append(bracketed_suffix, bracketed_suffix_len);
}

/* Returns true if the data looks safe to paste. Data is considered
 * unsafe if it contains any of the following:
 *
 * - `\n`: Newlines can be used to inject commands.
 * - `\x1b[201~`: This is the end of a bracketed paste. This cane be used
 *   to exit a bracketed paste and inject commands.
 *
 * We consider any scenario unsafe regardless of current terminal state.
 * For example, even if bracketed paste mode is not active, we still
 * consider `\x1b[201~` unsafe. The existence of these types of bytes
 * should raise suspicion that the producer of the paste data is
 * acting strangely. */
inline bool contains(const uint8_t *data, size_t data_len, const char *needle, size_t needle_len) {
    if (needle_len > data_len) return false;
    for (size_t i = 0; i + needle_len <= data_len; i++) {
        if (memcmp(data + i, needle, needle_len) == 0) return true;
    }
    return false;
}

inline bool isSafe(const uint8_t *data, size_t data_len) {
    return !contains(data, data_len, "\n", 1) &&
           !contains(data, data_len, bracketed_suffix, bracketed_suffix_len);
}

/* Returns true if the data looks safe to paste given how it will be
 * encoded. This is the terminal-state-aware counterpart of `isSafe`:
 *
 * - Bracketed (mode 2004 on): the program receives the data as one
 *   framed unit, so newlines are fine. The data is unsafe only if it
 *   contains the end of the frame (`\x1b[201~`), which would let the
 *   rest of the data escape the frame and inject commands.
 * - Unbracketed: the same rule as `isSafe`.
 *
 * Callers wanting the conservative rule regardless of terminal state
 * should use `isSafe` instead. */
inline bool isSafeWith(const uint8_t *data, size_t data_len, Options opts) {
    if (opts.bracketed) return !contains(data, data_len, bracketed_suffix, bracketed_suffix_len);
    return isSafe(data, data_len);
}

} /* namespace paste */
} /* namespace input */
} /* namespace wisp */

#endif /* WISP_INPUT_PASTE_HPP */
