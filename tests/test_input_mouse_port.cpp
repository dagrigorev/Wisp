/* Transliterated from the test blocks in Ghostty src/input/mouse.zig and
 * src/input/mouse_encode.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License - see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names.
 *
 * Wisp: mouse.zig's unnamed `test {}` inside ScrollMods is named
 * ScrollMods_layout. `inline for` over action/button lists is a plain loop.
 */

#include <stdio.h>

#include <string>

#include "test_helpers.h"
#include "../src/input/mouse.hpp"
#include "../src/input/mouse_encode.hpp"
/* Wisp: KeymapNoop.zig has no tests; including it here compiles it. */
#include "../src/input/keymap_noop.hpp"

using namespace wisp;
using namespace wisp::input;
using namespace wisp::input::mouse_encode;

typedef mouse::Action MAction;
typedef mouse::Button Button;

static Size testSize() {
    Size s;
    s.screen = {1000, 1000};
    s.cell = {1, 1};
    s.padding = ::wisp::renderer::size::Padding();
    return s;
}

static Event ev(MAction action, bool has_button, Button button, float x = 0, float y = 0) {
    Event e;
    e.action = action;
    e.has_button = has_button;
    e.button = button;
    e.pos.x = x;
    e.pos.y = y;
    return e;
}

static Options opts(MouseEvent event, MouseFormat format, const Size &size, LastCell *last) {
    Options o;
    o.event = event;
    o.format = format;
    o.size = size;
    o.last_cell = last;
    return o;
}

static Options reportOpts(MouseEvent event, const Size &size) {
    Options o;
    o.event = event;
    o.size = size;
    return o;
}

static Mods allMods() {
    Mods m;
    m.shift = true;
    m.alt = true;
    m.ctrl = true;
    return m;
}

/* ---- mouse.zig ---- */

TEST(input_mouse, ScrollMods_layout) {
    ASSERT_EQ(0, (int)mouse::ScrollMods().int_());
    mouse::ScrollMods p;
    p.precision = true;
    ASSERT_EQ(1, (int)p.int_());
}

/* ---- mouse_encode.zig ---- */

TEST(input_mouse_encode, shouldReport_none_mode_never_reports) {
    const Size size = testSize();
    for (MAction action : {MAction::press, MAction::release, MAction::motion}) {
        ASSERT_FALSE(shouldReport(ev(action, true, Button::left), reportOpts(MouseEvent::none, size)));
    }
}

TEST(input_mouse_encode, shouldReport_x10_reports_only_left_middle_right_press) {
    const Size size = testSize();
    /* Left, middle, right presses should report. */
    for (Button btn : {Button::left, Button::middle, Button::right}) {
        ASSERT_TRUE(shouldReport(ev(MAction::press, true, btn), reportOpts(MouseEvent::x10, size)));
    }

    /* Release is not reported. */
    ASSERT_FALSE(shouldReport(ev(MAction::release, true, Button::left), reportOpts(MouseEvent::x10, size)));

    /* Motion is not reported. */
    ASSERT_FALSE(shouldReport(ev(MAction::motion, true, Button::left), reportOpts(MouseEvent::x10, size)));

    /* Other buttons are not reported. */
    ASSERT_FALSE(shouldReport(ev(MAction::press, true, Button::four), reportOpts(MouseEvent::x10, size)));

    /* Null button is not reported. */
    ASSERT_FALSE(shouldReport(ev(MAction::press, false, Button::unknown), reportOpts(MouseEvent::x10, size)));
}

TEST(input_mouse_encode, shouldReport_normal_reports_press_and_release_but_not_motion) {
    const Size size = testSize();
    ASSERT_TRUE(shouldReport(ev(MAction::press, true, Button::left), reportOpts(MouseEvent::normal, size)));
    ASSERT_TRUE(shouldReport(ev(MAction::release, true, Button::left), reportOpts(MouseEvent::normal, size)));
    ASSERT_FALSE(shouldReport(ev(MAction::motion, true, Button::left), reportOpts(MouseEvent::normal, size)));
}

TEST(input_mouse_encode, shouldReport_button_mode_requires_a_button) {
    const Size size = testSize();
    /* With a button, all actions report. */
    for (MAction action : {MAction::press, MAction::release, MAction::motion}) {
        ASSERT_TRUE(shouldReport(ev(action, true, Button::left), reportOpts(MouseEvent::button, size)));
    }

    /* Without a button (null), nothing reports. */
    for (MAction action : {MAction::press, MAction::release, MAction::motion}) {
        ASSERT_FALSE(shouldReport(ev(action, false, Button::unknown), reportOpts(MouseEvent::button, size)));
    }
}

TEST(input_mouse_encode, shouldReport_any_mode_reports_everything) {
    const Size size = testSize();
    for (MAction action : {MAction::press, MAction::release, MAction::motion}) {
        ASSERT_TRUE(shouldReport(ev(action, true, Button::left), reportOpts(MouseEvent::any, size)));
    }

    /* Even null button + motion reports. */
    ASSERT_TRUE(shouldReport(ev(MAction::motion, false, Button::unknown), reportOpts(MouseEvent::any, size)));
}

TEST(input_mouse_encode, x10_press_left) {
    std::string writer;
    LastCell last;
    Event e = ev(MAction::press, true, Button::left, 0, 0);
    e.mods = allMods();
    encode(&writer, e, opts(MouseEvent::x10, MouseFormat::x10, testSize(), &last));

    ASSERT_TRUE(std::string("\x1b[M\x20\x21\x21") == writer);
}

TEST(input_mouse_encode, x10_ignores_release) {
    std::string writer;
    LastCell last;
    encode(&writer, ev(MAction::release, true, Button::left), opts(MouseEvent::x10, MouseFormat::x10, testSize(), &last));

    ASSERT_EQ(0L, (long)writer.size());
}

TEST(input_mouse_encode, normal_ignores_motion) {
    std::string writer;
    LastCell last;
    encode(&writer, ev(MAction::motion, true, Button::left), opts(MouseEvent::normal, MouseFormat::sgr, testSize(), &last));

    ASSERT_EQ(0L, (long)writer.size());
}

TEST(input_mouse_encode, button_mode_requires_button) {
    std::string writer;
    LastCell last;
    encode(&writer, ev(MAction::motion, false, Button::unknown), opts(MouseEvent::button, MouseFormat::sgr, testSize(), &last));

    ASSERT_EQ(0L, (long)writer.size());
}

TEST(input_mouse_encode, sgr_release_keeps_button_identity) {
    std::string writer;
    LastCell last;
    encode(&writer, ev(MAction::release, true, Button::right, 4, 5), opts(MouseEvent::any, MouseFormat::sgr, testSize(), &last));

    ASSERT_TRUE(std::string("\x1b[<2;5;6m") == writer);
}

TEST(input_mouse_encode, sgr_motion_with_no_button) {
    std::string writer;
    LastCell last;
    encode(&writer, ev(MAction::motion, false, Button::unknown, 1, 2), opts(MouseEvent::any, MouseFormat::sgr, testSize(), &last));

    ASSERT_TRUE(std::string("\x1b[<35;2;3M") == writer);
}

TEST(input_mouse_encode, urxvt_with_modifiers) {
    std::string writer;
    LastCell last;
    Event e = ev(MAction::press, true, Button::left, 2, 3);
    e.mods = allMods();
    encode(&writer, e, opts(MouseEvent::any, MouseFormat::urxvt, testSize(), &last));

    ASSERT_TRUE(std::string("\x1b[60;3;4M") == writer);
}

TEST(input_mouse_encode, utf8_encodes_large_coordinates) {
    std::string writer;
    LastCell last;
    encode(&writer, ev(MAction::press, true, Button::left, 300, 400), opts(MouseEvent::any, MouseFormat::utf8, testSize(), &last));

    const std::string &out = writer;
    ASSERT_TRUE(out.substr(0, 4) == std::string("\x1b[M\x20"));

    zigstd::Utf8View view;
    ASSERT_TRUE(zigstd::Utf8View::init(out.data() + 4, out.size() - 4, &view));
    uint32_t cp = 0;
    ASSERT_TRUE(view.nextCodepoint(&cp));
    ASSERT_EQ(333L, (long)cp);
    ASSERT_TRUE(view.nextCodepoint(&cp));
    ASSERT_EQ(433L, (long)cp);
    ASSERT_FALSE(view.nextCodepoint(&cp));
}

TEST(input_mouse_encode, x10_coordinate_limit) {
    std::string writer;
    LastCell last;
    encode(&writer, ev(MAction::press, true, Button::left, 223, 0), opts(MouseEvent::x10, MouseFormat::x10, testSize(), &last));

    ASSERT_EQ(0L, (long)writer.size());
}

TEST(input_mouse_encode, sgr_wheel_button_mappings) {
    struct Case {
        Button button;
        uint8_t code;
    };

    for (const Case &c : {Case{Button::four, 64}, Case{Button::five, 65}, Case{Button::six, 66},
                          Case{Button::seven, 67}}) {
        std::string writer;
        LastCell last;
        encode(&writer, ev(MAction::press, true, c.button, 0, 0), opts(MouseEvent::any, MouseFormat::sgr, testSize(), &last));

        char want[32];
        snprintf(want, sizeof want, "\x1b[<%u;1;1M", (unsigned)c.code);
        ASSERT_TRUE(std::string(want) == writer);
    }
}

TEST(input_mouse_encode, urxvt_release_uses_legacy_button_3_encoding) {
    std::string writer;
    LastCell last;
    encode(&writer, ev(MAction::release, true, Button::right, 2, 3), opts(MouseEvent::any, MouseFormat::urxvt, testSize(), &last));

    ASSERT_TRUE(std::string("\x1b[35;3;4M") == writer);
}

TEST(input_mouse_encode, unsupported_button_is_ignored) {
    std::string writer;
    LastCell last;
    encode(&writer, ev(MAction::press, true, Button::ten, 1, 1), opts(MouseEvent::any, MouseFormat::sgr, testSize(), &last));

    ASSERT_EQ(0L, (long)writer.size());
}

TEST(input_mouse_encode, sgr_pixels_uses_terminal_space_cursor_coordinates) {
    std::string writer;
    LastCell last;
    encode(&writer, ev(MAction::press, true, Button::left, 10, 20), opts(MouseEvent::any, MouseFormat::sgr_pixels, testSize(), &last));

    ASSERT_TRUE(std::string("\x1b[<0;10;20M") == writer);
}

TEST(input_mouse_encode, sgr_pixels_release_keeps_button_identity) {
    std::string writer;
    LastCell last;
    encode(&writer, ev(MAction::release, true, Button::right, 10, 20), opts(MouseEvent::any, MouseFormat::sgr_pixels, testSize(), &last));

    ASSERT_TRUE(std::string("\x1b[<2;10;20m") == writer);
}

TEST(input_mouse_encode, position_exactly_at_viewport_boundary_is_encoded_in_final_cell) {
    Size size;
    size.screen = {10, 10};
    size.cell = {2, 2};
    size.padding = ::wisp::renderer::size::Padding();

    std::string writer;
    LastCell last;
    encode(&writer, ev(MAction::press, true, Button::left, 10, 10), opts(MouseEvent::any, MouseFormat::sgr, size, &last));

    ASSERT_TRUE(std::string("\x1b[<0;5;5M") == writer);
}

TEST(input_mouse_encode, outside_viewport_motion_with_no_pressed_button_is_ignored) {
    std::string writer;
    LastCell last;
    Options o = opts(MouseEvent::any, MouseFormat::sgr, testSize(), &last);
    o.any_button_pressed = false;
    encode(&writer, ev(MAction::motion, true, Button::left, -1, -1), o);

    ASSERT_EQ(0L, (long)writer.size());
}

TEST(input_mouse_encode, outside_viewport_motion_with_pressed_button_is_reported) {
    std::string writer;
    LastCell last;
    Options o = opts(MouseEvent::any, MouseFormat::sgr, testSize(), &last);
    o.any_button_pressed = true;
    encode(&writer, ev(MAction::motion, true, Button::left, -1, -1), o);

    ASSERT_TRUE(std::string("\x1b[<32;1;1M") == writer);
}

TEST(input_mouse_encode, motion_is_deduped_by_last_cell_except_sgr_pixels) {
    LastCell last;

    {
        std::string writer;
        encode(&writer, ev(MAction::motion, true, Button::left, 5, 6), opts(MouseEvent::any, MouseFormat::sgr, testSize(), &last));
        ASSERT_TRUE(writer.size() > 0);
    }

    {
        std::string writer;
        encode(&writer, ev(MAction::motion, true, Button::left, 5, 6), opts(MouseEvent::any, MouseFormat::sgr, testSize(), &last));
        ASSERT_EQ(0L, (long)writer.size());
    }

    {
        std::string writer;
        encode(&writer, ev(MAction::motion, true, Button::left, 5, 6), opts(MouseEvent::any, MouseFormat::sgr_pixels, testSize(), &last));
        ASSERT_TRUE(writer.size() > 0);
    }
}
