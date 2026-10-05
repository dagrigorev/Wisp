/* Transliterated from the test blocks in Ghostty src/terminal/sgr.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * Upstream's tests: same inputs, same assertions, same names. These replace
 * the tests written for the earlier reimplementation of sgr.hpp.
 *
 * "sgr: Attribute C compat" only references the C ABI type, which is not
 * carried over, and has no counterpart here.
 */

#include "test_helpers.h"
#include "sgr.hpp"

using namespace wisp::terminal;
using sgr::Attribute;
using sgr::Parser;
using sgr::SepList;
typedef Attribute::Tag T;


static Attribute testParse(const uint16_t *params, size_t len) {
    Parser p(params, len);
    Attribute a;
    const bool ok = p.next(&a);
    (void)ok;
    return a;
}

static Attribute testParseColon(const uint16_t *params, size_t len) {
    Parser p(params, len);
    /* Mark all parameters except the last as having a colon after. */
    for (size_t i = 0; i + 1 < len; i++) p.params_sep.set(i);
    Attribute a;
    const bool ok = p.next(&a);
    (void)ok;
    return a;
}

#define PARSE(...) ([&] { static const uint16_t v_[] = { __VA_ARGS__ }; return testParse(v_, sizeof(v_) / sizeof(v_[0])); }())
#define PARSE_COLON(...) ([&] { static const uint16_t v_[] = { __VA_ARGS__ }; return testParseColon(v_, sizeof(v_) / sizeof(v_[0])); }())

static bool is(Parser &p, T tag) {
    Attribute a;
    return p.next(&a) && a.tag == tag;
}

static bool done(Parser &p) {
    Attribute a;
    return !p.next(&a);
}

static bool rgb(const Attribute &a, T tag, uint8_t r, uint8_t g, uint8_t b) {
    return a.tag == tag && a.rgb.r == r && a.rgb.g == g && a.rgb.b == b;
}

TEST(sgr, Parser) {
    ASSERT_TRUE(testParse(nullptr, 0).tag == T::unset);
    ASSERT_TRUE(PARSE(0).tag == T::unset);

    ASSERT_TRUE(rgb(PARSE(38, 2, 40, 44, 52), T::direct_color_fg, 40, 44, 52));

    ASSERT_TRUE(PARSE(38, 2, 44, 52).tag == T::unknown);

    ASSERT_TRUE(rgb(PARSE(48, 2, 40, 44, 52), T::direct_color_bg, 40, 44, 52));

    ASSERT_TRUE(PARSE(48, 2, 44, 52).tag == T::unknown);
}

TEST(sgr, Parser_multiple) {
    static const uint16_t params[] = { 0, 38, 2, 40, 44, 52 };
    Parser p(params, 6);
    ASSERT_TRUE(is(p, T::unset));
    ASSERT_TRUE(is(p, T::direct_color_fg));
    ASSERT_TRUE(done(p));
    ASSERT_TRUE(done(p));
}

TEST(sgr, unsupported_with_colon) {
    static const uint16_t params[] = { 0, 4, 1 };
    SepList list;
    list.set(0);
    Parser p(params, 3, list);
    ASSERT_TRUE(is(p, T::unknown));
    ASSERT_TRUE(is(p, T::bold));
    ASSERT_TRUE(done(p));
}

TEST(sgr, unsupported_with_multiple_colon) {
    static const uint16_t params[] = { 0, 4, 2, 1 };
    SepList list;
    list.set(0);
    list.set(1);
    Parser p(params, 4, list);
    ASSERT_TRUE(is(p, T::unknown));
    ASSERT_TRUE(is(p, T::bold));
    ASSERT_TRUE(done(p));
}

TEST(sgr, bold) {
    ASSERT_TRUE(PARSE(1).tag == T::bold);
    ASSERT_TRUE(PARSE(22).tag == T::reset_bold);
}

TEST(sgr, italic) {
    ASSERT_TRUE(PARSE(3).tag == T::italic);
    ASSERT_TRUE(PARSE(23).tag == T::reset_italic);
}

TEST(sgr, underline) {
    ASSERT_TRUE(PARSE(4).tag == T::underline);

    const Attribute v = PARSE(24);
    ASSERT_TRUE(v.tag == T::underline);
    ASSERT_TRUE(v.underline == Attribute::Underline::none);
}

TEST(sgr, underline_styles) {
    struct { uint16_t n; Attribute::Underline u; } cases[] = {
        { 2, Attribute::Underline::double_ },
        { 0, Attribute::Underline::none },
        { 1, Attribute::Underline::single },
        { 3, Attribute::Underline::curly },
        { 4, Attribute::Underline::dotted },
        { 5, Attribute::Underline::dashed },
    };
    for (size_t i = 0; i < 6; i++) {
        const uint16_t params[] = { 4, cases[i].n };
        const Attribute v = testParseColon(params, 2);
        ASSERT_TRUE(v.tag == T::underline);
        ASSERT_TRUE(v.underline == cases[i].u);
    }
}

TEST(sgr, underline_style_with_more) {
    static const uint16_t params[] = { 4, 2, 1 };
    SepList list;
    list.set(0);
    Parser p(params, 3, list);

    ASSERT_TRUE(is(p, T::underline));
    ASSERT_TRUE(is(p, T::bold));
    ASSERT_TRUE(done(p));
}

TEST(sgr, underline_style_with_too_many_colons) {
    static const uint16_t params[] = { 4, 2, 3, 1 };
    SepList list;
    list.set(0);
    list.set(1);
    Parser p(params, 4, list);

    ASSERT_TRUE(is(p, T::unknown));
    ASSERT_TRUE(is(p, T::bold));
    ASSERT_TRUE(done(p));
}

TEST(sgr, blink) {
    ASSERT_TRUE(PARSE(5).tag == T::blink);
    ASSERT_TRUE(PARSE(6).tag == T::blink);
    ASSERT_TRUE(PARSE(25).tag == T::reset_blink);
}

TEST(sgr, inverse) {
    ASSERT_TRUE(PARSE(7).tag == T::inverse);
    ASSERT_TRUE(PARSE(27).tag == T::reset_inverse);
}

TEST(sgr, strikethrough) {
    ASSERT_TRUE(PARSE(9).tag == T::strikethrough);
    ASSERT_TRUE(PARSE(29).tag == T::reset_strikethrough);
}

TEST(sgr, _8_color) {
    static const uint16_t params[] = { 31, 43, 90, 103 };
    Parser p(params, 4);
    Attribute v;

    ASSERT_TRUE(p.next(&v) && v.tag == T::fg_8 && v.name == Name::red);
    ASSERT_TRUE(p.next(&v) && v.tag == T::bg_8 && v.name == Name::yellow);
    ASSERT_TRUE(p.next(&v) && v.tag == T::bright_fg_8 && v.name == Name::bright_black);
    ASSERT_TRUE(p.next(&v) && v.tag == T::bright_bg_8 && v.name == Name::bright_yellow);
}

TEST(sgr, _256_color) {
    static const uint16_t params[] = { 38, 5, 161, 48, 5, 236 };
    Parser p(params, 6);
    ASSERT_TRUE(is(p, T::fg_256));
    ASSERT_TRUE(is(p, T::bg_256));
    ASSERT_TRUE(done(p));
}

TEST(sgr, _256_color_underline) {
    static const uint16_t params[] = { 58, 5, 9 };
    Parser p(params, 3);
    ASSERT_TRUE(is(p, T::underline_color_256));
    ASSERT_TRUE(done(p));
}

TEST(sgr, _24_bit_bg_color) {
    ASSERT_TRUE(rgb(PARSE_COLON(48, 2, 1, 2, 3), T::direct_color_bg, 1, 2, 3));
}

TEST(sgr, underline_color) {
    ASSERT_TRUE(rgb(PARSE_COLON(58, 2, 1, 2, 3), T::underline_color, 1, 2, 3));
    ASSERT_TRUE(rgb(PARSE_COLON(58, 2, 0, 1, 2, 3), T::underline_color, 1, 2, 3));
}

TEST(sgr, reset_underline_color) {
    static const uint16_t params[] = { 59 };
    Parser p(params, 1);
    ASSERT_TRUE(is(p, T::reset_underline_color));
}

TEST(sgr, invisible) {
    static const uint16_t params[] = { 8, 28 };
    Parser p(params, 2);
    ASSERT_TRUE(is(p, T::invisible));
    ASSERT_TRUE(is(p, T::reset_invisible));
}

TEST(sgr, underline_bg_and_fg) {
    static const uint16_t params[] = { 4, 38, 2, 255, 247, 219, 48, 2, 242, 93, 147, 4 };
    Parser p(params, 12);
    Attribute v;
    ASSERT_TRUE(p.next(&v) && v.tag == T::underline && v.underline == Attribute::Underline::single);
    ASSERT_TRUE(p.next(&v) && rgb(v, T::direct_color_fg, 255, 247, 219));
    ASSERT_TRUE(p.next(&v) && rgb(v, T::direct_color_bg, 242, 93, 147));
    ASSERT_TRUE(p.next(&v) && v.tag == T::underline && v.underline == Attribute::Underline::single);
}

TEST(sgr, direct_color_fg_missing_color) {
    /* This used to crash */
    static const uint16_t params[] = { 38, 5 };
    Parser p(params, 2);
    Attribute v;
    while (p.next(&v)) {}
}

TEST(sgr, direct_color_bg_missing_color) {
    /* This used to crash */
    static const uint16_t params[] = { 48, 5 };
    Parser p(params, 2);
    Attribute v;
    while (p.next(&v)) {}
}

TEST(sgr, direct_fg_bg_underline_ignore_optional_color_space) {
    /* These behaviors have been verified against xterm. */

    /* Colon version should skip the optional color space identifier */
    /* 3 8 : 2 : Pi : Pr : Pg : Pb */
    ASSERT_TRUE(rgb(PARSE_COLON(38, 2, 0, 1, 2, 3), T::direct_color_fg, 1, 2, 3));
    /* 4 8 : 2 : Pi : Pr : Pg : Pb */
    ASSERT_TRUE(rgb(PARSE_COLON(48, 2, 0, 1, 2, 3), T::direct_color_bg, 1, 2, 3));
    /* 5 8 : 2 : Pi : Pr : Pg : Pb */
    ASSERT_TRUE(rgb(PARSE_COLON(58, 2, 0, 1, 2, 3), T::underline_color, 1, 2, 3));

    /* Semicolon version should not parse optional color space identifier */
    /* 3 8 ; 2 ; Pr ; Pg ; Pb */
    ASSERT_TRUE(rgb(PARSE(38, 2, 0, 1, 2, 3), T::direct_color_fg, 0, 1, 2));
    /* 4 8 ; 2 ; Pr ; Pg ; Pb */
    ASSERT_TRUE(rgb(PARSE(48, 2, 0, 1, 2, 3), T::direct_color_bg, 0, 1, 2));
    /* 5 8 ; 2 ; Pr ; Pg ; Pb */
    ASSERT_TRUE(rgb(PARSE(58, 2, 0, 1, 2, 3), T::underline_color, 0, 1, 2));
}

TEST(sgr, direct_fg_colon_with_too_many_colons) {
    static const uint16_t params[] = { 38, 2, 0, 1, 2, 3, 4, 1 };
    SepList list;
    for (size_t i = 0; i < 6; i++) list.set(i);
    Parser p(params, 8, list);

    ASSERT_TRUE(is(p, T::unknown));
    ASSERT_TRUE(is(p, T::bold));
    ASSERT_TRUE(done(p));
}

TEST(sgr, direct_fg_colon_with_colorspace_and_extra_param) {
    static const uint16_t params[] = { 38, 2, 0, 1, 2, 3, 1 };
    SepList list;
    for (size_t i = 0; i < 5; i++) list.set(i);
    Parser p(params, 7, list);

    Attribute v;
    ASSERT_TRUE(p.next(&v) && rgb(v, T::direct_color_fg, 1, 2, 3));

    ASSERT_TRUE(is(p, T::bold));
    ASSERT_TRUE(done(p));
}

TEST(sgr, direct_fg_colon_no_colorspace_and_extra_param) {
    static const uint16_t params[] = { 38, 2, 1, 2, 3, 1 };
    SepList list;
    for (size_t i = 0; i < 4; i++) list.set(i);
    Parser p(params, 6, list);

    Attribute v;
    ASSERT_TRUE(p.next(&v) && rgb(v, T::direct_color_fg, 1, 2, 3));

    ASSERT_TRUE(is(p, T::bold));
    ASSERT_TRUE(done(p));
}

/* Kakoune sent this complex SGR sequence that caused invalid behavior. */
TEST(sgr, kakoune_input) {
    /* This used to crash */
    static const uint16_t params[] = { 0, 4, 3, 38, 2, 175, 175, 215, 58, 2, 0, 190, 80, 70 };
    SepList list;
    list.set(1);
    list.set(8);
    list.set(9);
    list.set(10);
    list.set(11);
    list.set(12);
    Parser p(params, 14, list);

    Attribute v;
    ASSERT_TRUE(p.next(&v) && v.tag == T::unset);
    ASSERT_TRUE(p.next(&v) && v.tag == T::underline && v.underline == Attribute::Underline::curly);
    ASSERT_TRUE(p.next(&v) && rgb(v, T::direct_color_fg, 175, 175, 215));
    ASSERT_TRUE(p.next(&v) && rgb(v, T::underline_color, 190, 80, 70));

    /* try testing.expect(p.next() == null); */
}

/* Discussion #5930, another input sent by kakoune */
TEST(sgr, kakoune_input_issue_underline_fg_and_bg) {
    /* echo -e "\033[4:3;38;2;51;51;51;48;2;170;170;170;58;2;255;97;136mset everything in one sequence, broken\033[m" */

    /* This used to crash */
    static const uint16_t params[] = { 4, 3, 38, 2, 51, 51, 51, 48, 2, 170, 170, 170, 58, 2, 255, 97, 136 };
    SepList list;
    list.set(0);
    Parser p(params, 17, list);

    Attribute v;
    ASSERT_TRUE(p.next(&v) && v.tag == T::underline && v.underline == Attribute::Underline::curly);
    ASSERT_TRUE(p.next(&v) && rgb(v, T::direct_color_fg, 51, 51, 51));
    ASSERT_TRUE(p.next(&v) && rgb(v, T::direct_color_bg, 170, 170, 170));
    ASSERT_TRUE(p.next(&v) && rgb(v, T::underline_color, 255, 97, 136));

    ASSERT_TRUE(done(p));
}

/* Fuzz crash: afl-out/stream/default/crashes/id:000021
 * Input "ESC [ 5 8 : 4 : m" produces params [58, 4] with colon
 * separator bits set at indices 0 and 1. The trailing colon causes
 * the second iteration to see param 4 (underline) with a colon,
 * triggering assert(slice.len >= 2) with slice.len == 1. */
TEST(sgr, underline_colon_with_trailing_separator_and_short_slice) {
    static const uint16_t params[] = { 58, 4 };
    SepList list;
    list.set(0);
    list.set(1);
    Parser p(params, 2, list);

    /* 58:4 is not a valid underline color (sub-param 4 is not 2 or 5),
     * so it falls through as unknown. */
    ASSERT_TRUE(is(p, T::unknown));

    /* Param 4 with a trailing colon but no sub-param is malformed,
     * so it also falls through as unknown rather than panicking. */
    ASSERT_TRUE(is(p, T::unknown));

    ASSERT_TRUE(done(p));
}
