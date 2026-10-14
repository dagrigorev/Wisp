/* Tests for src/terminal/sixel.hpp.
 *
 * Wisp-specific: there is no upstream counterpart. Ghostty does not implement
 * Sixel at all (its only mention is `sixel = 4` in device_attributes.zig), so
 * there is no upstream test suite to port. These tests are the format's own
 * constructs written down as assertions: the examples and rules in DEC STD
 * 070 and the VT330/VT340 Programmer Reference Manual, chapter 14.
 *
 * Everything asserts on actual pixel values at known coordinates. "It decoded
 * without error" proves nothing here -- the easiest bug in a sixel decoder is
 * a vertically mirrored band, which decodes perfectly happily.
 *
 * The last group is hostile input: huge repeats, huge raster attributes,
 * out-of-range registers, truncation, parameters that would overflow a u32,
 * and random bytes. None of them may crash, allocate without bound or write
 * out of bounds; the leak check at the end covers the allocation side.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "test_helpers.h"
#include "../terminal/sixel.hpp"

using namespace wisp;
using wisp::terminal::sixel::BackgroundSelect;
using wisp::terminal::sixel::Error;
using wisp::terminal::sixel::Image;
using wisp::terminal::sixel::Options;

static zigstd::Allocator talloc() { return zigstd::testing_allocator(); }

struct ImageGuard {
    Image img;
    ImageGuard() {
        img.pixels = nullptr;
        img.width = 0;
        img.height = 0;
    }
    ~ImageGuard() { img.deinit(talloc()); }
};

static Options optsPaint() { return Options::defaults(); }

static Options optsTransparent() {
    Options o = Options::defaults();
    o.background_select = BackgroundSelect::transparent;
    return o;
}

/* Decode a NUL-terminated body. Bodies here never contain an embedded NUL;
 * the random-byte test builds its own buffer and passes an explicit length. */
static Error dec(ImageGuard *g, const char *body, const Options &o) {
    return wisp::terminal::sixel::decode(talloc(), body, strlen(body), o, &g->img);
}

/* The pixel at (x, y) as 0xRRGGBBAA. Out-of-range reads return 0xDEADBEEF so
 * a wrong coordinate fails loudly instead of reading the neighbour. */
static uint32_t px(const Image &img, size_t x, size_t y) {
    if (img.pixels == nullptr || x >= img.width || y >= img.height) return 0xDEADBEEFu;
    const uint8_t *p = img.pixels + (y * img.width + x) * 4;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

/* The default palette after the 0..100 to 0..255 scaling, registers 0..15.
 * Spelled out as final RGBA so a change to either the table or the rounding
 * shows up here. */
static const uint32_t expected_default_palette[16] = {
    0x000000FFu, /*  0 black        */
    0x3333CCFFu, /*  1 blue         */
    0xCC2121FFu, /*  2 red          */
    0x33CC33FFu, /*  3 green        */
    0xCC33CCFFu, /*  4 magenta      */
    0x33CCCCFFu, /*  5 cyan         */
    0xCCCC33FFu, /*  6 yellow       */
    0x878787FFu, /*  7 gray 50%     */
    0x424242FFu, /*  8 gray 25%     */
    0x545499FFu, /*  9 blue 50%     */
    0x994242FFu, /* 10 red 50%      */
    0x549954FFu, /* 11 green 50%    */
    0x995499FFu, /* 12 magenta 50%  */
    0x549999FFu, /* 13 cyan 50%     */
    0x999954FFu, /* 14 yellow 50%   */
    0xCCCCCCFFu, /* 15 gray 75%     */
};

static const uint32_t blue = 0x3333CCFFu;  /* register 1 */
static const uint32_t red = 0xCC2121FFu;   /* register 2 */
static const uint32_t green = 0x33CC33FFu; /* register 3 */
static const uint32_t bg = 0x000000FFu;    /* Options::defaults().background */

/* ── Data bytes and bit order ──────────────────────────────────────────── */

/* '@' is 0x40, so value 1, so bit 0 only: the TOP pixel of the band. This is
 * the single easiest thing in the format to get upside down. */
TEST(sixel, single_sixel_bit0_is_top) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "#1@", optsPaint()));
    ASSERT_TRUE(1 == g.img.width);
    /* Zero bits paint the background, so all six rows of the band exist. */
    ASSERT_TRUE(6 == g.img.height);
    ASSERT_TRUE(blue == px(g.img, 0, 0));
    ASSERT_TRUE(bg == px(g.img, 0, 1));
    ASSERT_TRUE(bg == px(g.img, 0, 5));
}

/* 0x3F + 32 = 0x5F is bit 5 only: the BOTTOM pixel. */
TEST(sixel, bit5_is_bottom) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "#1_", optsPaint()));
    ASSERT_TRUE(1 == g.img.width);
    ASSERT_TRUE(6 == g.img.height);
    ASSERT_TRUE(bg == px(g.img, 0, 0));
    ASSERT_TRUE(bg == px(g.img, 0, 4));
    ASSERT_TRUE(blue == px(g.img, 0, 5));
}

/* '~' is 0x7E, value 0x3F: every bit set. */
TEST(sixel, all_bits_set) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "#2~", optsPaint()));
    ASSERT_TRUE(1 == g.img.width);
    ASSERT_TRUE(6 == g.img.height);
    for (size_t y = 0; y < 6; y++) ASSERT_TRUE(red == px(g.img, 0, y));
}

/* '?' is 0x3F, value 0: no bits. With P2 = 1 nothing is touched at all, so
 * nothing was drawn and the image is empty. */
TEST(sixel, zero_byte_transparent_draws_nothing) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "#1?", optsTransparent()));
    ASSERT_TRUE(0 == g.img.width);
    ASSERT_TRUE(0 == g.img.height);
}

/* With P2 = 0 the same byte paints a whole column of background. */
TEST(sixel, zero_byte_paints_background) {
    ImageGuard g;
    Options o = optsPaint();
    o.background = 0x102030FFu;
    ASSERT_TRUE(Error::none == dec(&g, "#1?", o));
    ASSERT_TRUE(1 == g.img.width);
    ASSERT_TRUE(6 == g.img.height);
    for (size_t y = 0; y < 6; y++) ASSERT_TRUE(0x102030FFu == px(g.img, 0, y));
}

/* In transparent mode an untouched pixel is all-zero, and only the rows the
 * set bits name exist. */
TEST(sixel, transparent_leaves_untouched_pixels_zero) {
    ImageGuard g;
    /* value 1 (bit 0) then value 2 (bit 1): the second column's top pixel is
     * never written. */
    ASSERT_TRUE(Error::none == dec(&g, "#1@A", optsTransparent()));
    ASSERT_TRUE(2 == g.img.width);
    ASSERT_TRUE(2 == g.img.height);
    ASSERT_TRUE(blue == px(g.img, 0, 0));
    ASSERT_TRUE(0x00000000u == px(g.img, 0, 1));
    ASSERT_TRUE(0x00000000u == px(g.img, 1, 0));
    ASSERT_TRUE(blue == px(g.img, 1, 1));
}

/* Each data byte advances the cursor exactly one pixel. */
TEST(sixel, data_bytes_advance_one_column_each) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "#1@#2@#3@", optsPaint()));
    ASSERT_TRUE(3 == g.img.width);
    ASSERT_TRUE(6 == g.img.height);
    ASSERT_TRUE(blue == px(g.img, 0, 0));
    ASSERT_TRUE(red == px(g.img, 1, 0));
    ASSERT_TRUE(green == px(g.img, 2, 0));
}

/* ── Cursor movement ───────────────────────────────────────────────────── */

/* '$' returns to the left edge of the same sixel row, so a second pass can
 * overdraw it in another colour. */
TEST(sixel, carriage_return_returns_to_left_edge) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "#1~~~$#2@", optsPaint()));
    ASSERT_TRUE(3 == g.img.width);
    ASSERT_TRUE(6 == g.img.height);
    /* Column 0 row 0 was overwritten by the second pass; row 1 was repainted
     * with the background, because a zero bit paints in this mode. */
    ASSERT_TRUE(red == px(g.img, 0, 0));
    ASSERT_TRUE(bg == px(g.img, 0, 1));
    /* Columns 1 and 2 were not revisited. */
    ASSERT_TRUE(blue == px(g.img, 1, 0));
    ASSERT_TRUE(blue == px(g.img, 2, 5));
}

/* '-' is a carriage return AND exactly six pixels down. */
TEST(sixel, new_line_moves_exactly_six_pixels) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "#1@-#2@", optsTransparent()));
    ASSERT_TRUE(1 == g.img.width);
    ASSERT_TRUE(7 == g.img.height);
    ASSERT_TRUE(blue == px(g.img, 0, 0));
    /* Nothing between the two bands. */
    for (size_t y = 1; y < 6; y++) ASSERT_TRUE(0x00000000u == px(g.img, 0, y));
    ASSERT_TRUE(red == px(g.img, 0, 6));
}

TEST(sixel, two_new_lines_move_twelve_pixels) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "#1@--#2@", optsTransparent()));
    ASSERT_TRUE(13 == g.img.height);
    ASSERT_TRUE(blue == px(g.img, 0, 0));
    ASSERT_TRUE(red == px(g.img, 0, 12));
}

/* ── Repeat introducer ─────────────────────────────────────────────────── */

TEST(sixel, repeat_introducer) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "#1!5@", optsTransparent()));
    ASSERT_TRUE(5 == g.img.width);
    ASSERT_TRUE(1 == g.img.height);
    for (size_t x = 0; x < 5; x++) ASSERT_TRUE(blue == px(g.img, x, 0));
}

TEST(sixel, repeat_then_more_data) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "#1!3@#2@", optsTransparent()));
    ASSERT_TRUE(4 == g.img.width);
    ASSERT_TRUE(blue == px(g.img, 2, 0));
    ASSERT_TRUE(red == px(g.img, 3, 0));
}

/* A repeat introducer with nothing after it: the body ended mid-construct.
 * The introducer is dropped and the image is whatever came before. */
TEST(sixel, repeat_at_end_of_input_is_dropped) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "#1@!5", optsTransparent()));
    ASSERT_TRUE(1 == g.img.width);
    ASSERT_TRUE(blue == px(g.img, 0, 0));

    ImageGuard g2;
    /* And with no count either. */
    ASSERT_TRUE(Error::none == dec(&g2, "#1@!", optsTransparent()));
    ASSERT_TRUE(1 == g2.img.width);
}

/* `!0` draws once; see the comment at the call site in sixel.hpp. */
TEST(sixel, repeat_zero_draws_once) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "#1!0@#2@", optsTransparent()));
    ASSERT_TRUE(2 == g.img.width);
    ASSERT_TRUE(blue == px(g.img, 0, 0));
    ASSERT_TRUE(red == px(g.img, 1, 0));
}

/* A repeat whose next byte is not a data byte: the introducer is dropped and
 * the byte is handled normally (here, a colour selection). */
TEST(sixel, repeat_followed_by_non_data_byte) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "!5#2@", optsTransparent()));
    ASSERT_TRUE(1 == g.img.width);
    ASSERT_TRUE(red == px(g.img, 0, 0));
}

/* ── Colour ────────────────────────────────────────────────────────────── */

/* RGB components are percentages 0..100, not 0..255. */
TEST(sixel, rgb_definition_scales_percentages) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "#20;2;100;0;0@", optsTransparent()));
    ASSERT_TRUE(0xFF0000FFu == px(g.img, 0, 0));

    ImageGuard g2;
    /* 50% is 128 after rounding, not 127. */
    ASSERT_TRUE(Error::none == dec(&g2, "#20;2;50;50;50@", optsTransparent()));
    ASSERT_TRUE(0x808080FFu == px(g2.img, 0, 0));

    ImageGuard g3;
    ASSERT_TRUE(Error::none == dec(&g3, "#20;2;0;100;100@", optsTransparent()));
    ASSERT_TRUE(0x00FFFFFFu == px(g3.img, 0, 0));
}

/* Percentages above 100 are clamped rather than overflowing the channel. */
TEST(sixel, rgb_definition_clamps_out_of_range_percentages) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "#20;2;60000;0;0@", optsTransparent()));
    ASSERT_TRUE(0xFF0000FFu == px(g.img, 0, 0));
}

/* Defining a register selects it, and `#Pc` alone reselects one defined
 * earlier. */
TEST(sixel, select_previously_defined_register) {
    ImageGuard g;
    ASSERT_TRUE(Error::none ==
                dec(&g, "#30;2;100;0;0@#31;2;0;0;100@#30@", optsTransparent()));
    ASSERT_TRUE(3 == g.img.width);
    ASSERT_TRUE(0xFF0000FFu == px(g.img, 0, 0));
    ASSERT_TRUE(0x0000FFFFu == px(g.img, 1, 0));
    ASSERT_TRUE(0xFF0000FFu == px(g.img, 2, 0));
}

/* HLS: Pu = 1. DEC's hue ring starts at blue and runs the other way from the
 * usual HSL, so hue 0 is blue, 120 is red, 240 is green. */
TEST(sixel, hls_definition) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "#20;1;120;50;100@", optsTransparent()));
    ASSERT_TRUE(0xFF0000FFu == px(g.img, 0, 0));

    ImageGuard g2;
    ASSERT_TRUE(Error::none == dec(&g2, "#20;1;0;50;100@", optsTransparent()));
    ASSERT_TRUE(0x0000FFFFu == px(g2.img, 0, 0));

    ImageGuard g3;
    ASSERT_TRUE(Error::none == dec(&g3, "#20;1;240;50;100@", optsTransparent()));
    ASSERT_TRUE(0x00FF00FFu == px(g3.img, 0, 0));
}

/* Zero saturation is a grey of the given lightness, whatever the hue. */
TEST(sixel, hls_zero_saturation_is_grey) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "#20;1;200;100;0@", optsTransparent()));
    ASSERT_TRUE(0xFFFFFFFFu == px(g.img, 0, 0));

    ImageGuard g2;
    ASSERT_TRUE(Error::none == dec(&g2, "#20;1;200;0;0@", optsTransparent()));
    ASSERT_TRUE(0x000000FFu == px(g2.img, 0, 0));
}

/* An unknown colour space (Pu neither 1 nor 2) leaves the register's colour
 * alone, and the construct degrades to the plain selection `#Pc`. */
TEST(sixel, unknown_color_space_is_ignored) {
    ImageGuard g;
    /* Register 1 is selected but keeps its default blue. */
    ASSERT_TRUE(Error::none == dec(&g, "#1;7;100;0;0@", optsTransparent()));
    ASSERT_TRUE(blue == px(g.img, 0, 0));

    ImageGuard g2;
    /* Register 2 was selected first; the bad definition of 1 moves the
     * selection to 1 rather than painting in 2's red. */
    ASSERT_TRUE(Error::none == dec(&g2, "#2#1;0;100;0;0@", optsTransparent()));
    ASSERT_TRUE(blue == px(g2.img, 0, 0));
}

/* The sixteen VT340 defaults, each selected in turn into its own column. */
TEST(sixel, default_palette_registers_0_to_15) {
    char body[16 * 5 + 1];
    size_t n = 0;
    for (int i = 0; i < 16; i++) {
        body[n++] = '#';
        if (i >= 10) body[n++] = (char)('0' + i / 10);
        body[n++] = (char)('0' + i % 10);
        body[n++] = '@';
    }
    body[n] = '\0';

    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, body, optsTransparent()));
    ASSERT_TRUE(16 == g.img.width);
    ASSERT_TRUE(1 == g.img.height);
    for (size_t i = 0; i < 16; i++) ASSERT_TRUE(expected_default_palette[i] == px(g.img, i, 0));
}

/* Register 0 is the one in force before any `#` appears. */
TEST(sixel, default_selected_register_is_zero) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "@", optsTransparent()));
    ASSERT_TRUE(expected_default_palette[0] == px(g.img, 0, 0));
}

/* ── Sizing ────────────────────────────────────────────────────────────── */

/* Raster attributes size the image: Pan;Pad;Ph;Pv. */
TEST(sixel, raster_attributes_size_the_image) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "\"1;1;4;3#1@", optsPaint()));
    ASSERT_TRUE(4 == g.img.width);
    ASSERT_TRUE(3 == g.img.height);
    ASSERT_TRUE(blue == px(g.img, 0, 0));
    /* The rest is the background fill, including the part the band drew
     * below row 3 and the columns nothing reached. */
    ASSERT_TRUE(bg == px(g.img, 0, 2));
    ASSERT_TRUE(bg == px(g.img, 3, 0));
}

/* Raster attributes also crop: the data drew six rows, the raster says two. */
TEST(sixel, raster_attributes_crop) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "\"1;1;2;2#1~~~~", optsTransparent()));
    ASSERT_TRUE(2 == g.img.width);
    ASSERT_TRUE(2 == g.img.height);
    ASSERT_TRUE(blue == px(g.img, 0, 0));
    ASSERT_TRUE(blue == px(g.img, 1, 1));
}

/* Without raster attributes the size is whatever the data drew, and the
 * buffer has to grow as it goes. 300 columns forces several growths past the
 * initial 64. */
TEST(sixel, grows_without_raster_attributes) {
    char body[8 + 300 + 1];
    size_t n = 0;
    body[n++] = '#';
    body[n++] = '1';
    for (int i = 0; i < 300; i++) body[n++] = '~';
    body[n] = '\0';

    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, body, optsTransparent()));
    ASSERT_TRUE(300 == g.img.width);
    ASSERT_TRUE(6 == g.img.height);
    ASSERT_TRUE(blue == px(g.img, 0, 0));
    ASSERT_TRUE(blue == px(g.img, 299, 5));
}

/* Growing downwards as well, across several bands. */
TEST(sixel, grows_vertically_across_bands) {
    char body[2 + 20 * 2 + 1];
    size_t n = 0;
    body[n++] = '#';
    body[n++] = '1';
    for (int i = 0; i < 20; i++) {
        body[n++] = '~';
        body[n++] = '-';
    }
    body[n] = '\0';

    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, body, optsTransparent()));
    ASSERT_TRUE(1 == g.img.width);
    ASSERT_TRUE(120 == g.img.height);
    ASSERT_TRUE(blue == px(g.img, 0, 0));
    ASSERT_TRUE(blue == px(g.img, 0, 119));
}

/* An empty body, and a body with no data bytes at all. */
TEST(sixel, empty_body_is_an_empty_image) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "", optsPaint()));
    ASSERT_TRUE(0 == g.img.width);
    ASSERT_TRUE(0 == g.img.height);
    ASSERT_TRUE(nullptr == g.img.pixels);

    ImageGuard g2;
    ASSERT_TRUE(Error::none == dec(&g2, "#1#2$---", optsPaint()));
    ASSERT_TRUE(0 == g2.img.width);
    ASSERT_TRUE(0 == g2.img.height);
}

/* Bytes the format does not define are ignored, not fatal. */
TEST(sixel, unrecognised_bytes_are_ignored) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "#1\r\n @\t\x01%&*@", optsTransparent()));
    ASSERT_TRUE(2 == g.img.width);
    ASSERT_TRUE(blue == px(g.img, 0, 0));
    ASSERT_TRUE(blue == px(g.img, 1, 0));
}

/* ── Hostile input ─────────────────────────────────────────────────────── */

/* A repeat count that would blow past max_width must be refused before any
 * allocation, not clamped into a gigantic buffer. */
TEST(sixel, hostile_huge_repeat_count) {
    ImageGuard g;
    ASSERT_TRUE(Error::TooLarge == dec(&g, "#1!999999999~", optsPaint()));
    ASSERT_TRUE(0 == g.img.width);
    ASSERT_TRUE(0 == g.img.height);
    ASSERT_TRUE(nullptr == g.img.pixels);
}

/* The same count is fine when the caller's limits allow it, which proves the
 * refusal above came from the limit and not from the parse. */
TEST(sixel, large_repeat_within_limits_is_allowed) {
    ImageGuard g;
    Options o = optsTransparent();
    o.max_width = 5000;
    o.max_height = 16;
    o.max_pixels = 100000;
    ASSERT_TRUE(Error::none == dec(&g, "#1!4000@", o));
    ASSERT_TRUE(4000 == g.img.width);
    ASSERT_TRUE(1 == g.img.height);
    ASSERT_TRUE(blue == px(g.img, 3999, 0));

    /* One past max_width is refused. */
    ImageGuard g2;
    ASSERT_TRUE(Error::TooLarge == dec(&g2, "#1!5001@", o));
}

/* Repeat counts that would overflow a u32 if accumulated naively. The
 * parameter saturates, and the saturated value is still over the limit. */
TEST(sixel, hostile_overflowing_repeat_parameter) {
    ImageGuard g;
    ASSERT_TRUE(Error::TooLarge == dec(&g, "#1!99999999999999999999~", optsPaint()));

    ImageGuard g2;
    ASSERT_TRUE(Error::TooLarge ==
                dec(&g2, "#1!4294967296~", optsPaint())); /* 2^32 */

    ImageGuard g3;
    ASSERT_TRUE(Error::TooLarge ==
                dec(&g3, "#1!18446744073709551617~", optsPaint())); /* 2^64 + 1 */
}

/* Raster attributes are attacker-controlled too. */
TEST(sixel, hostile_huge_raster_attributes) {
    ImageGuard g;
    ASSERT_TRUE(Error::TooLarge == dec(&g, "\"1;1;99999;99999#1@", optsPaint()));
    ASSERT_TRUE(nullptr == g.img.pixels);

    /* Inside max_width and max_height individually, but over max_pixels. */
    ImageGuard g2;
    Options o = optsPaint();
    o.max_width = 10000;
    o.max_height = 10000;
    o.max_pixels = 1000;
    ASSERT_TRUE(Error::TooLarge == dec(&g2, "\"1;1;9000;9000#1@", o));

    /* And a raster with saturating parameters. */
    ImageGuard g3;
    ASSERT_TRUE(Error::TooLarge ==
                dec(&g3, "\"99999999999;99999999999;99999999999;99999999999#1@", optsPaint()));
}

/* A zero in Ph or Pv means "no raster", not a zero-sized image. */
TEST(sixel, raster_with_zero_dimension_is_ignored) {
    ImageGuard g;
    ASSERT_TRUE(Error::none == dec(&g, "\"1;1;0;0#1@", optsTransparent()));
    ASSERT_TRUE(1 == g.img.width);
    ASSERT_TRUE(1 == g.img.height);
    ASSERT_TRUE(blue == px(g.img, 0, 0));
}

/* Enough '-' to put the band past max_height. */
TEST(sixel, hostile_many_new_lines) {
    char body[4000 + 4];
    size_t n = 0;
    for (int i = 0; i < 4000; i++) body[n++] = '-';
    body[n++] = '#';
    body[n++] = '1';
    body[n++] = '~';
    body[n] = '\0';

    ImageGuard g;
    ASSERT_TRUE(Error::TooLarge == dec(&g, body, optsPaint()));
    ASSERT_TRUE(nullptr == g.img.pixels);
}

/* Colour register numbers out of range are ignored, never wrapped into a
 * real register and never used as an index. */
TEST(sixel, hostile_out_of_range_color_register) {
    ImageGuard g;
    /* 256 is the first out-of-range register; selection is ignored, so
     * register 0 is still in force. */
    ASSERT_TRUE(Error::none == dec(&g, "#256@", optsTransparent()));
    ASSERT_TRUE(expected_default_palette[0] == px(g.img, 0, 0));

    ImageGuard g2;
    ASSERT_TRUE(Error::none == dec(&g2, "#1#99999@", optsTransparent()));
    ASSERT_TRUE(blue == px(g2.img, 0, 0)); /* the out-of-range select did nothing */

    ImageGuard g3;
    /* A definition of an out-of-range register writes nothing and does not
     * change the selection either. */
    ASSERT_TRUE(Error::none == dec(&g3, "#1#60000;2;100;0;0@", optsTransparent()));
    ASSERT_TRUE(blue == px(g3.img, 0, 0));

    ImageGuard g4;
    /* The highest in-range register works. */
    ASSERT_TRUE(Error::none == dec(&g4, "#255;2;100;0;0@", optsTransparent()));
    ASSERT_TRUE(0xFF0000FFu == px(g4.img, 0, 0));
}

/* Every construct truncated at every length: none may crash or leak. */
TEST(sixel, hostile_truncated_sequences) {
    static const char *bodies[] = {
        "#",      "#1",      "#1;",     "#1;2",    "#1;2;",   "#1;2;1",  "#1;2;1;",
        "#1;2;1;2", "#1;2;1;2;", "!",    "!1",      "!12;",    "\"",      "\"1",
        "\"1;",   "\"1;1",   "\"1;1;",  "\"1;1;4", "\"1;1;4;", "$",      "-",
        ";",      ";;;;;;;;;;;;", "#;;;;;;;;;;;;;;;;", "#1@!",  "\"1;1;4;3",
    };
    for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
        ImageGuard g;
        const Error err = dec(&g, bodies[i], optsPaint());
        ASSERT_TRUE(err == Error::none || err == Error::TooLarge);
        /* Whatever came back must be self-consistent. */
        if (g.img.width == 0 || g.img.height == 0) {
            ASSERT_TRUE(nullptr == g.img.pixels);
        } else {
            ASSERT_TRUE(nullptr != g.img.pixels);
        }
    }
}

/* A body of random bytes. It must terminate, stay inside the limits, and not
 * write anywhere it should not -- which the surrounding guard bytes and the
 * leak check at the end of the file both watch for. */
TEST(sixel, hostile_random_bytes) {
    static uint8_t buf[4096];
    for (uint32_t seed = 1; seed <= 32; seed++) {
        uint32_t s = seed * 2654435761u + 1u;
        for (size_t i = 0; i < sizeof(buf); i++) {
            s = s * 1103515245u + 12345u;
            buf[i] = (uint8_t)(s >> 16);
        }

        ImageGuard g;
        Options o = optsPaint();
        /* Deliberately tight, so a bomb hidden in the noise is refused
         * rather than merely slow. */
        o.max_width = 2048;
        o.max_height = 2048;
        o.max_pixels = 1 << 20;
        const Error err = wisp::terminal::sixel::decode(talloc(), (const char *)buf, sizeof(buf),
                                                        o, &g.img);
        ASSERT_TRUE(err == Error::none || err == Error::TooLarge || err == Error::OutOfMemory);
        ASSERT_TRUE(g.img.width <= o.max_width);
        ASSERT_TRUE(g.img.height <= o.max_height);
        if (g.img.width != 0 && g.img.height != 0) {
            ASSERT_TRUE(nullptr != g.img.pixels);
            ASSERT_TRUE(g.img.width * g.img.height <= o.max_pixels);
            /* Touch every byte, so a short buffer trips the heap checker
             * here rather than somewhere unrelated later. */
            size_t sum = 0;
            for (size_t k = 0; k < g.img.width * g.img.height * 4; k++) sum += g.img.pixels[k];
            ASSERT_TRUE(sum == sum);
        }
    }
}

/* A null body is not a special case the caller has to avoid. */
TEST(sixel, null_data_is_an_empty_image) {
    ImageGuard g;
    const Options o = optsPaint();
    ASSERT_TRUE(Error::none == wisp::terminal::sixel::decode(talloc(), nullptr, 0, o, &g.img));
    ASSERT_TRUE(0 == g.img.width);
    ASSERT_TRUE(nullptr == g.img.pixels);
}

/* `len` bounds the read, not a NUL: the decoder must stop at the length even
 * when more sixel data follows in the buffer. */
TEST(sixel, respects_explicit_length) {
    ImageGuard g;
    const char body[] = "#1@@@@@";
    const Options o = optsTransparent();
    /* Only "#1@@" is in scope. */
    ASSERT_TRUE(Error::none == wisp::terminal::sixel::decode(talloc(), body, 4, o, &g.img));
    ASSERT_TRUE(2 == g.img.width);
}

TEST(sixel, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
