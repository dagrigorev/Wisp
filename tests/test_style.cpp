/* Tests for src/terminal/style.hpp.
 *
 * Related to Ghostty src/terminal/style.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 */

#include "test_helpers.h"
#include "style.hpp"

using namespace wisp::terminal;
using namespace wisp::terminal::style;

struct Backing {
    uint64_t words[4096];
    uint8_t *base() { return reinterpret_cast<uint8_t *>(words); }
    Backing() { memset(words, 0, sizeof(words)); }
};

/* ─── equality ───────────────────────────────────────────────────────────── */

TEST(style, default_style_is_default) {
    Style s;
    ASSERT_TRUE(s.is_default());

    s.flags.bold = true;
    ASSERT_FALSE(s.is_default());
}

TEST(style, equality_covers_every_field) {
    Style a;
    Style b;
    ASSERT_TRUE(a.eql(b));

    /* Each field on its own must break equality, or a style change could be
     * silently interned as an existing one. */
    b = Style(); b.flags.bold = true;          ASSERT_FALSE(a.eql(b));
    b = Style(); b.flags.italic = true;        ASSERT_FALSE(a.eql(b));
    b = Style(); b.flags.faint = true;         ASSERT_FALSE(a.eql(b));
    b = Style(); b.flags.blink = true;         ASSERT_FALSE(a.eql(b));
    b = Style(); b.flags.inverse = true;       ASSERT_FALSE(a.eql(b));
    b = Style(); b.flags.invisible = true;     ASSERT_FALSE(a.eql(b));
    b = Style(); b.flags.strikethrough = true; ASSERT_FALSE(a.eql(b));
    b = Style(); b.flags.overline = true;      ASSERT_FALSE(a.eql(b));

    b = Style(); b.flags.underline = Underline::curly;    ASSERT_FALSE(a.eql(b));
    b = Style(); b.fg_color = StyleColor::from_palette(1); ASSERT_FALSE(a.eql(b));
    b = Style(); b.bg_color = StyleColor::from_palette(1); ASSERT_FALSE(a.eql(b));
    b = Style(); b.underline_color = StyleColor::from_rgb(RGB(1, 2, 3));
    ASSERT_FALSE(a.eql(b));
}

TEST(style, color_equality_by_tag_and_value) {
    ASSERT_TRUE(StyleColor::none_color().eql(StyleColor::none_color()));

    ASSERT_TRUE(StyleColor::from_palette(5).eql(StyleColor::from_palette(5)));
    ASSERT_FALSE(StyleColor::from_palette(5).eql(StyleColor::from_palette(6)));

    ASSERT_TRUE(StyleColor::from_rgb(RGB(1, 2, 3)).eql(StyleColor::from_rgb(RGB(1, 2, 3))));
    ASSERT_FALSE(StyleColor::from_rgb(RGB(1, 2, 3)).eql(StyleColor::from_rgb(RGB(1, 2, 4))));

    /* Same numeric value, different source, must not be equal: a palette
     * color follows palette changes and an RGB one does not. */
    ASSERT_FALSE(StyleColor::from_palette(1).eql(StyleColor::from_rgb(RGB(1, 0, 0))));
}

/* ─── canonical form ─────────────────────────────────────────────────────── */

TEST(style, canonical_ignores_fields_the_tag_does_not_select) {
    /* This is the invariant interning depends on. A palette color carries an
     * rgb field that means nothing; if it leaked into the image, two equal
     * styles would hash differently and be stored twice. */
    Style a;
    a.fg_color = StyleColor::from_palette(3);

    Style b;
    b.fg_color = StyleColor::from_palette(3);
    b.fg_color.rgb = RGB(9, 9, 9);   /* stale, not selected by the tag */

    ASSERT_TRUE(a.eql(b));

    uint8_t ca[STYLE_CANONICAL_SIZE], cb[STYLE_CANONICAL_SIZE];
    a.canonical(ca);
    b.canonical(cb);
    ASSERT_EQ(memcmp(ca, cb, STYLE_CANONICAL_SIZE), 0);
    ASSERT_TRUE(a.hash() == b.hash());
}

TEST(style, canonical_ignores_palette_byte_for_rgb_colors) {
    Style a;
    a.bg_color = StyleColor::from_rgb(RGB(1, 2, 3));

    Style b;
    b.bg_color = StyleColor::from_rgb(RGB(1, 2, 3));
    b.bg_color.palette = 200;   /* stale */

    ASSERT_TRUE(a.eql(b));

    uint8_t ca[STYLE_CANONICAL_SIZE], cb[STYLE_CANONICAL_SIZE];
    a.canonical(ca);
    b.canonical(cb);
    ASSERT_EQ(memcmp(ca, cb, STYLE_CANONICAL_SIZE), 0);
}

TEST(style, canonical_distinguishes_different_styles) {
    Style a;
    Style b;
    b.flags.bold = true;

    uint8_t ca[STYLE_CANONICAL_SIZE], cb[STYLE_CANONICAL_SIZE];
    a.canonical(ca);
    b.canonical(cb);
    ASSERT_TRUE(memcmp(ca, cb, STYLE_CANONICAL_SIZE) != 0);
}

TEST(style, underline_style_is_in_the_canonical_image) {
    /* Underline is three bits sharing a byte with the booleans, which is the
     * easiest place for a packing mistake to hide. */
    Style prev;
    for (int i = 1; i <= 5; i++) {
        Style s;
        s.flags.underline = (Underline)i;

        uint8_t c1[STYLE_CANONICAL_SIZE], c2[STYLE_CANONICAL_SIZE];
        s.canonical(c1);
        prev.canonical(c2);
        ASSERT_TRUE(memcmp(c1, c2, STYLE_CANONICAL_SIZE) != 0);
        prev = s;
    }
}

TEST(style, underline_does_not_collide_with_boolean_flags) {
    /* If underline were shifted wrong it could alias a boolean. */
    Style u;
    u.flags.underline = Underline::single;

    Style b;
    b.flags.bold = true;

    ASSERT_FALSE(u.eql(b));
    ASSERT_TRUE(u.hash() != b.hash());
}

TEST(style, equal_styles_hash_equal) {
    Style a;
    a.flags.bold = true;
    a.flags.underline = Underline::dotted;
    a.fg_color = StyleColor::from_rgb(RGB(10, 20, 30));
    a.bg_color = StyleColor::from_palette(7);

    Style b = a;
    ASSERT_TRUE(a.eql(b));
    ASSERT_TRUE(a.hash() == b.hash());
}

/* ─── foreground resolution ──────────────────────────────────────────────── */

TEST(style, fg_none_uses_default) {
    Palette p = default_palette();
    Style s;

    Style::FgOptions o;
    o.def = RGB(1, 2, 3);
    o.palette = &p;

    ASSERT_TRUE(s.fg(o).eql(RGB(1, 2, 3)));
}

TEST(style, fg_palette_index) {
    Palette p = default_palette();
    Style s;
    s.fg_color = StyleColor::from_palette(4);

    Style::FgOptions o;
    o.palette = &p;

    ASSERT_TRUE(s.fg(o).eql(p[4]));
}

TEST(style, fg_bold_brightens_the_first_eight_palette_colors) {
    Palette p = default_palette();

    Style s;
    s.fg_color = StyleColor::from_palette(2);
    s.flags.bold = true;

    Style::FgOptions o;
    o.palette = &p;
    o.has_bold = true;
    o.bold_bright = true;

    /* 2 promotes to 10. */
    ASSERT_TRUE(s.fg(o).eql(p[10]));
}

TEST(style, fg_bold_leaves_already_bright_colors_alone) {
    Palette p = default_palette();

    Style s;
    s.fg_color = StyleColor::from_palette(12);
    s.flags.bold = true;

    Style::FgOptions o;
    o.palette = &p;
    o.has_bold = true;
    o.bold_bright = true;

    ASSERT_TRUE(s.fg(o).eql(p[12]));
}

TEST(style, fg_bold_color_override_applies_to_default_colored_text) {
    Palette p = default_palette();

    Style s;
    s.flags.bold = true;

    Style::FgOptions o;
    o.def = RGB(50, 50, 50);
    o.palette = &p;
    o.has_bold = true;
    o.bold_bright = false;
    o.bold_color = RGB(200, 0, 0);

    ASSERT_TRUE(s.fg(o).eql(RGB(200, 0, 0)));
}

TEST(style, fg_rgb_is_left_alone_unless_it_matches_the_default) {
    Palette p = default_palette();

    Style::FgOptions o;
    o.def = RGB(50, 50, 50);
    o.palette = &p;
    o.has_bold = true;
    o.bold_bright = false;
    o.bold_color = RGB(200, 0, 0);

    /* An explicitly colored bold cell keeps its color. */
    Style explicit_color;
    explicit_color.flags.bold = true;
    explicit_color.fg_color = StyleColor::from_rgb(RGB(1, 2, 3));
    ASSERT_TRUE(explicit_color.fg(o).eql(RGB(1, 2, 3)));

    /* One that happens to equal the default follows the bold override. */
    Style matches_default;
    matches_default.flags.bold = true;
    matches_default.fg_color = StyleColor::from_rgb(RGB(50, 50, 50));
    ASSERT_TRUE(matches_default.fg(o).eql(RGB(200, 0, 0)));
}

TEST(style, underline_color_resolution) {
    Palette p = default_palette();
    RGB out;

    Style none_set;
    ASSERT_FALSE(none_set.underline_rgb(&p, &out));

    Style pal;
    pal.underline_color = StyleColor::from_palette(9);
    ASSERT_TRUE(pal.underline_rgb(&p, &out));
    ASSERT_TRUE(out.eql(p[9]));

    Style rgb;
    rgb.underline_color = StyleColor::from_rgb(RGB(7, 8, 9));
    ASSERT_TRUE(rgb.underline_rgb(&p, &out));
    ASSERT_TRUE(out.eql(RGB(7, 8, 9)));
}

/* ─── interning ──────────────────────────────────────────────────────────── */

TEST(style, interning_dedups_equal_styles) {
    /* The integration that matters: equal styles must collapse to one ID, or
     * a page would burn a style slot per cell. */
    Backing b;
    Set::Layout l = Set::Layout::init(64);
    Set set = Set::init(OffsetBuf::init(b.base()), l, Context());

    Style s;
    s.flags.bold = true;
    s.fg_color = StyleColor::from_palette(3);

    Id a = 0, c = 0;
    ASSERT_TRUE(set.add(b.base(), s, &a) == AddResult::ok);
    ASSERT_TRUE(set.add(b.base(), s, &c) == AddResult::ok);

    ASSERT_EQ(a, c);
    ASSERT_EQ(set.count(), 1);
    ASSERT_EQ(set.ref_count(b.base(), a), 2);
    ASSERT_TRUE(set.check_integrity(b.base()));
    ASSERT_TRUE(set.check_reachable(b.base()));
}

TEST(style, interning_separates_different_styles) {
    Backing b;
    Set::Layout l = Set::Layout::init(64);
    Set set = Set::init(OffsetBuf::init(b.base()), l, Context());

    Style bold;
    bold.flags.bold = true;

    Style italic;
    italic.flags.italic = true;

    Id a = 0, c = 0;
    ASSERT_TRUE(set.add(b.base(), bold, &a) == AddResult::ok);
    ASSERT_TRUE(set.add(b.base(), italic, &c) == AddResult::ok);

    ASSERT_TRUE(a != c);
    ASSERT_EQ(set.count(), 2);
    ASSERT_TRUE(set.check_reachable(b.base()));
}

TEST(style, interned_style_round_trips) {
    Backing b;
    Set::Layout l = Set::Layout::init(64);
    Set set = Set::init(OffsetBuf::init(b.base()), l, Context());

    Style s;
    s.flags.underline = Underline::curly;
    s.underline_color = StyleColor::from_rgb(RGB(1, 2, 3));
    s.bg_color = StyleColor::from_palette(200);

    Id id = 0;
    ASSERT_TRUE(set.add(b.base(), s, &id) == AddResult::ok);

    Style *got = set.get(b.base(), id);
    ASSERT_NOT_NULL(got);
    ASSERT_TRUE(got->eql(s));
    ASSERT_TRUE(got->flags.underline == Underline::curly);
    ASSERT_EQ(got->bg_color.palette, 200);
}

TEST(style, releasing_a_style_frees_it_for_reuse) {
    Backing b;
    Set::Layout l = Set::Layout::init(64);
    Set set = Set::init(OffsetBuf::init(b.base()), l, Context());

    Style s;
    s.flags.bold = true;

    Id id = 0;
    ASSERT_TRUE(set.add(b.base(), s, &id) == AddResult::ok);
    set.release(b.base(), id);
    ASSERT_EQ(set.count(), 0);

    /* Re-adding resurrects the same record. */
    Id again = 0;
    ASSERT_TRUE(set.add(b.base(), s, &again) == AddResult::ok);
    ASSERT_EQ(again, id);
    ASSERT_TRUE(set.check_reachable(b.base()));
}

TEST(style, many_distinct_styles_stay_reachable) {
    Backing b;
    Set::Layout l = Set::Layout::init(128);
    Set set = Set::init(OffsetBuf::init(b.base()), l, Context());

    Id ids[40];
    Style styles[40];
    for (int i = 0; i < 40; i++) {
        styles[i].fg_color = StyleColor::from_rgb(RGB((uint8_t)i, (uint8_t)(i * 3), 0));
        styles[i].flags.bold = (i % 2) == 0;
        styles[i].flags.underline = (Underline)(i % 6);
        ASSERT_TRUE(set.add(b.base(), styles[i], &ids[i]) == AddResult::ok);
    }

    ASSERT_TRUE(set.check_integrity(b.base()));
    ASSERT_TRUE(set.check_reachable(b.base()));

    for (int i = 0; i < 40; i++) {
        ASSERT_EQ(set.lookup(b.base(), styles[i]), ids[i]);
    }
}
