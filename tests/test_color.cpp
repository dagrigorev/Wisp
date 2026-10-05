/* Transliterated from the test blocks in Ghostty src/terminal/color.zig,
 * src/terminal/fraction.zig and src/terminal/x11_color.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * These are upstream's tests: same inputs, same assertions, same names.
 *
 * Not portable, and adapted as noted: "DynamicPalette: changeDefault shares
 * the built-in default" uses std.testing.FailingAllocator to prove two paths
 * never allocate. The pointer-identity and value assertions around it are
 * kept; the two has_induced_failure checks are not, since there is no
 * failing allocator to ask.
 */

#include <string>

#include "test_helpers.h"
#include "color.hpp"

using namespace wisp::terminal;

static RGB rgb(uint8_t r, uint8_t g, uint8_t b) { return RGB(r, g, b); }

static bool parses_to(const char *s, RGB want) {
    RGB got;
    if (RGB::parse(s, &got) != ColorError::none) return false;
    return got.eql(want);
}

static bool parse_fails(const char *s) {
    RGB got;
    return RGB::parse(s, &got) == ColorError::InvalidFormat;
}

static bool approx(float want, float got, float epsilon) {
    return fabsf(want - got) <= epsilon;
}

/* ─── color.zig ──────────────────────────────────────────────────────────── */

TEST(color, parsePaletteEntry) {
    {
        PaletteEntry entry;
        ASSERT_TRUE(parsePaletteEntry("0=#AABBCC", &entry) == ColorError::none);
        ASSERT_EQ(entry.index, 0);
        ASSERT_TRUE(entry.color.eql(rgb(170, 187, 204)));
    }
    {
        PaletteEntry entry;
        ASSERT_TRUE(parsePaletteEntry("0b1=#014589", &entry) == ColorError::none);
        ASSERT_EQ(entry.index, 1);
        ASSERT_TRUE(entry.color.eql(rgb(1, 69, 137)));
    }
    {
        PaletteEntry entry;
        ASSERT_TRUE(parsePaletteEntry("0o7=#234567", &entry) == ColorError::none);
        ASSERT_EQ(entry.index, 7);
        ASSERT_TRUE(entry.color.eql(rgb(35, 69, 103)));
    }
    {
        PaletteEntry entry;
        ASSERT_TRUE(parsePaletteEntry("0xF=#ABCDEF", &entry) == ColorError::none);
        ASSERT_EQ(entry.index, 15);
        ASSERT_TRUE(entry.color.eql(rgb(171, 205, 239)));
    }
    {
        PaletteEntry entry;
        ASSERT_TRUE(parsePaletteEntry("0 =  #AABBCC", &entry) == ColorError::none);
        ASSERT_EQ(entry.index, 0);
        ASSERT_TRUE(entry.color.eql(rgb(170, 187, 204)));
    }
    {
        PaletteEntry entry;
        ASSERT_TRUE(parsePaletteEntry(" 1= #DDEEFF    ", &entry) == ColorError::none);
        ASSERT_EQ(entry.index, 1);
        ASSERT_TRUE(entry.color.eql(rgb(221, 238, 255)));
    }
    {
        PaletteEntry entry;
        ASSERT_TRUE(parsePaletteEntry("  2  =  #123456 ", &entry) == ColorError::none);
        ASSERT_EQ(entry.index, 2);
        ASSERT_TRUE(entry.color.eql(rgb(18, 52, 86)));
    }
    {
        PaletteEntry entry;
        ASSERT_TRUE(parsePaletteEntry("1=black", &entry) == ColorError::none);
        ASSERT_EQ(entry.index, 1);
        ASSERT_TRUE(entry.color.eql(rgb(0, 0, 0)));
    }

    PaletteEntry e;
    ASSERT_TRUE(parsePaletteEntry(" ", &e) == ColorError::InvalidFormat);
    ASSERT_TRUE(parsePaletteEntry("a", &e) == ColorError::InvalidFormat);
    ASSERT_TRUE(parsePaletteEntry("256=#AABBCC", &e) == ColorError::Overflow);
    ASSERT_TRUE(parsePaletteEntry("1=notacolor", &e) == ColorError::InvalidFormat);
}

TEST(color, Special_osc4) {
    ASSERT_EQ(special_osc4(Special::bold), 256);
    ASSERT_EQ(special_osc4(Special::underline), 257);
    ASSERT_EQ(special_osc4(Special::blink), 258);
    ASSERT_EQ(special_osc4(Special::reverse), 259);
    ASSERT_EQ(special_osc4(Special::italic), 260);
}

TEST(color, Dynamic_next) {
    Dynamic d;
    ASSERT_TRUE(dynamic_next(Dynamic::foreground, &d) && d == Dynamic::background);
    ASSERT_TRUE(dynamic_next(Dynamic::background, &d) && d == Dynamic::cursor);
    ASSERT_TRUE(dynamic_next(Dynamic::cursor, &d) && d == Dynamic::pointer_foreground);
    ASSERT_TRUE(dynamic_next(Dynamic::pointer_foreground, &d) &&
                d == Dynamic::pointer_background);
    ASSERT_TRUE(dynamic_next(Dynamic::pointer_background, &d) &&
                d == Dynamic::tektronix_foreground);
    ASSERT_TRUE(dynamic_next(Dynamic::tektronix_foreground, &d) &&
                d == Dynamic::tektronix_background);
    ASSERT_TRUE(dynamic_next(Dynamic::tektronix_background, &d) &&
                d == Dynamic::highlight_background);
    ASSERT_TRUE(dynamic_next(Dynamic::highlight_background, &d) &&
                d == Dynamic::tektronix_cursor);
    ASSERT_TRUE(dynamic_next(Dynamic::tektronix_cursor, &d) &&
                d == Dynamic::highlight_foreground);
    ASSERT_FALSE(dynamic_next(Dynamic::highlight_foreground, &d));
}

TEST(color, palette_default) {
    /* Safety check */
    for (uint8_t i = 0; i < 16; i++) {
        RGB want;
        ASSERT_TRUE(name_default((Name)i, &want));
        ASSERT_TRUE(want.eql(default_palette()[i]));
    }
}

TEST(color, RGB_parse) {
    ASSERT_TRUE(parses_to("rgbi:1.0/0/0", rgb(255, 0, 0)));
    ASSERT_TRUE(parses_to("rgb:7f/a0a0/0", rgb(127, 160, 0)));
    ASSERT_TRUE(parses_to("rgb:f/ff/fff", rgb(255, 255, 255)));
    ASSERT_TRUE(parses_to("#ffffff", rgb(255, 255, 255)));
    ASSERT_TRUE(parses_to("#fff", rgb(255, 255, 255)));
    ASSERT_TRUE(parses_to("#fffffffff", rgb(255, 255, 255)));
    ASSERT_TRUE(parses_to("#ffffffffffff", rgb(255, 255, 255)));
    ASSERT_TRUE(parses_to("#ff0010", rgb(255, 0, 16)));
    ASSERT_TRUE(parses_to("0A0B0C", rgb(10, 11, 12)));
    ASSERT_TRUE(parses_to("FFFFFF", rgb(255, 255, 255)));
    ASSERT_TRUE(parses_to("FFF", rgb(255, 255, 255)));
    ASSERT_TRUE(parses_to("#345", rgb(51, 68, 85)));
    ASSERT_TRUE(parses_to(" #AABBCC   ", rgb(170, 187, 204)));

    ASSERT_TRUE(parses_to("black", rgb(0, 0, 0)));
    ASSERT_TRUE(parses_to("red", rgb(255, 0, 0)));
    ASSERT_TRUE(parses_to("green", rgb(0, 255, 0)));
    ASSERT_TRUE(parses_to("blue", rgb(0, 0, 255)));
    ASSERT_TRUE(parses_to("white", rgb(255, 255, 255)));

    ASSERT_TRUE(parses_to("LawnGreen", rgb(124, 252, 0)));
    ASSERT_TRUE(parses_to("medium spring green", rgb(0, 250, 154)));
    ASSERT_TRUE(parses_to(" Forest Green ", rgb(34, 139, 34)));
    ASSERT_TRUE(parses_to("\tForestGreen\t", rgb(34, 139, 34)));

    /* Invalid format */
    ASSERT_TRUE(parse_fails(""));
    ASSERT_TRUE(parse_fails("  "));
    ASSERT_TRUE(parse_fails("rgb;"));
    ASSERT_TRUE(parse_fails("rgb:"));
    ASSERT_TRUE(parse_fails(":a/a/a"));
    ASSERT_TRUE(parse_fails("a/a/a"));
    ASSERT_TRUE(parse_fails("rgb:a/a/a/"));
    ASSERT_TRUE(parse_fails("rgb:00000///"));
    ASSERT_TRUE(parse_fails("rgb:000/"));
    ASSERT_TRUE(parse_fails("rgbi:a/a/a"));
    ASSERT_TRUE(parse_fails("rgb:0.5/0.0/1.0"));
    ASSERT_TRUE(parse_fails("rgb:not/hex/zz"));
    ASSERT_TRUE(parse_fails("#"));
    ASSERT_TRUE(parse_fails("#ff"));
    ASSERT_TRUE(parse_fails("#ffff"));
    ASSERT_TRUE(parse_fails("#fffff"));
    ASSERT_TRUE(parse_fails("#gggggg"));
    ASSERT_TRUE(parse_fails("#12345"));
    ASSERT_TRUE(parse_fails("12345"));
    ASSERT_TRUE(parse_fails("nosuchcolor"));
}

TEST(color, RGB_encode) {
    const RGB c = rgb(0x01, 0x23, 0xff);

    char buf[64];
    c.encodeRgb8(buf, sizeof(buf));
    ASSERT_TRUE(strcmp(buf, "rgb:01/23/ff") == 0);

    c.encodeRgb16(buf, sizeof(buf));
    ASSERT_TRUE(strcmp(buf, "rgb:0101/2323/ffff") == 0);
}

TEST(color, DynamicPalette_init) {
    DynamicPalette p = DynamicPalette::default_();
    ASSERT_TRUE(p.current.eql(default_palette()));
    ASSERT_TRUE(p.original->eql(default_palette()));
    ASSERT_TRUE(p.original == &default_palette());
    ASSERT_EQ(p.mask.count(), 0u);
    p.deinit();
}

TEST(color, DynamicPalette_set) {
    DynamicPalette p = DynamicPalette::default_();
    const RGB new_color = rgb(255, 0, 0);

    p.set(0, new_color);
    ASSERT_TRUE(p.current[0].eql(new_color));
    ASSERT_TRUE(p.mask.isSet(0));
    ASSERT_EQ(p.mask.count(), 1u);

    ASSERT_TRUE(default_palette()[0].eql((*p.original)[0]));
}

TEST(color, DynamicPalette_reset) {
    DynamicPalette p = DynamicPalette::default_();
    const RGB new_color = rgb(255, 0, 0);

    p.set(0, new_color);
    ASSERT_TRUE(p.mask.isSet(0));

    p.reset(0);
    ASSERT_TRUE(p.current[0].eql(default_palette()[0]));
    ASSERT_FALSE(p.mask.isSet(0));
    ASSERT_EQ(p.mask.count(), 0u);
}

TEST(color, DynamicPalette_resetAll) {
    DynamicPalette p = DynamicPalette::default_();
    const RGB new_color = rgb(255, 0, 0);

    p.set(0, new_color);
    p.set(5, new_color);
    p.set(10, new_color);
    ASSERT_EQ(p.mask.count(), 3u);

    p.resetAll();
    ASSERT_TRUE(p.current.eql(default_palette()));
    ASSERT_TRUE(p.original->eql(default_palette()));
    ASSERT_EQ(p.mask.count(), 0u);
}

TEST(color, DynamicPalette_changeDefault_with_no_changes) {
    DynamicPalette p = DynamicPalette::default_();
    Palette new_palette = default_palette();
    new_palette[0] = rgb(100, 100, 100);

    p.changeDefault(new_palette);
    ASSERT_TRUE(p.original->eql(new_palette));
    ASSERT_TRUE(p.current.eql(new_palette));
    ASSERT_EQ(p.mask.count(), 0u);
    p.deinit();
}

TEST(color, DynamicPalette_changeDefault_shares_the_built_in_default) {
    Palette new_palette = default_palette();
    new_palette[0] = rgb(100, 100, 100);

    /* A custom default is an owned copy. */
    DynamicPalette p = DynamicPalette::init(new_palette);
    ASSERT_TRUE(p.original != &default_palette());
    ASSERT_TRUE(p.original->eql(new_palette));

    /* Changing the custom default reuses the copy. */
    const Palette *owned = p.original;
    new_palette[1] = rgb(101, 101, 101);
    p.changeDefault(new_palette);
    ASSERT_TRUE(p.original == owned);
    ASSERT_TRUE(p.original->eql(new_palette));

    /* Changing back to the built-in default releases the copy and shares
     * it again. */
    p.changeDefault(default_palette());
    ASSERT_TRUE(p.original == &default_palette());
    ASSERT_TRUE(p.current.eql(default_palette()));

    /* resetDefault does the same directly, preserving changed values.
     * Wisp: upstream also asserts, with a failing allocator, that this
     * never allocates; see the file header. */
    p.changeDefault(new_palette);
    p.set(2, rgb(1, 2, 3));
    p.resetDefault();
    ASSERT_TRUE(p.original == &default_palette());
    ASSERT_TRUE(p.current[0].eql(default_palette()[0]));
    ASSERT_TRUE(p.current[2].eql(rgb(1, 2, 3)));
    ASSERT_TRUE(p.mask.isSet(2));

    /* The built-in default never allocates. */
    DynamicPalette q = DynamicPalette::init(default_palette());
    ASSERT_TRUE(q.original == &default_palette());
    q.deinit();
    p.deinit();
}

TEST(color, DynamicPalette_changeDefault_preserves_changes) {
    DynamicPalette p = DynamicPalette::default_();
    const RGB custom_color = rgb(255, 0, 0);

    p.set(5, custom_color);
    ASSERT_TRUE(p.mask.isSet(5));

    Palette new_palette = default_palette();
    new_palette[0] = rgb(100, 100, 100);
    new_palette[5] = rgb(50, 50, 50);

    p.changeDefault(new_palette);

    ASSERT_TRUE(p.original->eql(new_palette));
    ASSERT_TRUE(p.current[0].eql(new_palette[0]));
    ASSERT_TRUE(p.current[5].eql(custom_color));
    ASSERT_TRUE(p.mask.isSet(5));
    ASSERT_EQ(p.mask.count(), 1u);
    p.deinit();
}

TEST(color, DynamicPalette_changeDefault_with_multiple_changes) {
    DynamicPalette p = DynamicPalette::default_();
    const RGB red = rgb(255, 0, 0);
    const RGB green = rgb(0, 255, 0);
    const RGB blue = rgb(0, 0, 255);

    p.set(1, red);
    p.set(2, green);
    p.set(3, blue);

    Palette new_palette = default_palette();
    new_palette[0] = rgb(50, 50, 50);
    new_palette[1] = rgb(60, 60, 60);

    p.changeDefault(new_palette);

    ASSERT_TRUE(p.current[0].eql(new_palette[0]));
    ASSERT_TRUE(p.current[1].eql(red));
    ASSERT_TRUE(p.current[2].eql(green));
    ASSERT_TRUE(p.current[3].eql(blue));
    ASSERT_EQ(p.mask.count(), 3u);
    p.deinit();
}

TEST(color, LAB_fromRgb) {
    const float epsilon = 0.5f;

    /* White (255, 255, 255) -> L*=100, a*=0, b*=0 */
    const LAB white = LAB::fromRgb(rgb(255, 255, 255));
    ASSERT_TRUE(approx(100.0f, white.l, epsilon));
    ASSERT_TRUE(approx(0.0f, white.a, epsilon));
    ASSERT_TRUE(approx(0.0f, white.b, epsilon));

    /* Black (0, 0, 0) -> L*=0, a*=0, b*=0 */
    const LAB black = LAB::fromRgb(rgb(0, 0, 0));
    ASSERT_TRUE(approx(0.0f, black.l, epsilon));
    ASSERT_TRUE(approx(0.0f, black.a, epsilon));
    ASSERT_TRUE(approx(0.0f, black.b, epsilon));

    /* Pure red (255, 0, 0) -> L*≈53.23, a*≈80.11, b*≈67.22 */
    const LAB red = LAB::fromRgb(rgb(255, 0, 0));
    ASSERT_TRUE(approx(53.23f, red.l, epsilon));
    ASSERT_TRUE(approx(80.11f, red.a, epsilon));
    ASSERT_TRUE(approx(67.22f, red.b, epsilon));

    /* Pure green (0, 128, 0) -> L*≈46.23, a*≈-51.70, b*≈49.90 */
    const LAB green = LAB::fromRgb(rgb(0, 128, 0));
    ASSERT_TRUE(approx(46.23f, green.l, epsilon));
    ASSERT_TRUE(approx(-51.70f, green.a, epsilon));
    ASSERT_TRUE(approx(49.90f, green.b, epsilon));

    /* Pure blue (0, 0, 255) -> L*≈32.30, a*≈79.20, b*≈-107.86 */
    const LAB blue = LAB::fromRgb(rgb(0, 0, 255));
    ASSERT_TRUE(approx(32.30f, blue.l, epsilon));
    ASSERT_TRUE(approx(79.20f, blue.a, epsilon));
    ASSERT_TRUE(approx(-107.86f, blue.b, epsilon));
}

TEST(color, generate256Color_base16_preserved) {
    const RGB bg = rgb(0, 0, 0);
    const RGB fg = rgb(255, 255, 255);
    const Palette palette =
        generate256Color(default_palette(), PaletteMask::initEmpty(), bg, fg, false);

    /* The first 16 colors (base16) must remain unchanged. */
    for (size_t i = 0; i < 16; i++) {
        ASSERT_TRUE(default_palette()[i].eql(palette[i]));
    }
}

TEST(color, generate256Color_cube_corners_match_base_colors) {
    const RGB bg = rgb(0, 0, 0);
    const RGB fg = rgb(255, 255, 255);
    const Palette palette =
        generate256Color(default_palette(), PaletteMask::initEmpty(), bg, fg, false);

    /* Index 16 is cube (0,0,0) which should equal bg. */
    ASSERT_TRUE(bg.eql(palette[16]));

    /* Index 231 is cube (5,5,5) which should equal fg. */
    ASSERT_TRUE(fg.eql(palette[231]));
}

TEST(color, generate256Color_cube_corners_black_white_with_harmonious_false) {
    const RGB black = rgb(0, 0, 0);
    const RGB white = rgb(255, 255, 255);

    /* Dark theme: bg=black, fg=white. */
    const Palette dark = generate256Color(default_palette(),
                                          PaletteMask::initEmpty(), black, white, false);
    ASSERT_TRUE(black.eql(dark[16]));
    ASSERT_TRUE(white.eql(dark[231]));

    /* Light theme: bg=white, fg=black. The bg/red swap ensures
     * the cube still runs from black (16) to white (231). */
    const Palette light = generate256Color(default_palette(),
                                           PaletteMask::initEmpty(), white, black, false);
    ASSERT_TRUE(black.eql(light[16]));
    ASSERT_TRUE(white.eql(light[231]));
}

TEST(color, generate256Color_light_theme_cube_corners_with_harmonious_true) {
    const RGB white = rgb(255, 255, 255);
    const RGB black = rgb(0, 0, 0);

    /* harmonious=true skips the bg/fg swap, so the cube preserves the
     * original orientation: (0,0,0)=bg=white, (5,5,5)=fg=black. */
    const Palette palette = generate256Color(default_palette(),
                                             PaletteMask::initEmpty(), white, black, true);
    ASSERT_TRUE(white.eql(palette[16]));
    ASSERT_TRUE(black.eql(palette[231]));
}

TEST(color, generate256Color_grayscale_ramp_monotonic_luminance) {
    const RGB bg = rgb(0, 0, 0);
    const RGB fg = rgb(255, 255, 255);
    const Palette palette =
        generate256Color(default_palette(), PaletteMask::initEmpty(), bg, fg, false);

    /* The grayscale ramp (232–255) should have monotonically increasing
     * luminance from near-black to near-white. */
    double prev_lum = 0.0;
    for (size_t i = 232; i < 256; i++) {
        const double lum = palette[i].luminance();
        ASSERT_TRUE(lum >= prev_lum);
        prev_lum = lum;
    }
}

TEST(color, generate256Color_skip_mask_preserves_original_colors) {
    const RGB bg = rgb(0, 0, 0);
    const RGB fg = rgb(255, 255, 255);

    /* Mark a few indices as skipped; they should keep their base value. */
    PaletteMask skip = PaletteMask::initEmpty();
    skip.set(20);
    skip.set(100);
    skip.set(240);

    const Palette palette = generate256Color(default_palette(), skip, bg, fg, false);
    ASSERT_TRUE(default_palette()[20].eql(palette[20]));
    ASSERT_TRUE(default_palette()[100].eql(palette[100]));
    ASSERT_TRUE(default_palette()[240].eql(palette[240]));

    /* A non-skipped index in the cube should differ from the default. */
    ASSERT_FALSE(palette[21].eql(default_palette()[21]));
}

TEST(color, generate256Color_dark_theme_harmonious_has_no_effect) {
    /* For a dark theme (fg lighter than bg), harmonious should not change
     * the output because the inversion is only relevant for light themes. */
    const RGB bg = rgb(0, 0, 0);
    const RGB fg = rgb(255, 255, 255);
    const Palette normal =
        generate256Color(default_palette(), PaletteMask::initEmpty(), bg, fg, false);
    const Palette harmonious =
        generate256Color(default_palette(), PaletteMask::initEmpty(), bg, fg, true);

    for (size_t i = 16; i < 256; i++) {
        ASSERT_TRUE(normal[i].eql(harmonious[i]));
    }
}

TEST(color, generate256Color_light_theme_harmonious_skips_inversion) {
    /* For a light theme (fg darker than bg), harmonious=true skips the
     * bg/red swap, producing different cube colors than harmonious=false. */
    const RGB bg = rgb(255, 255, 255);
    const RGB fg = rgb(0, 0, 0);
    const Palette inverted =
        generate256Color(default_palette(), PaletteMask::initEmpty(), bg, fg, false);
    const Palette harmonious =
        generate256Color(default_palette(), PaletteMask::initEmpty(), bg, fg, true);

    /* Cube origin (0,0,0) at index 16: without harmonious, bg and red are
     * swapped so it becomes the red base; with harmonious it stays as bg. */
    ASSERT_TRUE(bg.eql(harmonious[16]));
    ASSERT_FALSE(inverted[16].eql(bg));

    /* At least some cube colors should differ between the two modes. */
    size_t differ = 0;
    for (size_t i = 16; i < 232; i++) {
        if (!inverted[i].eql(harmonious[i])) differ += 1;
    }
    ASSERT_TRUE(differ > 0);
}

TEST(color, generate256Color_light_theme_harmonious_grayscale_ramp) {
    const RGB bg = rgb(255, 255, 255);
    const RGB fg = rgb(0, 0, 0);

    /* harmonious=false swaps bg/fg, so the ramp runs black→white
     * (increasing). */
    {
        const Palette palette =
            generate256Color(default_palette(), PaletteMask::initEmpty(), bg, fg, false);
        double prev_lum = 0.0;
        for (size_t i = 232; i < 256; i++) {
            const double lum = palette[i].luminance();
            ASSERT_TRUE(lum >= prev_lum);
            prev_lum = lum;
        }
    }

    /* harmonious=true keeps original order, so the ramp runs white→black
     * (decreasing). */
    {
        const Palette palette =
            generate256Color(default_palette(), PaletteMask::initEmpty(), bg, fg, true);
        double prev_lum = 1.0;
        for (size_t i = 232; i < 256; i++) {
            const double lum = palette[i].luminance();
            ASSERT_TRUE(lum <= prev_lum);
            prev_lum = lum;
        }
    }
}

TEST(color, LAB_toRgb) {
    /* Round-trip: RGB -> LAB -> RGB should recover the original values. */
    const RGB cases[] = {
        rgb(255, 255, 255),
        rgb(0, 0, 0),
        rgb(255, 0, 0),
        rgb(0, 128, 0),
        rgb(0, 0, 255),
        rgb(128, 128, 128),
        rgb(64, 224, 208),
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const RGB expected = cases[i];
        const LAB lab = LAB::fromRgb(expected);
        const RGB actual = lab.toRgb();
        ASSERT_EQ(expected.r, actual.r);
        ASSERT_EQ(expected.g, actual.g);
        ASSERT_EQ(expected.b, actual.b);
    }
}

TEST(color, paletteCvalSlice) {
    /* Every length from empty through a full palette, so that the
     * vectorized groups, the overlapping store bound, and the scalar
     * tail are all exercised at their edges. */
    Palette src;
    for (size_t i = 0; i < 256; i++) {
        src[i] = rgb((uint8_t)(i % 256), (uint8_t)((i * 7 + 1) % 256),
                     (uint8_t)((i * 13 + 2) % 256));
    }

    PaletteC dst;
    for (size_t len = 0; len < 256 + 1; len++) {
        /* Poison the destination so unwritten bytes are detected. */
        memset(&dst, 0xAA, sizeof(dst));

        paletteCvalSlice(src.colors, dst.colors, len);
        for (size_t i = 0; i < len; i++) {
            ASSERT_EQ(src[i].r, dst.colors[i].r);
            ASSERT_EQ(src[i].g, dst.colors[i].g);
            ASSERT_EQ(src[i].b, dst.colors[i].b);
        }

        /* Nothing beyond the requested length may be written. */
        const uint8_t *bytes = (const uint8_t *)&dst;
        for (size_t b = len * 3; b < sizeof(dst); b++) {
            ASSERT_EQ(bytes[b], 0xAA);
        }
    }
}

/* ─── fraction.zig ───────────────────────────────────────────────────────── */

static bool frac_is(const char *s, double want) {
    double got;
    return fraction::parse(s, &got) && got == want;
}

static bool frac_null(const char *s) {
    double got;
    return !fraction::parse(s, &got);
}

TEST(fraction, parse) {
    /* Plain values */
    ASSERT_TRUE(frac_is("0", 0));
    ASSERT_TRUE(frac_is("1", 1));
    ASSERT_TRUE(frac_is("0.5", 0.5));
    ASSERT_TRUE(frac_is("0.25", 0.25));
    ASSERT_TRUE(frac_is("1.0", 1));
    ASSERT_TRUE(frac_is("1.000000", 1));
    ASSERT_TRUE(frac_is("0.0", 0));

    /* Partial forms */
    ASSERT_TRUE(frac_is(".5", 0.5));
    ASSERT_TRUE(frac_is("1.", 1));
    ASSERT_TRUE(frac_is("00.00", 0));

    /* Signs */
    ASSERT_TRUE(frac_is("+0.5", 0.5));
    ASSERT_TRUE(frac_is("-0", 0));
    ASSERT_TRUE(frac_is("-0.000", 0));
    ASSERT_TRUE(frac_null("-0.5"));
    ASSERT_TRUE(frac_null("-1"));

    /* Matches std.fmt.parseFloat rounding for typical inputs.
     * Wisp: strtod is the correctly-rounded parser here. */
    ASSERT_TRUE(frac_is("0.3", strtod("0.3", nullptr)));
    ASSERT_TRUE(frac_is("0.123456789012345", strtod("0.123456789012345", nullptr)));

    /* Digits past the 15th are ignored: long fractions stay finite and
     * equal their 15-digit truncation. */
    {
        double short_;
        ASSERT_TRUE(fraction::parse("0.333333333333333", &short_));
        const std::string long_ = "0." + std::string(400, '3');
        double got;
        ASSERT_TRUE(fraction::parse(long_.c_str(), long_.size(), &got));
        ASSERT_TRUE(got == short_);
    }

    /* Out of range */
    ASSERT_TRUE(frac_null("1.0000001"));
    ASSERT_TRUE(frac_null("2"));
    ASSERT_TRUE(frac_null("255"));
    {
        const std::string ones(400, '1');
        double got;
        ASSERT_FALSE(fraction::parse(ones.c_str(), ones.size(), &got));
    }

    /* Invalid syntax */
    ASSERT_TRUE(frac_null(""));
    ASSERT_TRUE(frac_null("."));
    ASSERT_TRUE(frac_null("+"));
    ASSERT_TRUE(frac_null("-"));
    ASSERT_TRUE(frac_null("+."));
    ASSERT_TRUE(frac_null("abc"));
    ASSERT_TRUE(frac_null("0.5x"));
    ASSERT_TRUE(frac_null("0x0.8"));
    ASSERT_TRUE(frac_null("0..5"));
    ASSERT_TRUE(frac_null("0.5.5"));
    ASSERT_TRUE(frac_null(" 0.5"));
    ASSERT_TRUE(frac_null("0.5 "));
    ASSERT_TRUE(frac_null("1e-1"));
    ASSERT_TRUE(frac_null("nan"));
    ASSERT_TRUE(frac_null("inf"));
}

/* ─── x11_color.zig ──────────────────────────────────────────────────────── */

static bool x11_is(const char *name, RGB want) {
    RGB got;
    return x11_color::map_get(name, &got) && got.eql(want);
}

TEST(x11_color, unnamed) {
    RGB none;
    ASSERT_FALSE(x11_color::map_get("nosuchcolor", &none));
    ASSERT_TRUE(x11_is("white", rgb(255, 255, 255)));
    ASSERT_TRUE(x11_is("medium spring green", rgb(0, 250, 154)));
    ASSERT_TRUE(x11_is("ForestGreen", rgb(34, 139, 34)));
    ASSERT_TRUE(x11_is("FoReStGReen", rgb(34, 139, 34)));
    ASSERT_TRUE(x11_is("black", rgb(0, 0, 0)));
    ASSERT_TRUE(x11_is("red", rgb(255, 0, 0)));
    ASSERT_TRUE(x11_is("green", rgb(0, 255, 0)));
    ASSERT_TRUE(x11_is("blue", rgb(0, 0, 255)));
    ASSERT_TRUE(x11_is("white", rgb(255, 255, 255)));
    ASSERT_TRUE(x11_is("lawngreen", rgb(124, 252, 0)));
    ASSERT_TRUE(x11_is("mediumspringgreen", rgb(0, 250, 154)));
    ASSERT_TRUE(x11_is("forestgreen", rgb(34, 139, 34)));
}

TEST(x11_color, entries) {
    ASSERT_TRUE(x11_color::entries_len() > 700);
    for (size_t i = 0; i < x11_color::entries_len(); i++) {
        const x11_color::Entry &entry = x11_color::entries()[i];
        RGB got;
        ASSERT_TRUE(x11_color::map_get(entry.name, entry.name_len, &got));
        ASSERT_TRUE(entry.color.eql(got));
        ASSERT_EQ(entry.name[entry.name_len], '\0');
    }
    ASSERT_TRUE(strcmp(x11_color::entries()[0].name, "snow") == 0);
}
