/* Transliterated from Ghostty src/input/mouse_encode.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp, differences in shape rather than behavior:
 *   - `*std.Io.Writer` is `std::string *`, so encoding cannot fail.
 *   - `?mouse.Button` is `has_button` plus `button`; `?*?point.Coordinate`
 *     last_cell is a pointer to a `LastCell` (presence flag plus value).
 *   - The X10 coordinate-limit log line is omitted (no logging in the port).
 */
#pragma once
#ifndef WISP_INPUT_MOUSE_ENCODE_HPP
#define WISP_INPUT_MOUSE_ENCODE_HPP

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include <string>

#include "../renderer/size.hpp"
#include "../terminal/mouse.hpp"
#include "../vt/point.hpp"
#include "../vt/terminal.hpp"
#include "../zigstd/unicode.hpp"
#include "key.hpp"
#include "mouse.hpp"

namespace wisp {
namespace input {
namespace mouse_encode {

typedef ::wisp::vt::Terminal Terminal;
typedef ::wisp::vt::point::Coordinate Coordinate;
typedef ::wisp::renderer::size::Size Size;
typedef ::wisp::terminal::mouse::Event MouseEvent;
typedef ::wisp::terminal::mouse::Format MouseFormat;

/* Wisp: `?point.Coordinate`. */
struct LastCell {
    bool has = false;
    Coordinate value;
};

/* Options that affect mouse encoding behavior and provide runtime context. */
struct Options {
    /* Terminal mouse reporting mode (X10, normal, button, any). */
    MouseEvent event = MouseEvent::none;

    /* Terminal mouse reporting format. */
    MouseFormat format = MouseFormat::x10;

    /* Full renderer size used to convert surface-space pixel positions
     * into grid cell coordinates (for most formats) and terminal-space
     * pixel coordinates (for SGR-Pixels), as well as to determine
     * whether a position falls outside the visible viewport. */
    Size size;

    /* Whether any mouse button is currently pressed. When a motion
     * event occurs outside the viewport, it is only reported if a
     * button is held down and the event mode supports motion tracking.
     * Without this, out-of-viewport motions are silently dropped.
     *
     * This should reflect the state of the current event as well, so
     * if the encoded event is a button press, this should be true. */
    bool any_button_pressed = false;

    /* Last reported viewport cell for motion deduplication.
     * If null, motion deduplication state is not tracked. */
    LastCell *last_cell = nullptr;

    /* Initialize from terminal and renderer state. The caller may still
     * set any_button_pressed and last_cell on the returned value. */
    static Options fromTerminal(const Terminal *t, const Size &size) {
        Options o;
        o.event = t->flags.mouse_event;
        o.format = t->flags.mouse_format;
        o.size = size;
        return o;
    }
};

/* A normalized mouse event for protocol encoding. */
struct Event {
    /* Mouse position in surface-space pixels. */
    struct Pos {
        float x = 0;
        float y = 0;
    };

    /* The action of this mouse event. */
    mouse::Action action = mouse::Action::press;

    /* The button involved in this event. This can be null in the
     * case of a motion action with no pressed buttons. */
    bool has_button = false;
    mouse::Button button = mouse::Button::unknown;

    /* Keyboard modifiers held during this event. */
    Mods mods;

    /* Mouse position in terminal-space pixels, with (0, 0) at the top-left
     * of the terminal. Negative values are allowed and indicate positions
     * above or to the left of the terminal. Values larger than the terminal
     * size are also allowed and indicate right or below the terminal. */
    Pos pos;
};

/* Returns true if this event should be reported for the given mouse
 * event mode. */
inline bool shouldReport(const Event &event, const Options &opts) {
    switch (opts.event) {
        case MouseEvent::none: return false;

        /* X10 only reports button presses of left, middle, and right. */
        case MouseEvent::x10:
            return event.action == mouse::Action::press && event.has_button &&
                   (event.button == mouse::Button::left || event.button == mouse::Button::middle ||
                    event.button == mouse::Button::right);

        /* Normal mode does not report motion. */
        case MouseEvent::normal: return event.action != mouse::Action::motion;

        /* Button mode requires an active button for motion events. */
        case MouseEvent::button: return event.has_button;

        /* Any mode reports everything. */
        case MouseEvent::any: return true;
    }
    return false;
}

/* Wisp: `?u8` is the bool return plus `*out`. */
inline bool buttonCode(const Event &event, const Options &opts, uint8_t *out) {
    uint8_t acc;
    if (!event.has_button) {
        /* Null button means motion with no pressed button. */
        acc = 3;
    } else if (event.action == mouse::Action::release && opts.format != MouseFormat::sgr &&
               opts.format != MouseFormat::sgr_pixels) {
        /* Legacy releases are always encoded as button 3. */
        acc = 3;
    } else {
        switch (event.button) {
            case mouse::Button::left: acc = 0; break;
            case mouse::Button::middle: acc = 1; break;
            case mouse::Button::right: acc = 2; break;
            case mouse::Button::four: acc = 64; break;
            case mouse::Button::five: acc = 65; break;
            case mouse::Button::six: acc = 66; break;
            case mouse::Button::seven: acc = 67; break;
            case mouse::Button::eight: acc = 128; break;
            case mouse::Button::nine: acc = 129; break;
            default: return false;
        }
    }

    /* X10 does not include modifiers. */
    if (opts.event != MouseEvent::x10) {
        if (event.mods.shift) acc += 4;
        if (event.mods.alt) acc += 8;
        if (event.mods.ctrl) acc += 16;
    }

    /* Motion adds another bit. */
    if (event.action == mouse::Action::motion) acc += 32;

    *out = acc;
    return true;
}

/* Terminal-space pixel position for SGR pixel reporting. */
struct PixelPoint {
    int32_t x;
    int32_t y;
};

/* Returns true if the surface-space pixel position is outside the
 * visible viewport bounds (negative or beyond screen dimensions). */
inline bool posOutOfViewport(const Event::Pos &pos, const Size &size) {
    const float max_x = (float)size.screen.width;
    const float max_y = (float)size.screen.height;
    return pos.x < 0 || pos.y < 0 || pos.x > max_x || pos.y > max_y;
}

/* Converts a surface-space pixel position to a zero-based grid cell
 * coordinate (column, row) within the terminal viewport. Out-of-bounds
 * values are clamped to the valid grid range (0 to columns/rows - 1). */
inline Coordinate posToCell(const Event::Pos &pos, const Size &size) {
    typedef ::wisp::renderer::size::Coordinate RCoord;
    const RCoord coord = RCoord::makeSurface((double)pos.x, (double)pos.y);
    const RCoord::Grid grid = coord.convert(RCoord::Tag::grid, size).grid;
    return Coordinate(grid.x, grid.y);
}

/* Converts a surface-space pixel position to terminal-space pixel
 * coordinates (accounting for padding/scaling) used by SGR-Pixels mode.
 * Unlike grid conversion, terminal-space coordinates are not clamped
 * and may be negative or exceed the terminal dimensions. */
inline PixelPoint posToPixels(const Event::Pos &pos, const Size &size) {
    typedef ::wisp::renderer::size::Coordinate RCoord;
    const RCoord::Terminal coord =
        RCoord::makeSurface((double)pos.x, (double)pos.y).convert(RCoord::Tag::terminal, size).terminal;

    PixelPoint p;
    p.x = (int32_t)round(coord.x);
    p.y = (int32_t)round(coord.y);
    return p;
}

/* Encode the mouse event to the writer according to the options.
 *
 * Not all events result in output. */
inline void encode(std::string *writer, const Event &event, const Options &opts) {
    if (!shouldReport(event, opts)) return;

    /* Handle scenarios where the mouse position is outside the viewport.
     * We always report release events no matter where they happen. */
    if (event.action != mouse::Action::release && posOutOfViewport(event.pos, opts.size)) {
        /* If we don't have a motion-tracking event mode, do nothing,
         * because events outside the viewport are never reported in
         * such cases. */
        if (!::wisp::terminal::mouse::eventSendsMotion(opts.event)) return;

        /* For motion modes, we only report if a button is currently pressed.
         * This lets a TUI detect a click over the surface + drag out
         * of the surface. */
        if (!opts.any_button_pressed) return;
    }

    const Coordinate cell = posToCell(event.pos, opts.size);

    /* We only send motion events when the cell changed unless
     * we're tracking raw pixels. */
    if (event.action == mouse::Action::motion && opts.format != MouseFormat::sgr_pixels) {
        if (opts.last_cell != nullptr) {
            if (opts.last_cell->has) {
                if (opts.last_cell->value.eql(cell)) return;
            }
        }
    }

    /* Update the last reported cell if we are tracking it. */
    if (opts.last_cell != nullptr) {
        opts.last_cell->has = true;
        opts.last_cell->value = cell;
    }

    uint8_t button_code = 0;
    if (!buttonCode(event, opts, &button_code)) return;
    char buf[64];
    switch (opts.format) {
        case MouseFormat::x10: {
            if (cell.x > 222 || cell.y > 222) {
                /* log.info("X10 mouse format can only encode X/Y up to 223") */
                return;
            }

            /* + 1 because our x/y are zero-indexed and the protocol uses 1-indexing. */
            writer->append("\x1B[M");
            writer->push_back((char)(uint8_t)(32 + button_code));
            writer->push_back((char)(uint8_t)(32 + (uint8_t)cell.x + 1));
            writer->push_back((char)(uint8_t)(32 + (uint8_t)cell.y + 1));
            break;
        }

        case MouseFormat::utf8: {
            writer->append("\x1B[M");

            /* The button code always fits in a single byte. */
            writer->push_back((char)(uint8_t)(32 + button_code));

            uint8_t enc[4];
            const uint32_t x_cp = (uint32_t)cell.x + 33;
            const uint32_t y_cp = cell.y + 33;

            const size_t x_len = zigstd::utf8Encode(x_cp, enc);
            writer->append((const char *)enc, x_len);

            const size_t y_len = zigstd::utf8Encode(y_cp, enc);
            writer->append((const char *)enc, y_len);
            break;
        }

        case MouseFormat::sgr:
            snprintf(buf, sizeof buf, "\x1B[<%u;%u;%u%c", (unsigned)button_code, (unsigned)cell.x + 1,
                     (unsigned)cell.y + 1, event.action == mouse::Action::release ? 'm' : 'M');
            writer->append(buf);
            break;

        case MouseFormat::urxvt:
            snprintf(buf, sizeof buf, "\x1B[%u;%u;%uM", 32u + button_code, (unsigned)cell.x + 1,
                     (unsigned)cell.y + 1);
            writer->append(buf);
            break;

        case MouseFormat::sgr_pixels: {
            const PixelPoint pixels = posToPixels(event.pos, opts.size);
            snprintf(buf, sizeof buf, "\x1B[<%u;%d;%d%c", (unsigned)button_code, (int)pixels.x, (int)pixels.y,
                     event.action == mouse::Action::release ? 'm' : 'M');
            writer->append(buf);
            break;
        }
    }
}

} /* namespace mouse_encode */
} /* namespace input */
} /* namespace wisp */

#endif /* WISP_INPUT_MOUSE_ENCODE_HPP */
