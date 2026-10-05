/* Tests for src/terminal/screen.hpp.
 *
 * Related to Ghostty src/terminal/Screen.zig
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * MIT License — see THIRD_PARTY_NOTICES.md
 *
 * A screen is a page list plus a cursor, and the cursor is two things at once:
 * a column and row on the screen, which is what escape sequences talk about,
 * and a pin into the text, which is what survives the pages changing. These
 * tests are mostly about those two staying in agreement.
 *
 * Named test_screen_port to keep it apart from test_screen.c, which covers
 * the live terminal buffer this does not yet replace.
 */

#include "test_helpers.h"
#include "screen.hpp"

using namespace wisp::terminal;

/* ─── setting up ─────────────────────────────────────────────────────────── */

TEST(screen, starts_at_the_top_left) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 10, 0));

    ASSERT_EQ(s.cursor.x, 0);
    ASSERT_EQ(s.cursor.y, 0);
    ASSERT_TRUE(s.cursor.page_pin.pin.valid());
    ASSERT_TRUE(s.cursor.page_cell != nullptr);
    ASSERT_TRUE(s.cursor.page_row != nullptr);

    screen_deinit(&s);
}

TEST(screen, the_cursor_is_tracked_from_the_start) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 10, 0));

    /* A cursor that was not tracked would be wrong after the first resize,
     * which is not a state worth having even briefly. */
    ASSERT_TRUE(s.pages.tracked == &s.cursor.page_pin);

    screen_deinit(&s);
    ASSERT_TRUE(s.cursor.page_cell == nullptr);
}

TEST(screen, the_cached_cell_is_the_cell_at_the_cursor) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 10, 0));

    ASSERT_TRUE(screen_cursor_absolute(&s, 7, 3));
    s.cursor.page_cell->set_codepoint('q');

    /* The cached pointer is a shortcut, not a second copy. */
    ASSERT_EQ(screen_cell(&s, 7, 3)->codepoint(), 'q');

    screen_deinit(&s);
}

/* ─── moving ─────────────────────────────────────────────────────────────── */

TEST(screen, absolute_movement) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 10, 0));

    ASSERT_TRUE(screen_cursor_absolute(&s, 12, 6));
    ASSERT_EQ(s.cursor.x, 12);
    ASSERT_EQ(s.cursor.y, 6);
    ASSERT_EQ(s.cursor.page_pin.pin.x, 12);

    screen_deinit(&s);
}

TEST(screen, movement_off_the_screen_is_refused) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 10, 0));
    ASSERT_TRUE(screen_cursor_absolute(&s, 5, 5));

    ASSERT_FALSE(screen_cursor_absolute(&s, 40, 5));
    ASSERT_FALSE(screen_cursor_absolute(&s, 5, 10));

    /* Refused means unmoved. */
    ASSERT_EQ(s.cursor.x, 5);
    ASSERT_EQ(s.cursor.y, 5);

    screen_deinit(&s);
}

TEST(screen, relative_movement_clamps_at_the_edges) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 10, 0));

    screen_cursor_left(&s, 5);
    ASSERT_EQ(s.cursor.x, 0);
    screen_cursor_up(&s, 5);
    ASSERT_EQ(s.cursor.y, 0);

    screen_cursor_right(&s, 1000);
    ASSERT_EQ(s.cursor.x, 39);
    screen_cursor_down(&s, 1000);
    ASSERT_EQ(s.cursor.y, 9);

    screen_deinit(&s);
}

TEST(screen, relative_movement_moves) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 10, 0));

    screen_cursor_right(&s, 4);
    screen_cursor_down(&s, 3);
    ASSERT_EQ(s.cursor.x, 4);
    ASSERT_EQ(s.cursor.y, 3);

    screen_cursor_left(&s, 1);
    screen_cursor_up(&s, 2);
    ASSERT_EQ(s.cursor.x, 3);
    ASSERT_EQ(s.cursor.y, 1);

    screen_deinit(&s);
}

TEST(screen, the_pin_follows_the_coordinates) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 10, 0));

    const size_t top = page_list_row_index(&s.pages,
                                           page_list_active_start(&s.pages));

    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 4));
    ASSERT_EQ(page_list_row_index(&s.pages, s.cursor.page_pin.pin), top + 4);

    screen_deinit(&s);
}

/* ─── scrolling ──────────────────────────────────────────────────────────── */

TEST(screen, moving_down_inside_the_screen_does_not_scroll) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 10, 0));
    const size_t before = s.pages.row_count;

    ASSERT_TRUE(screen_cursor_down_scroll(&s));
    ASSERT_EQ(s.cursor.y, 1);
    ASSERT_EQ(s.pages.row_count, before);

    screen_deinit(&s);
}

TEST(screen, moving_down_at_the_bottom_scrolls) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 10, 0));
    ASSERT_TRUE(screen_cursor_absolute(&s, 2, 9));
    const size_t before = s.pages.row_count;

    ASSERT_TRUE(screen_cursor_down_scroll(&s));

    /* The cursor stays on the last row; the screen moved under it. */
    ASSERT_EQ(s.cursor.y, 9);
    ASSERT_EQ(s.cursor.x, 2);
    ASSERT_EQ(s.pages.row_count, before + 1);
    ASSERT_EQ(page_list_max_scroll(&s.pages), 1u);

    screen_deinit(&s);
}

TEST(screen, what_scrolls_off_is_still_there) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 3, 0));

    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 0));
    s.cursor.page_cell->set_codepoint('t');
    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 2));

    ASSERT_TRUE(screen_cursor_down_scroll(&s));

    /* The row that left the top of the screen was not moved anywhere. It is
     * the same row, in the same page — it is simply not on the screen. */
    ASSERT_EQ(screen_cell(&s, 0, 0)->codepoint(), 0u);
    Pin scrolled = page_list_pin(&s.pages, 0);
    scrolled.x = 0;
    ASSERT_EQ(scrolled.cell()->codepoint(), 't');

    screen_deinit(&s);
}

TEST(screen, scrolling_leaves_the_cached_pointers_usable) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 3, 0));
    ASSERT_TRUE(screen_cursor_absolute(&s, 1, 2));

    /* Growth can allocate a page, and the cached pointers are addresses
     * inside one. Writing through a stale pointer here would land in the row
     * that used to be at the bottom. */
    ASSERT_TRUE(screen_cursor_down_scroll(&s));
    s.cursor.page_cell->set_codepoint('n');

    ASSERT_EQ(screen_cell(&s, 1, 2)->codepoint(), 'n');

    screen_deinit(&s);
}

TEST(screen, scrolling_many_times_keeps_the_cursor_at_the_bottom) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 4, 0));
    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 3));

    for (int i = 0; i < 500; i++) {
        ASSERT_TRUE(screen_cursor_down_scroll(&s));
        s.cursor.page_cell->set_codepoint('x');
        ASSERT_EQ(s.cursor.y, 3);
    }

    ASSERT_EQ(s.pages.row_count, 504u);
    ASSERT_EQ(screen_cell(&s, 0, 3)->codepoint(), 'x');

    screen_deinit(&s);
}

/* ─── resizing ───────────────────────────────────────────────────────────── */

TEST(screen, resizing_the_height_keeps_the_cursor_on_its_row) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 10, 0));
    ASSERT_TRUE(screen_cursor_absolute(&s, 3, 4));
    s.cursor.page_cell->set_codepoint('h');

    ASSERT_TRUE(screen_resize(&s, 40, 20));

    /* Nothing was re-laid, so the cursor is on the same character. Its row
     * number changed because the screen got taller underneath it. */
    ASSERT_EQ(s.cursor.page_cell->codepoint(), 'h');
    ASSERT_EQ(s.cursor.x, 3);

    screen_deinit(&s);
}

TEST(screen, resizing_the_width_keeps_the_cursor_on_its_character) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 4, 0));

    /* A line long enough that narrowing re-lays it. */
    for (CellCountInt x = 0; x < 20; x++) {
        ASSERT_TRUE(screen_cursor_absolute(&s, x, 0));
        s.cursor.page_cell->set_codepoint((uint32_t)('a' + x));
    }
    s.pages.first->page.get_row(s.cursor.page_pin.pin.y)->set_wrap(false);

    ASSERT_TRUE(screen_cursor_absolute(&s, 13, 0));
    ASSERT_EQ(s.cursor.page_cell->codepoint(), 'n');

    ASSERT_TRUE(screen_resize(&s, 5, 4));

    /* The cursor is a tracked pin, so the reflow moved it; the screen
     * coordinates were worked out again from where it landed. */
    ASSERT_EQ(s.cursor.page_cell->codepoint(), 'n');
    ASSERT_EQ(s.cursor.x, 3);

    screen_deinit(&s);
}

TEST(screen, the_cursor_stays_on_the_screen_after_a_resize) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 60, 4, 0));

    for (int i = 0; i < 30; i++) {
        ASSERT_TRUE(screen_cursor_absolute(&s, 0, 3));
        for (CellCountInt x = 0; x < 60; x++) {
            ASSERT_TRUE(screen_cursor_absolute(&s, x, 3));
            s.cursor.page_cell->set_codepoint('a');
        }
        ASSERT_TRUE(screen_cursor_down_scroll(&s));
    }

    ASSERT_TRUE(screen_cursor_absolute(&s, 10, 0));
    ASSERT_TRUE(screen_resize(&s, 6, 4));

    /* A cursor near the top of a screen that just got much narrower reflows
     * into the scrollback. A terminal cannot have a cursor off its screen,
     * so it is clamped onto it. */
    ASSERT_TRUE(s.cursor.y < s.pages.rows);
    ASSERT_TRUE(s.cursor.x < s.pages.cols);
    ASSERT_TRUE(s.cursor.page_cell != nullptr);

    screen_deinit(&s);
}

TEST(screen, the_cursor_is_still_writable_after_a_resize) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 4, 0));
    ASSERT_TRUE(screen_cursor_absolute(&s, 5, 2));

    ASSERT_TRUE(screen_resize(&s, 9, 6));

    /* The list was rebuilt, so every page the cursor knew about is freed.
     * Writing through the cached pointer is the test. */
    s.cursor.page_cell->set_codepoint('w');
    ASSERT_EQ(screen_cell(&s, s.cursor.x, s.cursor.y)->codepoint(), 'w');

    screen_deinit(&s);
}

TEST(screen, a_refused_resize_leaves_the_cursor_alone) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 4, 0));
    ASSERT_TRUE(screen_cursor_absolute(&s, 5, 2));

    ASSERT_FALSE(screen_resize(&s, 0, 4));
    ASSERT_EQ(s.cursor.x, 5);
    ASSERT_EQ(s.cursor.y, 2);
    ASSERT_TRUE(s.cursor.page_cell != nullptr);

    screen_deinit(&s);
}

TEST(screen, resizing_repeatedly_keeps_everything_consistent) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 6, 0));
    for (int i = 0; i < 20; i++) {
        ASSERT_TRUE(screen_cursor_down_scroll(&s));
    }

    const CellCountInt widths[] = {7, 13, 40, 5, 22, 40};
    for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        ASSERT_TRUE(screen_resize(&s, widths[i], 6));

        ASSERT_TRUE(s.cursor.x < s.pages.cols);
        ASSERT_TRUE(s.cursor.y < s.pages.rows);
        ASSERT_TRUE(s.cursor.page_cell != nullptr);

        /* The cursor's two descriptions of itself still agree. */
        Pin from_coords = page_list_active_pin(&s.pages, s.cursor.x, s.cursor.y);
        ASSERT_TRUE(from_coords.valid());
        ASSERT_TRUE(from_coords.node == s.cursor.page_pin.pin.node);
        ASSERT_EQ(from_coords.y, s.cursor.page_pin.pin.y);

        s.cursor.page_cell->set_codepoint('k');
    }

    screen_deinit(&s);
}

/* ─── reading ────────────────────────────────────────────────────────────── */

TEST(screen, reading_off_the_screen_gives_nothing) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 10, 0));

    ASSERT_TRUE(screen_cell(&s, 39, 9) != nullptr);
    ASSERT_TRUE(screen_cell(&s, 40, 9) == nullptr);
    ASSERT_TRUE(screen_cell(&s, 39, 10) == nullptr);
    ASSERT_TRUE(screen_row(&s, 9) != nullptr);
    ASSERT_TRUE(screen_row(&s, 10) == nullptr);

    screen_deinit(&s);
}

TEST(screen, the_screen_is_the_bottom_of_the_list) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 3, 0));

    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(screen_cursor_absolute(&s, 0, s.pages.rows - 1));
        s.cursor.page_cell->set_codepoint((uint32_t)('0' + i));
        ASSERT_TRUE(screen_cursor_down_scroll(&s));
    }

    /* Five lines written, three rows of screen: the last two plus the blank
     * row the cursor is on. */
    ASSERT_EQ(screen_cell(&s, 0, 0)->codepoint(), '3');
    ASSERT_EQ(screen_cell(&s, 0, 1)->codepoint(), '4');
    ASSERT_EQ(screen_cell(&s, 0, 2)->codepoint(), 0u);

    screen_deinit(&s);
}

/* ─── blank rows below the cursor ────────────────────────────────────────── */

TEST(screen, blank_rows_below_the_cursor_are_not_text) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 4, 0));

    /* A full line at the top, cursor on it, three blank rows below — which is
     * what a screen almost always looks like. Narrowing turns that one line
     * into four rows, and if the blanks below kept the room they used to, the
     * cursor would be pushed off the top of the screen. */
    for (CellCountInt x = 0; x < 20; x++) {
        ASSERT_TRUE(screen_cursor_absolute(&s, x, 0));
        s.cursor.page_cell->set_codepoint((uint32_t)('a' + x));
    }
    ASSERT_TRUE(screen_cursor_absolute(&s, 13, 0));

    ASSERT_TRUE(screen_resize(&s, 5, 4));

    ASSERT_EQ(s.cursor.page_cell->codepoint(), 'n');
    ASSERT_EQ(s.cursor.x, 3);
    ASSERT_EQ(s.cursor.y, 2);

    screen_deinit(&s);
}

TEST(screen, blank_lines_above_the_cursor_are_kept) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 6, 0));

    /* A blank line somebody printed, with text after it. Only rows past the
     * cursor are spare room; a gap above it is content. */
    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 0));
    s.cursor.page_cell->set_codepoint('A');
    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 2));
    s.cursor.page_cell->set_codepoint('B');
    ASSERT_TRUE(screen_cursor_absolute(&s, 1, 2));

    ASSERT_TRUE(screen_resize(&s, 10, 6));

    ASSERT_EQ(screen_cell(&s, 0, 0)->codepoint(), 'A');
    ASSERT_EQ(screen_cell(&s, 0, 1)->codepoint(), 0u);
    ASSERT_EQ(screen_cell(&s, 0, 2)->codepoint(), 'B');

    screen_deinit(&s);
}

TEST(screen, trimming_stops_at_anything_pinned) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 6, 0));
    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 0));

    /* A selection end, say, parked on a blank row below the cursor. It is not
     * spare room if something is holding on to it. */
    TrackedPin other;
    page_list_track(&s.pages, &other, page_list_active_pin(&s.pages, 0, 4));
    const size_t before = page_list_row_index(&s.pages, other.pin);

    screen_trim_blank_rows_below_cursor(&s);

    ASSERT_EQ(s.pages.row_count, before + 1);
    ASSERT_TRUE(other.pin.valid());

    page_list_untrack(&s.pages, &other);
    screen_deinit(&s);
}

TEST(screen, a_wrapped_line_below_the_cursor_is_kept) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 6, 0));

    /* The last row is blank but continues a wrapped line, so it is part of
     * the text rather than the end of the screen. */
    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 0));
    screen_row(&s, 4)->set_wrap(true);
    screen_row(&s, 5)->set_wrap_continuation(true);

    const size_t before = s.pages.row_count;
    screen_trim_blank_rows_below_cursor(&s);

    ASSERT_EQ(s.pages.row_count, before);

    screen_deinit(&s);
}

/* ─── writing ────────────────────────────────────────────────────────────── */

static void read_row(Screen *s, CellCountInt y, char *out, size_t out_len) {
    size_t n = 0;
    for (CellCountInt x = 0; x < s->pages.cols && n + 1 < out_len; x++) {
        Cell *c = screen_cell(s, x, y);
        const uint32_t cp = c->codepoint();
        out[n++] = cp ? (char)cp : ' ';
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = '\0';
}

TEST(write, one_character) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 10, 4, 0));

    ASSERT_TRUE(screen_write_codepoint(&s, 'a', 1));
    ASSERT_EQ(screen_cell(&s, 0, 0)->codepoint(), 'a');
    ASSERT_EQ(s.cursor.x, 1);
    ASSERT_FALSE(s.cursor.pending_wrap);

    screen_deinit(&s);
}

TEST(write, a_run_of_text) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 4, 0));

    ASSERT_TRUE(screen_write_ascii(&s, "hello", 5));

    char got[32];
    read_row(&s, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "hello") == 0);
    ASSERT_EQ(s.cursor.x, 5);

    screen_deinit(&s);
}

TEST(write, carries_the_cursor_style) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 10, 4, 0));

    s.cursor.style.fg_color.tag = style::StyleColor::Tag::palette;
    s.cursor.style.fg_color.palette = 42;
    s.cursor.style.flags.bold = true;
    ASSERT_TRUE(screen_write_codepoint(&s, 'x', 1));

    Pin p = page_list_active_pin(&s.pages, 0, 0);
    const style::Style got = p.node->page.get_cell_style(0, p.y);
    ASSERT_EQ(got.fg_color.palette, 42);
    ASSERT_TRUE(got.flags.bold);

    screen_deinit(&s);
}

TEST(write, an_unstyled_character_costs_no_style_slot) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 10, 4, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "plain", 5));

    /* The default style is not interned — that is what makes ordinary text
     * free. */
    ASSERT_EQ(s.pages.first->page.style_count(), 0u);

    screen_deinit(&s);
}

TEST(write, overwriting_releases_the_old_style) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 10, 4, 0));

    s.cursor.style.fg_color.tag = style::StyleColor::Tag::palette;
    s.cursor.style.fg_color.palette = 5;
    ASSERT_TRUE(screen_write_codepoint(&s, 'a', 1));
    ASSERT_EQ(s.pages.first->page.style_count(), 1u);

    s.cursor.style = style::Style();
    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 0));
    ASSERT_TRUE(screen_write_codepoint(&s, 'b', 1));

    ASSERT_EQ(screen_cell(&s, 0, 0)->codepoint(), 'b');
    ASSERT_EQ(s.pages.first->page.style_count(), 0u);

    screen_deinit(&s);
}

TEST(write, running_out_of_styles_grows_the_page) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 4, 0));
    const StyleCountInt budget = s.pages.first->page.capacity.styles;

    /* More distinct styles than the page was laid out for. The page is
     * replaced by a roomier one rather than the write failing, and the
     * cursor's cached pointers have to survive that. */
    for (CellCountInt i = 0; i < 40; i++) {
        ASSERT_TRUE(screen_cursor_absolute(&s, i, 0));
        s.cursor.style = style::Style();
        s.cursor.style.fg_color.tag = style::StyleColor::Tag::palette;
        s.cursor.style.fg_color.palette = (uint8_t)(i + 1);
        ASSERT_TRUE(screen_write_codepoint(&s, (uint32_t)('a' + (i % 26)), 1));
    }

    ASSERT_TRUE(s.pages.first->page.capacity.styles > budget);

    Pin p = page_list_active_pin(&s.pages, 0, 0);
    for (CellCountInt i = 0; i < 40; i++) {
        ASSERT_EQ(p.node->page.get_cell_style(i, p.y).fg_color.palette,
                  (uint8_t)(i + 1));
    }

    screen_deinit(&s);
}

/* ─── wrapping ───────────────────────────────────────────────────────────── */

TEST(write, the_last_column_does_not_move_the_cursor) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 5, 4, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "abcde", 5));

    /* There is nowhere to move to, so the cursor stays and leaves a note
     * that the next character wraps first. */
    ASSERT_EQ(s.cursor.x, 4);
    ASSERT_EQ(s.cursor.y, 0);
    ASSERT_TRUE(s.cursor.pending_wrap);

    screen_deinit(&s);
}

TEST(write, a_line_ending_exactly_at_the_margin_does_not_wrap) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 5, 4, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "abcde", 5));

    /* Nothing followed it, so no wrap happened and the row is not joined to
     * the next. Doing the wrap eagerly is what produces a spurious blank
     * line after a line that exactly fits. */
    ASSERT_FALSE(screen_row(&s, 0)->wrap());
    ASSERT_FALSE(screen_row(&s, 1)->wrap_continuation());

    screen_deinit(&s);
}

TEST(write, one_more_character_wraps) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 5, 4, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "abcdef", 6));

    ASSERT_TRUE(screen_row(&s, 0)->wrap());
    ASSERT_TRUE(screen_row(&s, 1)->wrap_continuation());
    ASSERT_EQ(screen_cell(&s, 0, 1)->codepoint(), 'f');
    ASSERT_EQ(s.cursor.x, 1);
    ASSERT_EQ(s.cursor.y, 1);

    screen_deinit(&s);
}

TEST(write, wrapping_at_the_bottom_scrolls) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 4, 2, 0));
    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 1));

    ASSERT_TRUE(screen_write_ascii(&s, "abcdef", 6));

    /* The wrap had nowhere to go, so the screen moved instead. */
    ASSERT_EQ(page_list_max_scroll(&s.pages), 1u);
    ASSERT_EQ(s.cursor.y, 1);
    ASSERT_EQ(screen_cell(&s, 0, 1)->codepoint(), 'e');

    screen_deinit(&s);
}

TEST(write, with_wrapping_off_the_margin_overwrites) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 5, 4, 0));
    s.auto_wrap = false;

    ASSERT_TRUE(screen_write_ascii(&s, "abcdefg", 7));

    /* Each character past the margin replaces the one before it, and the
     * line never becomes two. */
    char got[32];
    read_row(&s, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "abcdg") == 0);
    ASSERT_FALSE(screen_row(&s, 0)->wrap());
    ASSERT_EQ(s.cursor.y, 0);

    screen_deinit(&s);
}

TEST(write, moving_the_cursor_clears_a_pending_wrap) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 5, 4, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "abcde", 5));
    ASSERT_TRUE(s.cursor.pending_wrap);

    screen_cursor_left(&s, 1);
    ASSERT_FALSE(s.cursor.pending_wrap);

    ASSERT_TRUE(screen_write_codepoint(&s, 'z', 1));
    ASSERT_EQ(screen_cell(&s, 3, 0)->codepoint(), 'z');
    ASSERT_FALSE(screen_row(&s, 0)->wrap());

    screen_deinit(&s);
}

/* ─── wide characters ────────────────────────────────────────────────────── */

TEST(write, a_wide_character_takes_two_cells) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 10, 4, 0));

    ASSERT_TRUE(screen_write_codepoint(&s, 0x4E2D, 2));

    ASSERT_EQ(screen_cell(&s, 0, 0)->codepoint(), 0x4E2Du);
    ASSERT_TRUE(screen_cell(&s, 0, 0)->wide() == Wide::wide);
    ASSERT_TRUE(screen_cell(&s, 1, 0)->wide() == Wide::spacer_tail);
    ASSERT_EQ(s.cursor.x, 2);

    screen_deinit(&s);
}

TEST(write, a_wide_character_will_not_straddle_the_margin) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 5, 4, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "abcd", 4));

    ASSERT_TRUE(screen_write_codepoint(&s, 0x4E2D, 2));

    /* The column it could not use says so, which is how a reflow later knows
     * the gap was the margin's doing and not something that was printed. */
    ASSERT_TRUE(screen_cell(&s, 4, 0)->wide() == Wide::spacer_head);
    ASSERT_TRUE(screen_row(&s, 0)->wrap());
    ASSERT_EQ(screen_cell(&s, 0, 1)->codepoint(), 0x4E2Du);
    ASSERT_TRUE(screen_cell(&s, 1, 1)->wide() == Wide::spacer_tail);

    screen_deinit(&s);
}

TEST(write, overwriting_a_wide_lead_clears_its_spacer) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 10, 4, 0));
    ASSERT_TRUE(screen_write_codepoint(&s, 0x4E2D, 2));

    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 0));
    ASSERT_TRUE(screen_write_codepoint(&s, 'a', 1));

    /* A spacer with nothing in front of it is a state the terminal should
     * never produce. */
    ASSERT_EQ(screen_cell(&s, 0, 0)->codepoint(), 'a');
    ASSERT_TRUE(screen_cell(&s, 1, 0)->wide() == Wide::narrow);
    ASSERT_EQ(screen_cell(&s, 1, 0)->codepoint(), 0u);

    screen_deinit(&s);
}

TEST(write, overwriting_a_wide_spacer_clears_its_lead) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 10, 4, 0));
    ASSERT_TRUE(screen_write_codepoint(&s, 0x4E2D, 2));

    ASSERT_TRUE(screen_cursor_absolute(&s, 1, 0));
    ASSERT_TRUE(screen_write_codepoint(&s, 'b', 1));

    ASSERT_EQ(screen_cell(&s, 1, 0)->codepoint(), 'b');
    ASSERT_TRUE(screen_cell(&s, 0, 0)->wide() == Wide::narrow);
    ASSERT_EQ(screen_cell(&s, 0, 0)->codepoint(), 0u);

    screen_deinit(&s);
}

/* ─── combining marks ────────────────────────────────────────────────────── */

TEST(write, a_combining_mark_joins_the_character_before_it) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 10, 4, 0));

    ASSERT_TRUE(screen_write_codepoint(&s, 'e', 1));
    ASSERT_TRUE(screen_append_grapheme(&s, 0x0301));

    /* It attaches to the cell behind the cursor, because a mark arrives after
     * the character it modifies. */
    ASSERT_EQ(s.cursor.x, 1);
    Pin p = page_list_active_pin(&s.pages, 0, 0);
    ASSERT_TRUE(p.node->page.get_cell(0, p.y)->has_grapheme());

    const uint32_t *cps = nullptr;
    uint32_t len = 0;
    ASSERT_TRUE(page_grapheme_codepoints(&p.node->page, 0, p.y, &cps, &len));
    ASSERT_EQ(len, 1u);
    ASSERT_EQ(cps[0], 0x0301u);

    screen_deinit(&s);
}

TEST(write, a_combining_mark_with_nothing_to_join_is_refused) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 10, 4, 0));

    ASSERT_FALSE(screen_append_grapheme(&s, 0x0301));

    screen_deinit(&s);
}

TEST(write, a_combining_mark_at_the_margin_joins_the_last_character) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 5, 4, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "abcde", 5));
    ASSERT_TRUE(s.cursor.pending_wrap);

    /* With a wrap pending the cursor is still on the character it wrote, so
     * the mark goes there rather than one cell back. */
    ASSERT_TRUE(screen_append_grapheme(&s, 0x0301));

    Pin p = page_list_active_pin(&s.pages, 4, 0);
    ASSERT_TRUE(p.node->page.get_cell(4, p.y)->has_grapheme());
    ASSERT_FALSE(screen_row(&s, 0)->wrap());

    screen_deinit(&s);
}

TEST(write, a_combining_mark_joins_a_wide_character_not_its_spacer) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 10, 4, 0));
    ASSERT_TRUE(screen_write_codepoint(&s, 0x4E2D, 2));

    ASSERT_TRUE(screen_append_grapheme(&s, 0x0301));

    /* The cell behind the cursor is the spacer, which is not where the
     * character's codepoints live. */
    Pin p = page_list_active_pin(&s.pages, 0, 0);
    ASSERT_TRUE(p.node->page.get_cell(0, p.y)->has_grapheme());
    ASSERT_FALSE(p.node->page.get_cell(1, p.y)->has_grapheme());

    screen_deinit(&s);
}

/* ─── what writing leaves behind ─────────────────────────────────────────── */

TEST(write, text_survives_a_resize) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 4, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "the quick brown fox", 19));

    ASSERT_TRUE(screen_resize(&s, 8, 4));

    /* Written as one line at twenty columns, read back as one line at
     * eight. */
    char got[64];
    size_t n = 0;
    Pin p = page_list_active_start(&s.pages);
    while (p.valid()) {
        const CellCountInt w = page_row_used_width(&p.node->page, p.y);
        for (CellCountInt x = 0; x < w && n + 1 < sizeof(got); x++) {
            got[n++] = (char)p.node->page.get_cell(x, p.y)->codepoint();
        }
        if (!p.row()->wrap()) break;
        if (!p.down(1)) break;
    }
    got[n] = '\0';
    ASSERT_TRUE(strcmp(got, "the quick brown fox") == 0);

    screen_deinit(&s);
}

TEST(write, a_screenful_of_output_scrolls_correctly) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 10, 4, 0));

    /* Twelve lines through a four-row screen, each ending with a scroll the
     * way a newline would. */
    for (int i = 0; i < 12; i++) {
        char line[4];
        line[0] = (char)('a' + i);
        line[1] = (char)('a' + i);
        ASSERT_TRUE(screen_write_ascii(&s, line, 2));
        ASSERT_TRUE(screen_cursor_down_scroll(&s));
        ASSERT_TRUE(screen_cursor_absolute(&s, 0, s.cursor.y));
    }

    char got[32];
    read_row(&s, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "jj") == 0);
    read_row(&s, 2, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "ll") == 0);

    /* And the older lines are in the scrollback rather than gone. */
    Pin old = page_list_pin(&s.pages, 0);
    old.x = 0;
    ASSERT_EQ(old.cell()->codepoint(), 'a');

    screen_deinit(&s);
}

TEST(write, a_wide_character_on_a_one_column_screen_is_refused) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 1, 4, 0));

    /* Wrapping would not help, since the next row is just as narrow. The
     * alternative to refusing is writing the spacer past the end of the
     * row. */
    ASSERT_FALSE(screen_write_codepoint(&s, 0x4E2D, 2));
    ASSERT_EQ(screen_cell(&s, 0, 0)->codepoint(), 0u);
    ASSERT_TRUE(screen_cell(&s, 0, 0)->wide() == Wide::narrow);

    /* A narrow character still works there. */
    ASSERT_TRUE(screen_write_codepoint(&s, 'a', 1));
    ASSERT_EQ(screen_cell(&s, 0, 0)->codepoint(), 'a');

    screen_deinit(&s);
}

/* ─── erasing ────────────────────────────────────────────────────────────── */

static void fill_screen(Screen *s, char c) {
    for (CellCountInt y = 0; y < s->pages.rows; y++) {
        for (CellCountInt x = 0; x < s->pages.cols; x++) {
            ASSERT_TRUE(screen_cursor_absolute(s, x, y));
            ASSERT_TRUE(screen_write_codepoint(s, (uint32_t)c, 1));
        }
    }
}

TEST(erase, to_the_end_of_the_line) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 8, 3, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "abcdefgh", 8));
    ASSERT_TRUE(screen_cursor_absolute(&s, 3, 0));

    ASSERT_TRUE(screen_erase_line(&s, 0, false));

    char got[32];
    read_row(&s, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "abc") == 0);

    screen_deinit(&s);
}

TEST(erase, to_the_start_of_the_line) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 8, 3, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "abcdefgh", 8));
    ASSERT_TRUE(screen_cursor_absolute(&s, 3, 0));

    ASSERT_TRUE(screen_erase_line(&s, 1, false));

    /* The cursor's own cell goes too, which is what the standard says and
     * what every program expects. */
    ASSERT_EQ(screen_cell(&s, 3, 0)->codepoint(), 0u);
    ASSERT_EQ(screen_cell(&s, 4, 0)->codepoint(), 'e');

    screen_deinit(&s);
}

TEST(erase, the_whole_line) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 8, 3, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "abcdefgh", 8));
    ASSERT_TRUE(screen_cursor_absolute(&s, 3, 0));

    ASSERT_TRUE(screen_erase_line(&s, 2, false));

    char got[32];
    read_row(&s, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "") == 0);

    screen_deinit(&s);
}

TEST(erase, characters_without_moving_the_cursor) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 8, 3, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "abcdefgh", 8));
    ASSERT_TRUE(screen_cursor_absolute(&s, 2, 0));

    ASSERT_TRUE(screen_erase_chars(&s, 3, false));

    /* A hole in the middle, and the cursor still where it was. */
    ASSERT_EQ(screen_cell(&s, 1, 0)->codepoint(), 'b');
    ASSERT_EQ(screen_cell(&s, 2, 0)->codepoint(), 0u);
    ASSERT_EQ(screen_cell(&s, 4, 0)->codepoint(), 0u);
    ASSERT_EQ(screen_cell(&s, 5, 0)->codepoint(), 'f');
    ASSERT_EQ(s.cursor.x, 2);

    screen_deinit(&s);
}

TEST(erase, characters_past_the_end_of_the_row_is_clamped) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 8, 3, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "abcdefgh", 8));
    ASSERT_TRUE(screen_cursor_absolute(&s, 6, 0));

    ASSERT_TRUE(screen_erase_chars(&s, 100, false));

    char got[32];
    read_row(&s, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "abcdef") == 0);

    screen_deinit(&s);
}

/* ─── the line's shape ───────────────────────────────────────────────────── */

TEST(erase, to_the_end_of_a_line_ends_it) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 4, 3, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "abcdef", 6));
    ASSERT_TRUE(screen_row(&s, 0)->wrap());

    ASSERT_TRUE(screen_cursor_absolute(&s, 2, 0));
    ASSERT_TRUE(screen_erase_line(&s, 0, false));

    /* What the line continued into is gone, so it does not continue. Leaving
     * the flag would have a later resize join the row to something that is
     * no longer there. */
    ASSERT_FALSE(screen_row(&s, 0)->wrap());

    screen_deinit(&s);
}

TEST(erase, characters_do_not_end_the_line) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 4, 3, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "abcdef", 6));
    ASSERT_TRUE(screen_row(&s, 0)->wrap());

    ASSERT_TRUE(screen_cursor_absolute(&s, 1, 0));
    ASSERT_TRUE(screen_erase_chars(&s, 2, false));

    /* A hole in the middle of a line is still the middle of a line. */
    ASSERT_TRUE(screen_row(&s, 0)->wrap());

    screen_deinit(&s);
}

TEST(erase, clears_a_pending_wrap) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 4, 3, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "abcd", 4));
    ASSERT_TRUE(s.cursor.pending_wrap);

    ASSERT_TRUE(screen_erase_line(&s, 0, false));

    /* There is somewhere to write again, so the wrap that was waiting is not
     * waiting any more. */
    ASSERT_FALSE(s.cursor.pending_wrap);
    ASSERT_TRUE(screen_write_codepoint(&s, 'z', 1));
    ASSERT_EQ(screen_cell(&s, 3, 0)->codepoint(), 'z');
    ASSERT_FALSE(screen_row(&s, 0)->wrap());

    screen_deinit(&s);
}

/* ─── the background ─────────────────────────────────────────────────────── */

TEST(erase, an_erased_cell_takes_the_current_background) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 8, 3, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "abcdefgh", 8));

    s.cursor.style.bg_color.tag = style::StyleColor::Tag::palette;
    s.cursor.style.bg_color.palette = 4;
    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 0));
    ASSERT_TRUE(screen_erase_line(&s, 2, false));

    /* A program that sets a colour and clears the screen expects the screen
     * to be that colour. */
    Cell *c = screen_cell(&s, 2, 0);
    ASSERT_TRUE(c->content_tag() == ContentTag::bg_color_palette);
    ASSERT_EQ(c->color_palette(), 4);
    ASSERT_TRUE(c->has_bg_color());

    screen_deinit(&s);
}

TEST(erase, a_background_costs_no_style_slot) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 8, 3, 0));

    s.cursor.style.bg_color.tag = style::StyleColor::Tag::rgb;
    s.cursor.style.bg_color.rgb = RGB(10, 20, 30);
    ASSERT_TRUE(screen_erase_display(&s, 2, false));

    /* The colour goes into the cell rather than through the style set, which
     * is what keeps an erase of the whole screen from wanting a style slot
     * for something whose only content is a background. */
    ASSERT_EQ(s.pages.first->page.style_count(), 0u);
    Cell *c = screen_cell(&s, 3, 1);
    ASSERT_TRUE(c->content_tag() == ContentTag::bg_color_rgb);
    ASSERT_TRUE(c->color_rgb().eql(RGB(10, 20, 30)));

    screen_deinit(&s);
}

TEST(erase, releases_what_the_cells_held) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 8, 3, 0));

    s.cursor.style.fg_color.tag = style::StyleColor::Tag::palette;
    s.cursor.style.fg_color.palette = 9;
    ASSERT_TRUE(screen_write_ascii(&s, "abcd", 4));
    ASSERT_TRUE(screen_append_grapheme(&s, 0x0301));
    ASSERT_EQ(s.pages.first->page.style_count(), 1u);

    s.cursor.style = style::Style();
    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 0));
    ASSERT_TRUE(screen_erase_line(&s, 2, false));

    /* Erasing is not just blanking the cells: what they referred to has to go
     * back, or a screen cleared often enough runs its page out of styles. */
    ASSERT_EQ(s.pages.first->page.style_count(), 0u);
    Pin p = page_list_active_pin(&s.pages, 0, 0);
    ASSERT_FALSE(p.node->page.get_cell(3, p.y)->has_grapheme());

    screen_deinit(&s);
}

/* ─── wide characters ────────────────────────────────────────────────────── */

TEST(erase, takes_both_halves_of_a_wide_character) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 8, 3, 0));
    ASSERT_TRUE(screen_cursor_absolute(&s, 2, 0));
    ASSERT_TRUE(screen_write_codepoint(&s, 0x4E2D, 2));

    /* Erasing from column 3 catches the spacer, and its lead has to go with
     * it — a spacer with nothing in front of it is the same broken state
     * whichever way it is arrived at. */
    ASSERT_TRUE(screen_cursor_absolute(&s, 3, 0));
    ASSERT_TRUE(screen_erase_line(&s, 0, false));

    ASSERT_EQ(screen_cell(&s, 2, 0)->codepoint(), 0u);
    ASSERT_TRUE(screen_cell(&s, 2, 0)->wide() == Wide::narrow);

    screen_deinit(&s);
}

TEST(erase, takes_the_spacer_of_a_wide_character_at_the_edge) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 8, 3, 0));
    ASSERT_TRUE(screen_cursor_absolute(&s, 2, 0));
    ASSERT_TRUE(screen_write_codepoint(&s, 0x4E2D, 2));

    /* The other direction: erasing up to and including the lead must not
     * leave its spacer behind. */
    ASSERT_TRUE(screen_cursor_absolute(&s, 2, 0));
    ASSERT_TRUE(screen_erase_line(&s, 1, false));

    ASSERT_TRUE(screen_cell(&s, 3, 0)->wide() == Wide::narrow);
    ASSERT_EQ(screen_cell(&s, 3, 0)->codepoint(), 0u);

    screen_deinit(&s);
}

/* ─── protection ─────────────────────────────────────────────────────────── */

TEST(erase, a_selective_erase_spares_protected_cells) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 8, 3, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "abcdefgh", 8));

    Pin p = page_list_active_pin(&s.pages, 0, 0);
    p.node->page.get_cell(2, p.y)->set_protect(true);
    p.node->page.get_cell(3, p.y)->set_protect(true);

    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 0));
    ASSERT_TRUE(screen_erase_line(&s, 2, true));

    char got[32];
    read_row(&s, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "  cd") == 0);

    screen_deinit(&s);
}

TEST(erase, an_ordinary_erase_ignores_protection) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 8, 3, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "abcdefgh", 8));

    Pin p = page_list_active_pin(&s.pages, 0, 0);
    p.node->page.get_cell(2, p.y)->set_protect(true);

    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 0));
    ASSERT_TRUE(screen_erase_line(&s, 2, false));

    char got[32];
    read_row(&s, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "") == 0);

    screen_deinit(&s);
}

/* ─── the display ────────────────────────────────────────────────────────── */

TEST(erase, display_to_the_bottom) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 4, 4, 0));
    fill_screen(&s, 'x');
    ASSERT_TRUE(screen_cursor_absolute(&s, 2, 1));

    ASSERT_TRUE(screen_erase_display(&s, 0, false));

    char got[32];
    read_row(&s, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "xxxx") == 0);
    read_row(&s, 1, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "xx") == 0);
    read_row(&s, 2, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "") == 0);
    read_row(&s, 3, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "") == 0);

    screen_deinit(&s);
}

TEST(erase, display_to_the_top) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 4, 4, 0));
    fill_screen(&s, 'x');
    ASSERT_TRUE(screen_cursor_absolute(&s, 1, 2));

    ASSERT_TRUE(screen_erase_display(&s, 1, false));

    char got[32];
    read_row(&s, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "") == 0);
    read_row(&s, 2, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "  xx") == 0);
    read_row(&s, 3, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "xxxx") == 0);

    screen_deinit(&s);
}

TEST(erase, the_whole_display) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 4, 4, 0));
    fill_screen(&s, 'x');

    ASSERT_TRUE(screen_erase_display(&s, 2, false));

    for (CellCountInt y = 0; y < 4; y++) {
        char got[32];
        read_row(&s, y, got, sizeof(got));
        ASSERT_TRUE(strcmp(got, "") == 0);
    }

    screen_deinit(&s);
}

TEST(erase, the_display_keeps_the_scrollback) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 4, 3, 0));

    ASSERT_TRUE(screen_write_ascii(&s, "old", 3));
    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(screen_cursor_absolute(&s, 0, s.cursor.y));
        ASSERT_TRUE(screen_cursor_down_scroll(&s));
    }
    const size_t before = page_list_max_scroll(&s.pages);
    ASSERT_TRUE(before > 0u);

    ASSERT_TRUE(screen_erase_display(&s, 2, false));

    /* Clearing the screen does not throw away what scrolled off it. */
    ASSERT_EQ(page_list_max_scroll(&s.pages), before);
    Pin old = page_list_pin(&s.pages, 0);
    old.x = 0;
    ASSERT_EQ(old.cell()->codepoint(), 'o');

    screen_deinit(&s);
}

/* ─── the scrollback ─────────────────────────────────────────────────────── */

TEST(erase, the_scrollback_and_nothing_else) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 4, 3, 0));

    for (int i = 0; i < 10; i++) {
        ASSERT_TRUE(screen_cursor_absolute(&s, 0, s.cursor.y));
        ASSERT_TRUE(screen_write_codepoint(&s, (uint32_t)('0' + i), 1));
        ASSERT_TRUE(screen_cursor_down_scroll(&s));
    }
    ASSERT_TRUE(page_list_max_scroll(&s.pages) > 0u);

    ASSERT_TRUE(screen_erase_display(&s, 3, false));

    /* Nothing above the screen is left, and the screen is untouched. */
    ASSERT_EQ(page_list_max_scroll(&s.pages), 0u);
    ASSERT_EQ(s.pages.row_count, 3u);
    ASSERT_EQ(screen_cell(&s, 0, 0)->codepoint(), '8');
    ASSERT_EQ(screen_cell(&s, 0, 1)->codepoint(), '9');

    screen_deinit(&s);
}

TEST(erase, the_scrollback_leaves_the_cursor_working) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 4, 3, 0));
    for (int i = 0; i < 10; i++) {
        ASSERT_TRUE(screen_cursor_absolute(&s, 0, s.cursor.y));
        ASSERT_TRUE(screen_cursor_down_scroll(&s));
    }

    ASSERT_TRUE(screen_cursor_absolute(&s, 1, 1));
    ASSERT_TRUE(screen_erase_display(&s, 3, false));

    /* Pages were freed, and the cursor's cached pointers were addresses
     * inside them. */
    ASSERT_TRUE(s.cursor.page_cell != nullptr);
    ASSERT_TRUE(screen_write_codepoint(&s, 'k', 1));
    ASSERT_EQ(screen_cell(&s, 1, 1)->codepoint(), 'k');

    screen_deinit(&s);
}

TEST(erase, the_scrollback_when_there_is_none) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 4, 3, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "ab", 2));

    ASSERT_TRUE(screen_erase_display(&s, 3, false));

    ASSERT_EQ(s.pages.row_count, 3u);
    ASSERT_EQ(screen_cell(&s, 0, 0)->codepoint(), 'a');

    screen_deinit(&s);
}

TEST(erase, an_unknown_mode_is_refused) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 4, 3, 0));
    fill_screen(&s, 'x');

    ASSERT_FALSE(screen_erase_display(&s, 9, false));
    ASSERT_FALSE(screen_erase_line(&s, 9, false));

    char got[32];
    read_row(&s, 0, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "xxxx") == 0);

    screen_deinit(&s);
}

TEST(erase, characters_to_the_end_of_a_wrapped_row_keep_the_wrap) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 4, 3, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "abcdef", 6));
    ASSERT_TRUE(screen_row(&s, 0)->wrap());

    /* ECH reaching the right edge still is not the end of the line: the row
     * below carries on from it, and a resize must still join them. */
    ASSERT_TRUE(screen_cursor_absolute(&s, 1, 0));
    ASSERT_TRUE(screen_erase_chars(&s, 10, false));

    ASSERT_TRUE(screen_row(&s, 0)->wrap());
    ASSERT_EQ(screen_cell(&s, 0, 0)->codepoint(), 'a');
    ASSERT_EQ(screen_cell(&s, 3, 0)->codepoint(), 0u);
    ASSERT_EQ(screen_cell(&s, 0, 1)->codepoint(), 'e');

    screen_deinit(&s);
}

/* ─── selections ─────────────────────────────────────────────────────────── */

/* Write text at the cursor, wrapping the way output does. */
static void put_line(Screen *s, CellCountInt y, const char *text) {
    ASSERT_TRUE(screen_cursor_absolute(s, 0, y));
    ASSERT_TRUE(screen_write_ascii(s, text, strlen(text)));
}

static Pin at(Screen *s, CellCountInt x, CellCountInt y) {
    return page_list_active_pin(&s->pages, x, y);
}

TEST(selection, selecting_and_clearing) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 10, 4, 0));

    ASSERT_FALSE(s.selection.active);
    screen_select(&s, at(&s, 1, 0), at(&s, 4, 1), false);

    ASSERT_TRUE(s.selection.active);
    /* Both ends are tracked, so the list will maintain them. */
    ASSERT_TRUE(s.pages.tracked != nullptr);

    screen_select_clear(&s);
    ASSERT_FALSE(s.selection.active);

    screen_deinit(&s);
}

TEST(selection, contains_a_run_across_rows) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 10, 4, 0));
    screen_select(&s, at(&s, 3, 0), at(&s, 5, 2), false);

    ASSERT_FALSE(screen_selection_contains(&s, at(&s, 2, 0)));
    ASSERT_TRUE(screen_selection_contains(&s, at(&s, 3, 0)));
    ASSERT_TRUE(screen_selection_contains(&s, at(&s, 9, 0)));

    /* A middle row is selected end to end. */
    ASSERT_TRUE(screen_selection_contains(&s, at(&s, 0, 1)));
    ASSERT_TRUE(screen_selection_contains(&s, at(&s, 9, 1)));

    ASSERT_TRUE(screen_selection_contains(&s, at(&s, 5, 2)));
    ASSERT_FALSE(screen_selection_contains(&s, at(&s, 6, 2)));
    ASSERT_FALSE(screen_selection_contains(&s, at(&s, 0, 3)));

    screen_deinit(&s);
}

TEST(selection, dragging_upwards_selects_the_same_thing) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 10, 4, 0));

    /* Selecting upwards is ordinary, so the ends arrive the other way round
     * and everything that needs them in order asks for them in order. */
    screen_select(&s, at(&s, 5, 2), at(&s, 3, 0), false);

    Pin tl, br;
    ASSERT_TRUE(screen_selection_ordered(&s, &tl, &br));
    ASSERT_EQ(tl.x, 3);
    ASSERT_EQ(br.x, 5);
    ASSERT_TRUE(screen_selection_contains(&s, at(&s, 0, 1)));
    ASSERT_FALSE(screen_selection_contains(&s, at(&s, 2, 0)));

    screen_deinit(&s);
}

TEST(selection, a_rectangle_keeps_its_columns) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 10, 4, 0));
    screen_select(&s, at(&s, 2, 0), at(&s, 5, 2), true);

    /* A block is the columns between its edges on every row, not everything
     * from one corner to the other. */
    ASSERT_TRUE(screen_selection_contains(&s, at(&s, 2, 1)));
    ASSERT_TRUE(screen_selection_contains(&s, at(&s, 5, 1)));
    ASSERT_FALSE(screen_selection_contains(&s, at(&s, 1, 1)));
    ASSERT_FALSE(screen_selection_contains(&s, at(&s, 6, 1)));
    ASSERT_FALSE(screen_selection_contains(&s, at(&s, 9, 0)));

    screen_deinit(&s);
}

TEST(selection, a_rectangle_dragged_leftwards_still_covers_its_columns) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 10, 4, 0));
    screen_select(&s, at(&s, 6, 0), at(&s, 2, 2), true);

    ASSERT_TRUE(screen_selection_contains(&s, at(&s, 4, 1)));
    ASSERT_FALSE(screen_selection_contains(&s, at(&s, 7, 1)));

    screen_deinit(&s);
}

/* ─── reading it out ─────────────────────────────────────────────────────── */

TEST(selection, text_of_one_row) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 4, 0));
    put_line(&s, 0, "hello world");

    screen_select(&s, at(&s, 0, 0), at(&s, 4, 0), false);

    char got[64];
    ASSERT_EQ(screen_selection_text(&s, got, sizeof(got)), 5u);
    ASSERT_TRUE(strcmp(got, "hello") == 0);

    screen_deinit(&s);
}

TEST(selection, text_across_hard_line_ends) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 4, 0));
    put_line(&s, 0, "one");
    put_line(&s, 1, "two");

    screen_select(&s, at(&s, 0, 0), at(&s, 2, 1), false);

    char got[64];
    screen_selection_text(&s, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "one\ntwo") == 0);

    screen_deinit(&s);
}

TEST(selection, a_soft_wrap_is_not_a_line_ending) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 5, 4, 0));

    /* Written as one line, broken across rows by the screen's width. Pasting
     * it back should give what the program printed, not the shape the screen
     * happened to break it into. */
    put_line(&s, 0, "abcdefghij");
    ASSERT_TRUE(screen_row(&s, 0)->wrap());

    screen_select(&s, at(&s, 0, 0), at(&s, 4, 1), false);

    char got[64];
    screen_selection_text(&s, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "abcdefghij") == 0);

    screen_deinit(&s);
}

TEST(selection, trailing_blanks_are_dropped) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 4, 0));
    put_line(&s, 0, "short");
    put_line(&s, 1, "next");

    screen_select(&s, at(&s, 0, 0), at(&s, 19, 1), false);

    /* The blanks after a line are the rest of the screen, not spaces anybody
     * typed. */
    char got[64];
    screen_selection_text(&s, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "short\nnext") == 0);

    screen_deinit(&s);
}

TEST(selection, a_blank_row_between_lines_is_kept) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 4, 0));
    put_line(&s, 0, "a");
    put_line(&s, 2, "b");

    screen_select(&s, at(&s, 0, 0), at(&s, 0, 2), false);

    char got[64];
    screen_selection_text(&s, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "a\n\nb") == 0);

    screen_deinit(&s);
}

TEST(selection, a_rectangle_is_all_line_endings) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 5, 4, 0));
    put_line(&s, 0, "abcdefghij");

    /* Its rows are not joined to each other in any sense, whatever the wrap
     * flags say, because the shape is what the user asked for. */
    screen_select(&s, at(&s, 1, 0), at(&s, 3, 1), true);

    char got[64];
    screen_selection_text(&s, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "bcd\nghi") == 0);

    screen_deinit(&s);
}

TEST(selection, text_includes_combining_marks) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 4, 0));
    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 0));
    ASSERT_TRUE(screen_write_codepoint(&s, 'e', 1));
    ASSERT_TRUE(screen_append_grapheme(&s, 0x0301));

    screen_select(&s, at(&s, 0, 0), at(&s, 0, 0), false);

    /* The rest of a cluster lives in the page rather than the cell, and it is
     * as much a part of the character as the codepoint that does not. */
    char got[64];
    ASSERT_EQ(screen_selection_text(&s, got, sizeof(got)), 3u);
    ASSERT_TRUE(strcmp(got, "e\xcc\x81") == 0);

    screen_deinit(&s);
}

TEST(selection, text_of_a_wide_character_is_one_character) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 4, 0));
    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 0));
    ASSERT_TRUE(screen_write_codepoint(&s, 0x4E2D, 2));

    screen_select(&s, at(&s, 0, 0), at(&s, 1, 0), false);

    /* Two cells, one character: the spacer is not text. */
    char got[64];
    ASSERT_EQ(screen_selection_text(&s, got, sizeof(got)), 3u);
    ASSERT_TRUE(strcmp(got, "\xe4\xb8\xad") == 0);

    screen_deinit(&s);
}

TEST(selection, text_stops_at_the_buffer_it_was_given) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 4, 0));
    put_line(&s, 0, "abcdefghij");
    screen_select(&s, at(&s, 0, 0), at(&s, 9, 0), false);

    char got[5];
    const size_t n = screen_selection_text(&s, got, sizeof(got));
    ASSERT_TRUE(n < sizeof(got));
    ASSERT_EQ(got[n], '\0');
    ASSERT_TRUE(strncmp(got, "abcd", n) == 0);

    screen_deinit(&s);
}

TEST(selection, no_selection_gives_nothing) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 4, 0));
    put_line(&s, 0, "text");

    char got[64];
    ASSERT_EQ(screen_selection_text(&s, got, sizeof(got)), 0u);
    ASSERT_TRUE(strcmp(got, "") == 0);
    ASSERT_FALSE(screen_selection_contains(&s, at(&s, 0, 0)));

    screen_deinit(&s);
}

/* ─── selecting by line ──────────────────────────────────────────────────── */

TEST(selection, a_line_is_what_was_printed) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 5, 4, 0));
    put_line(&s, 0, "abcdefghijkl");

    /* Three rows on screen, one line as far as anyone printing it was
     * concerned. Selecting from the middle row takes the whole thing. */
    ASSERT_TRUE(screen_select_line(&s, at(&s, 2, 1)));

    char got[64];
    screen_selection_text(&s, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "abcdefghijkl") == 0);

    screen_deinit(&s);
}

TEST(selection, a_line_stops_at_a_hard_end) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 4, 0));
    put_line(&s, 0, "before");
    put_line(&s, 1, "middle");
    put_line(&s, 2, "after");

    ASSERT_TRUE(screen_select_line(&s, at(&s, 1, 1)));

    char got[64];
    screen_selection_text(&s, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "middle") == 0);

    screen_deinit(&s);
}

TEST(selection, selecting_everything_includes_the_scrollback) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 3, 0));
    for (int i = 0; i < 5; i++) {
        ASSERT_TRUE(screen_cursor_absolute(&s, 0, s.cursor.y));
        ASSERT_TRUE(screen_write_codepoint(&s, (uint32_t)('a' + i), 1));
        ASSERT_TRUE(screen_cursor_down_scroll(&s));
    }
    ASSERT_TRUE(page_list_max_scroll(&s.pages) > 0u);

    ASSERT_TRUE(screen_select_all(&s));

    char got[128];
    screen_selection_text(&s, got, sizeof(got));
    ASSERT_TRUE(strncmp(got, "a\nb\nc\nd\ne", 9) == 0);

    screen_deinit(&s);
}

/* ─── what a selection survives ──────────────────────────────────────────── */

TEST(selection, survives_output_scrolling_under_it) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 4, 0));
    put_line(&s, 0, "keep me");
    screen_select(&s, at(&s, 0, 0), at(&s, 6, 0), false);

    /* A selection is made and then sits there while output arrives. Screen
     * coordinates would be pointing at a different line by now. */
    for (int i = 0; i < 10; i++) {
        ASSERT_TRUE(screen_cursor_absolute(&s, 0, s.pages.rows - 1));
        ASSERT_TRUE(screen_cursor_down_scroll(&s));
    }

    char got[64];
    screen_selection_text(&s, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "keep me") == 0);

    screen_deinit(&s);
}

TEST(selection, survives_a_resize) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 4, 0));
    put_line(&s, 0, "the quick brown fox");

    screen_select(&s, at(&s, 4, 0), at(&s, 8, 0), false);
    char before[64];
    screen_selection_text(&s, before, sizeof(before));
    ASSERT_TRUE(strcmp(before, "quick") == 0);

    ASSERT_TRUE(screen_resize(&s, 7, 4));

    /* Both ends are tracked pins, so the reflow moved them with their
     * characters. */
    char got[64];
    screen_selection_text(&s, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "quick") == 0);

    screen_deinit(&s);
}

TEST(selection, is_released_with_the_screen) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 4, 0));
    screen_select(&s, at(&s, 0, 0), at(&s, 3, 0), false);

    screen_deinit(&s);

    /* The pins the list was maintaining are given back, and nothing is left
     * naming a page that has gone. */
    ASSERT_FALSE(s.selection.start_pin.pin.valid());
    ASSERT_FALSE(s.selection.end_pin.pin.valid());
}

TEST(selection, selecting_again_replaces_the_old_one) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 4, 0));
    put_line(&s, 0, "first");
    put_line(&s, 1, "second");

    screen_select(&s, at(&s, 0, 0), at(&s, 4, 0), false);
    screen_select(&s, at(&s, 0, 1), at(&s, 5, 1), false);

    char got[64];
    screen_selection_text(&s, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "second") == 0);

    /* And the pins of the old one were given back rather than left tracked
     * forever. */
    size_t tracked = 0;
    for (TrackedPin *t = s.pages.tracked; t; t = t->next) tracked++;
    ASSERT_EQ(tracked, 3u);   /* the cursor, and the two ends */

    screen_deinit(&s);
}

/* ─── the saved cursor ───────────────────────────────────────────────────── */

TEST(saved, restores_the_position) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 6, 0));
    ASSERT_TRUE(screen_cursor_absolute(&s, 7, 3));

    screen_save_cursor(&s);
    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 0));
    ASSERT_TRUE(screen_restore_cursor(&s));

    ASSERT_EQ(s.cursor.x, 7);
    ASSERT_EQ(s.cursor.y, 3);

    screen_deinit(&s);
}

TEST(saved, restores_the_style) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 6, 0));

    s.cursor.style.fg_color.tag = style::StyleColor::Tag::palette;
    s.cursor.style.fg_color.palette = 12;
    s.cursor.style.flags.bold = true;
    screen_save_cursor(&s);

    /* A program that saves the cursor, prints something in another colour and
     * restores expects its colour back as well as its place. */
    s.cursor.style = style::Style();
    ASSERT_TRUE(screen_restore_cursor(&s));

    ASSERT_EQ(s.cursor.style.fg_color.palette, 12);
    ASSERT_TRUE(s.cursor.style.flags.bold);

    screen_deinit(&s);
}

TEST(saved, restores_a_pending_wrap) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 5, 4, 0));
    ASSERT_TRUE(screen_write_ascii(&s, "abcde", 5));
    ASSERT_TRUE(s.cursor.pending_wrap);

    screen_save_cursor(&s);
    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 2));
    ASSERT_TRUE(screen_restore_cursor(&s));

    /* The pending wrap is as much a part of where the cursor is as the
     * column, so writing after a restore wraps exactly as it would have. */
    ASSERT_TRUE(s.cursor.pending_wrap);
    ASSERT_TRUE(screen_write_codepoint(&s, 'f', 1));
    ASSERT_EQ(screen_cell(&s, 0, 1)->codepoint(), 'f');

    screen_deinit(&s);
}

TEST(saved, an_unsaved_restore_goes_to_the_top_left) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 6, 0));
    ASSERT_TRUE(screen_cursor_absolute(&s, 9, 4));

    ASSERT_TRUE(screen_restore_cursor(&s));
    ASSERT_EQ(s.cursor.x, 0);
    ASSERT_EQ(s.cursor.y, 0);

    screen_deinit(&s);
}

TEST(saved, is_a_place_on_the_screen_not_in_the_text) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 4, 0));
    ASSERT_TRUE(screen_cursor_absolute(&s, 2, 1));
    put_line(&s, 1, "here");
    ASSERT_TRUE(screen_cursor_absolute(&s, 2, 1));
    screen_save_cursor(&s);

    /* Scroll the line it was on off the top. DECSC means "this place on the
     * screen", so the cursor comes back to row 1 rather than chasing the
     * line, which is the opposite of what the cursor's own pin does. */
    for (int i = 0; i < 3; i++) {
        ASSERT_TRUE(screen_cursor_absolute(&s, 0, s.pages.rows - 1));
        ASSERT_TRUE(screen_cursor_down_scroll(&s));
    }

    ASSERT_TRUE(screen_restore_cursor(&s));
    ASSERT_EQ(s.cursor.y, 1);
    ASSERT_EQ(s.cursor.x, 2);
    ASSERT_EQ(s.cursor.page_cell->codepoint(), 0u);

    screen_deinit(&s);
}

TEST(saved, a_position_off_a_shrunken_screen_is_clamped) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 10, 0));
    ASSERT_TRUE(screen_cursor_absolute(&s, 30, 8));
    screen_save_cursor(&s);

    ASSERT_TRUE(screen_resize(&s, 10, 4));
    ASSERT_TRUE(screen_restore_cursor(&s));

    /* Refusing would leave the cursor somewhere the program has no reason to
     * expect, which is worse than as close as the screen allows. */
    ASSERT_TRUE(s.cursor.x < s.pages.cols);
    ASSERT_TRUE(s.cursor.y < s.pages.rows);
    ASSERT_TRUE(s.cursor.page_cell != nullptr);

    screen_deinit(&s);
}

TEST(saved, saving_again_replaces_it) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 6, 0));

    ASSERT_TRUE(screen_cursor_absolute(&s, 1, 1));
    screen_save_cursor(&s);
    ASSERT_TRUE(screen_cursor_absolute(&s, 4, 4));
    screen_save_cursor(&s);

    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 0));
    ASSERT_TRUE(screen_restore_cursor(&s));
    ASSERT_EQ(s.cursor.x, 4);
    ASSERT_EQ(s.cursor.y, 4);

    screen_deinit(&s);
}

TEST(saved, restoring_twice_gives_the_same_place) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 6, 0));
    ASSERT_TRUE(screen_cursor_absolute(&s, 3, 2));
    screen_save_cursor(&s);

    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 0));
    ASSERT_TRUE(screen_restore_cursor(&s));
    ASSERT_TRUE(screen_cursor_absolute(&s, 9, 5));
    ASSERT_TRUE(screen_restore_cursor(&s));

    ASSERT_EQ(s.cursor.x, 3);
    ASSERT_EQ(s.cursor.y, 2);

    screen_deinit(&s);
}

/* ─── selecting by word ──────────────────────────────────────────────────── */

TEST(word, selects_the_run_it_is_in) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 4, 0));
    put_line(&s, 0, "the quick brown fox");

    ASSERT_TRUE(screen_select_word(&s, at(&s, 6, 0)));

    char got[64];
    screen_selection_text(&s, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "quick") == 0);

    screen_deinit(&s);
}

TEST(word, selects_from_either_end_of_a_word) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 4, 0));
    put_line(&s, 0, "the quick brown fox");

    char got[64];
    ASSERT_TRUE(screen_select_word(&s, at(&s, 4, 0)));
    screen_selection_text(&s, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "quick") == 0);

    ASSERT_TRUE(screen_select_word(&s, at(&s, 8, 0)));
    screen_selection_text(&s, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "quick") == 0);

    screen_deinit(&s);
}

TEST(word, selects_a_run_of_spaces) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 4, 0));
    put_line(&s, 0, "a    b");

    /* Clicking in a gap selects the gap, which is what taking "the run of
     * things that are not the other kind" means both ways round. */
    ASSERT_TRUE(screen_select_word(&s, at(&s, 3, 0)));

    Pin tl, br;
    ASSERT_TRUE(screen_selection_ordered(&s, &tl, &br));
    ASSERT_EQ(tl.x, 1);
    ASSERT_EQ(br.x, 4);

    screen_deinit(&s);
}

TEST(word, keeps_punctuation_with_the_word) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 40, 4, 0));
    put_line(&s, 0, "see /usr/local/bin/wisp here");

    /* A path or a URL comes out in one piece, which is what somebody
     * double-clicking one is after. */
    ASSERT_TRUE(screen_select_word(&s, at(&s, 8, 0)));

    char got[64];
    screen_selection_text(&s, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "/usr/local/bin/wisp") == 0);

    screen_deinit(&s);
}

TEST(word, a_word_wrapped_across_rows_is_one_word) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 8, 4, 0));
    put_line(&s, 0, "/a/long/path/here");
    ASSERT_TRUE(screen_row(&s, 0)->wrap());

    /* The screen broke it, not the program, so the run follows the wrap. */
    ASSERT_TRUE(screen_select_word(&s, at(&s, 2, 1)));

    char got[64];
    screen_selection_text(&s, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "/a/long/path/here") == 0);

    screen_deinit(&s);
}

TEST(word, a_hard_line_end_stops_it) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 8, 4, 0));
    put_line(&s, 0, "abcdefgh");
    put_line(&s, 1, "ijkl");

    /* The first row is full but was not wrapped into the second — the program
     * ended the line — so the two are not one word however they look. */
    ASSERT_FALSE(screen_row(&s, 0)->wrap());
    ASSERT_TRUE(screen_select_word(&s, at(&s, 2, 0)));

    char got[64];
    screen_selection_text(&s, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "abcdefgh") == 0);

    screen_deinit(&s);
}

TEST(word, stops_at_the_start_and_end_of_everything) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 6, 3, 0));
    put_line(&s, 0, "ab");

    ASSERT_TRUE(screen_select_word(&s, at(&s, 0, 0)));

    char got[64];
    screen_selection_text(&s, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "ab") == 0);

    screen_deinit(&s);
}

TEST(word, a_wide_character_is_not_split_from_its_spacer) {
    Screen s;
    ASSERT_TRUE(screen_init(&s, 20, 4, 0));
    ASSERT_TRUE(screen_cursor_absolute(&s, 0, 0));
    ASSERT_TRUE(screen_write_codepoint(&s, 0x4E2D, 2));
    ASSERT_TRUE(screen_write_codepoint(&s, 0x6587, 2));

    /* Clicking the spacer means clicking the character in front of it. */
    ASSERT_TRUE(screen_select_word(&s, at(&s, 1, 0)));

    char got[64];
    screen_selection_text(&s, got, sizeof(got));
    ASSERT_TRUE(strcmp(got, "\xe4\xb8\xad\xe6\x96\x87") == 0);

    screen_deinit(&s);
}
