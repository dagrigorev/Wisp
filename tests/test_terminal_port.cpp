/* Tests for src/terminal/terminal.hpp.
 *
 * Related to Ghostty src/terminal/Terminal.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * A screen has no opinions: it does not know that a line feed differs from
 * moving down, that a tab goes somewhere particular, or that DECAWM exists.
 * Those are properties of the machine being emulated, and these tests are
 * about them.
 *
 * Named test_terminal_port to keep it apart from the tests for the live C
 * emulator this does not yet replace.
 */

#include "test_helpers.h"
#include "terminal.hpp"

using namespace wisp::terminal;

static void write_text(Terminal *t, const char *text) {
    for (const char *c = text; *c; c++) {
        ASSERT_TRUE(terminal_print(t, (uint32_t)(unsigned char)*c, 1));
    }
}

static void read_row(Terminal *t, CellCountInt y, char *out, size_t out_len) {
    size_t n = 0;
    for (CellCountInt x = 0; x < t->cols && n + 1 < out_len; x++) {
        Cell *c = screen_cell(t->active, x, y);
        const uint32_t cp = c ? c->codepoint() : 0;
        out[n++] = cp ? (char)cp : ' ';
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = '\0';
}

/* ─── setting up ─────────────────────────────────────────────────────────── */

TEST(terminal, starts_on_the_primary_screen) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 5, 0));

    ASSERT_TRUE(t.active == &t.primary);
    ASSERT_FALSE(t.modes.alt_screen);
    ASSERT_TRUE(t.modes.wraparound);
    ASSERT_EQ(t.active->cursor.x, 0);

    terminal_deinit(&t);
}

TEST(terminal, both_screens_exist_from_the_start) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 5, 0));

    /* Making the alternate one on demand would mean an allocation failing in
     * the middle of an escape sequence. */
    ASSERT_EQ(t.alternate.pages.cols, 20);
    ASSERT_TRUE(t.alternate.pages.no_scrollback);
    ASSERT_FALSE(t.primary.pages.no_scrollback);

    terminal_deinit(&t);
}

/* ─── printing ───────────────────────────────────────────────────────────── */

TEST(terminal, prints_to_the_active_screen) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 5, 0));
    write_text(&t, "hello");

    char got[32];
    read_row(&t, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "hello") == 0);

    terminal_deinit(&t);
}

TEST(terminal, wraparound_off_keeps_the_cursor_at_the_margin) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 5, 4, 0));

    t.modes.wraparound = false;
    terminal_apply_modes(&t);
    write_text(&t, "abcdefg");

    /* The mode lives here now and the screen is told; it is not something a
     * caller sets on the screen behind the terminal's back. */
    char got[32];
    read_row(&t, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "abcdg") == 0);
    ASSERT_EQ(t.active->cursor.y, 0);

    terminal_deinit(&t);
}

TEST(terminal, insert_mode_pushes_the_line_right) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 4, 0));
    write_text(&t, "abcdef");

    ASSERT_TRUE(screen_cursor_absolute(t.active, 2, 0));
    t.modes.insert = true;
    write_text(&t, "XY");

    char got[32];
    read_row(&t, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "abXYcdef") == 0);

    terminal_deinit(&t);
}

TEST(terminal, insert_mode_drops_what_falls_off_the_end) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 6, 4, 0));
    write_text(&t, "abcdef");

    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 0));
    t.modes.insert = true;
    write_text(&t, "Z");

    char got[32];
    read_row(&t, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "Zabcde") == 0);

    terminal_deinit(&t);
}

/* ─── the C0 controls ────────────────────────────────────────────────────── */

TEST(terminal, carriage_return_stays_on_its_row) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 5, 0));
    write_text(&t, "abc");

    terminal_carriage_return(&t);
    ASSERT_EQ(t.active->cursor.x, 0);
    ASSERT_EQ(t.active->cursor.y, 0);

    terminal_deinit(&t);
}

TEST(terminal, a_line_feed_keeps_the_column) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 5, 0));
    write_text(&t, "abc");

    /* What distinguishes a line feed from a new line: the cursor lands
     * directly below where it was, and it is the CR that brings it back. */
    ASSERT_TRUE(terminal_linefeed(&t));
    ASSERT_EQ(t.active->cursor.x, 3);
    ASSERT_EQ(t.active->cursor.y, 1);

    terminal_deinit(&t);
}

TEST(terminal, a_line_feed_at_the_bottom_scrolls) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 3, 0));

    write_text(&t, "one");
    ASSERT_TRUE(terminal_linefeed(&t));
    terminal_carriage_return(&t);
    write_text(&t, "two");
    ASSERT_TRUE(terminal_linefeed(&t));
    terminal_carriage_return(&t);
    write_text(&t, "three");
    ASSERT_TRUE(terminal_linefeed(&t));
    terminal_carriage_return(&t);
    write_text(&t, "four");

    char got[32];
    read_row(&t, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "two") == 0);
    read_row(&t, 2, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "four") == 0);

    /* And "one" went to the scrollback rather than nowhere. */
    Pin old = page_list_pin(&t.active->pages, 0);
    old.x = 0;
    ASSERT_EQ(old.cell()->codepoint(), 'o');

    terminal_deinit(&t);
}

TEST(terminal, backspace_stops_at_the_left_margin) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 5, 0));

    terminal_backspace(&t);
    ASSERT_EQ(t.active->cursor.x, 0);

    write_text(&t, "ab");
    terminal_backspace(&t);
    ASSERT_EQ(t.active->cursor.x, 1);

    terminal_deinit(&t);
}

TEST(terminal, reverse_index_moves_up) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 5, 0));
    ASSERT_TRUE(screen_cursor_absolute(t.active, 4, 3));

    ASSERT_TRUE(terminal_reverse_index(&t));
    ASSERT_EQ(t.active->cursor.y, 2);
    ASSERT_EQ(t.active->cursor.x, 4);

    terminal_deinit(&t);
}

TEST(terminal, reverse_index_at_the_top_scrolls_down) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 3, 0));

    write_text(&t, "one");
    ASSERT_TRUE(terminal_linefeed(&t));
    terminal_carriage_return(&t);
    write_text(&t, "two");
    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 0));

    ASSERT_TRUE(terminal_reverse_index(&t));

    /* A blank line appears above, which is what lets a program insert a line
     * at the top without redrawing everything below it. */
    char got[32];
    read_row(&t, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "") == 0);
    read_row(&t, 1, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "one") == 0);
    read_row(&t, 2, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "two") == 0);

    terminal_deinit(&t);
}

TEST(terminal, reverse_index_does_not_make_scrollback) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 3, 0));
    write_text(&t, "bottom");
    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 2));
    write_text(&t, "gone");
    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 0));

    const size_t before = t.active->pages.row_count;
    ASSERT_TRUE(terminal_reverse_index(&t));

    /* The bottom row is lost rather than saved: it never left the top of the
     * screen, so it was never scrollback. */
    ASSERT_EQ(t.active->pages.row_count, before);
    char got[32];
    read_row(&t, 2, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "") == 0);

    terminal_deinit(&t);
}

/* ─── tab stops ──────────────────────────────────────────────────────────── */

TEST(tabs, default_to_every_eighth_column) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 30, 4, 0));

    terminal_horizontal_tab(&t, 1);
    ASSERT_EQ(t.active->cursor.x, 8);
    terminal_horizontal_tab(&t, 1);
    ASSERT_EQ(t.active->cursor.x, 16);

    terminal_deinit(&t);
}

TEST(tabs, a_tab_moves_without_writing) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 30, 4, 0));
    write_text(&t, "0123456789");
    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 0));

    terminal_horizontal_tab(&t, 1);

    /* A tab moves the cursor; it does not write spaces. Anything it passes
     * over is still there, which is how a program can tab back over its own
     * output. */
    char got[32];
    read_row(&t, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "0123456789") == 0);
    ASSERT_EQ(t.active->cursor.x, 8);

    terminal_deinit(&t);
}

TEST(tabs, stop_at_the_last_column_when_there_are_no_more) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 4, 0));
    ASSERT_TRUE(screen_cursor_absolute(t.active, 17, 0));

    terminal_horizontal_tab(&t, 1);
    ASSERT_EQ(t.active->cursor.x, 19);
    terminal_horizontal_tab(&t, 1);
    ASSERT_EQ(t.active->cursor.x, 19);

    terminal_deinit(&t);
}

TEST(tabs, several_at_once) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 40, 4, 0));

    terminal_horizontal_tab(&t, 3);
    ASSERT_EQ(t.active->cursor.x, 24);

    terminal_deinit(&t);
}

TEST(tabs, setting_one_where_the_cursor_is) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 30, 4, 0));

    ASSERT_TRUE(screen_cursor_absolute(t.active, 3, 0));
    terminal_tab_set(&t);
    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 0));

    terminal_horizontal_tab(&t, 1);
    ASSERT_EQ(t.active->cursor.x, 3);

    terminal_deinit(&t);
}

TEST(tabs, clearing_one_and_clearing_all) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 30, 4, 0));

    ASSERT_TRUE(screen_cursor_absolute(t.active, 8, 0));
    terminal_tab_clear(&t, 0);
    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 0));
    terminal_horizontal_tab(&t, 1);
    ASSERT_EQ(t.active->cursor.x, 16);

    terminal_tab_clear(&t, 3);
    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 0));
    terminal_horizontal_tab(&t, 1);
    ASSERT_EQ(t.active->cursor.x, 29);

    terminal_deinit(&t);
}

TEST(tabs, going_backwards) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 30, 4, 0));
    ASSERT_TRUE(screen_cursor_absolute(t.active, 20, 0));

    terminal_reverse_tab(&t, 1);
    ASSERT_EQ(t.active->cursor.x, 16);
    terminal_reverse_tab(&t, 2);
    ASSERT_EQ(t.active->cursor.x, 0);

    terminal_deinit(&t);
}

/* ─── the alternate screen ───────────────────────────────────────────────── */

TEST(alt, switching_shows_a_blank_screen) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 4, 0));
    write_text(&t, "shell output");

    terminal_alt_screen_enter(&t);

    ASSERT_TRUE(t.active == &t.alternate);
    ASSERT_TRUE(t.modes.alt_screen);
    ASSERT_EQ(t.active->cursor.x, 0);
    ASSERT_EQ(t.active->cursor.y, 0);

    char got[32];
    read_row(&t, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "") == 0);

    terminal_deinit(&t);
}

TEST(alt, the_primary_screen_is_untouched) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 4, 0));
    write_text(&t, "shell output");

    terminal_alt_screen_enter(&t);
    write_text(&t, "editor");
    terminal_alt_screen_leave(&t);

    /* The whole point: what was on the screen before is still there, which is
     * why a shell prompt survives running an editor. */
    ASSERT_TRUE(t.active == &t.primary);
    char got[32];
    read_row(&t, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "shell output") == 0);

    terminal_deinit(&t);
}

TEST(alt, the_cursor_comes_back_where_it_was) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 4, 0));
    ASSERT_TRUE(screen_cursor_absolute(t.active, 7, 2));

    terminal_alt_screen_enter(&t);
    ASSERT_TRUE(screen_cursor_absolute(t.active, 1, 1));
    terminal_alt_screen_leave(&t);

    /* Saving the cursor on the way in is what ?1049 does and ?47 did not,
     * and it is why ?1049 is the one everything uses. */
    ASSERT_EQ(t.active->cursor.x, 7);
    ASSERT_EQ(t.active->cursor.y, 2);

    terminal_deinit(&t);
}

TEST(alt, is_cleared_each_time_it_is_entered) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 4, 0));

    terminal_alt_screen_enter(&t);
    write_text(&t, "first run");
    terminal_alt_screen_leave(&t);

    terminal_alt_screen_enter(&t);
    char got[32];
    read_row(&t, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "") == 0);

    terminal_deinit(&t);
}

TEST(alt, keeps_no_scrollback) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 4, 0));
    terminal_alt_screen_enter(&t);

    /* Scrollback here would be a record of half-drawn frames rather than of
     * output, since the programs that use it redraw whenever they like. */
    for (int i = 0; i < 50; i++) {
        ASSERT_TRUE(screen_cursor_absolute(t.active, 0, t.active->cursor.y));
        ASSERT_TRUE(terminal_linefeed(&t));
    }

    ASSERT_EQ(page_list_max_scroll(&t.active->pages), 0u);
    ASSERT_EQ(t.active->pages.row_count, 4u);

    terminal_deinit(&t);
}

TEST(alt, the_primary_screen_keeps_its_scrollback) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 3, 0));
    for (int i = 0; i < 10; i++) {
        ASSERT_TRUE(screen_cursor_absolute(&t.primary, 0, t.primary.cursor.y));
        ASSERT_TRUE(terminal_linefeed(&t));
    }
    const size_t before = page_list_max_scroll(&t.primary.pages);
    ASSERT_TRUE(before > 0u);

    terminal_alt_screen_enter(&t);
    for (int i = 0; i < 20; i++) {
        ASSERT_TRUE(screen_cursor_absolute(t.active, 0, t.active->cursor.y));
        ASSERT_TRUE(terminal_linefeed(&t));
    }
    terminal_alt_screen_leave(&t);

    ASSERT_EQ(page_list_max_scroll(&t.primary.pages), before);

    terminal_deinit(&t);
}

TEST(alt, entering_twice_is_not_two_saves) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 4, 0));
    ASSERT_TRUE(screen_cursor_absolute(t.active, 5, 1));

    terminal_alt_screen_enter(&t);
    terminal_alt_screen_enter(&t);
    terminal_alt_screen_leave(&t);

    ASSERT_EQ(t.active->cursor.x, 5);
    ASSERT_EQ(t.active->cursor.y, 1);

    terminal_deinit(&t);
}

TEST(alt, leaving_when_not_there_does_nothing) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 4, 0));
    write_text(&t, "text");

    terminal_alt_screen_leave(&t);

    ASSERT_TRUE(t.active == &t.primary);
    ASSERT_EQ(t.active->cursor.x, 4);

    terminal_deinit(&t);
}

/* ─── resizing ───────────────────────────────────────────────────────────── */

TEST(terminal, resizing_touches_both_screens) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 4, 0));
    write_text(&t, "primary text");

    terminal_alt_screen_enter(&t);
    ASSERT_TRUE(terminal_resize(&t, 9, 6));

    /* The screen that is not showing is resized too: a program on the
     * alternate screen expects the shell underneath to know the new size by
     * the time it exits. */
    ASSERT_EQ(t.cols, 9);
    ASSERT_EQ(t.primary.pages.cols, 9);
    ASSERT_EQ(t.alternate.pages.cols, 9);

    terminal_alt_screen_leave(&t);
    char got[32];
    read_row(&t, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "primary t") == 0);

    terminal_deinit(&t);
}

TEST(terminal, resizing_resets_the_tab_stops) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 40, 4, 0));
    ASSERT_TRUE(screen_cursor_absolute(t.active, 3, 0));
    terminal_tab_set(&t);

    ASSERT_TRUE(terminal_resize(&t, 20, 4));

    /* A stop is a column number, and the columns have changed. */
    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 0));
    terminal_horizontal_tab(&t, 1);
    ASSERT_EQ(t.active->cursor.x, 8);

    terminal_deinit(&t);
}

TEST(terminal, resizing_to_nothing_is_refused) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 4, 0));

    ASSERT_FALSE(terminal_resize(&t, 0, 4));
    ASSERT_FALSE(terminal_resize(&t, 20, 0));
    ASSERT_EQ(t.cols, 20);

    terminal_deinit(&t);
}

/* ─── scroll regions ─────────────────────────────────────────────────────── */

/* Put a letter per row so a scroll can be read off. */
static void label(Terminal *t, CellCountInt rows) {
    for (CellCountInt y = 0; y < rows; y++) {
        ASSERT_TRUE(screen_cursor_absolute(t->active, 0, y));
        ASSERT_TRUE(terminal_print(t, (uint32_t)('A' + y), 1));
    }
}

static bool rows_read(Terminal *t, const char *want) {
    for (CellCountInt y = 0; want[y]; y++) {
        Cell *c = screen_cell(t->active, 0, y);
        const uint32_t got = c ? c->codepoint() : 0xFFFF;
        const uint32_t expect = want[y] == '.' ? 0 : (uint32_t)want[y];
        if (got != expect) return false;
    }
    return true;
}

TEST(region, defaults_to_the_whole_screen) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 5, 0));

    ASSERT_EQ(t.scroll_top, 0);
    ASSERT_EQ(t.scroll_bot, 4);
    ASSERT_TRUE(terminal_region_is_whole_screen(&t));

    terminal_deinit(&t);
}

TEST(region, setting_one_homes_the_cursor) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 6, 0));
    ASSERT_TRUE(screen_cursor_absolute(t.active, 5, 4));

    ASSERT_TRUE(terminal_set_scroll_region(&t, 1, 4));

    ASSERT_EQ(t.scroll_top, 1);
    ASSERT_EQ(t.scroll_bot, 4);
    /* Programs set a region and then draw from the top without a separate
     * positioning sequence. */
    ASSERT_EQ(t.active->cursor.x, 0);
    ASSERT_EQ(t.active->cursor.y, 0);

    terminal_deinit(&t);
}

TEST(region, a_nonsense_region_is_ignored) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 6, 0));
    ASSERT_TRUE(terminal_set_scroll_region(&t, 1, 4));

    /* Ignored entirely rather than clamped, which is what real terminals
     * do. */
    ASSERT_FALSE(terminal_set_scroll_region(&t, 4, 4));
    ASSERT_FALSE(terminal_set_scroll_region(&t, 5, 2));
    ASSERT_EQ(t.scroll_top, 1);
    ASSERT_EQ(t.scroll_bot, 4);

    terminal_deinit(&t);
}

TEST(region, a_line_feed_inside_a_region_scrolls_only_it) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 6, 0));
    label(&t, 6);

    ASSERT_TRUE(terminal_set_scroll_region(&t, 2, 4));
    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 4));
    ASSERT_TRUE(terminal_linefeed(&t));

    /* Rows outside the region do not move, which is the whole point of a
     * region: a status line stays still while the rest scrolls. */
    ASSERT_TRUE(rows_read(&t, "ABDE.F"));
    ASSERT_EQ(t.active->cursor.y, 4);

    terminal_deinit(&t);
}

TEST(region, a_line_feed_inside_a_region_makes_no_scrollback) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 6, 0));
    label(&t, 6);
    ASSERT_TRUE(terminal_set_scroll_region(&t, 1, 4));
    const size_t before = t.active->pages.row_count;

    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 4));
    ASSERT_TRUE(terminal_linefeed(&t));

    /* The row that left the top of the region has not left the screen — the
     * rows below the region are still showing. Keeping it would fill the
     * history with the middle frames of a progress bar. */
    ASSERT_EQ(t.active->pages.row_count, before);
    ASSERT_EQ(page_list_max_scroll(&t.active->pages), 0u);

    terminal_deinit(&t);
}

TEST(region, a_line_feed_with_the_whole_screen_still_makes_scrollback) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 4, 0));
    label(&t, 4);

    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 3));
    ASSERT_TRUE(terminal_linefeed(&t));

    /* The distinction the whole thing turns on: with nothing set, the row
     * leaving the top is the oldest thing shown and belongs in the
     * history. */
    ASSERT_EQ(page_list_max_scroll(&t.active->pages), 1u);
    Pin old = page_list_pin(&t.active->pages, 0);
    old.x = 0;
    ASSERT_EQ(old.cell()->codepoint(), 'A');

    terminal_deinit(&t);
}

TEST(region, a_line_feed_below_the_region_just_moves) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 6, 0));
    label(&t, 6);
    ASSERT_TRUE(terminal_set_scroll_region(&t, 0, 3));

    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 4));
    ASSERT_TRUE(terminal_linefeed(&t));

    /* A cursor left outside the region is not scrolling anything. */
    ASSERT_TRUE(rows_read(&t, "ABCDEF"));
    ASSERT_EQ(t.active->cursor.y, 5);

    terminal_deinit(&t);
}

TEST(region, reverse_index_at_the_top_of_a_region) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 6, 0));
    label(&t, 6);

    ASSERT_TRUE(terminal_set_scroll_region(&t, 2, 5));
    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 2));
    ASSERT_TRUE(terminal_reverse_index(&t));

    /* The region's rows move down, its last row falls off, and rows outside
     * it are untouched. */
    ASSERT_TRUE(rows_read(&t, "AB.CDE"));
    ASSERT_EQ(t.active->cursor.y, 2);

    terminal_deinit(&t);
}

/* ─── the page boundary ──────────────────────────────────────────────────── */

TEST(region, scrolling_works_across_a_page_boundary) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 6, 0));

    /* Push the screen across a page seam by filling the first page. Rows
     * inside a page move by swapping handles; rows spanning pages have to be
     * copied, and both must produce the same thing. */
    const CellCountInt per_page = t.primary.pages.first->page.capacity.rows;
    for (CellCountInt i = 0; i < (CellCountInt)(per_page - 3); i++) {
        ASSERT_TRUE(screen_cursor_absolute(t.active, 0, t.rows - 1));
        ASSERT_TRUE(terminal_linefeed(&t));
    }

    label(&t, 6);
    Pin a = page_list_active_pin(&t.active->pages, 0, 0);
    Pin b = page_list_active_pin(&t.active->pages, 0, 5);
    ASSERT_TRUE(a.node != b.node);

    ASSERT_TRUE(terminal_set_scroll_region(&t, 1, 5));
    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 5));
    ASSERT_TRUE(terminal_linefeed(&t));

    ASSERT_TRUE(rows_read(&t, "ACDEF."));

    terminal_deinit(&t);
}

TEST(region, styles_survive_a_scroll_across_a_page_boundary) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 6, 0));

    const CellCountInt per_page = t.primary.pages.first->page.capacity.rows;
    for (CellCountInt i = 0; i < (CellCountInt)(per_page - 3); i++) {
        ASSERT_TRUE(screen_cursor_absolute(t.active, 0, t.rows - 1));
        ASSERT_TRUE(terminal_linefeed(&t));
    }

    /* Copying a row between pages re-interns everything it carries, so the
     * style has to come out the other side even though its ID will not. */
    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 5));
    t.active->cursor.style.fg_color.tag = style::StyleColor::Tag::palette;
    t.active->cursor.style.fg_color.palette = 33;
    ASSERT_TRUE(terminal_print(&t, 'z', 1));
    t.active->cursor.style = style::Style();

    ASSERT_TRUE(terminal_set_scroll_region(&t, 1, 5));
    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 5));
    ASSERT_TRUE(terminal_linefeed(&t));

    Pin moved = page_list_active_pin(&t.active->pages, 0, 4);
    ASSERT_EQ(moved.node->page.get_cell(0, moved.y)->codepoint(), 'z');
    ASSERT_EQ(moved.node->page.get_cell_style(0, moved.y).fg_color.palette, 33);

    terminal_deinit(&t);
}

/* ─── origin mode ────────────────────────────────────────────────────────── */

TEST(origin, off_addresses_the_whole_screen) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 6, 0));
    ASSERT_TRUE(terminal_set_scroll_region(&t, 2, 5));

    ASSERT_TRUE(terminal_cursor_position(&t, 0, 0));
    ASSERT_EQ(t.active->cursor.y, 0);

    terminal_deinit(&t);
}

TEST(origin, on_addresses_the_region) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 6, 0));
    ASSERT_TRUE(terminal_set_scroll_region(&t, 2, 5));
    t.modes.origin = true;

    /* A program that set a region can address it from the top without
     * knowing where on the screen it put it. */
    ASSERT_TRUE(terminal_cursor_position(&t, 0, 0));
    ASSERT_EQ(t.active->cursor.y, 2);

    ASSERT_TRUE(terminal_cursor_position(&t, 0, 2));
    ASSERT_EQ(t.active->cursor.y, 4);

    terminal_deinit(&t);
}

TEST(origin, on_keeps_the_cursor_inside_the_region) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 6, 0));
    ASSERT_TRUE(terminal_set_scroll_region(&t, 2, 4));
    t.modes.origin = true;

    ASSERT_TRUE(terminal_cursor_position(&t, 0, 99));
    ASSERT_EQ(t.active->cursor.y, t.scroll_bot);

    terminal_deinit(&t);
}

/* ─── inserting and deleting lines ───────────────────────────────────────── */

TEST(lines, inserting_pushes_the_rest_down) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 6, 0));
    label(&t, 6);

    ASSERT_TRUE(screen_cursor_absolute(t.active, 3, 2));
    ASSERT_TRUE(terminal_insert_lines(&t, 1));

    ASSERT_TRUE(rows_read(&t, "AB.CDE"));
    /* Left margin afterwards, which is in the standard and which programs
     * rely on. */
    ASSERT_EQ(t.active->cursor.x, 0);
    ASSERT_EQ(t.active->cursor.y, 2);

    terminal_deinit(&t);
}

TEST(lines, deleting_pulls_the_rest_up) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 6, 0));
    label(&t, 6);

    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 1));
    ASSERT_TRUE(terminal_delete_lines(&t, 2));

    ASSERT_TRUE(rows_read(&t, "ADEF.."));

    terminal_deinit(&t);
}

TEST(lines, inserting_stops_at_the_bottom_of_the_region) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 6, 0));
    label(&t, 6);
    ASSERT_TRUE(terminal_set_scroll_region(&t, 1, 4));

    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 1));
    ASSERT_TRUE(terminal_insert_lines(&t, 1));

    /* Row 5 is outside the region and does not move; the region's last row
     * falls off rather than pushing past it. */
    ASSERT_TRUE(rows_read(&t, "A.BCDF"));

    terminal_deinit(&t);
}

TEST(lines, a_cursor_outside_the_region_does_nothing) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 6, 0));
    label(&t, 6);
    ASSERT_TRUE(terminal_set_scroll_region(&t, 1, 3));

    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 5));
    ASSERT_TRUE(terminal_insert_lines(&t, 2));
    ASSERT_TRUE(terminal_delete_lines(&t, 2));

    ASSERT_TRUE(rows_read(&t, "ABCDEF"));

    terminal_deinit(&t);
}

TEST(lines, more_than_the_region_holds_clears_it) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 6, 0));
    label(&t, 6);
    ASSERT_TRUE(terminal_set_scroll_region(&t, 1, 4));

    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 1));
    ASSERT_TRUE(terminal_delete_lines(&t, 99));

    ASSERT_TRUE(rows_read(&t, "A....F"));

    terminal_deinit(&t);
}

/* ─── inserting and deleting characters ──────────────────────────────────── */

TEST(chars, inserting_pushes_the_line_right) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 4, 0));
    write_text(&t, "abcdef");

    ASSERT_TRUE(screen_cursor_absolute(t.active, 2, 0));
    ASSERT_TRUE(terminal_insert_chars(&t, 2));

    char got[32];
    read_row(&t, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "ab  cdef") == 0);

    terminal_deinit(&t);
}

TEST(chars, inserting_drops_what_falls_off_the_end) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 6, 4, 0));
    write_text(&t, "abcdef");

    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 0));
    ASSERT_TRUE(terminal_insert_chars(&t, 2));

    char got[32];
    read_row(&t, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "  abcd") == 0);

    terminal_deinit(&t);
}

TEST(chars, deleting_pulls_the_line_left) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 4, 0));
    write_text(&t, "abcdef");

    ASSERT_TRUE(screen_cursor_absolute(t.active, 1, 0));
    ASSERT_TRUE(terminal_delete_chars(&t, 2));

    char got[32];
    read_row(&t, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "adef") == 0);

    terminal_deinit(&t);
}

TEST(chars, deleting_more_than_the_line_holds_clears_it) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 4, 0));
    write_text(&t, "abcdef");

    ASSERT_TRUE(screen_cursor_absolute(t.active, 2, 0));
    ASSERT_TRUE(terminal_delete_chars(&t, 99));

    char got[32];
    read_row(&t, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "ab") == 0);

    terminal_deinit(&t);
}

TEST(chars, inserting_and_deleting_carry_styles) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 10, 4, 0));

    t.active->cursor.style.fg_color.tag = style::StyleColor::Tag::palette;
    t.active->cursor.style.fg_color.palette = 21;
    write_text(&t, "xyz");
    t.active->cursor.style = style::Style();

    ASSERT_TRUE(screen_cursor_absolute(t.active, 0, 0));
    ASSERT_TRUE(terminal_insert_chars(&t, 2));

    Pin p = page_list_active_pin(&t.active->pages, 0, 0);
    ASSERT_EQ(p.node->page.get_cell(2, p.y)->codepoint(), 'x');
    ASSERT_EQ(p.node->page.get_cell_style(2, p.y).fg_color.palette, 21);

    terminal_deinit(&t);
}

/* ─── resizing ───────────────────────────────────────────────────────────── */

TEST(region, is_reset_by_a_resize) {
    Terminal t;
    ASSERT_TRUE(terminal_init(&t, 20, 10, 0));
    ASSERT_TRUE(terminal_set_scroll_region(&t, 3, 7));

    ASSERT_TRUE(terminal_resize(&t, 20, 5));

    /* Its bounds are row numbers, and a region referring to rows the screen
     * no longer has would leave a program scrolling something invisible. */
    ASSERT_EQ(t.scroll_top, 0);
    ASSERT_EQ(t.scroll_bot, 4);
    ASSERT_TRUE(terminal_region_is_whole_screen(&t));

    terminal_deinit(&t);
}
