/* Tests for src/terminal/style_table.{h,cpp}.
 *
 * The property that matters most here is that interned IDs stay valid across
 * a growth rebuild. Cells all over the scrollback hold IDs; if growth
 * renumbered them, every one of those cells would silently point at the wrong
 * style, and nothing about the rendering path would notice.
 */

#include "test_helpers.h"
#include "style_table.h"

static CellAttr make_attr(uint8_t fg, uint8_t bg) {
    CellAttr a;
    memset(&a, 0, sizeof(a));
    a.fg_idx = fg;
    a.bg_idx = bg;
    return a;
}

static CellAttr make_rgb_attr(uint32_t fg_rgb) {
    CellAttr a;
    memset(&a, 0, sizeof(a));
    a.fg_idx = 0xFF;
    a.fg_rgb = fg_rgb;
    a.bg_idx = 0;
    return a;
}

static bool attr_eq(const CellAttr &a, const CellAttr &b) {
    return a.fg_idx == b.fg_idx && a.bg_idx == b.bg_idx &&
           a.fg_rgb == b.fg_rgb && a.bg_rgb == b.bg_rgb &&
           a.bold == b.bold && a.italic == b.italic &&
           a.underline == b.underline && a.blink == b.blink &&
           a.reverse == b.reverse && a.dim == b.dim &&
           a.strikethrough == b.strikethrough;
}

/* ─── round trip ─────────────────────────────────────────────────────────── */

TEST(styletable, default_attr_interns_to_zero) {
    StyleTable *t = style_table_create();
    ASSERT_NOT_NULL(t);

    /* The default costs no slot, which matters because most cells on a screen
     * are unstyled. */
    const uint16_t id = style_table_intern(t, style_table_default_attr(), NULL);
    ASSERT_EQ(id, 0);
    ASSERT_EQ(style_table_count(t), 0);

    style_table_destroy(t);
}

TEST(styletable, palette_attrs_round_trip) {
    StyleTable *t = style_table_create();

    CellAttr a = make_attr(3, 5);
    a.bold = 1;
    a.italic = 1;

    const uint16_t id = style_table_intern(t, a, NULL);
    ASSERT_TRUE(id != 0);
    ASSERT_TRUE(attr_eq(style_table_resolve(t, id), a));

    style_table_destroy(t);
}

TEST(styletable, truecolor_attrs_round_trip) {
    StyleTable *t = style_table_create();

    CellAttr a;
    memset(&a, 0, sizeof(a));
    a.fg_idx = 0xFF;
    a.fg_rgb = 0x123456;
    a.bg_idx = 0xFF;
    a.bg_rgb = 0xABCDEF;
    a.underline = 1;

    const uint16_t id = style_table_intern(t, a, NULL);
    ASSERT_TRUE(attr_eq(style_table_resolve(t, id), a));

    style_table_destroy(t);
}

TEST(styletable, every_flag_round_trips) {
    /* One flag mapped to the wrong bit would be invisible in ordinary use but
     * wrong on screen, so each is checked on its own. */
    StyleTable *t = style_table_create();

    for (int bit = 0; bit < 7; bit++) {
        CellAttr a = make_attr(7, 0);
        switch (bit) {
            case 0: a.bold = 1; break;
            case 1: a.italic = 1; break;
            case 2: a.underline = 1; break;
            case 3: a.blink = 1; break;
            case 4: a.reverse = 1; break;
            case 5: a.dim = 1; break;
            case 6: a.strikethrough = 1; break;
        }

        const uint16_t id = style_table_intern(t, a, NULL);
        ASSERT_TRUE(id != 0);
        ASSERT_TRUE(attr_eq(style_table_resolve(t, id), a));
    }

    style_table_destroy(t);
}

TEST(styletable, unknown_id_resolves_to_default) {
    StyleTable *t = style_table_create();

    /* Must not read past the end or return garbage. */
    ASSERT_TRUE(attr_eq(style_table_resolve(t, 9999), style_table_default_attr()));
    ASSERT_TRUE(attr_eq(style_table_resolve(t, 0), style_table_default_attr()));

    style_table_destroy(t);
}

/* ─── interning ──────────────────────────────────────────────────────────── */

TEST(styletable, identical_attrs_share_an_id) {
    StyleTable *t = style_table_create();

    CellAttr a = make_attr(1, 2);
    const uint16_t first = style_table_intern(t, a, NULL);
    const uint16_t second = style_table_intern(t, a, NULL);

    ASSERT_EQ(first, second);
    ASSERT_EQ(style_table_count(t), 1);

    style_table_destroy(t);
}

TEST(styletable, different_attrs_get_different_ids) {
    StyleTable *t = style_table_create();

    const uint16_t a = style_table_intern(t, make_attr(1, 0), NULL);
    const uint16_t b = style_table_intern(t, make_attr(2, 0), NULL);

    ASSERT_TRUE(a != b);
    ASSERT_EQ(style_table_count(t), 2);

    style_table_destroy(t);
}

TEST(styletable, release_frees_the_slot) {
    StyleTable *t = style_table_create();

    CellAttr a = make_attr(4, 4);
    const uint16_t id = style_table_intern(t, a, NULL);
    ASSERT_EQ(style_table_count(t), 1);

    style_table_release(t, id);
    ASSERT_EQ(style_table_count(t), 0);

    style_table_destroy(t);
}

TEST(styletable, refcounts_track_multiple_users) {
    StyleTable *t = style_table_create();

    CellAttr a = make_attr(6, 1);
    const uint16_t id = style_table_intern(t, a, NULL);
    style_table_use(t, id);
    style_table_use(t, id);

    /* Three users, one record. */
    ASSERT_EQ(style_table_count(t), 1);

    style_table_release(t, id);
    ASSERT_EQ(style_table_count(t), 1);
    style_table_release(t, id);
    ASSERT_EQ(style_table_count(t), 1);
    style_table_release(t, id);
    ASSERT_EQ(style_table_count(t), 0);

    style_table_destroy(t);
}

TEST(styletable, releasing_zero_is_harmless) {
    StyleTable *t = style_table_create();
    style_table_release(t, 0);
    style_table_use(t, 0);
    ASSERT_EQ(style_table_count(t), 0);
    style_table_destroy(t);
}

/* ─── growth ─────────────────────────────────────────────────────────────── */

TEST(styletable, grows_past_its_initial_capacity) {
    /* Truecolor output emits a distinct style per cell. A fixed table would
     * run out and those cells would lose their styling, so the table has to
     * grow instead. */
    StyleTable *t = style_table_create();
    const uint32_t initial = style_table_capacity(t);

    for (uint32_t i = 0; i < initial * 4; i++) {
        bool ok = false;
        const uint16_t id = style_table_intern(t, make_rgb_attr(i + 1), &ok);
        ASSERT_TRUE(ok);
        ASSERT_TRUE(id != 0);
    }

    ASSERT_TRUE(style_table_capacity(t) > initial);
    ASSERT_EQ(style_table_count(t), initial * 4);

    style_table_destroy(t);
}

TEST(styletable, ids_stay_valid_across_growth) {
    /* The critical property. Every ID handed out before a rebuild must still
     * resolve to the same attributes afterwards, because cells throughout the
     * scrollback are holding those IDs and nothing revisits them. */
    StyleTable *t = style_table_create();
    const uint32_t initial = style_table_capacity(t);

    const uint32_t n = initial * 4;
    uint16_t *ids = (uint16_t *)malloc(n * sizeof(uint16_t));
    ASSERT_NOT_NULL(ids);

    for (uint32_t i = 0; i < n; i++) {
        bool ok = false;
        ids[i] = style_table_intern(t, make_rgb_attr(i + 1), &ok);
        ASSERT_TRUE(ok);
    }

    /* Growth definitely happened. */
    ASSERT_TRUE(style_table_capacity(t) > initial);

    /* Now the real check: every ID, including ones issued before any rebuild,
     * still names the style it was issued for. */
    for (uint32_t i = 0; i < n; i++) {
        const CellAttr got = style_table_resolve(t, ids[i]);
        ASSERT_TRUE(attr_eq(got, make_rgb_attr(i + 1)));
    }

    /* And no two distinct styles collapsed onto one ID. */
    for (uint32_t i = 1; i < n; i++) {
        ASSERT_TRUE(ids[i] != ids[i - 1]);
    }

    free(ids);
    style_table_destroy(t);
}

TEST(styletable, refcounts_survive_growth) {
    /* A rebuild has to carry reference counts over, or styles still in use
     * would be reaped the next time something is released. */
    StyleTable *t = style_table_create();
    const uint32_t initial = style_table_capacity(t);

    CellAttr pinned = make_attr(2, 3);
    const uint16_t pinned_id = style_table_intern(t, pinned, NULL);
    style_table_use(t, pinned_id);
    style_table_use(t, pinned_id);   /* three references */

    /* Force growth. */
    for (uint32_t i = 0; i < initial * 4; i++) {
        style_table_intern(t, make_rgb_attr(i + 1), NULL);
    }
    ASSERT_TRUE(style_table_capacity(t) > initial);

    /* Still the same style, and still held three times. */
    ASSERT_TRUE(attr_eq(style_table_resolve(t, pinned_id), pinned));

    style_table_release(t, pinned_id);
    ASSERT_TRUE(attr_eq(style_table_resolve(t, pinned_id), pinned));
    style_table_release(t, pinned_id);
    ASSERT_TRUE(attr_eq(style_table_resolve(t, pinned_id), pinned));

    /* The third release is the last one. */
    style_table_release(t, pinned_id);
    ASSERT_TRUE(attr_eq(style_table_resolve(t, pinned_id), style_table_default_attr()));

    style_table_destroy(t);
}

TEST(styletable, dedup_still_works_after_growth) {
    StyleTable *t = style_table_create();
    const uint32_t initial = style_table_capacity(t);

    CellAttr early = make_attr(5, 6);
    const uint16_t early_id = style_table_intern(t, early, NULL);

    for (uint32_t i = 0; i < initial * 4; i++) {
        style_table_intern(t, make_rgb_attr(i + 1), NULL);
    }

    /* Re-interning a style from before the rebuild must find the existing
     * record rather than making a second one under a new ID. */
    const uint16_t again = style_table_intern(t, early, NULL);
    ASSERT_EQ(again, early_id);

    style_table_destroy(t);
}
