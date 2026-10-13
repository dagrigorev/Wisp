/* Transliterated from Ghostty src/terminal/SelectionGesture.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * TRANSLITERATION, see ../terminal/parser.hpp and ../terminal/osc.hpp for the
 * Zig-to-C++ mapping. Comments are upstream's unless marked "Wisp:".
 *
 * Wisp, differences in shape rather than behavior:
 *   - `?Selection` is `Maybe<Selection>`; `?*Pin` is a plain `Pin *` whose
 *     null is upstream's null; `?std.Io.Timestamp` is a nanosecond count
 *     plus a has_time flag.
 *   - `lib.Enum(lib.target, ...)` generates a C-ABI enum upstream; here the
 *     enums are ordinary enum classes.
 *   - `Allocator.Error!?Selection` from `press` is the Maybe return plus a
 *     bool out-parameter for OutOfMemory.
 *   - `error{PressRequiresReset}!void` from `pressRepeat` is a bool.
 *   - slices are pointer plus length, so the word boundary codepoints and
 *     the behaviors array arrive as pointer plus length.
 */

#pragma once
#ifndef WISP_VT_SELECTION_GESTURE_HPP
#define WISP_VT_SELECTION_GESTURE_HPP

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "page_list.hpp"
#include "screen.hpp"
#include "screen_set.hpp"
#include "selection.hpp"
#include "terminal.hpp"

namespace wisp {
namespace vt {

/* SelectionGesture manages gesture-based terminal text selection for one
 * pointer stream: press, drag, release, autoscroll, and pressure/deep-press
 * selection.
 *
 * This type owns only the state required to interpret a gesture. It does not
 * modify the terminal selection directly, except for scrolling the viewport
 * during `autoscrollTick`. The caller feeds platform events into this type and
 * applies the returned `Selection` to the active screen when appropriate.
 *
 * A typical single-click drag flow looks like this:
 *
 *     bool oom = false;
 *     const Maybe<Selection> sel = gesture.press(&terminal, press_event, &oom);
 *     if (sel.has) terminal.screens.active->select(sel.value);
 *     const Maybe<Selection> dragged = gesture.drag(&terminal, drag_event);
 *     if (dragged.has) terminal.screens.active->select(dragged.value);
 *     gesture.release(&terminal, release_event);
 *
 * Double- and triple-click gestures use the same event flow. Repeated presses
 * inside `Press.repeat_interval` and within `Press.max_distance` increment the
 * internal click count up to three. `Press.behaviors` maps single-, double-,
 * and triple-clicks to behavior. By default, a single press returns null to
 * clear any existing selection, a double-click returns a word selection, and a
 * triple-click returns a line selection. Drags use the behavior selected by the
 * corresponding press. A new press that is too late, too far away, or on
 * another active screen starts a new single-click gesture.
 *
 * # Resetting and lifetime
 *
 * `release` ends the active drag/autoscroll phase but intentionally preserves
 * enough state for a subsequent press to become a double- or triple-click.
 * Call `reset` when the gesture is cancelled rather than released normally, or
 * when another subsystem takes ownership of pointer input. Examples include
 * enabling mouse reporting for an application, losing pointer/button state,
 * destroying the surface, switching to a mode that must not continue text
 * selection, or otherwise abandoning the current click sequence. Call `deinit`
 * once before discarding the gesture object so any tracked click pin is
 * released.
 *
 * # Terminal and screen changes
 *
 * The initial press pin is tracked in the active screen's `PageList`, so normal
 * terminal output and viewport scrolling can move rows without making the
 * gesture immediately stale. Selection results are computed against the current
 * terminal contents at the time of each call. For example, a double-click drag
 * selects word boundaries from the screen as it exists during `drag`, not from a
 * snapshot captured at `press`. */
struct SelectionGesture {
    typedef PageList::Pin Pin;

    /* The direction that selection dragging should autoscroll the viewport.
     * This is derived from the most recent drag position relative to the
     * surface bounds and reset whenever there is no active drag gesture.
     *
     * When autoscroll is non-none, the caller should setup a timer
     * to periodically call autoscrollTick. The timer interval is up to the
     * caller but reasonable defaults are approximately every 15 milliseconds.
     *
     * This is used to implement selection above/below the viewport that
     * wants to drag the viewport. */
    enum class Autoscroll : uint8_t {
        none,
        up,
        down,
    };

    /* The selection behavior for a click and subsequent drag. */
    enum class Behavior : uint8_t {
        /* Cell-granular drag selection. Press returns null to clear selection. */
        cell,

        /* Word selection on press and word-granular drag selection. */
        word,

        /* Line selection on press and line-granular drag selection. */
        line,

        /* Semantic command output selection on press and drag. */
        output,
    };

    /* Standard terminal selection behavior for single-, double-, and
     * triple-clicks.
     *
     * A single click uses cell behavior, which returns null on press so callers
     * can clear any existing selection and then drag by cell. A double-click
     * selects and drags by word. A triple-click selects and drags by line. */
    static const Behavior *default_behaviors() {
        static const Behavior tbl[3] = {Behavior::cell, Behavior::word, Behavior::line};
        return tbl;
    }

    /* Wisp: `?std.Io.Timestamp`. Only the difference between two timestamps
     * matters, so this is a nanosecond count plus a flag for upstream's null,
     * which means the platform could not supply a time. */
    struct Timestamp {
        bool has;
        int64_t nanoseconds;

        Timestamp() : has(false), nanoseconds(0) {}
        explicit Timestamp(int64_t ns) : has(true), nanoseconds(ns) {}
    };

    /* The tracked pin of the initial left click along with the screen
     * that the pin is part of. */
    Pin *left_click_pin;
    ScreenSet::Key left_click_screen;
    size_t left_click_screen_generation;

    /* The count of clicks to count double and triple clicks and so on.
     * The left click time was the last time the left click was done, if the
     * caller could provide one. If this is null then we only support single
     * clicks. */
    uint8_t left_click_count;
    Timestamp left_click_time;

    /* The selection behavior chosen for the active left-click gesture. */
    Behavior left_click_behavior;

    /* The starting xpos/ypos of the left click. Note that if scrolling occurs,
     * these will point to different cells, but the xpos/ypos will stay
     * stable during scrolling relative to the surface. */
    double left_click_xpos;
    double left_click_ypos;

    /* True once the active left-click gesture has moved away from the initially
     * pressed cell. This is reset on every press that starts or continues a
     * multi-click sequence, and is left available for callers to inspect while
     * handling the corresponding release. */
    bool left_click_dragged;

    /* The current autoscroll state for the active left-click drag gesture. */
    Autoscroll left_drag_autoscroll;

    SelectionGesture()
        : left_click_pin(nullptr), left_click_screen(ScreenSet::Key::primary),
          left_click_screen_generation(0), left_click_count(0), left_click_time(),
          left_click_behavior(Behavior::cell), left_click_xpos(0), left_click_ypos(0),
          left_click_dragged(false), left_drag_autoscroll(Autoscroll::none) {}

    void deinit(Terminal *t) {
        /* Grab our pagelist that is associated with the pin. If it doesn't
         * exist anymore then our tracked pin is already free. */
        Pin *pin = left_click_pin;
        if (pin == nullptr) return;
        if (t->screens.generation(left_click_screen) != left_click_screen_generation) return;
        Screen *screen = t->screens.get(left_click_screen);
        if (screen == nullptr) return;
        screen->pages.untrackPin(pin);
    }

    /* Reset any active gesture state and untrack the tracked click pin.
     *
     * Use this for cancellation/abandonment, not for the ordinary left-button
     * release path. `release` deliberately keeps the last press time/count so a
     * following press can become a double- or triple-click; `reset` clears that
     * sequence and makes the next press a fresh single click.
     *
     * Examples of reset-worthy events are: mouse reporting taking over, pointer
     * capture being lost, a surface/window being torn down, or another
     * interaction mode deciding that text selection must stop immediately. If
     * the active screen was already removed or recycled, this safely drops the
     * stale reference without trying to untrack a pin from the wrong screen
     * generation. */
    void reset(Terminal *t) {
        left_click_count = 0;
        left_click_time = Timestamp();
        left_click_behavior = Behavior::cell;
        left_click_dragged = false;
        left_drag_autoscroll = Autoscroll::none;
        untrackPin(t);
    }

    /* Return the tracked left-click pin only if it still belongs to the current
     * active screen instance.
     *
     * This validates both the screen key and generation so a pin from a
     * removed, recycled, or inactive screen is never exposed to callers. A null
     * result means callers should treat the in-progress gesture as temporarily
     * or permanently unable to produce a selection. For a normal drag this
     * usually means "do nothing for this event"; for autoscroll it is treated
     * as cancellation because a timer should not continue firing for a gesture
     * that no longer has a valid anchor. */
    Pin *validatedLeftClickPin(const ScreenSet *screens) const {
        Pin *pin = left_click_pin;
        if (pin == nullptr) return nullptr;
        if (left_click_screen != screens->active_key) return nullptr;
        if (screens->generation(left_click_screen) != left_click_screen_generation) return nullptr;
        if (screens->get(left_click_screen) == nullptr) return nullptr;
        return pin;
    }

    struct Press {
        /* The time when the press event occurred. Prefer a monotonic timer;
         * backwards timestamps reset the repeat sequence.
         * This can be null if you're on a system that doesn't support
         * time for some reason. In that case, we only support single clicks. */
        Timestamp time;

        /* The cell where the click was.
         *
         * `press` stores a tracked copy of this pin. The caller does not need
         * to keep `p.pin` alive after the call returns, but the pin must belong
         * to the terminal's active screen when passed in. */
        Pin pin;

        /* The x/y value of the click relative to the surface with (0,0) being
         * top-left. This is used for distance detection for multi-clicks so
         * double/triple clicks too far away from each other will reset the
         * click count as well more accurate drag behaviors. */
        double xpos;
        double ypos;

        /* Maximum distance a click can be from the original click to register
         * as a repeat. If uncertain, set this to cell width. */
        double max_distance;

        /* The maximum interval in nanoseconds that a press is considered
         * a repeat e.g. to record double/triple clicks. */
        uint64_t repeat_interval;

        /* The codepoints that delimit words for double-click selection. */
        const uint32_t *word_boundary_codepoints;
        size_t word_boundary_len;

        /* Selection behaviors for single-, double-, and triple-clicks. */
        const Behavior *behaviors; /* = &default_behaviors */

        Press()
            : time(), pin(), xpos(0), ypos(0), max_distance(0), repeat_interval(0),
              word_boundary_codepoints(nullptr), word_boundary_len(0),
              behaviors(default_behaviors()) {}
    };

    /* Record a press event and return the standard selection for this click.
     *
     * If this press continues the existing click sequence, the click count is
     * incremented up to three and the original anchor pin is kept. Otherwise,
     * the previous gesture state is cleared and this press becomes the new
     * anchor. The returned selection is untracked and represents the standard
     * terminal click behavior for the resulting click count. The caller is
     * responsible for applying it to the screen, usually with `Screen.select`,
     * and for arranging any copy-on-select behavior.
     *
     * Examples:
     *
     * * first press: `left_click_count == 1`, defaults to cell behavior;
     * * second nearby press within the repeat interval: `left_click_count == 2`,
     *   defaults to word behavior;
     * * third nearby press within the repeat interval: `left_click_count == 3`,
     *   defaults to line behavior;
     * * press after the interval, too far away, or after a screen generation
     *   change: starts over at `left_click_count == 1` and returns null.
     *
     * Wisp: `*oom` is set on Allocator.Error. */
    Maybe<Selection> press(Terminal *t, const Press &p, bool *oom) {
        *oom = false;
        if (left_click_count > 0) {
            if (pressRepeat(t, p)) {
                /* Successful repeat. */
                return pressSelection(t->screens.active, p);
            }
            /* error.PressRequiresReset: fall through to the initial press. */
        }

        /* Initial click or the repeat failed for some reason such as
         * the subsequent click being too far away. */
        if (!pressInitial(t, p)) {
            *oom = true;
            return Maybe<Selection>();
        }
        return pressSelection(t->screens.active, p);
    }

    struct Drag {
        /* Display geometry needed to translate surface-relative pointer
         * positions into selection behavior. */
        struct Geometry {
            /* The number of columns in the rendered terminal grid. */
            uint32_t columns;

            /* The width of one terminal cell in surface pixels. */
            uint32_t cell_width;

            /* The left padding before the terminal grid begins, in surface
             * pixels. */
            uint32_t padding_left;

            /* The height of the rendered terminal surface in surface pixels. */
            uint32_t screen_height;

            Geometry() : columns(0), cell_width(0), padding_left(0), screen_height(0) {}
        };

        /* The cell where the current drag position is. This is used
         * synchronously to calculate the selection and is not tracked. */
        Pin pin;

        /* The x/y value of the drag relative to the surface with (0,0) being
         * top-left. */
        double xpos;
        double ypos;

        /* True if the current drag should produce a rectangular selection. */
        bool rectangle;

        /* The codepoints that delimit words for double-click drag selection. */
        const uint32_t *word_boundary_codepoints;
        size_t word_boundary_len;

        /* Geometry required for selection threshold and autoscroll
         * calculations. */
        Geometry geometry;

        Drag()
            : pin(), xpos(0), ypos(0), rectangle(false), word_boundary_codepoints(nullptr),
              word_boundary_len(0), geometry() {}
    };

    /* Record a drag event and return the current untracked drag selection.
     *
     * The returned selection is untracked and represents the best selection for
     * the terminal contents at the time of this call. The caller is responsible
     * for applying it to the screen, usually with `Screen.select`, and for
     * arranging any copy-on-select behavior. A null result means either there
     * is no active selection gesture, the original press is no longer valid for
     * the active screen, or the drag has not crossed the threshold required to
     * select a cell.
     *
     * This method also updates `left_click_dragged` and
     * `left_drag_autoscroll`. If `left_drag_autoscroll` becomes `.up` or
     * `.down`, the caller should start or keep a timer that calls
     * `autoscrollTick` while the button remains pressed. If it becomes `.none`,
     * the caller should stop that timer.
     *
     * Normal terminal output and viewport movement between drag events are
     * allowed: the tracked press pin follows the page list, and the drag pin is
     * used only synchronously. Content-sensitive selections such as word and
     * line selection are recalculated from the current active screen every
     * time. */
    Maybe<Selection> drag(Terminal *t, const Drag &d) {
        /* If we aren't currently clicked then we don't do any dragging
         * behavior. */
        if (left_click_count == 0) {
            assert(left_drag_autoscroll == Autoscroll::none);
            return Maybe<Selection>();
        }

        /* Get our click pin. We get a validated pin because if our
         * screen changed out from under us then we aren't actually
         * clicking anymore. */
        Pin *click_pin = validatedLeftClickPin(&t->screens);
        if (click_pin == nullptr) return Maybe<Selection>();
        if (!d.pin.eql(*click_pin)) left_click_dragged = true;

        /* Determine if we should autoscroll. If our drag position is above
         * the top, we go up. If its below the bottom we go down. Easy. */
        const double max_y = (double)d.geometry.screen_height;
        left_drag_autoscroll = d.ypos <= autoscroll_buffer
                                   ? Autoscroll::up
                                   : (d.ypos > max_y - autoscroll_buffer ? Autoscroll::down
                                                                        : Autoscroll::none);

        Maybe<Selection> selection;
        switch (left_click_behavior) {
        case Behavior::cell:
            selection = dragSelection(*click_pin, d.pin, pixelFromFloat(left_click_xpos),
                                      pixelFromFloat(d.xpos), d.rectangle, d.geometry);
            break;

        case Behavior::word:
            selection = dragSelectionWord(t->screens.active, *click_pin, d.pin,
                                          d.word_boundary_codepoints, d.word_boundary_len);
            break;

        case Behavior::line:
            selection = dragSelectionLine(t->screens.active, *click_pin, d.pin);
            break;

        case Behavior::output:
            selection = dragSelectionOutput(t->screens.active, *click_pin, d.pin);
            break;
        }

        /* Same-cell cell selections can still become real selections when the
         * drag crosses the within-cell threshold. Treat those as drags so
         * callers don't also process click-only actions such as opening
         * links. */
        if (left_click_behavior == Behavior::cell && selection.has) left_click_dragged = true;

        return selection;
    }

    struct AutoscrollTick {
        /* The viewport cell where the current drag position is. This is
         * resolved after the viewport is scrolled so the selection tracks the
         * newly visible row under the pointer. */
        point::Coordinate viewport;

        /* The x/y value of the drag relative to the surface with (0,0) being
         * top-left. */
        double xpos;
        double ypos;

        /* True if the current drag should produce a rectangular selection. */
        bool rectangle;

        /* The codepoints that delimit words for double-click drag selection. */
        const uint32_t *word_boundary_codepoints;
        size_t word_boundary_len;

        /* Geometry required for selection threshold and autoscroll
         * calculations. */
        Drag::Geometry geometry;

        AutoscrollTick()
            : viewport(), xpos(0), ypos(0), rectangle(false),
              word_boundary_codepoints(nullptr), word_boundary_len(0), geometry() {}
    };

    /* Record a selection autoscroll tick for the active left-click drag
     * gesture.
     *
     * This scrolls the viewport in the active autoscroll direction and then
     * continues the drag at the provided viewport position. The viewport
     * position is resolved to a pin after scrolling so the drag applies to the
     * row now under the pointer.
     *
     * This always scrolls the viewport by exactly one row in the current
     * autoscroll direction. If you want to scroll by more, increase your
     * tick rate.
     *
     * If the original press pin no longer belongs to the active screen, this
     * calls `reset` and returns null. That is a signal for the caller to stop
     * its autoscroll timer and leave any existing terminal selection alone
     * unless some other event says otherwise. */
    Maybe<Selection> autoscrollTick(Terminal *t, const AutoscrollTick &tick) {
        if (left_click_count == 0) {
            assert(left_drag_autoscroll == Autoscroll::none);
            return Maybe<Selection>();
        }

        ptrdiff_t delta = 0;
        switch (left_drag_autoscroll) {
        case Autoscroll::none: return Maybe<Selection>();
        case Autoscroll::up: delta = -1; break;
        case Autoscroll::down: delta = 1; break;
        }

        /* If our click pin no longer belongs to the active screen, the gesture
         * is no longer valid. Stop it so callers can stop their autoscroll
         * timer without clearing the current selection as if this were a real
         * drag. */
        if (validatedLeftClickPin(&t->screens) == nullptr) {
            reset(t);
            return Maybe<Selection>();
        }

        t->scrollViewport(Terminal::ScrollViewport::makeDelta(delta));

        const Maybe<Pin> pin = t->screens.active->pages.pin(
            point::Point(point::Tag::viewport, tick.viewport));
        if (!pin.has) return Maybe<Selection>();

        Drag d;
        d.pin = pin.value;
        d.xpos = tick.xpos;
        d.ypos = tick.ypos;
        d.rectangle = tick.rectangle;
        d.word_boundary_codepoints = tick.word_boundary_codepoints;
        d.word_boundary_len = tick.word_boundary_len;
        d.geometry = tick.geometry;
        return drag(t, d);
    }

    /* A pressure-based activation during an existing left-click gesture.
     *
     * This is the terminal gesture model for platform features such as macOS
     * force click / deep click on pressure-sensitive trackpads. It is not a
     * distinct mouse button and it is not part of the normal single/double/triple
     * click count sequence; it can only occur after a left press is already
     * active. */
    struct DeepPress {
        /* The codepoints that delimit words for the word selection produced by
         * the deep press. */
        const uint32_t *word_boundary_codepoints;
        size_t word_boundary_len;

        DeepPress() : word_boundary_codepoints(nullptr), word_boundary_len(0) {}
    };

    /* Record a deep press event for the active left-click gesture.
     *
     * A deep press is a force/pressure activation while the primary pointer is
     * already down. Ghostty treats it like the platform text-selection
     * affordance: select the word under the original press, then consume the
     * gesture so further cursor movement while the button remains pressed does
     * not drag or autoscroll the selection.
     *
     * After a successful deep press, the click sequence is cleared and the
     * tracked pin is untracked. The returned selection should be applied by the
     * caller. A null result means there was no valid active left-click anchor,
     * commonly because the screen changed or the gesture had already been
     * cancelled. */
    Maybe<Selection> deepPress(Terminal *t, const DeepPress &p) {
        Pin *click_pin = validatedLeftClickPin(&t->screens);
        if (click_pin == nullptr) return Maybe<Selection>();
        const Maybe<Selection> sel = t->screens.active->selectWord(
            *click_pin, p.word_boundary_codepoints, p.word_boundary_len);

        left_click_count = 0;
        left_click_time = Timestamp();
        left_click_behavior = Behavior::cell;
        left_click_dragged = true;
        left_drag_autoscroll = Autoscroll::none;
        untrackPin(t);

        return sel;
    }

    struct Release {
        /* The cell where the release occurred, if the release position mapped
         * to a valid cell. This is used synchronously to update gesture state
         * and is not tracked. */
        Maybe<Pin> pin;

        Release() : pin() {}
    };

    /* Record a release event for the active left-click gesture.
     *
     * This stops autoscroll and updates `left_click_dragged`, but it does not
     * clear the click count or time. Keeping that state is what lets the next
     * nearby press become a double- or triple-click. Call `reset` instead if
     * the release should cancel the click sequence entirely.
     *
     * Pass the release pin when the pointer position maps to a valid terminal
     * cell. If it does not, pass null; the gesture then conservatively records
     * that the pointer moved away from the original pressed cell. This is
     * useful for callers that use `left_click_dragged` after release to decide
     * whether a click should activate links or other hit targets. */
    void release(Terminal *t, const Release &r) {
        if (left_click_count == 0) {
            assert(left_drag_autoscroll == Autoscroll::none);
            return;
        }

        if (r.pin.has) {
            Pin *click_pin = validatedLeftClickPin(&t->screens);
            if (click_pin != nullptr) {
                if (!r.pin.value.eql(*click_pin)) left_click_dragged = true;
            } else {
                /* If the original anchor is no longer valid, conservatively
                 * treat this as a drag/cancelled click so callers don't perform
                 * click-only actions on a different or recycled screen. */
                left_click_dragged = true;
            }
        } else {
            left_click_dragged = true;
        }
        left_drag_autoscroll = Autoscroll::none;
    }

public:
    /* Wisp: these are file-private upstream, where the tests live in the same
     * file and can reach them. They are pure functions of their arguments, so
     * exposing them for the ported tests changes reach, not behavior. */
    /* Calculates the appropriate selection given pins and pixel x positions for
     * the click point and the drag point, as well as selection mode and
     * geometry. */
    static Maybe<Selection> dragSelection(const Pin &click_pin, const Pin &drag_pin,
                                          uint32_t click_x, uint32_t drag_x,
                                          bool rectangle_selection,
                                          const Drag::Geometry &geometry) {
        /* Explanation:
         *
         * # Normal selections
         *
         * ## Left-to-right selections
         * - The clicked cell is included if it was clicked to the left of its
         *   threshold point and the drag location is right of the threshold
         *   point.
         * - The cell under the cursor (the "drag cell") is included if the drag
         *   location is right of its threshold point.
         *
         * ## Right-to-left selections
         * - The clicked cell is included if it was clicked to the right of its
         *   threshold point and the drag location is left of the threshold
         *   point.
         * - The cell under the cursor (the "drag cell") is included if the drag
         *   location is left of its threshold point.
         *
         * # Rectangular selections
         *
         * Rectangular selections are handled similarly, except that
         * entire columns are considered rather than individual cells. */

        if (geometry.columns == 0 || geometry.cell_width == 0) return Maybe<Selection>();

        /* We only include cells in the selection if the threshold point lies
         * between the start and end points of the selection. A threshold of
         * 60% of the cell width was chosen empirically because it felt good. */
        const uint32_t threshold_point = (uint32_t)floor((double)geometry.cell_width * 0.6 + 0.5);

        /* We use this to clamp the pixel positions below.
         * Wisp: std.math.mul returning error.Overflow saturates to maxInt. */
        const uint64_t span = (uint64_t)geometry.columns * (uint64_t)geometry.cell_width;
        const uint32_t pixel_span = span > UINT32_MAX ? UINT32_MAX : (uint32_t)span;
        const uint32_t max_x = pixel_span - 1;

        /* We need to know how far across in the cell the drag pos is, so
         * we subtract the padding and then take it modulo the cell width.
         * Wisp: `-|` is a saturating subtraction. */
        const uint32_t drag_sat =
            drag_x > geometry.padding_left ? drag_x - geometry.padding_left : 0;
        const uint32_t drag_x_frac = (drag_sat < max_x ? drag_sat : max_x) % geometry.cell_width;

        /* We figure out the fractional part of the click x position
         * similarly. */
        const uint32_t click_sat =
            click_x > geometry.padding_left ? click_x - geometry.padding_left : 0;
        const uint32_t click_x_frac = (click_sat < max_x ? click_sat : max_x) % geometry.cell_width;

        /* Whether the click pin and drag pin are equal. */
        const bool same_pin = drag_pin.eql(click_pin);

        /* Whether or not the end point of our selection is before the start
         * point. */
        bool end_before_start;
        if (same_pin) {
            end_before_start = drag_x_frac < click_x_frac;
        } else if (rectangle_selection) {
            /* Special handling for rectangular selections, we only use x
             * position. */
            if (drag_pin.x == click_pin.x) {
                end_before_start = drag_x_frac < click_x_frac;
            } else {
                end_before_start = drag_pin.x < click_pin.x;
            }
        } else {
            end_before_start = drag_pin.before(click_pin);
        }

        /* Whether or not the click pin cell
         * should be included in the selection. */
        const bool include_click_cell = end_before_start ? click_x_frac >= threshold_point
                                                         : click_x_frac < threshold_point;

        /* Whether or not the drag pin cell
         * should be included in the selection. */
        const bool include_drag_cell = end_before_start ? drag_x_frac < threshold_point
                                                        : drag_x_frac >= threshold_point;

        /* If the click cell should be included in the selection then it's the
         * start, otherwise we get the previous or next cell to it depending on
         * the type and direction of the selection. */
        Pin start_pin;
        if (include_click_cell) {
            start_pin = click_pin;
        } else if (end_before_start) {
            if (rectangle_selection) {
                start_pin = click_pin.leftClamp(1);
            } else {
                const Maybe<Pin> wrapped = click_pin.leftWrap(1);
                start_pin = wrapped.has ? wrapped.value : click_pin;
            }
        } else if (rectangle_selection) {
            start_pin = click_pin.rightClamp(1);
        } else {
            const Maybe<Pin> wrapped = click_pin.rightWrap(1);
            start_pin = wrapped.has ? wrapped.value : click_pin;
        }

        /* Likewise for the end pin with the drag cell. */
        Pin end_pin;
        if (include_drag_cell) {
            end_pin = drag_pin;
        } else if (end_before_start) {
            if (rectangle_selection) {
                end_pin = drag_pin.rightClamp(1);
            } else {
                const Maybe<Pin> wrapped = drag_pin.rightWrap(1);
                end_pin = wrapped.has ? wrapped.value : drag_pin;
            }
        } else if (rectangle_selection) {
            end_pin = drag_pin.leftClamp(1);
        } else {
            const Maybe<Pin> wrapped = drag_pin.leftWrap(1);
            end_pin = wrapped.has ? wrapped.value : drag_pin;
        }

        /* If the click cell is the same as the drag cell and the click cell
         * shouldn't be included, or if the cells are adjacent such that the
         * start or end pin becomes the other cell, and that cell should not
         * be included, then we have no selection, so we set it to null.
         *
         * If in rectangular selection mode, we compare columns as well.
         *
         * TODO(qwerasd): this can/should probably be refactored, it's a bit
         *                repetitive and does excess work in rectangle mode. */
        if ((!include_click_cell && same_pin) ||
            (!include_click_cell && rectangle_selection && click_pin.x == drag_pin.x) ||
            (!include_click_cell && end_pin.eql(click_pin)) ||
            (!include_click_cell && rectangle_selection && end_pin.x == click_pin.x) ||
            (!include_drag_cell && start_pin.eql(drag_pin)) ||
            (!include_drag_cell && rectangle_selection && start_pin.x == drag_pin.x)) {
            return Maybe<Selection>();
        }

        /* TODO: Clamp selection to the screen area, don't
         *       let it extend past the last written row. */

        return Maybe<Selection>(Selection::init(start_pin, end_pin, rectangle_selection));
    }

    /* Calculates the appropriate word-wise selection for a double-click drag. */
    static Maybe<Selection> dragSelectionWord(Screen *screen, const Pin &click_pin,
                                              const Pin &drag_pin,
                                              const uint32_t *boundary_codepoints,
                                              size_t boundary_len) {
        /* Get the word closest to our starting click. */
        const Maybe<Selection> word_start =
            screen->selectWordBetween(click_pin, drag_pin, boundary_codepoints, boundary_len);
        if (!word_start.has) return Maybe<Selection>();

        /* Get the word closest to our current point. */
        const Maybe<Selection> word_current =
            screen->selectWordBetween(drag_pin, click_pin, boundary_codepoints, boundary_len);
        if (!word_current.has) return Maybe<Selection>();

        /* If our current mouse position is before the starting position,
         * then the selection start is the word nearest our current position. */
        if (drag_pin.before(click_pin)) {
            return Maybe<Selection>(
                Selection::init(word_current.value.start(), word_start.value.end(), false));
        }
        return Maybe<Selection>(
            Selection::init(word_start.value.start(), word_current.value.end(), false));
    }

    /* Calculates the appropriate line-wise selection for a triple-click drag. */
    static Maybe<Selection> dragSelectionLine(Screen *screen, const Pin &click_pin,
                                              const Pin &drag_pin) {
        /* Get the line selection under our current drag point. If there isn't a
         * line, do nothing. */
        const Maybe<Selection> line = screen->selectLine(Screen::SelectLine(drag_pin));
        if (!line.has) return Maybe<Selection>();

        /* Get the selection under our click point. We first try to trim
         * whitespace if we've selected a word. But if no word exists then
         * we select the blank line. */
        Maybe<Selection> sel_ = screen->selectLine(Screen::SelectLine(click_pin));
        if (!sel_.has) {
            Screen::SelectLine no_ws(click_pin);
            /* Wisp: `.whitespace = null` is whitespace_len == SIZE_MAX. */
            no_ws.whitespace = nullptr;
            no_ws.whitespace_len = (size_t)-1;
            sel_ = screen->selectLine(no_ws);
        }

        if (!sel_.has) return Maybe<Selection>();
        Selection sel = sel_.value;
        if (drag_pin.before(click_pin)) {
            *sel.startPtr() = line.value.start();
        } else {
            *sel.endPtr() = line.value.end();
        }
        return Maybe<Selection>(sel);
    }

    /* Calculates the appropriate semantic-output-wise selection for an output
     * drag. This expands from the output block under the click point to the
     * output block under the current drag point. If the drag point is not
     * output, keep the original output selection. */
    static Maybe<Selection> dragSelectionOutput(Screen *screen, const Pin &click_pin,
                                                const Pin &drag_pin) {
        const Maybe<Selection> sel_ = screen->selectOutput(click_pin);
        if (!sel_.has) return Maybe<Selection>();
        Selection sel = sel_.value;
        const Maybe<Selection> current = screen->selectOutput(drag_pin);
        if (!current.has) return Maybe<Selection>(sel);

        if (drag_pin.before(click_pin)) {
            *sel.startPtr() = current.value.start();
        } else {
            *sel.endPtr() = current.value.end();
        }
        return Maybe<Selection>(sel);
    }

private:
    /* Distance from the top or bottom surface edge, in pixels, where dragging
     * should request autoscroll. This preserves the historical 1px buffer used
     * so fullscreen-edge drags can still trigger autoscroll. */
    static const int autoscroll_buffer = 1;

    /* Wisp: false is Allocator.Error.OutOfMemory. */
    bool pressInitial(Terminal *t, const Press &p) {
        /* Setup our pin first, reusing our existing pin if we can. */
        if (left_click_pin != nullptr) {
#if WISP_SLOW_RUNTIME_SAFETY
            assert(left_click_screen == t->screens.active_key);
            assert(left_click_screen_generation == t->screens.generation(t->screens.active_key));
#endif
            *left_click_pin = p.pin;
        } else {
            ScreenSet *screens = &t->screens;
            Pin *tracked = screens->active->pages.trackPin(p.pin);
            if (tracked == nullptr) return false;
            left_click_pin = tracked;
            left_click_screen = screens->active_key;
            left_click_screen_generation = screens->generation(screens->active_key);
        }
        left_click_count = 1;
        left_click_behavior = p.behaviors[0];
        left_click_xpos = p.xpos;
        left_click_ypos = p.ypos;
        left_click_time = p.time;
        left_click_dragged = false;
        left_drag_autoscroll = Autoscroll::none;
        return true;
    }

    /* Wisp: false is error.PressRequiresReset. The errdefer that resets the
     * gesture on failure is run on every false return. */
    bool pressRepeat(Terminal *t, const Press &p) {
        /* If too much time has passed then we always reset. */
        if (!p.time.has) return pressRequiresReset(t);
        {
            if (!left_click_time.has) return pressRequiresReset(t);
            bool ok = false;
            const uint64_t since = timeSince(p.time, left_click_time, &ok);
            if (!ok) return pressRequiresReset(t);
            if (since > p.repeat_interval) return pressRequiresReset(t);
        }

        /* If the click is too far away from the initial click we can't
         * continue. */
        const double dx = p.xpos - left_click_xpos;
        const double dy = p.ypos - left_click_ypos;
        const double distance = sqrt(dx * dx + dy * dy);
        if (distance > p.max_distance) return pressRequiresReset(t);

        /* If our prior click was on another screen then free and reset.
         * "Another screen" doesn't just mean alt vs primary, it could mean an
         * alt screen that was recycled since we free tracked pins on
         * recycle. */
        ScreenSet *screens = &t->screens;
        if (left_click_screen != screens->active_key ||
            screens->generation(left_click_screen) != left_click_screen_generation) {
            return pressRequiresReset(t);
        }

        left_click_time = p.time;
        left_click_dragged = false;
        left_drag_autoscroll = Autoscroll::none;
        left_click_count = (uint8_t)(left_click_count + 1 < 3 ? left_click_count + 1 : 3);
        left_click_behavior = p.behaviors[left_click_count - 1];
        return true;
    }

    /* Wisp: upstream's `errdefer` on pressRepeat. */
    bool pressRequiresReset(Terminal *t) {
        left_click_count = 0;
        left_click_behavior = Behavior::cell;
        untrackPin(t);
        return false;
    }

    /* Wisp: `?u64`; *ok is false for upstream's null, a backwards timestamp. */
    static uint64_t timeSince(const Timestamp &time, const Timestamp &prev_time, bool *ok) {
        const int64_t delta = time.nanoseconds - prev_time.nanoseconds;
        if (delta < 0) {
            *ok = false;
            return 0;
        }
        *ok = true;
        return (uint64_t)delta;
    }

    /* Convert a caller-provided floating-point position to a pixel coordinate.
     * Negative and NaN values clamp to the origin, matching the drag behavior
     * outside the left edge of the surface. */
    static uint32_t pixelFromFloat(double value) {
        if (value != value /* NaN */ || value <= 0) return 0;

        /* @intFromFloat requires a value representable by the destination type.
         * Saturate first so positive infinity and oversized coordinates are
         * safe. */
        const double max = (double)UINT32_MAX;
        if (value >= max) return UINT32_MAX;
        return (uint32_t)value;
    }

    Maybe<Selection> pressSelection(Screen *screen, const Press &p) const {
        switch (left_click_behavior) {
        case Behavior::cell: return Maybe<Selection>();
        case Behavior::word:
            return screen->selectWord(p.pin, p.word_boundary_codepoints, p.word_boundary_len);
        case Behavior::line: {
            return screen->selectLine(Screen::SelectLine(p.pin));
        }
        case Behavior::output: return screen->selectOutput(p.pin);
        }
        return Maybe<Selection>();
    }

    void untrackPin(Terminal *t) {
        /* Can't untrack unless we have a pin. */
        Pin *pin = left_click_pin;
        if (pin == nullptr) return;
        left_click_pin = nullptr;

        /* If the generation changed our pin is already invalid. */
        ScreenSet *screens = &t->screens;
        if (screens->generation(left_click_screen) != left_click_screen_generation) return;

        /* If we can't get a screen then its already freed. */
        Screen *screen = screens->get(left_click_screen);
        if (screen == nullptr) return;
        screen->pages.untrackPin(pin);
    }
};

} /* namespace vt */
} /* namespace wisp */

#endif /* WISP_VT_SELECTION_GESTURE_HPP */
