/* Transliterated from the test blocks in Ghostty src/terminal/mouse.zig,
 * device_status.zig, device_attributes.zig and kitty/key.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names. ansi.zig and
 * csi.zig have no tests.
 */

#include "test_helpers.h"
#include "ansi.hpp"
#include "mouse.hpp"
#include "device_status.hpp"
#include "device_attributes.hpp"
#include "kitty/key.hpp"

using namespace wisp::terminal;

/* mouse.zig */

TEST(mouse, cursor_shape_from_string) {
    mouse::Shape s;
    ASSERT_TRUE(mouse::Shape_fromString("default", &s) && s == mouse::Shape::default_);
}

/* device_status.zig */

TEST(device_status, Tag_order) {
    ASSERT_TRUE(device_status::Tag::make(1, false) == 1);
}

TEST(device_status, encode_color_scheme_report_dark) {
    ASSERT_TRUE(device_status::max_color_scheme_report_encode_size == 9);

    std::string w;
    device_status::encodeColorSchemeReport(&w, device_status::ColorScheme::dark);
    ASSERT_TRUE(w == "\x1B[?997;1n");
}

TEST(device_status, encode_color_scheme_report_light) {
    std::string w;
    device_status::encodeColorSchemeReport(&w, device_status::ColorScheme::light);
    ASSERT_TRUE(w == "\x1B[?997;2n");
}

TEST(device_status, encode_visibility_report) {
    std::string w;
    device_status::encodeVisibilityReport(&w, device_status::Visibility::potentially_visible);
    ASSERT_TRUE(w == "\x1B[?999;1n");

    w.clear();
    device_status::encodeVisibilityReport(&w, device_status::Visibility::not_visible);
    ASSERT_TRUE(w == "\x1B[?999;2n");
}

/* device_attributes.zig */

using namespace wisp::terminal::device_attributes;

TEST(device_attributes, primary_default) {
    std::string w;
    Primary().encode(&w);
    ASSERT_TRUE(w == "\x1b[?62;22c");
}

TEST(device_attributes, primary_with_clipboard) {
    std::string w;
    Primary p;
    const Primary::Feature f[] = { Primary::Feature::ansi_color, Primary::Feature::clipboard };
    p.features = f;
    p.features_len = 2;
    p.encode(&w);
    ASSERT_TRUE(w == "\x1b[?62;22;52c");
}

TEST(device_attributes, primary_with_multiple_features) {
    std::string w;
    Primary p;
    p.conformance_level = ConformanceLevel::vt420;
    const Primary::Feature f[] = {
        Primary::Feature::columns_132, Primary::Feature::selective_erase,
        Primary::Feature::ansi_color,
    };
    p.features = f;
    p.features_len = 3;
    p.encode(&w);
    ASSERT_TRUE(w == "\x1b[?64;1;6;22c");
}

TEST(device_attributes, primary_no_features) {
    std::string w;
    Primary p;
    p.conformance_level = ConformanceLevel::vt100;
    p.features = nullptr;
    p.features_len = 0;
    p.encode(&w);
    ASSERT_TRUE(w == "\x1b[?1c");
}

TEST(device_attributes, secondary_default) {
    std::string w;
    Secondary().encode(&w);
    ASSERT_TRUE(w == "\x1b[>1;0;0c");
}

TEST(device_attributes, tertiary_default) {
    std::string w;
    Tertiary().encode(&w);
    ASSERT_TRUE(w == "\x1bP!|00000000\x1b\\");
}

TEST(device_attributes, tertiary_custom_unit_id) {
    std::string w;
    Tertiary t;
    t.unit_id = 0xAABBCCDD;
    t.encode(&w);
    ASSERT_TRUE(w == "\x1bP!|AABBCCDD\x1b\\");
}

/* kitty/key.zig */

using kitty::KeyFlags;
using kitty::KeyFlagStack;
using kitty::KeySetMode;

static KeyFlags flags(bool disambiguate, bool report_events) {
    KeyFlags f;
    f.disambiguate = disambiguate;
    f.report_events = report_events;
    return f;
}

/* Make sure we the overflow works as expected */
TEST(kitty_key, FlagStack_overflow) {
    KeyFlagStack stack;
    stack.idx = KeyFlagStack::len - 1;
    stack.idx = (uint8_t)((stack.idx + 1) & 7);
    ASSERT_TRUE(stack.idx == 0);

    stack.idx = 0;
    stack.idx = (uint8_t)((stack.idx - 1) & 7);
    ASSERT_TRUE(stack.idx == KeyFlagStack::len - 1);
}

/* Its easy to get packed struct ordering wrong so this test checks. */
TEST(kitty_key, Flags_order) {
    ASSERT_TRUE(flags(true, false).int_() == 0x1);
    ASSERT_TRUE(flags(false, true).int_() == 0x2);
}

TEST(kitty_key, FlagStack_push_pop) {
    KeyFlagStack stack;
    stack.push(flags(true, false));
    ASSERT_TRUE(stack.current().eql(flags(true, false)));

    stack.pop(1);
    ASSERT_TRUE(stack.current().eql(KeyFlags()));
}

TEST(kitty_key, FlagStack_pop_big_number) {
    KeyFlagStack stack;
    stack.pop(100);
    ASSERT_TRUE(stack.current().eql(KeyFlags()));
}

TEST(kitty_key, FlagStack_set) {
    KeyFlagStack stack;
    stack.set(KeySetMode::set, flags(true, false));
    ASSERT_TRUE(stack.current().eql(flags(true, false)));

    stack.set(KeySetMode::or_, flags(false, true));
    ASSERT_TRUE(stack.current().eql(flags(true, true)));

    stack.set(KeySetMode::not_, flags(false, true));
    ASSERT_TRUE(stack.current().eql(flags(true, false)));
}
