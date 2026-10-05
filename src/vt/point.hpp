/* Transliterated from Ghostty src/terminal/point.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 */

#pragma once
#ifndef WISP_VT_POINT_HPP
#define WISP_VT_POINT_HPP

#include <stdint.h>

#include "size.hpp"

namespace wisp {
namespace vt {
namespace point {

/* The possible reference locations for a point. When someone says "(42, 80)"
 * in the context of a terminal, that could mean multiple things: it is in the
 * current visible viewport? the current active area of the screen where the
 * cursor is? the entire scrollback history? etc.
 *
 * This tag is used to differentiate those cases. */
enum class Tag : uint8_t {
    /* Top-left is part of the active area where a running program can
     * jump the cursor and make changes. The active area is the "editable"
     * part of the screen.
     *
     * The bottom-right of the active tag differs from all other tags
     * because it includes the full height (rows) of the screen, including
     * rows that may not be written yet. This is required because the active
     * area is fully "addressable" by the running program (see below) whereas
     * the other tags are used primarily for reading/modifying past-written
     * data so they can't address unwritten rows.
     *
     * Note for those less familiar with terminal functionality: there
     * are escape sequences to move the cursor to any position on
     * the screen, but it is limited to the size of the viewport and
     * the bottommost part of the screen. Terminal programs can't --
     * with sequences at the time of writing this comment -- modify
     * anything in the scrollback, visible viewport (if it differs
     * from the active area), etc. */
    active,

    /* Top-left is the visible viewport. This means that if the user
     * has scrolled in any direction, top-left changes. The bottom-right
     * is the last written row from the top-left. */
    viewport,

    /* Top-left is the furthest back in the scrollback history
     * supported by the screen and the bottom-right is the bottom-right
     * of the last written row. Note this last point is important: the
     * bottom right is NOT necessarily the same as "active" because
     * "active" always allows referencing the full rows tall of the
     * screen whereas "screen" only contains written rows. */
    screen,

    /* The top-left is the same as "screen" but the bottom-right is
     * the line just before the top of "active". This contains only
     * the scrollback history. */
    history,
};

struct Coordinate {
    /* x can use size.CellCountInt because the number of columns
     * can't ever be more than a valid number of columns in a Page. */
    size::CellCountInt x; /* = 0 */

    /* y does not use size.CellCountInt because certain coordinate
     * usage such as screen/history can have more rows than are possible
     * in a single page. */
    uint32_t y; /* = 0 */

    Coordinate() : x(0), y(0) {}
    Coordinate(size::CellCountInt x_, uint32_t y_) : x(x_), y(y_) {}

    bool eql(const Coordinate &other) const { return x == other.x && y == other.y; }
};

/* An x/y point in the terminal for some definition of location (tag).
 * Wisp: union(Tag) of four Coordinates is a tag plus one Coordinate. */
struct Point {
    Tag tag;
    Coordinate c;

    Point() : tag(Tag::active), c() {}
    Point(Tag t, Coordinate co) : tag(t), c(co) {}

    static Point active(size::CellCountInt x = 0, uint32_t y = 0) { return Point(Tag::active, Coordinate(x, y)); }
    static Point viewport(size::CellCountInt x = 0, uint32_t y = 0) { return Point(Tag::viewport, Coordinate(x, y)); }
    static Point screen(size::CellCountInt x = 0, uint32_t y = 0) { return Point(Tag::screen, Coordinate(x, y)); }
    static Point history(size::CellCountInt x = 0, uint32_t y = 0) { return Point(Tag::history, Coordinate(x, y)); }

    Coordinate coord() const { return c; }

    bool eql(const Point &o) const { return tag == o.tag && c.eql(o.c); }
};

} /* namespace point */
} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_POINT_HPP */
