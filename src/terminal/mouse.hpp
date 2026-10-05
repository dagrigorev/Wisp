/* Transliterated from Ghostty src/terminal/mouse.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 * Wisp: Shape.getGObjectType and the ghostty.h enum check are GTK / C-ABI
 * build concerns and are not carried over.
 */

#pragma once
#ifndef WISP_TERMINAL_MOUSE_HPP
#define WISP_TERMINAL_MOUSE_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace wisp {
namespace terminal {
namespace mouse {

/* The event types that can be reported for mouse-related activities.
 * These are all mutually exclusive (hence in a single enum). */
enum class Event : uint8_t {
    none,
    x10,    /* 9 */
    normal, /* 1000 */
    button, /* 1002 */
    any,    /* 1003 */
};

/* Returns true if this event sends motion events. */
inline bool eventSendsMotion(Event event) {
    return event == Event::button || event == Event::any;
}

/* The format of mouse events when enabled.
 * These are all mutually exclusive (hence in a single enum). */
enum class Format : uint8_t {
    x10,
    utf8,       /* 1005 */
    sgr,        /* 1006 */
    urxvt,      /* 1015 */
    sgr_pixels, /* 1016 */
};

/* The possible cursor shapes. Not all app runtimes support these shapes.
 * The shapes are always based on the W3C supported cursor styles so we
 * can have a cross platform list.
 *
 * Must be kept in sync with ghostty_cursor_shape_e */
enum class Shape : int {
    default_,
    context_menu,
    help,
    pointer,
    progress,
    wait,
    cell,
    crosshair,
    text,
    vertical_text,
    alias,
    copy,
    move,
    no_drop,
    not_allowed,
    grab,
    grabbing,
    all_scroll,
    col_resize,
    row_resize,
    n_resize,
    e_resize,
    s_resize,
    w_resize,
    ne_resize,
    nw_resize,
    se_resize,
    sw_resize,
    ew_resize,
    ns_resize,
    nesw_resize,
    nwse_resize,
    zoom_in,
    zoom_out,
};

/* Wisp: std.StaticStringMap(Shape) — a linear scan over the same pairs. */
struct ShapeEntry {
    const char *name;
    Shape shape;
};

inline const ShapeEntry *string_map(size_t *len) {
    static const ShapeEntry map[] = {
        /* W3C */
        { "default", Shape::default_ },
        { "context-menu", Shape::context_menu },
        { "help", Shape::help },
        { "pointer", Shape::pointer },
        { "progress", Shape::progress },
        { "wait", Shape::wait },
        { "cell", Shape::cell },
        { "crosshair", Shape::crosshair },
        { "text", Shape::text },
        { "vertical-text", Shape::vertical_text },
        { "alias", Shape::alias },
        { "copy", Shape::copy },
        { "move", Shape::move },
        { "no-drop", Shape::no_drop },
        { "not-allowed", Shape::not_allowed },
        { "grab", Shape::grab },
        { "grabbing", Shape::grabbing },
        { "all-scroll", Shape::all_scroll },
        { "col-resize", Shape::col_resize },
        { "row-resize", Shape::row_resize },
        { "n-resize", Shape::n_resize },
        { "e-resize", Shape::e_resize },
        { "s-resize", Shape::s_resize },
        { "w-resize", Shape::w_resize },
        { "ne-resize", Shape::ne_resize },
        { "nw-resize", Shape::nw_resize },
        { "se-resize", Shape::se_resize },
        { "sw-resize", Shape::sw_resize },
        { "ew-resize", Shape::ew_resize },
        { "ns-resize", Shape::ns_resize },
        { "nesw-resize", Shape::nesw_resize },
        { "nwse-resize", Shape::nwse_resize },
        { "zoom-in", Shape::zoom_in },
        { "zoom-out", Shape::zoom_out },

        /* xterm/foot */
        { "left_ptr", Shape::default_ },
        { "question_arrow", Shape::help },
        { "hand", Shape::pointer },
        { "left_ptr_watch", Shape::progress },
        { "watch", Shape::wait },
        { "cross", Shape::crosshair },
        { "xterm", Shape::text },
        { "dnd-link", Shape::alias },
        { "dnd-copy", Shape::copy },
        { "dnd-move", Shape::move },
        { "dnd-no-drop", Shape::no_drop },
        { "crossed_circle", Shape::not_allowed },
        { "hand1", Shape::grab },
        { "right_side", Shape::e_resize },
        { "top_side", Shape::n_resize },
        { "top_right_corner", Shape::ne_resize },
        { "top_left_corner", Shape::nw_resize },
        { "bottom_side", Shape::s_resize },
        { "bottom_right_corner", Shape::se_resize },
        { "bottom_left_corner", Shape::sw_resize },
        { "left_side", Shape::w_resize },
        { "fleur", Shape::all_scroll },
    };
    *len = sizeof(map) / sizeof(map[0]);
    return map;
}

/* Build cursor shape from string or null if its unknown.
 * Wisp: ?Shape is the bool return plus *out. */
inline bool Shape_fromString(const char *v, size_t len, Shape *out) {
    size_t n;
    const ShapeEntry *map = string_map(&n);
    for (size_t i = 0; i < n; i++) {
        if (strlen(map[i].name) == len && memcmp(map[i].name, v, len) == 0) {
            *out = map[i].shape;
            return true;
        }
    }
    return false;
}
inline bool Shape_fromString(const char *v, Shape *out) {
    return Shape_fromString(v, strlen(v), out);
}

} /* namespace mouse */
} /* namespace terminal */
} /* namespace wisp */

#endif /* WISP_TERMINAL_MOUSE_HPP */
