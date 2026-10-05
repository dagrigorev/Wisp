/* Transliterated from Ghostty src/terminal/Selection.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Represents a single selection within the terminal (i.e. a highlight region).
 *
 * TRANSLITERATION. Comments are upstream's unless marked "Wisp:".
 * Wisp: functions taking a Screen are declared here and defined in
 * screen.hpp once Screen is complete (Selection.zig and Screen.zig import
 * each other).
 */

#pragma once
#ifndef WISP_VT_SELECTION_HPP
#define WISP_VT_SELECTION_HPP

#include "page_list.hpp"

namespace wisp {
namespace vt {

struct Screen;

/* NOTE(mitchellh): I'm not very happy with how this is implemented, because
 * the ordering operations which are used frequently require using
 * pointFromPin which -- at the time of writing this -- is slow. The overall
 * style of this struct is due to porting it from the previous implementation
 * which had an efficient ordering operation.
 *
 * While reimplementing this, there were too many callers that already
 * depended on this behavior so I kept it despite the inefficiency. In the
 * future, we should take a look at this again! */
struct Selection {
    typedef PageList::Pin Pin;

    /* The bounds of the selection. A selection bounds can be either tracked
     * or untracked. Untracked bounds are unsafe beyond the point the terminal
     * screen may be modified, since they may point to invalid memory. Tracked
     * bounds are always valid and will be updated as the screen changes, but
     * are more expensive to exist.
     *
     * In all cases, start and end can be in any order. There is no guarantee that
     * start is before end or vice versa. If a user selects backwards,
     * start will be after end, and vice versa. Use the struct functions
     * to not have to worry about this. */
    struct Bounds {
        enum class Tag { untracked, tracked } tag;
        struct {
            Pin start;
            Pin end;
        } untracked;
        struct {
            Pin *start;
            Pin *end;
        } tracked;
    };

    /* The bounds of the selection. */
    Bounds bounds;

    /* Whether or not this selection refers to a rectangle, rather than whole
     * lines of a buffer. In this mode, start and end refer to the top left and
     * bottom right of the rectangle, or vice versa if the selection is backwards. */
    bool rectangle; /* = false */

    /* Initialize a new selection with the given start and end pins on
     * the screen. The screen will be used for pin tracking. */
    static Selection init(const Pin &start_pin, const Pin &end_pin, bool rect) {
        Selection s;
        s.bounds.tag = Bounds::Tag::untracked;
        s.bounds.untracked.start = start_pin;
        s.bounds.untracked.end = end_pin;
        s.bounds.tracked.start = nullptr;
        s.bounds.tracked.end = nullptr;
        s.rectangle = rect;
        return s;
    }

    void deinit(Screen *s) const;

    /* Returns true if this selection is equal to another selection. */
    bool eql(const Selection &other) const {
        return start().eql(other.start()) && end().eql(other.end()) && rectangle == other.rectangle;
    }

    /* The starting pin of the selection. This is NOT ordered. */
    Pin *startPtr() { return bounds.tag == Bounds::Tag::untracked ? &bounds.untracked.start : bounds.tracked.start; }

    /* The ending pin of the selection. This is NOT ordered. */
    Pin *endPtr() { return bounds.tag == Bounds::Tag::untracked ? &bounds.untracked.end : bounds.tracked.end; }

    Pin start() const { return bounds.tag == Bounds::Tag::untracked ? bounds.untracked.start : *bounds.tracked.start; }

    Pin end() const { return bounds.tag == Bounds::Tag::untracked ? bounds.untracked.end : *bounds.tracked.end; }

    /* Returns true if this is a tracked selection. */
    bool tracked() const { return bounds.tag == Bounds::Tag::tracked; }

    /* Convert this selection a tracked selection. It is asserted this is
     * an untracked selection. The tracked selection is returned.
     * Wisp: false is OutOfMemory. */
    bool track(Screen *s, Selection *out) const;

    /* Returns the top left point of the selection. */
    Pin topLeft(const Screen *s) const;

    /* Returns the bottom right point of the selection. */
    Pin bottomRight(const Screen *s) const;

    /* The order of the selection:
     *
     *  * forward: start(x, y) is before end(x, y) (top-left to bottom-right).
     *  * reverse: end(x, y) is before start(x, y) (bottom-right to top-left).
     *  * mirrored_[forward|reverse]: special, rectangle selections only (see below).
     *
     *  For regular selections, the above also holds for top-right to bottom-left
     *  (forward) and bottom-left to top-right (reverse). However, for rectangle
     *  selections, both of these selections are *mirrored* as orientation
     *  operations only flip the x or y axis, not both. Depending on the y axis
     *  direction, this is either mirrored_forward or mirrored_reverse. */
    enum class Order { forward, reverse, mirrored_forward, mirrored_reverse };

    Order order(const Screen *s) const;

    /* Returns the selection in the given order.
     *
     * The returned selection is always a new untracked selection.
     *
     * Note that only forward and reverse are useful desired orders for this
     * function. All other orders act as if forward order was desired. */
    Selection ordered(const Screen *s, Order desired) const;

    /* Returns true if the selection contains the given point.
     *
     * This recalculates top left and bottom right each call. If you have
     * many points to check, it is cheaper to do the containment logic
     * yourself and cache the topleft/bottomright. */
    bool contains(const Screen *s, const Pin &pin) const;

    /* Get a selection for a single row in the screen. This will return null
     * if the row is not included in the selection.
     *
     * This is a very expensive operation. It has to traverse the linked list
     * of pages for the top-left, bottom-right, and the given pin to find
     * the coordinates. If you are calling this repeatedly, prefer
     * `containedRowCached`. */
    Maybe<Selection> containedRow(const Screen *s, const Pin &pin) const;

    /* Same as containedRow but useful if you're calling it repeatedly
     * so that the pins can be cached across calls. Advanced. */
    Maybe<Selection> containedRowCached(const Screen *s, const Pin &tl_pin, const Pin &br_pin, const Pin &pin,
                                        const point::Coordinate &tl, const point::Coordinate &br,
                                        const point::Coordinate &p) const {
        (void)s;
        if (p.y < tl.y || p.y > br.y) return Maybe<Selection>::none();

        /* Rectangle case: we can return early as the x range will always be the
         * same. We've already validated that the row is in the selection. */
        if (rectangle) {
            Pin s0 = pin;
            const size::CellCountInt last = (size::CellCountInt)(s0.node->cols() - 1);
            s0.x = tl.x < last ? tl.x : last;
            Pin e0 = pin;
            e0.x = br.x < last ? br.x : last;
            return init(s0, e0, true);
        }

        if (p.y == tl.y) {
            /* If the selection is JUST this line, return it as-is. */
            if (p.y == br.y) {
                return init(tl_pin, br_pin, false);
            }

            /* Selection top-left line matches only. */
            Pin e0 = pin;
            e0.x = (size::CellCountInt)(e0.node->cols() - 1);
            return init(tl_pin, e0, false);
        }

        /* Row is our bottom selection, so we return the selection from the
         * beginning of the line to the br. We know our selection is more than
         * one line (due to conditionals above) */
        if (p.y == br.y) {
            assert(p.y != tl.y);
            Pin s0 = pin;
            s0.x = 0;
            return init(s0, br_pin, false);
        }

        /* Row is somewhere between our selection lines so we return the full line. */
        Pin s0 = pin;
        s0.x = 0;
        Pin e0 = pin;
        e0.x = (size::CellCountInt)(e0.node->cols() - 1);
        return init(s0, e0, false);
    }

    /* Possible adjustments to the selection. */
    enum class Adjustment {
        left,
        right,
        up,
        down,
        home,
        end,
        page_up,
        page_down,
        beginning_of_line,
        end_of_line,
    };

    /* Adjust the selection by some given adjustment. An adjustment allows
     * a selection to be expanded slightly left, right, up, down, etc. */
    void adjust(const Screen *s, Adjustment adjustment);
};

} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_SELECTION_HPP */
