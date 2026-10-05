/* Transliterated from Ghostty src/terminal/ansi.zig and src/terminal/csi.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 * Non-exhaustive Zig enums (with `_`) are enum classes over the same
 * backing integer, so any value converts without failing, as upstream.
 */

#pragma once
#ifndef WISP_TERMINAL_ANSI_HPP
#define WISP_TERMINAL_ANSI_HPP

#include <stdint.h>

namespace wisp {
namespace terminal {
namespace ansi {

/* C0 (7-bit) control characters from ANSI.
 *
 * This is not complete, control characters are only added to this
 * as the terminal emulator handles them. */
enum class C0 : uint8_t {
    /* Null */
    NUL = 0x00,
    /* Start of heading */
    SOH = 0x01,
    /* Start of text */
    STX = 0x02,
    /* Enquiry */
    ENQ = 0x05,
    /* Bell */
    BEL = 0x07,
    /* Backspace */
    BS = 0x08,
    /* Horizontal tab */
    HT = 0x09,
    /* Line feed */
    LF = 0x0A,
    /* Vertical Tab */
    VT = 0x0B,
    /* Form feed */
    FF = 0x0C,
    /* Carriage return */
    CR = 0x0D,
    /* Shift out */
    SO = 0x0E,
    /* Shift in */
    SI = 0x0F,

    /* Non-exhaustive so that @intToEnum never fails since the inputs are
     * user-generated. */
};

/* The SGR rendition aspects that can be set, sometimes known as attributes.
 * The value corresponds to the parameter value for the SGR command (ESC [ m). */
enum class RenditionAspect : uint16_t {
    default_ = 0,
    bold = 1,
    default_fg = 39,
    default_bg = 49,

    /* Non-exhaustive so that @intToEnum never fails since the inputs are
     * user-generated. */
};

/* Possible cursor styles (ESC [ q) */
enum class CursorStyle : uint8_t {
    default_,
    blinking_block,
    steady_block,
    blinking_underline,
    steady_underline,
    blinking_bar,
    steady_bar,
};

/* The status line type for DECSSDT. */
enum class StatusLineType : uint16_t {
    none = 0,
    indicator = 1,
    host_writable = 2,

    /* Non-exhaustive so that @intToEnum never fails for unsupported values. */
};

/* The display to target for status updates (DECSASD). */
enum class StatusDisplay : uint8_t {
    main,
    status_line,
};

/* The possible modify key formats to ESC[>{a};{b}m
 * Note: this is not complete, we should add more as we support more */
enum class ModifyKeyFormat : uint8_t {
    legacy,
    cursor_keys,
    function_keys,
    other_keys_none,
    other_keys_numeric_except,
    other_keys_numeric,
};

/* The protection modes that can be set for the terminal. See DECSCA and
 * ESC V, W. */
enum class ProtectedMode : uint8_t {
    off,
    iso, /* ESC V, W */
    dec, /* CSI Ps " q */
};

} /* namespace ansi */

namespace csi {

/* Modes for the ED CSI command.
 * Wisp: exhaustive upstream; callers check the value before converting. */
enum class EraseDisplay : uint8_t {
    below = 0,
    above = 1,
    complete = 2,
    scrollback = 3,

    /* This is an extension added by Kitty to move the viewport into the
     * scrollback and then erase the display. */
    scroll_complete = 22,
};

/* Modes for the EL CSI command. */
enum class EraseLine : uint8_t {
    right = 0,
    left = 1,
    complete = 2,
    right_unless_pending_wrap = 4,

    /* Non-exhaustive so that @intToEnum never fails since the inputs are
     * user-generated. */
};

/* Modes for the TBC (tab clear) command. */
enum class TabClear : uint8_t {
    current = 0,
    all = 3,

    /* Non-exhaustive so that @intToEnum never fails since the inputs are
     * user-generated. */
};

/* Style formats for terminal size reports. */
enum class SizeReportStyle : uint8_t {
    /* XTWINOPS */
    csi_14_t,
    csi_16_t,
    csi_18_t,
    csi_21_t,
};

/* XTWINOPS CSI 22/23 */
struct TitlePushPop {
    enum class Op : uint8_t { push, pop };

    Op op;
    uint16_t index;
};

} /* namespace csi */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_ANSI_HPP */
