/* Transliterated from the test blocks in Ghostty
 * src/terminal/SelectionGesture.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Wisp:
 *   `?Selection`                    Maybe<Selection> (.has / .value)
 *   `try gesture.press(t, p)`       gesture.press(&t, p, &oom)
 *   `std.Io.Timestamp`              SelectionGesture::Timestamp (nanoseconds)
 *   slices                          pointer plus length
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "test_helpers.h"
#include "../vt/selection_gesture.hpp"

using namespace wisp;

typedef vt::SelectionGesture SG;
typedef vt::PageList::Pin Pin;
typedef vt::Selection Selection;

static zigstd::Allocator talloc() { return zigstd::testing_allocator(); }

struct TermHolder {
    vt::Terminal t;
    bool ok;
    TermHolder(unsigned cols, unsigned rows) {
        vt::Terminal::Options o((vt::size::CellCountInt)cols, (vt::size::CellCountInt)rows);
        ok = vt::Terminal::init(talloc(), o, &t);
    }
    ~TermHolder() {
        if (ok) t.deinit(talloc());
    }
};

#define TERM(v, c, r)                                                                              \
    TermHolder v##_holder((c), (r));                                                               \
    ASSERT_TRUE(v##_holder.ok);                                                                    \
    vt::Terminal &v = v##_holder.t

struct ScreenHolder {
    vt::Screen s;
    bool ok;
    ScreenHolder(unsigned cols, unsigned rows) {
        const vt::Screen::Options o((vt::size::CellCountInt)cols, (vt::size::CellCountInt)rows,
                                    vt::Maybe<size_t>((size_t)0));
        ok = vt::Screen::init(talloc(), o, &s);
    }
    ~ScreenHolder() {
        if (ok) s.deinit();
    }
};

/* Wisp: upstream's test helpers, which build a Press/Drag with the fixed
 * geometry the tests share. */
static SG::Press testPress(vt::Terminal &t, uint16_t x, uint32_t y, SG::Timestamp time) {
    SG::Press p;
    p.time = time;
    const vt::Maybe<Pin> pin =
        t.screens.active->pages.pin(vt::point::Point::active((vt::size::CellCountInt)x, y));
    if (!pin.has) return p;
    p.pin = pin.value;
    p.xpos = (double)x;
    p.ypos = (double)y;
    p.max_distance = 1;
    p.repeat_interval = (uint64_t)-1;
    p.word_boundary_codepoints = nullptr;
    p.word_boundary_len = 0;
    return p;
}

static SG::Drag testDrag(vt::Terminal &t, uint16_t x, uint32_t y, double xpos, double ypos) {
    SG::Drag d;
    const vt::Maybe<Pin> pin =
        t.screens.active->pages.pin(vt::point::Point::active((vt::size::CellCountInt)x, y));
    if (!pin.has) return d;
    d.pin = pin.value;
    d.xpos = xpos;
    d.ypos = ypos;
    d.rectangle = false;
    d.word_boundary_codepoints = nullptr;
    d.word_boundary_len = 0;
    d.geometry.columns = 5;
    d.geometry.cell_width = 10;
    d.geometry.padding_left = 0;
    d.geometry.screen_height = 100;
    return d;
}

static SG::AutoscrollTick testAutoscrollTick(vt::point::Coordinate viewport, double xpos,
                                             double ypos) {
    SG::AutoscrollTick tick;
    tick.viewport = viewport;
    tick.xpos = xpos;
    tick.ypos = ypos;
    tick.rectangle = false;
    tick.word_boundary_codepoints = nullptr;
    tick.word_boundary_len = 0;
    tick.geometry.columns = 5;
    tick.geometry.cell_width = 10;
    tick.geometry.padding_left = 0;
    tick.geometry.screen_height = 100;
    return tick;
}

static Pin testPin(vt::Terminal &t, uint16_t x, uint32_t y) {
    const vt::Maybe<Pin> pin =
        t.screens.active->pages.pin(vt::point::Point::active((vt::size::CellCountInt)x, y));
    return pin.value;
}

/* Utility function for the unit tests for drag selection logic.
 *
 * Tests a click and drag on a 10x5 cell grid, x positions are given in
 * fractional cells, e.g. 3.1 would be 10% through the cell at x = 3.
 *
 * NOTE: The geometry tested with has 10px wide cells, meaning only one digit
 *       after the decimal place has any meaning, e.g. 3.14 is equal to 3.1.
 *
 * The provided start_x/y and end_x/y are the expected start and end points
 * of the resulting selection. */
static bool dragSelectionCase(double click_x, uint32_t click_y, double drag_x, uint32_t drag_y,
                              bool rect, vt::Maybe<Selection> *out, Pin *click_pin_out,
                              Pin *drag_pin_out, vt::Screen *screen) {
    /* Our screen size is 10x5 cells that are
     * 10x20 px, with 5px padding on all sides. */
    SG::Drag::Geometry geometry;
    geometry.columns = 10;
    geometry.cell_width = 10;
    geometry.padding_left = 5;
    geometry.screen_height = 110;

    const vt::Maybe<Pin> click_pin = screen->pages.pin(
        vt::point::Point::viewport((vt::size::CellCountInt)floor(click_x), click_y));
    const vt::Maybe<Pin> drag_pin = screen->pages.pin(
        vt::point::Point::viewport((vt::size::CellCountInt)floor(drag_x), drag_y));
    if (!click_pin.has || !drag_pin.has) return false;

    const double cell_width_f64 = (double)geometry.cell_width;
    const uint32_t click_x_pos =
        (uint32_t)floor(click_x * cell_width_f64) + geometry.padding_left;
    const uint32_t drag_x_pos = (uint32_t)floor(drag_x * cell_width_f64) + geometry.padding_left;

    *click_pin_out = click_pin.value;
    *drag_pin_out = drag_pin.value;
    *out = SG::dragSelection(click_pin.value, drag_pin.value, click_x_pos, drag_x_pos, rect,
                             geometry);
    return true;
}

#define testDragSelection(cx, cy, dx, dy, sx, sy, ex, ey, rect)                                    \
    do {                                                                                           \
        ScreenHolder sh(10, 5);                                                                    \
        ASSERT_TRUE(sh.ok);                                                                        \
        vt::Maybe<Selection> got;                                                                  \
        Pin cp, dp;                                                                                \
        ASSERT_TRUE(dragSelectionCase((cx), (cy), (dx), (dy), (rect), &got, &cp, &dp, &sh.s));     \
        const vt::Maybe<Pin> want_start =                                                          \
            sh.s.pages.pin(vt::point::Point::viewport((vt::size::CellCountInt)(sx), (sy)));        \
        const vt::Maybe<Pin> want_end =                                                            \
            sh.s.pages.pin(vt::point::Point::viewport((vt::size::CellCountInt)(ex), (ey)));        \
        ASSERT_TRUE(want_start.has && want_end.has);                                               \
        ASSERT_TRUE(got.has);                                                                      \
        ASSERT_TRUE(got.value.start().eql(want_start.value));                                      \
        ASSERT_TRUE(got.value.end().eql(want_end.value));                                          \
        ASSERT_TRUE(got.value.rectangle == (rect));                                                \
    } while (0)

#define testDragSelectionIsNull(cx, cy, dx, dy, rect)                                              \
    do {                                                                                           \
        ScreenHolder sh(10, 5);                                                                    \
        ASSERT_TRUE(sh.ok);                                                                        \
        vt::Maybe<Selection> got;                                                                  \
        Pin cp, dp;                                                                                \
        ASSERT_TRUE(dragSelectionCase((cx), (cy), (dx), (dy), (rect), &got, &cp, &dp, &sh.s));     \
        ASSERT_TRUE(!got.has);                                                                     \
    } while (0)

TEST(SelectionGesture, drag_selection_logic) {
    /* -- LTR */
    /* single cell selection */
    testDragSelection(3.0, 3, 3.9, 3, 3, 3, 3, 3, false);
    /* including click and drag pin cells */
    testDragSelection(3.0, 3, 5.9, 3, 3, 3, 5, 3, false);
    /* including click pin cell but not drag pin cell */
    testDragSelection(3.0, 3, 5.0, 3, 3, 3, 4, 3, false);
    /* including drag pin cell but not click pin cell */
    testDragSelection(3.9, 3, 5.9, 3, 4, 3, 5, 3, false);
    /* including neither click nor drag pin cells */
    testDragSelection(3.9, 3, 5.0, 3, 4, 3, 4, 3, false);
    /* empty selection (single cell on only left half) */
    testDragSelectionIsNull(3.0, 3, 3.1, 3, false);
    /* empty selection (single cell on only right half) */
    testDragSelectionIsNull(3.8, 3, 3.9, 3, false);
    /* empty selection (between two cells, not crossing threshold) */
    testDragSelectionIsNull(3.9, 3, 4.0, 3, false);

    /* -- RTL */
    /* single cell selection */
    testDragSelection(3.9, 3, 3.0, 3, 3, 3, 3, 3, false);
    /* including click and drag pin cells */
    testDragSelection(5.9, 3, 3.0, 3, 5, 3, 3, 3, false);
    /* including click pin cell but not drag pin cell */
    testDragSelection(5.9, 3, 3.9, 3, 5, 3, 4, 3, false);
    /* including drag pin cell but not click pin cell */
    testDragSelection(5.0, 3, 3.0, 3, 4, 3, 3, 3, false);
    /* including neither click nor drag pin cells */
    testDragSelection(5.0, 3, 3.9, 3, 4, 3, 4, 3, false);
    /* empty selection (single cell on only left half) */
    testDragSelectionIsNull(3.1, 3, 3.0, 3, false);
    /* empty selection (single cell on only right half) */
    testDragSelectionIsNull(3.9, 3, 3.8, 3, false);
    /* empty selection (between two cells, not crossing threshold) */
    testDragSelectionIsNull(4.0, 3, 3.9, 3, false);

    /* -- Wrapping */
    /* LTR, wrap excluded cells */
    testDragSelection(9.9, 2, 0.0, 4, 0, 3, 9, 3, false);
    /* RTL, wrap excluded cells */
    testDragSelection(0.0, 4, 9.9, 2, 9, 3, 0, 3, false);
}

TEST(SelectionGesture, rectangle_drag_selection_logic) {
    /* -- LTR */
    /* single column selection */
    testDragSelection(3.0, 2, 3.9, 4, 3, 2, 3, 4, true);
    /* including click and drag pin columns */
    testDragSelection(3.0, 2, 5.9, 4, 3, 2, 5, 4, true);
    /* including click pin column but not drag pin column */
    testDragSelection(3.0, 2, 5.0, 4, 3, 2, 4, 4, true);
    /* including drag pin column but not click pin column */
    testDragSelection(3.9, 2, 5.9, 4, 4, 2, 5, 4, true);
    /* including neither click nor drag pin columns */
    testDragSelection(3.9, 2, 5.0, 4, 4, 2, 4, 4, true);
    /* empty selection (single column on only left half) */
    testDragSelectionIsNull(3.0, 2, 3.1, 4, true);
    /* empty selection (single column on only right half) */
    testDragSelectionIsNull(3.8, 2, 3.9, 4, true);
    /* empty selection (between two columns, not crossing threshold) */
    testDragSelectionIsNull(3.9, 2, 4.0, 4, true);

    /* -- RTL */
    /* single column selection */
    testDragSelection(3.9, 2, 3.0, 4, 3, 2, 3, 4, true);
    /* including click and drag pin columns */
    testDragSelection(5.9, 2, 3.0, 4, 5, 2, 3, 4, true);
    /* including click pin column but not drag pin column */
    testDragSelection(5.9, 2, 3.9, 4, 5, 2, 4, 4, true);
    /* including drag pin column but not click pin column */
    testDragSelection(5.0, 2, 3.0, 4, 4, 2, 3, 4, true);
    /* including neither click nor drag pin columns */
    testDragSelection(5.0, 2, 3.9, 4, 4, 2, 4, 4, true);
    /* empty selection (single column on only left half) */
    testDragSelectionIsNull(3.1, 2, 3.0, 4, true);
    /* empty selection (single column on only right half) */
    testDragSelectionIsNull(3.9, 2, 3.8, 4, true);
    /* empty selection (between two columns, not crossing threshold) */
    testDragSelectionIsNull(4.0, 2, 3.9, 4, true);

    /* -- Wrapping */
    /* LTR, do not wrap */
    testDragSelection(9.9, 2, 0.0, 4, 9, 2, 0, 4, true);
    /* RTL, do not wrap */
    testDragSelection(0.0, 4, 9.9, 2, 0, 4, 9, 2, true);
}


/* Wisp: `std.Io.Timestamp.now(io, .awake)`. The tests only compare timestamps
 * with each other, so a fixed base plus explicit offsets is equivalent. */
static SG::Timestamp testNow() { return SG::Timestamp(1000000000); }

/* Wisp: `&.{ ' ' }` as a pointer plus length. */
static const uint32_t space_boundary[] = {(uint32_t)' '};

struct GestureHolder {
    SG g;
    vt::Terminal *t;
    explicit GestureHolder(vt::Terminal *term) : g(), t(term) {}
    ~GestureHolder() { g.deinit(t); }
};

#define GESTURE(v, term)                                                                           \
    GestureHolder v##_holder(&(term));                                                             \
    SG &v = v##_holder.g

static bool selEq(const vt::Maybe<Selection> &got, const Pin &start, const Pin &end, bool rect) {
    if (!got.has) return false;
    return got.value.start().eql(start) && got.value.end().eql(end) &&
           got.value.rectangle == rect;
}

TEST(SelectionGesture, press_records_initial_click) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    const SG::Timestamp time = testNow();
    bool oom = false;
    (void)gesture.press(&t, testPress(t, 1, 2, time), &oom);
    ASSERT_TRUE(!oom);

    ASSERT_TRUE(1 == gesture.left_click_count);
    ASSERT_TRUE(gesture.left_click_time.has);
    ASSERT_TRUE(time.nanoseconds == gesture.left_click_time.nanoseconds);
    ASSERT_TRUE(1.0 == gesture.left_click_xpos);
    ASSERT_TRUE(2.0 == gesture.left_click_ypos);
    ASSERT_TRUE(false == gesture.left_click_dragged);
}

TEST(SelectionGesture, press_returns_standard_click_selections) {
    TERM(t, 20, 5);
    ASSERT_TRUE(t.printString("alpha beta\none two"));

    GESTURE(gesture, t);

    const SG::Timestamp time = testNow();
    SG::Press event = testPress(t, 1, 0, time);
    event.word_boundary_codepoints = space_boundary;
    event.word_boundary_len = 1;

    bool oom = false;
    ASSERT_TRUE(!gesture.press(&t, event, &oom).has);
    ASSERT_TRUE(!oom);

    ASSERT_TRUE(selEq(gesture.press(&t, event, &oom), testPin(t, 0, 0), testPin(t, 4, 0), false));
    ASSERT_TRUE(selEq(gesture.press(&t, event, &oom), testPin(t, 0, 0), testPin(t, 9, 0), false));
}

TEST(SelectionGesture, press_behaviors_choose_press_and_drag_behavior) {
    TERM(t, 20, 5);
    ASSERT_TRUE(t.printString("alpha beta\none two\nthree four"));

    GESTURE(gesture, t);

    const SG::Timestamp time = testNow();
    static const SG::Behavior behaviors[3] = {SG::Behavior::cell, SG::Behavior::line,
                                              SG::Behavior::word};
    SG::Press event = testPress(t, 1, 0, time);
    event.behaviors = behaviors;
    event.word_boundary_codepoints = space_boundary;
    event.word_boundary_len = 1;

    bool oom = false;
    (void)gesture.press(&t, event, &oom);
    ASSERT_TRUE(SG::Behavior::cell == gesture.left_click_behavior);

    const vt::Maybe<Selection> double_click = gesture.press(&t, event, &oom);
    ASSERT_TRUE(SG::Behavior::line == gesture.left_click_behavior);
    ASSERT_TRUE(selEq(double_click, testPin(t, 0, 0), testPin(t, 9, 0), false));

    const vt::Maybe<Selection> line_drag = gesture.drag(&t, testDrag(t, 2, 2, 20, 50));
    ASSERT_TRUE(selEq(line_drag, testPin(t, 0, 0), testPin(t, 9, 2), false));
}

TEST(SelectionGesture, output_behavior_selects_and_drags_semantic_output) {
    TERM(t, 10, 6);

    vt::Screen *screen = t.screens.active;
    screen->cursorSetSemanticContent(vt::Screen::SemanticContentSet::makeOutput());
    ASSERT_TRUE(screen->testWriteString("out1\n"));
    screen->cursorSetSemanticContent(vt::Screen::SemanticContentSet::makePrompt(
            terminal::osc::semantic_prompt::PromptKind::initial));
    ASSERT_TRUE(screen->testWriteString("$ "));
    screen->cursorSetSemanticContent(vt::Screen::SemanticContentSet::makeInput(
            vt::Screen::SemanticContentSet::InputClear::clear_explicit));
    ASSERT_TRUE(screen->testWriteString("cmd\n"));
    screen->cursorSetSemanticContent(vt::Screen::SemanticContentSet::makeOutput());
    ASSERT_TRUE(screen->testWriteString("out2"));

    GESTURE(gesture, t);

    static const SG::Behavior behaviors[3] = {SG::Behavior::output, SG::Behavior::word,
                                              SG::Behavior::line};
    SG::Press event = testPress(t, 1, 0, testNow());
    event.behaviors = behaviors;

    bool oom = false;
    const vt::Maybe<Selection> press_selection = gesture.press(&t, event, &oom);
    ASSERT_TRUE(SG::Behavior::output == gesture.left_click_behavior);
    ASSERT_TRUE(selEq(press_selection, testPin(t, 0, 0), testPin(t, 3, 0), false));

    const vt::Maybe<Selection> output_drag = gesture.drag(&t, testDrag(t, 1, 2, 10, 50));
    ASSERT_TRUE(selEq(output_drag, testPin(t, 0, 0), testPin(t, 3, 2), false));
}

TEST(SelectionGesture, drag_returns_selection_and_records_autoscroll) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    SG::Press press_event = testPress(t, 1, 1, testNow());
    press_event.xpos = 10;
    bool oom = false;
    (void)gesture.press(&t, press_event, &oom);

    const vt::Maybe<Selection> sel = gesture.drag(&t, testDrag(t, 3, 1, 39, 50));
    ASSERT_TRUE(SG::Autoscroll::none == gesture.left_drag_autoscroll);
    ASSERT_TRUE(true == gesture.left_click_dragged);
    ASSERT_TRUE(selEq(sel, testPin(t, 1, 1), testPin(t, 3, 1), false));

    (void)gesture.drag(&t, testDrag(t, 3, 1, 39, 1));
    ASSERT_TRUE(SG::Autoscroll::up == gesture.left_drag_autoscroll);

    (void)gesture.drag(&t, testDrag(t, 3, 1, 39, 100));
    ASSERT_TRUE(SG::Autoscroll::down == gesture.left_drag_autoscroll);
}

TEST(SelectionGesture, drag_clamps_unrepresentable_positions) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    SG::Press press_event = testPress(t, 1, 1, testNow());
    press_event.xpos = 10;
    bool oom = false;
    (void)gesture.press(&t, press_event, &oom);

    const vt::Maybe<Selection> positive =
        gesture.drag(&t, testDrag(t, 1, 1, INFINITY, 50));
    ASSERT_TRUE(positive.has);
    ASSERT_TRUE(testPin(t, 1, 1).eql(positive.value.start()));
    ASSERT_TRUE(testPin(t, 1, 1).eql(positive.value.end()));

    ASSERT_TRUE(!gesture.drag(&t, testDrag(t, 1, 1, NAN, 50)).has);
}

TEST(SelectionGesture, drag_saturates_overflowing_geometry) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    bool oom = false;
    (void)gesture.press(&t, testPress(t, 1, 1, testNow()), &oom);
    SG::Drag drag_event = testDrag(t, 1, 1, 10, 50);
    drag_event.geometry.columns = UINT32_MAX;
    drag_event.geometry.cell_width = UINT32_MAX;
    ASSERT_TRUE(!gesture.drag(&t, drag_event).has);
}

TEST(SelectionGesture, drag_rejects_empty_geometry) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    bool oom = false;
    (void)gesture.press(&t, testPress(t, 1, 1, testNow()), &oom);
    SG::Drag drag_event = testDrag(t, 1, 1, 10, 50);
    drag_event.geometry.columns = 0;
    drag_event.geometry.cell_width = 0;
    ASSERT_TRUE(!gesture.drag(&t, drag_event).has);
}

TEST(SelectionGesture, release_clears_autoscroll_and_records_drag) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    bool oom = false;
    (void)gesture.press(&t, testPress(t, 1, 1, testNow()), &oom);
    ASSERT_TRUE(false == gesture.left_click_dragged);

    (void)gesture.drag(&t, testDrag(t, 1, 1, 10, 1));
    ASSERT_TRUE(SG::Autoscroll::up == gesture.left_drag_autoscroll);
    ASSERT_TRUE(false == gesture.left_click_dragged);

    SG::Release r;
    r.pin = vt::Maybe<Pin>(testPin(t, 2, 1));
    gesture.release(&t, r);
    ASSERT_TRUE(SG::Autoscroll::none == gesture.left_drag_autoscroll);
    ASSERT_TRUE(true == gesture.left_click_dragged);
}

TEST(SelectionGesture, same_cell_threshold_selection_records_drag) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    SG::Press press_event = testPress(t, 1, 1, testNow());
    press_event.xpos = 10;
    bool oom = false;
    (void)gesture.press(&t, press_event, &oom);
    ASSERT_TRUE(false == gesture.left_click_dragged);

    const vt::Maybe<Selection> sel = gesture.drag(&t, testDrag(t, 1, 1, 19, 50));
    ASSERT_TRUE(true == gesture.left_click_dragged);
    ASSERT_TRUE(selEq(sel, testPin(t, 1, 1), testPin(t, 1, 1), false));
}

TEST(SelectionGesture, drag_without_press_returns_null) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    ASSERT_TRUE(!gesture.drag(&t, testDrag(t, 1, 1, 10, 50)).has);
    ASSERT_TRUE(SG::Autoscroll::none == gesture.left_drag_autoscroll);
}

TEST(SelectionGesture, drag_autoscroll_edge_boundaries) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    SG::Press press_event = testPress(t, 1, 1, testNow());
    press_event.xpos = 10;
    bool oom = false;
    (void)gesture.press(&t, press_event, &oom);

    (void)gesture.drag(&t, testDrag(t, 2, 1, 20, 1));
    ASSERT_TRUE(SG::Autoscroll::up == gesture.left_drag_autoscroll);

    (void)gesture.drag(&t, testDrag(t, 2, 1, 20, 1.1));
    ASSERT_TRUE(SG::Autoscroll::none == gesture.left_drag_autoscroll);

    (void)gesture.drag(&t, testDrag(t, 2, 1, 20, 99));
    ASSERT_TRUE(SG::Autoscroll::none == gesture.left_drag_autoscroll);

    (void)gesture.drag(&t, testDrag(t, 2, 1, 20, 99.1));
    ASSERT_TRUE(SG::Autoscroll::down == gesture.left_drag_autoscroll);
}


/* Wisp: `t.screens.getInit(io, alloc, .alternate, .{ .cols = ..., .rows = ... })` */
static bool initAlternate(vt::Terminal &t) {
    const vt::Screen::Options o(t.cols, t.rows);
    return t.screens.getInit(talloc(), vt::ScreenSet::Key::alternate, o) != nullptr;
}

TEST(SelectionGesture, autoscroll_tick_scrolls_and_continues_drag) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    SG::Press press_event = testPress(t, 1, 1, testNow());
    press_event.xpos = 10;
    bool oom = false;
    (void)gesture.press(&t, press_event, &oom);

    (void)gesture.drag(&t, testDrag(t, 3, 1, 39, 100));
    ASSERT_TRUE(SG::Autoscroll::down == gesture.left_drag_autoscroll);

    const vt::Maybe<Selection> sel =
        gesture.autoscrollTick(&t, testAutoscrollTick(vt::point::Coordinate(3, 2), 39, 100));
    ASSERT_TRUE(SG::Autoscroll::down == gesture.left_drag_autoscroll);
    ASSERT_TRUE(true == gesture.left_click_dragged);
    ASSERT_TRUE(selEq(sel, testPin(t, 1, 1), testPin(t, 3, 2), false));
}

TEST(SelectionGesture, autoscroll_tick_resolves_drag_pin_after_scrolling) {
    vt::Terminal::Options opts((vt::size::CellCountInt)5, (vt::size::CellCountInt)3);
    opts.max_scrollback_bytes = vt::Maybe<size_t>((size_t)10);
    vt::Terminal t;
    ASSERT_TRUE(vt::Terminal::init(talloc(), opts, &t));

    ASSERT_TRUE(t.printString("1111\n2222\n3333\n4444\n5555"));
    t.scrollViewport(vt::Terminal::ScrollViewport::makeDelta(-2));

    {
        GESTURE(gesture, t);

        SG::Press press_event = testPress(t, 1, 1, testNow());
        press_event.xpos = 10;
        bool oom = false;
        (void)gesture.press(&t, press_event, &oom);

        (void)gesture.drag(&t, testDrag(t, 3, 2, 39, 100));
        ASSERT_TRUE(SG::Autoscroll::down == gesture.left_drag_autoscroll);

        const vt::point::Coordinate viewport(3, 2);
        const vt::Maybe<Pin> pre_scroll_pin =
            t.screens.active->pages.pin(vt::point::Point(vt::point::Tag::viewport, viewport));
        ASSERT_TRUE(pre_scroll_pin.has);
        const vt::Maybe<Selection> sel =
            gesture.autoscrollTick(&t, testAutoscrollTick(viewport, 39, 100));
        const vt::Maybe<Pin> post_scroll_pin =
            t.screens.active->pages.pin(vt::point::Point(vt::point::Tag::viewport, viewport));
        ASSERT_TRUE(post_scroll_pin.has);

        ASSERT_TRUE(!pre_scroll_pin.value.eql(post_scroll_pin.value));
        ASSERT_TRUE(selEq(sel, testPin(t, 1, 1), post_scroll_pin.value, false));
    }

    t.deinit(talloc());
}

TEST(SelectionGesture, autoscroll_tick_stops_with_invalidated_click) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    SG::Press press_event = testPress(t, 1, 1, testNow());
    press_event.xpos = 10;
    bool oom = false;
    (void)gesture.press(&t, press_event, &oom);

    (void)gesture.drag(&t, testDrag(t, 2, 1, 20, 1));
    ASSERT_TRUE(SG::Autoscroll::up == gesture.left_drag_autoscroll);

    ASSERT_TRUE(initAlternate(t));
    t.screens.switchTo(vt::ScreenSet::Key::alternate);

    ASSERT_TRUE(
        !gesture.autoscrollTick(&t, testAutoscrollTick(vt::point::Coordinate(2, 1), 20, 1)).has);
    ASSERT_TRUE(SG::Autoscroll::none == gesture.left_drag_autoscroll);
    ASSERT_TRUE(0 == gesture.left_click_count);
}

TEST(SelectionGesture, deep_press_selects_word_and_consumes_drag) {
    TERM(t, 20, 5);
    ASSERT_TRUE(t.printString("alpha beta"));

    GESTURE(gesture, t);

    bool oom = false;
    (void)gesture.press(&t, testPress(t, 1, 0, testNow()), &oom);
    (void)gesture.drag(&t, testDrag(t, 1, 0, 10, 1));
    ASSERT_TRUE(SG::Autoscroll::up == gesture.left_drag_autoscroll);

    SG::DeepPress dp;
    dp.word_boundary_codepoints = space_boundary;
    dp.word_boundary_len = 1;
    const vt::Maybe<Selection> sel = gesture.deepPress(&t, dp);

    ASSERT_TRUE(selEq(sel, testPin(t, 0, 0), testPin(t, 4, 0), false));
    ASSERT_TRUE(0 == gesture.left_click_count);
    ASSERT_TRUE(!gesture.left_click_time.has);
    ASSERT_TRUE(true == gesture.left_click_dragged);
    ASSERT_TRUE(SG::Autoscroll::none == gesture.left_drag_autoscroll);
    ASSERT_TRUE(gesture.left_click_pin == nullptr);

    ASSERT_TRUE(!gesture.drag(&t, testDrag(t, 7, 0, 70, 50)).has);
    SG::Release r;
    r.pin = vt::Maybe<Pin>(testPin(t, 7, 0));
    gesture.release(&t, r);
    ASSERT_TRUE(true == gesture.left_click_dragged);
}

TEST(SelectionGesture, drag_with_invalidated_click_returns_null) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    SG::Press press_event = testPress(t, 1, 1, testNow());
    press_event.xpos = 10;
    bool oom = false;
    (void)gesture.press(&t, press_event, &oom);

    (void)gesture.drag(&t, testDrag(t, 2, 1, 20, 1));
    ASSERT_TRUE(SG::Autoscroll::up == gesture.left_drag_autoscroll);

    ASSERT_TRUE(initAlternate(t));
    t.screens.switchTo(vt::ScreenSet::Key::alternate);

    ASSERT_TRUE(!gesture.drag(&t, testDrag(t, 2, 1, 20, 50)).has);
    ASSERT_TRUE(SG::Autoscroll::up == gesture.left_drag_autoscroll);
}

TEST(SelectionGesture, double_click_drag_selects_by_word) {
    TERM(t, 20, 5);
    ASSERT_TRUE(t.printString("alpha beta gamma"));

    GESTURE(gesture, t);

    const SG::Timestamp time = testNow();
    bool oom = false;
    (void)gesture.press(&t, testPress(t, 1, 0, time), &oom);
    (void)gesture.press(&t, testPress(t, 1, 0, time), &oom);

    SG::Drag drag_event = testDrag(t, 7, 0, 70, 50);
    drag_event.word_boundary_codepoints = space_boundary;
    drag_event.word_boundary_len = 1;
    const vt::Maybe<Selection> sel = gesture.drag(&t, drag_event);

    ASSERT_TRUE(selEq(sel, testPin(t, 0, 0), testPin(t, 9, 0), false));
}

TEST(SelectionGesture, double_click_drag_selects_by_word_backwards) {
    TERM(t, 20, 5);
    ASSERT_TRUE(t.printString("alpha beta gamma"));

    GESTURE(gesture, t);

    const SG::Timestamp time = testNow();
    bool oom = false;
    (void)gesture.press(&t, testPress(t, 7, 0, time), &oom);
    (void)gesture.press(&t, testPress(t, 7, 0, time), &oom);

    SG::Drag drag_event = testDrag(t, 1, 0, 10, 50);
    drag_event.word_boundary_codepoints = space_boundary;
    drag_event.word_boundary_len = 1;
    const vt::Maybe<Selection> sel = gesture.drag(&t, drag_event);

    ASSERT_TRUE(selEq(sel, testPin(t, 0, 0), testPin(t, 9, 0), false));
}

TEST(SelectionGesture, double_click_drag_on_empty_cell_selects_nearest_word) {
    TERM(t, 20, 5);
    ASSERT_TRUE(t.printString("alpha beta"));

    GESTURE(gesture, t);

    const SG::Timestamp time = testNow();
    bool oom = false;
    (void)gesture.press(&t, testPress(t, 1, 0, time), &oom);
    (void)gesture.press(&t, testPress(t, 1, 0, time), &oom);

    SG::Drag drag_event = testDrag(t, 15, 0, 150, 50);
    drag_event.word_boundary_codepoints = space_boundary;
    drag_event.word_boundary_len = 1;
    const vt::Maybe<Selection> sel = gesture.drag(&t, drag_event);

    ASSERT_TRUE(selEq(sel, testPin(t, 0, 0), testPin(t, 9, 0), false));
}

TEST(SelectionGesture, triple_click_drag_selects_by_line) {
    TERM(t, 20, 5);
    ASSERT_TRUE(t.printString("alpha beta\none two\nthree four"));

    GESTURE(gesture, t);

    const SG::Timestamp time = testNow();
    bool oom = false;
    (void)gesture.press(&t, testPress(t, 1, 0, time), &oom);
    (void)gesture.press(&t, testPress(t, 1, 0, time), &oom);
    (void)gesture.press(&t, testPress(t, 1, 0, time), &oom);

    const vt::Maybe<Selection> sel = gesture.drag(&t, testDrag(t, 2, 2, 20, 50));

    ASSERT_TRUE(selEq(sel, testPin(t, 0, 0), testPin(t, 9, 2), false));
}

TEST(SelectionGesture, triple_click_drag_selects_by_line_backwards) {
    TERM(t, 20, 5);
    ASSERT_TRUE(t.printString("alpha beta\none two\nthree four"));

    GESTURE(gesture, t);

    const SG::Timestamp time = testNow();
    bool oom = false;
    (void)gesture.press(&t, testPress(t, 2, 2, time), &oom);
    (void)gesture.press(&t, testPress(t, 2, 2, time), &oom);
    (void)gesture.press(&t, testPress(t, 2, 2, time), &oom);

    const vt::Maybe<Selection> sel = gesture.drag(&t, testDrag(t, 1, 0, 10, 50));

    ASSERT_TRUE(selEq(sel, testPin(t, 0, 0), testPin(t, 9, 2), false));
}

TEST(SelectionGesture, repeat_increments_click_count) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    const SG::Timestamp time = testNow();
    bool oom = false;
    (void)gesture.press(&t, testPress(t, 1, 1, time), &oom);
    (void)gesture.press(&t, testPress(t, 1, 1, time), &oom);

    ASSERT_TRUE(2 == gesture.left_click_count);
}

TEST(SelectionGesture, repeat_clamps_at_triple_click) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    const SG::Timestamp time = testNow();
    bool oom = false;
    for (int i = 0; i < 4; i++) (void)gesture.press(&t, testPress(t, 1, 1, time), &oom);

    ASSERT_TRUE(3 == gesture.left_click_count);
}

TEST(SelectionGesture, null_initial_time_stays_single_click) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    bool oom = false;
    (void)gesture.press(&t, testPress(t, 1, 1, SG::Timestamp()), &oom);
    (void)gesture.press(&t, testPress(t, 1, 1, testNow()), &oom);

    ASSERT_TRUE(1 == gesture.left_click_count);
    ASSERT_TRUE(gesture.left_click_time.has);
}

TEST(SelectionGesture, null_repeat_time_stays_single_click) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    bool oom = false;
    (void)gesture.press(&t, testPress(t, 1, 1, testNow()), &oom);
    (void)gesture.press(&t, testPress(t, 1, 1, SG::Timestamp()), &oom);

    ASSERT_TRUE(1 == gesture.left_click_count);
    ASSERT_TRUE(!gesture.left_click_time.has);
}

TEST(SelectionGesture, distant_press_resets_click_count) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    const SG::Timestamp time = testNow();
    bool oom = false;
    (void)gesture.press(&t, testPress(t, 1, 1, time), &oom);
    (void)gesture.press(&t, testPress(t, 4, 1, time), &oom);

    ASSERT_TRUE(1 == gesture.left_click_count);
    ASSERT_TRUE(4.0 == gesture.left_click_xpos);
}

TEST(SelectionGesture, expired_repeat_resets_click_count) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    SG::Press event = testPress(t, 1, 1, testNow());
    event.repeat_interval = 0;
    bool oom = false;
    (void)gesture.press(&t, event, &oom);

    /* Wisp: upstream sleeps 1ms and reads the clock again. The timestamps are
     * explicit here, so the later press is simply 1ms after the first. */
    event.time = SG::Timestamp(testNow().nanoseconds + 1000000);
    (void)gesture.press(&t, event, &oom);

    ASSERT_TRUE(1 == gesture.left_click_count);
}

TEST(SelectionGesture, backwards_repeat_time_resets_click_count) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    const SG::Timestamp now = testNow();
    const SG::Timestamp earlier = SG::Timestamp(now.nanoseconds + 1000000000);
    const SG::Timestamp later = SG::Timestamp(now.nanoseconds + 2000000000);
    bool oom = false;
    (void)gesture.press(&t, testPress(t, 1, 1, later), &oom);
    (void)gesture.press(&t, testPress(t, 1, 1, earlier), &oom);

    ASSERT_TRUE(1 == gesture.left_click_count);
    ASSERT_TRUE(gesture.left_click_time.has);
    ASSERT_TRUE(earlier.nanoseconds == gesture.left_click_time.nanoseconds);
}

TEST(SelectionGesture, screen_switch_resets_click_count) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    const SG::Timestamp time = testNow();
    const size_t primary_tracked = t.screens.active->pages.countTrackedPins();
    bool oom = false;
    (void)gesture.press(&t, testPress(t, 1, 1, time), &oom);

    ASSERT_TRUE(initAlternate(t));
    t.screens.switchTo(vt::ScreenSet::Key::alternate);
    (void)gesture.press(&t, testPress(t, 1, 1, time), &oom);

    ASSERT_TRUE(1 == gesture.left_click_count);
    ASSERT_TRUE(vt::ScreenSet::Key::alternate == gesture.left_click_screen);
    ASSERT_TRUE(primary_tracked ==
                t.screens.get(vt::ScreenSet::Key::primary)->pages.countTrackedPins());
}

TEST(SelectionGesture, removed_screen_resets_without_untracking_stale_pin) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    ASSERT_TRUE(initAlternate(t));
    t.screens.switchTo(vt::ScreenSet::Key::alternate);
    bool oom = false;
    (void)gesture.press(&t, testPress(t, 1, 1, testNow()), &oom);

    t.screens.switchTo(vt::ScreenSet::Key::primary);
    t.screens.remove(talloc(), vt::ScreenSet::Key::alternate);
    (void)gesture.press(&t, testPress(t, 1, 1, testNow()), &oom);

    ASSERT_TRUE(1 == gesture.left_click_count);
    ASSERT_TRUE(vt::ScreenSet::Key::primary == gesture.left_click_screen);
}

TEST(SelectionGesture, deinit_untracks_pin) {
    TERM(t, 5, 5);

    SG gesture;
    const size_t tracked = t.screens.active->pages.countTrackedPins();
    bool oom = false;
    (void)gesture.press(&t, testPress(t, 1, 1, testNow()), &oom);
    ASSERT_TRUE(tracked + 1 == t.screens.active->pages.countTrackedPins());

    gesture.deinit(&t);
    ASSERT_TRUE(tracked == t.screens.active->pages.countTrackedPins());
}


TEST(SelectionGesture, release_with_invalidated_click_records_drag) {
    TERM(t, 5, 5);
    GESTURE(gesture, t);

    bool oom = false;
    (void)gesture.press(&t, testPress(t, 1, 1, testNow()), &oom);
    ASSERT_TRUE(false == gesture.left_click_dragged);

    ASSERT_TRUE(initAlternate(t));
    t.screens.switchTo(vt::ScreenSet::Key::alternate);

    SG::Release r;
    r.pin = vt::Maybe<Pin>(testPin(t, 1, 1));
    gesture.release(&t, r);
    ASSERT_TRUE(true == gesture.left_click_dragged);
    ASSERT_TRUE(SG::Autoscroll::none == gesture.left_drag_autoscroll);
}

TEST(SelectionGesture, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
