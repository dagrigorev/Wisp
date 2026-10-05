/* Tests for the bridge between the ported terminal and the renderer's view.
 *
 * Wisp-specific: there is no upstream counterpart. These check that the
 * snapshot vt_pane produces matches the terminal state it was built from. */

#include <string.h>

#include <string>

#include "test_helpers.h"
#include "../terminal/vt_pane.hpp"

using namespace wisp;

typedef wisp::terminal::VtPane VtPane;

struct PaneHolder {
    VtPane p;
    bool ok;
    PaneHolder(unsigned cols, unsigned rows, size_t scrollback_bytes = 65536) {
        ok = p.init(cols, rows, scrollback_bytes);
    }
    ~PaneHolder() {
        if (ok) p.deinit();
    }
};

#define PANE(v, c, r)                                                                              \
    PaneHolder v##_holder((c), (r));                                                               \
    ASSERT_TRUE(v##_holder.ok);                                                                    \
    VtPane &v = v##_holder.p

/* ASSERT_STR_EQ expands to several statements, so a temporary std::string
 * argument would dangle; bind it first. */
#define EXPECT_STR(expected, actual)                                                               \
    do {                                                                                           \
        const std::string _actual = (actual);                                                      \
        ASSERT_STR_EQ(expected, _actual.c_str());                                                  \
    } while (0)

/* The visible text of one row, trailing blanks trimmed. */
static std::string rowText(const ScreenBuffer &view, int row) {
    std::string s;
    for (int x = 0; x < view.cols; x++) {
        const ScreenCell *cell = screen_visible_cell(&view, row, x);
        if (cell == nullptr) break;
        s.push_back(cell->ch < 128 ? (char)cell->ch : '?');
    }
    while (!s.empty() && s[s.size() - 1] == ' ') s.erase(s.size() - 1);
    return s;
}

static std::string g_written;

static void captureWrite(void *, const char *data, size_t len) { g_written.append(data, len); }

TEST(vt_pane, printed_text_appears_in_the_view) {
    PANE(p, 20, 5);

    p.feed("hello", 5);
    EXPECT_STR("hello", rowText(p.view, 0));
    ASSERT_TRUE(5 == p.view.cursor_x);
    ASSERT_TRUE(0 == p.view.cursor_y);
    ASSERT_TRUE(p.view.cursor_visible);
}

TEST(vt_pane, newlines_advance_rows) {
    PANE(p, 20, 5);

    p.feed("one\r\ntwo\r\nthree", 15);
    EXPECT_STR("one", rowText(p.view, 0));
    EXPECT_STR("two", rowText(p.view, 1));
    EXPECT_STR("three", rowText(p.view, 2));
    ASSERT_TRUE(2 == p.view.cursor_y);
}

TEST(vt_pane, styles_are_interned_per_distinct_style) {
    PANE(p, 20, 5);

    /* "ab" plain, "cd" bold: two styles, so two distinct ids. */
    p.feed("ab\x1b[1mcd", 9);
    const ScreenCell *a = screen_visible_cell(&p.view, 0, 0);
    const ScreenCell *c = screen_visible_cell(&p.view, 0, 2);
    ASSERT_TRUE(a != nullptr && c != nullptr);
    ASSERT_TRUE(0 == a->style_id);
    ASSERT_TRUE(c->style_id != a->style_id);

    const CellAttr bold = style_table_resolve(p.view.styles, c->style_id);
    ASSERT_TRUE(bold.bold);
}

TEST(vt_pane, palette_and_rgb_colors_reach_the_view) {
    PANE(p, 20, 5);

    p.feed("\x1b[31mA\x1b[38;2;1;2;3mB", 22);

    const ScreenCell *a = screen_visible_cell(&p.view, 0, 0);
    const ScreenCell *b = screen_visible_cell(&p.view, 0, 1);
    ASSERT_TRUE(a != nullptr && b != nullptr);

    const CellAttr pal = style_table_resolve(p.view.styles, a->style_id);
    ASSERT_TRUE(1 == pal.fg_idx);

    const CellAttr rgb = style_table_resolve(p.view.styles, b->style_id);
    ASSERT_TRUE(0xFF == rgb.fg_idx);
    ASSERT_TRUE(0x010203u == rgb.fg_rgb);
}

TEST(vt_pane, wide_characters_mark_lead_and_tail) {
    PANE(p, 20, 5);

    /* U+26A1 is wide: cell 0 is the lead, cell 1 the spacer tail. */
    p.feed("\xE2\x9A\xA1", 3);
    const ScreenCell *lead = screen_visible_cell(&p.view, 0, 0);
    const ScreenCell *tail = screen_visible_cell(&p.view, 0, 1);
    ASSERT_TRUE(lead != nullptr && tail != nullptr);
    ASSERT_TRUE(lead->wide);
    ASSERT_TRUE(!lead->wide_cont);
    ASSERT_TRUE(tail->wide_cont);
}

TEST(vt_pane, modes_and_title_are_mirrored) {
    PANE(p, 20, 5);

    ASSERT_TRUE(!p.view.bracketed_paste);
    ASSERT_TRUE(!p.view.app_cursor_keys);
    ASSERT_TRUE(!p.view.alt_screen_active);
    ASSERT_TRUE('\0' == p.view.title[0]);

    p.feed("\x1b[?2004h\x1b[?1h\x1b]2;Hi\x1b\\", 21);
    ASSERT_TRUE(p.view.bracketed_paste);
    ASSERT_TRUE(p.view.app_cursor_keys);
    ASSERT_STR_EQ("Hi", p.view.title);

    p.feed("\x1b[?1049h", 8);
    ASSERT_TRUE(p.view.alt_screen_active);
    p.feed("\x1b[?1049l", 8);
    ASSERT_TRUE(!p.view.alt_screen_active);
}

TEST(vt_pane, cursor_visibility_follows_mode_25) {
    PANE(p, 20, 5);

    p.feed("\x1b[?25l", 6);
    ASSERT_TRUE(!p.view.cursor_visible);
    p.feed("\x1b[?25h", 6);
    ASSERT_TRUE(p.view.cursor_visible);
}

TEST(vt_pane, scrollback_grows_and_the_viewport_can_move) {
    PANE(p, 20, 3);

    /* Six lines in a three-row terminal leaves three in scrollback. */
    for (int i = 0; i < 6; i++) {
        char line[8];
        const int n = snprintf(line, sizeof line, "L%d\r\n", i);
        p.feed(line, (size_t)n);
    }

    ASSERT_TRUE(p.view.scrollback_count > 0);
    ASSERT_TRUE(0 == p.view.viewport_offset);

    p.scrollViewport(1);
    ASSERT_TRUE(1 == p.view.viewport_offset);
    /* The cursor row shifts down with the viewport, and off it eventually. */
    p.resetViewport();
    ASSERT_TRUE(0 == p.view.viewport_offset);

    /* Paging back clamps at the oldest row. */
    p.pageViewport(100);
    ASSERT_TRUE(p.view.viewport_offset == screen_max_viewport_offset(&p.view) ||
                p.view.viewport_offset > 0);
    p.resetViewport();
    ASSERT_TRUE(0 == p.view.viewport_offset);
}

TEST(vt_pane, resize_reshapes_the_view) {
    PANE(p, 20, 5);

    p.feed("hello", 5);
    ASSERT_TRUE(p.resize(10, 3));
    ASSERT_TRUE(10 == p.view.cols);
    ASSERT_TRUE(3 == p.view.rows);
    for (int y = 0; y < p.view.rows; y++)
    EXPECT_STR("hello", rowText(p.view, 0));
    ASSERT_TRUE(nullptr == screen_visible_cell(&p.view, 3, 0));
    ASSERT_TRUE(nullptr == screen_visible_cell(&p.view, 0, 10));
}

TEST(vt_pane, replies_go_to_the_write_callback) {
    PANE(p, 20, 5);

    g_written.clear();
    p.ctx = nullptr;
    p.write_fn = &captureWrite;

    /* DSR cursor position report. */
    p.feed("\x1b[6n", 4);
    ASSERT_STR_EQ("\x1b[1;1R", g_written.c_str());
}

TEST(vt_pane, no_write_callback_is_not_a_crash) {
    PANE(p, 20, 5);

    p.feed("\x1b[6n\x1b[c", 7);
    EXPECT_STR("", rowText(p.view, 0));
}

TEST(vt_pane, zero_scrollback_never_scrolls) {
    PaneHolder holder(20, 3, 0);
    ASSERT_TRUE(holder.ok);
    VtPane &p = holder.p;

    for (int i = 0; i < 6; i++) p.feed("x\r\n", 3);
    p.scrollViewport(5);
    ASSERT_TRUE(0 == p.view.viewport_offset);
}

TEST(vt_pane, zz_Wisp_no_leaks) { ASSERT_TRUE(0 == zigstd::testing_state().live); }
